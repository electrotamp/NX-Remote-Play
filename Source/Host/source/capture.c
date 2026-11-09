#include <string.h>
#include <stdio.h>

#include "core.h"
#include "grcd.h"
#include "capture.h"
#include "nxrp_video.h"
#include "nxrp_source.h"
#include "modes/defines.h"
#include "modes/Viewers.h"
#include "modes/UdpmVideo.h"
#include <stdatomic.h>

VideoPacket alignas(0x1000) VPkt;
AudioPacket alignas(0x1000) APkt;

static Service grcdVideo;
static Service grcdAudio;

// To support disabling and enabling nxremoteplay without rebooting the console we now don't call grcdServiceBegin on
// start. This is because this function globally enables a flag in grc, when this is called with the flag already set it
// will cause the console to crash The solution is to attempt to record directory and if we get the error code
// 0x3E8D4/2212-0500 we know grcdServiceBegin was not called before and it's safe to call Note that this same error code
// is also returned when grcdTransfer is called with an invalid stream argument, but this does not apply to us.
#define GRCD_NOT_INITIALIZED 0x3E8D4
static atomic_bool grcdBeginCalled = false;
static Mutex grcdBeginMutex;

// On a new connection, drop non-IDR units until a genuine IDR is available. Injecting
// SPS/PPS before a P-slice leaves the decoder without a reference frame and can
// produce no output despite incoming packets.
static bool g_waitingForResumeIdr = false;

static atomic_bool capturing = false;

static bool EnsureGrcdInit(Result *rc)
{
    if (grcdBeginCalled)
        return false;

    mutexLock(&grcdBeginMutex);

    if (grcdBeginCalled)
    {
        mutexUnlock(&grcdBeginMutex);
        return false;
    }

    grcdBeginCalled = true;

    *rc = grcdServiceBegin(&grcdVideo);

    mutexUnlock(&grcdBeginMutex);

    return true;
}

// User configuration
static int AudioBatching = DefaultABatching;

// Bounded waits keep the transport consumer independent of capture workers.
#define REAL_VIDEO_INITIAL_WAIT_NS (1000000000ull) // 1s

static bool g_wasScreenshotLastFrame = false;

static Mutex g_videoRawMutex;
static CondVar g_videoRawCondVar;
// this used to be a SINGLE-slot mailbox (one buffer + a sequence number). If the
// consumer (the per-client send loop) was even one frame late, the pump overwrote the frame that
// had not been read yet - a silently dropped P-frame, after which the client decodes against a
// missing reference and shows blocky smear until the next IDR (confirmed on hardware: a corrupt
// in-game screenshot lined up exactly with a ~125 ms consumer hiccup; the "frames sent" counter
// never saw the lost frame). Now a bounded FIFO of whole frames: a late consumer just gets them
// late, in order, and catches up. If the FIFO ever does overflow (consumer stalled for far longer
// than a few hundred ms) it is cleared and non-IDR frames are discarded until the next IDR, so the
// client freezes on its last good picture instead of decoding damaged frames.
#define VRING_SIZE 0xC0000u          // 768 KiB of variable-size records
#define VRING_HDR 12u                // u32 size + u64 grc timestamp
static u8 g_vring[VRING_SIZE];
static u32 g_vHead = 0, g_vTail = 0, g_vUsed = 0, g_vCount = 0;
static bool g_vDropUntilIdr = false;
// Last transport-consumer activity; capture idles without a viewer.
static u64 g_vLastConsumerTick = 0;

// The wrap-around copies below are bounded by construction (first <= n, first <= ring space left,
// and the caller checked the record fits), but GCC cannot see through the modular arithmetic and
// reports a false positive under -Werror.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
static void VringCopyIn(const void *src, u32 n)
{
    const u8 *s = (const u8 *)src;
    const u32 t = g_vTail % VRING_SIZE; // (modulo lets the compiler prove the bounds)
    u32 first = VRING_SIZE - t;
    if (first > n)
        first = n;
    memcpy(g_vring + t, s, first);
    if (n > first)
        memcpy(g_vring, s + first, n - first);
    g_vTail = (g_vTail + n) % VRING_SIZE;
}

static void VringCopyOut(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    const u32 h = g_vHead % VRING_SIZE;
    u32 first = VRING_SIZE - h;
    if (first > n)
        first = n;
    memcpy(d, g_vring + h, first);
    if (n > first)
        memcpy(d + first, g_vring, n - first);
    g_vHead = (g_vHead + n) % VRING_SIZE;
}

// Call with g_videoRawMutex held. False if the record does not fit.
static bool VringPush(const u8 *data, u32 size, u64 timestamp)
{
    const u32 need = VRING_HDR + size;
    if (need > VRING_SIZE || VRING_SIZE - g_vUsed < need)
        return false;
    VringCopyIn(&size, 4);
    VringCopyIn(&timestamp, 8);
    VringCopyIn(data, size);
    g_vUsed += need;
    g_vCount++;
    return true;
}

// Call with g_videoRawMutex held. False if empty.
static bool VringPop(u8 *dst, u32 *size, u64 *timestamp)
{
    if (g_vCount == 0)
        return false;
    VringCopyOut(size, 4);
    VringCopyOut(timestamp, 8);
    VringCopyOut(dst, *size);
    g_vUsed -= VRING_HDR + *size;
    g_vCount--;
    return true;
}

static void VringClear(void)
{
    g_vHead = g_vTail = g_vUsed = g_vCount = 0;
}

#pragma GCC diagnostic pop
// Local monotonic capture time of the last real frame's arrival - g_videoRawTimestamp above
// is grc:d's own timestamp, not directly comparable to wall-clock elapsed time, so this is a
// separate field specifically for "how long ago, in real time, did a real frame last arrive"
// (see Capture_IsRealVideoRecentlyActive below).
static u64 g_videoRawLastArrivalTick = 0;

// Tap timestamps share this monotonic microsecond clock. Never apply this to GRC timestamps.

bool CaptureVideoViewerPresent(void)
{
    if (Viewers_VideoActive())
        return true;
    mutexLock(&g_videoRawMutex);
    bool present = g_vLastConsumerTick != 0 && armTicksToNs(svcGetSystemTick() - g_vLastConsumerTick) < 2000000000ULL;
    mutexUnlock(&g_videoRawMutex);
    return present;
}

static bool g_videoDeliveryDirect;
bool CaptureVideoUsesDirectDelivery(void)
{
    return Viewers_VideoActive();
}
bool CaptureVideoWaitDelivery(void)
{
    if (!g_videoDeliveryDirect)
        return true;
    const uint64_t begin = armTicksToNs(svcGetSystemTick());
    const bool ok = Viewers_VideoWaitDelivery();
    UdpmVideo_NoteDeliveryWait((armTicksToNs(svcGetSystemTick()) - begin) / 1000);
    return ok;
}

uint32_t CaptureVideoBudgetBps(void)
{
    return Viewers_VideoBudgetBps();
}

bool CaptureVideoSubmit(const void *data, size_t size, uint64_t timestamp_us)
{
    if (!data || size <= 4 || size > VbufSz)
        return false;
    g_videoDeliveryDirect = Viewers_VideoActive();
    if (g_videoDeliveryDirect)
    {
        mutexLock(&g_videoRawMutex);
        g_videoRawLastArrivalTick = svcGetSystemTick();
        mutexUnlock(&g_videoRawMutex);
        return Viewers_VideoSubmit(data, (u32)size, timestamp_us);
    }
    const bool idr = CaptureVideoHasIdr(data, size);
    mutexLock(&g_videoRawMutex);
    if (!g_vLastConsumerTick || armTicksToNs(svcGetSystemTick() - g_vLastConsumerTick) >= 2000000000ULL)
    {
        mutexUnlock(&g_videoRawMutex);
        return false;
    }
    if (g_vDropUntilIdr && !idr)
    {
        mutexUnlock(&g_videoRawMutex);
        return true; // consumed, withheld until a decoder-safe boundary
    }
    if (idr)
        g_vDropUntilIdr = false;
    if (!VringPush(data, (u32)size, timestamp_us))
    {
        VringClear();
        g_vDropUntilIdr = !idr;
        if (idr)
            VringPush(data, (u32)size, timestamp_us);
        NxrpVideo_RequestKeyframe();
    }
    g_videoRawLastArrivalTick = svcGetSystemTick();
    condvarWakeAll(&g_videoRawCondVar);
    mutexUnlock(&g_videoRawMutex);
    return true;
}

bool CaptureVideoCanProduce(void)
{
    mutexLock(&g_videoRawMutex);
    const bool empty = g_vCount == 0;
    mutexUnlock(&g_videoRawMutex);
    // Never nest capture and viewer locks: the hub transfers between them.
    return empty && Viewers_VideoCanProduce();
}

bool Capture_IsRealVideoRecentlyActive(u64 windowNs)
{
    mutexLock(&g_videoRawMutex);
    bool active =
        g_videoRawLastArrivalTick != 0 && armTicksToNs(svcGetSystemTick() - g_videoRawLastArrivalTick) < windowNs;
    mutexUnlock(&g_videoRawMutex);
    return active;
}

bool Capture_IsViPatchBusy(void)
{
    return NxrpGateBusy();
}

#define AUDIO_RING_SLOTS 16 // comfortably more than MaxABatching+1 worth of producer/consumer slack

static Mutex g_audioRingMutex;
static u8 g_audioRingBuf[AUDIO_RING_SLOTS][AbufSz];
static u32 g_audioRingSize[AUDIO_RING_SLOTS];
static u64 g_audioRingWriteSeq = 0;
static u64 g_audioRingReadSeq = 0;
static u64 g_audioLastArrivalTimestamp = 0;

// read from the capture hub independently of a possibly-blocked GRC call.
static u8 alignas(0x1000) AudioPumpStackArea[0x2000 + LOGGING_STACK_BOOST];
static Thread AudioPumpThread;

static void AudioPumpThreadMain(void *arg)
{
    // Static storage keeps audio packets off the worker stack
    // (8KB) free of anything but small local variables and call frames.
    static u8 tempBuf[AbufSz];

    while (true)
    {
        u32 dataSize = 0;
        u64 timestamp = 0;
        Result res = grcdServiceTransfer(&grcdAudio, GrcStream_Audio, tempBuf, AbufSz, NULL, &dataSize, &timestamp);

        if (res == GRCD_NOT_INITIALIZED && EnsureGrcdInit(&res))
        {

            continue;
        }

        if (R_FAILED(res) || dataSize == 0)
        {
            svcSleepThread(2000000ULL); // inactive/error source must not spin on IPC
            continue;
        }
        if (dataSize > sizeof(tempBuf))
        {
            svcSleepThread(2000000ULL);
            continue;
        }

        mutexLock(&g_audioRingMutex);
        u64 slot = g_audioRingWriteSeq % AUDIO_RING_SLOTS;
        memcpy(g_audioRingBuf[slot], tempBuf, dataSize);
        g_audioRingSize[slot] = dataSize;
        g_audioRingWriteSeq++;
        g_audioLastArrivalTimestamp = timestamp;
        // If the consumer fell behind by a full ring, catch it up to the oldest data we
        // still have rather than let it read a slot that's about to be overwritten again.
        if (g_audioRingWriteSeq - g_audioRingReadSeq > AUDIO_RING_SLOTS)
            g_audioRingReadSeq = g_audioRingWriteSeq - AUDIO_RING_SLOTS;
        mutexUnlock(&g_audioRingMutex);
    }
}

void CaptureAudioConnected()
{
    APkt.Header.Magic = STREAM_PACKET_HEADER;
    APkt.Header.MetaData = PacketMeta_Type_Audio | PacketMeta_Content_Data;
    APkt.Header.ReplaySlot = 0xFF;

    // Start listening from whatever the pump produces next, don't replay its backlog
    mutexLock(&g_audioRingMutex);
    g_audioRingReadSeq = g_audioRingWriteSeq;
    mutexUnlock(&g_audioRingMutex);
}

// No caps:sc-style unrestricted equivalent exists for audio (see the capture.h note
// above - deferred for later), so while the audio pump has nothing fresh this just sends
// silence.

// non-blocking variant for sessions that multiplex audio onto the video connection
// (PunchProbe.c). Returns true and fills APkt only when a full batch (AudioBatching+1 chunks) is
// waiting; never blocks and never fabricates silence - when nothing is playing (or no game is
// running so the pump is parked in grc:d) the client simply underruns into silence by itself.
bool CaptureTryReadAudio()
{
    mutexLock(&g_audioRingMutex);

    int wanted = AudioBatching + 1;
    u64 available = g_audioRingWriteSeq - g_audioRingReadSeq;
    if (available < (u64)wanted)
    {
        mutexUnlock(&g_audioRingMutex);
        return false;
    }

    APkt.Header.DataSize = 0;
    for (int i = 0; i < wanted; i++)
    {
        u64 slot = (g_audioRingReadSeq + i) % AUDIO_RING_SLOTS;
        memcpy(APkt.Data + APkt.Header.DataSize, g_audioRingBuf[slot], g_audioRingSize[slot]);
        APkt.Header.DataSize += g_audioRingSize[slot];
    }
    g_audioRingReadSeq += wanted;
    APkt.Header.Timestamp = g_audioLastArrivalTimestamp;

    mutexUnlock(&g_audioRingMutex);
    return true;
}

//  (multi-viewer): each unbridged viewer keeps its own read cursor into the shared audio ring and its own
// batch size, so any number of viewers can read the same PCM without disturbing each other or the legacy single
// reader above. Non-blocking. The ring slots are overwritten after AUDIO_RING_SLOTS writes, so a cursor that lags that
// far is moved up to the oldest data still present.
u64 CaptureAudioCursorNow(void)
{
    mutexLock(&g_audioRingMutex);
    u64 now = g_audioRingWriteSeq;
    mutexUnlock(&g_audioRingMutex);
    return now;
}

bool CaptureTryReadAudioFor(u64 *cursor, int batching, AudioPacket *out)
{
    if (batching < 0)
        batching = 0;
    if (batching > MaxABatching)
        batching = MaxABatching;
    mutexLock(&g_audioRingMutex);

    if (g_audioRingWriteSeq - *cursor > AUDIO_RING_SLOTS)
        *cursor = g_audioRingWriteSeq - AUDIO_RING_SLOTS;

    const int wanted = batching + 1;
    if (g_audioRingWriteSeq - *cursor < (u64)wanted)
    {
        mutexUnlock(&g_audioRingMutex);
        return false;
    }

    out->Header.Magic = STREAM_PACKET_HEADER;
    out->Header.MetaData = PacketMeta_Type_Audio | PacketMeta_Content_Data;
    out->Header.ReplaySlot = 0xFF;
    out->Header.DataSize = 0;
    for (int i = 0; i < wanted; i++)
    {
        u64 slot = (*cursor + i) % AUDIO_RING_SLOTS;
        memcpy(out->Data + out->Header.DataSize, g_audioRingBuf[slot], g_audioRingSize[slot]);
        out->Header.DataSize += g_audioRingSize[slot];
    }
    *cursor += wanted;
    out->Header.Timestamp = g_audioLastArrivalTimestamp;

    mutexUnlock(&g_audioRingMutex);
    return true;
}

void CaptureVideoConnected()
{
    VPkt.Header.Magic = STREAM_PACKET_HEADER;
    VPkt.Header.MetaData = PacketMeta_Type_Video;
    VPkt.Header.ReplaySlot = 0xFF;
    g_waitingForResumeIdr = true;

    // Start listening from whatever the pump produces next, don't replay its backlog
    mutexLock(&g_videoRawMutex);
    VringClear(); // nothing queued from before this connection
    g_vDropUntilIdr = false;
    g_vLastConsumerTick = svcGetSystemTick();
    mutexUnlock(&g_videoRawMutex);

    // Fresh connection - give real video the full generous window before assuming
    // unfocused, rather than picking up mid-fallback at the fast 30fps re-check rate.
    g_wasScreenshotLastFrame = false;
    NxrpVideo_RequestKeyframe();
}

// Read the next encoded frame from the bounded capture FIFO.
bool CaptureReadVideo()
{
    mutexLock(&g_videoRawMutex);

    // Only worth waiting for a real frame the first time we fall back (detecting "just
    // went unfocused"). Once already in steady-state screenshot fallback, waiting here
    // again each cycle was pure dead time: real video never arrives while unfocused, so
    // this always burned the full timeout for nothing - measured at ~33ms wasted per
    // frame on top of capsscCaptureJpegScreenShot's own ~47ms, roughly halving the
    // achievable rate. capsscCaptureJpegScreenShot's own cost already paces this loop
    // plenty; if real video resumes, the next call picks it up immediately regardless.
    g_vLastConsumerTick = svcGetSystemTick();
    if (g_vCount == 0 && !g_wasScreenshotLastFrame)
        condvarWaitTimeout(&g_videoRawCondVar, &g_videoRawMutex, REAL_VIDEO_INITIAL_WAIT_NS);

    if (g_vCount == 0)
    {
        mutexUnlock(&g_videoRawMutex);
        g_wasScreenshotLastFrame = true;
        return false;
    }

    // Oldest queued frame first (the FIFO keeps every frame in order; see the comment at the
    // top of the ring). Only one video-reading client thread exists at a time.
    u32 dataSize = 0;
    u64 timestamp = 0;
    VringPop(VPkt.Data, &dataSize, &timestamp);

    mutexUnlock(&g_videoRawMutex);

    g_wasScreenshotLastFrame = false;

    const bool idr = CaptureVideoHasIdr(VPkt.Data, dataSize);
    if (g_waitingForResumeIdr && !idr)
        return false;
    if (idr)
        g_waitingForResumeIdr = false;
    VPkt.Header.DataSize = dataSize;
    VPkt.Header.Timestamp = timestamp;
    VPkt.Header.ReplaySlot = 0xFF;
    VPkt.Header.MetaData = PacketMeta_Type_Video | PacketMeta_Content_Data | PacketMeta_Content_MultiNal;
    return true;

}

// Configurable options
int CaptureSetAudioBatching(int batch)
{
    if (batch < 0)
        batch = 0;

    if (batch > MaxABatching)
        batch = MaxABatching;

    AudioBatching = batch;
    return AudioBatching;
}

Result CaptureInitialize()
{
    capturing = true;
    R_RET_ON_FAIL(grcdServiceOpen(&grcdVideo));
    R_RET_ON_FAIL(grcdServiceOpen(&grcdAudio));

    // Keep audio capture open for the lifetime of the host.
    LaunchThread(&AudioPumpThread, AudioPumpThreadMain, NULL, AudioPumpStackArea, sizeof(AudioPumpStackArea), 0x2C);

    return 0;
}

