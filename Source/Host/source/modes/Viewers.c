#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "Viewers.h"
#include "OpusAudio.h"
#include "pcm_accum.h"
#include "UdpmVideo.h"
#include "modes.h"
#include "ClientInput.h"
#include "proto.h"
#include "../core.h"
#include "../nxrp_video.h"
#include "../net/sockets.h"
#include "../net/delivery_control.h"

// Each viewer owns a queue fed directly by the video producer.

// Place 0 (the first viewer, the one that is there in every ordinary session) keeps a ring as large as the old
// single-viewer path had (768 KiB: a long cellular stall must not cost more resyncs than before); the extra places get
// 384 KiB each.
#define VIEWER_RING0_SIZE 0xC0000u
#define VIEWER_RINGN_SIZE 0x60000u
#define VIEWER_SEND_MAX (VbufSz + sizeof(PacketHeader)) // a complete capture AU plus its wire header
// Opus encodes on this stack, which needs far more room than sending does.
#define VIEWER_STACK_SIZE (0x16000 + LOGGING_STACK_BOOST)
#define VIEWER_STALE_NS (20ULL * 1000 * 1000 * 1000)

// ---- variable-size record FIFO (same algorithm as capture.c's g_vring, per viewer) ---------------------
typedef struct
{
    u32 size; // ring capacity in bytes
    u32 head, tail, used, count;
} VQ;

typedef struct
{
    const void *data;
    u32 size;
} Part;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
static void VqCopyIn(VQ *q, u8 *ring, const void *src, u32 n)
{
    const u8 *s = (const u8 *)src;
    const u32 t = q->tail % q->size;
    u32 first = q->size - t;
    if (first > n)
        first = n;
    memcpy(ring + t, s, first);
    if (n > first)
        memcpy(ring, s + first, n - first);
    q->tail = (q->tail + n) % q->size;
}

static void VqCopyOut(VQ *q, const u8 *ring, void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    const u32 h = q->head % q->size;
    u32 first = q->size - h;
    if (first > n)
        first = n;
    memcpy(d, ring + h, first);
    if (n > first)
        memcpy(d + first, ring, n - first);
    q->head = (q->head + n) % q->size;
}

static bool VqPush(VQ *q, u8 *ring, const Part *parts, int np, u32 wireLen)
{
    const u32 need = 4 + wireLen;
    if (need > q->size || q->size - q->used < need)
        return false;
    VqCopyIn(q, ring, &wireLen, 4);
    for (int i = 0; i < np; i++)
        VqCopyIn(q, ring, parts[i].data, parts[i].size);
    q->used += need;
    q->count++;
    return true;
}

static bool VqPop(VQ *q, const u8 *ring, u8 *dst, u32 *wireLen)
{
    if (q->count == 0)
        return false;
    u32 len = 0;
    VqCopyOut(q, ring, &len, 4);
    VqCopyOut(q, ring, dst, len);
    q->used -= 4 + len;
    q->count--;
    *wireLen = len;
    return true;
}

static void VqClear(VQ *q)
{
    q->head = q->tail = q->used = q->count = 0;
}

// Drops everything queued before the newest queued IDR (a decodable run: the IDR and the P-frames after it).

#pragma GCC diagnostic pop

// ---- viewers --------------------------------------------------------------------------------------------
struct Viewer
{
    int slot;
    ViewerKind kind;
    bool reserved;      // guarded by g_gate
    volatile bool kick; // a USB host took the console: end this session (set by Viewers_KickAll, cleared on reserve)
    bool punchPhase;    // guarded by g_gate: a punch attempt that is registering / waiting for its connector
    bool streaming;     // guarded by lock
    int socket;

    Thread thread;
    bool threadStarted;

    Mutex lock; // guards the queue and streaming state
    CondVar cv;
    VQ q;
    bool needIdr, dropUntilIdr;

    bool wantAudio;
    int batching;
    u64 audioCursor;
    OpusAudio *opus; // set while this viewer takes Opus audio
    PcmAccum pcm;

    u8 inBuf[NQ_SIZE];
    bool deliveryFeedback;
    bool udpVideo; // guarded by lock;  feedback is consumed but not applied on UDP
    NqControl deliveryControl;
    u32 inHave;
    u64 lastKeys;

    // one queued AU plus the immutable packet currently being sent.
    // Ticket counters are protected by lock; input worker lifetime by atomics.
    u64 queuedTicket, sendingTicket, completedTicket, completedNs;
    atomic_bool inputRunning, inputAlive;
    Thread inputThread;
    bool inputStarted;
};

#include "../nxrp_host_budget.h"
static Viewer g_viewers[VIEWER_MAX];

u32 Viewers_VideoBudgetBps(void)
{
    u32 viewers = 0, audio = 0;
    for (int i = 0; i < VIEWER_MAX; ++i)
    {
        Viewer *v = &g_viewers[i];
        mutexLock(&v->lock);
        if (v->streaming)
        {
            ++viewers;
            if (v->wantAudio)
                ++audio;
        }
        mutexUnlock(&v->lock);
    }
    const u32 ceiling = NxrpHostVideoBudgetBps(viewers, audio);
    u32 target = ceiling;
    for (int i = 0; i < VIEWER_MAX; ++i)
    {
        Viewer *v = &g_viewers[i];
        mutexLock(&v->lock);
        if (v->streaming && (v->udpVideo || v->deliveryControl.target_bps))
        {
            const u32 limit = NqClamp(v->udpVideo ? UdpmVideo_TargetBps() : v->deliveryControl.target_bps, ceiling);
            if (limit < target)
                target = limit;
        }
        mutexUnlock(&v->lock);
    }
    return target;
}

static Mutex g_deliveryBudgetLock;
static NqBucket g_deliveryBucket;
static u32 g_deliveryWaiting;
static int g_deliveryLastSlot = -1;
static u32 DeliveryPayloadRate(void)
{
    const u32 video = Viewers_VideoBudgetBps();
    u32 viewers = 0, audio = 0;
    for (int i = 0; i < VIEWER_MAX; ++i)
    {
        Viewer *v = &g_viewers[i];
        mutexLock(&v->lock);
        if (v->streaming)
        {
            ++viewers;
            if (v->wantAudio)
                ++audio;
        }
        mutexUnlock(&v->lock);
    }
    const u64 rate = (u64)video * viewers + (u64)audio * 1536000u;
    return rate > 9000000u ? 9000000u : (u32)rate;
}
static u32 DeliveryReserve(void *context, u32 wanted)
{
    Viewer *v = context;
    const u32 rate = DeliveryPayloadRate();
    if (!rate)
        return 0;
    const u64 now = armTicksToNs(svcGetSystemTick()) / 1000;
    mutexLock(&g_deliveryBudgetLock);
    NqBucketUpdate(&g_deliveryBucket, now, rate);
    g_deliveryWaiting |= 1u << v->slot;
    int selected = -1;
    for (int n = 1; n <= VIEWER_MAX; ++n)
    {
        const int slot = (g_deliveryLastSlot + n) % VIEWER_MAX;
        if (g_deliveryWaiting & (1u << slot))
        {
            selected = slot;
            break;
        }
    }
    const u32 grant = selected == v->slot ? NqBucketGrant(&g_deliveryBucket, wanted) : 0;
    if (grant)
    {
        g_deliveryWaiting &= ~(1u << v->slot);
        g_deliveryLastSlot = v->slot;
    }
    mutexUnlock(&g_deliveryBudgetLock);
    return grant;
}
static void DeliveryRefund(void *context, u32 unused, u32 accepted)
{
    (void)context;
    mutexLock(&g_deliveryBudgetLock);
    NqBucketRefund(&g_deliveryBucket, unused, accepted);
    mutexUnlock(&g_deliveryBudgetLock);
}
void Viewers_EnableFeedback(Viewer *v, bool enabled, bool udp)
{
    mutexLock(&v->lock);
    v->deliveryFeedback = enabled;
    v->udpVideo = udp;
    NqReset(&v->deliveryControl);
    mutexUnlock(&v->lock);
}
static u8 g_ring0[VIEWER_RING0_SIZE];
static u8 g_ringN[VIEWER_MAX - 1][VIEWER_RINGN_SIZE];
static u8 *RingOf(int i)
{
    return i == 0 ? g_ring0 : g_ringN[i - 1];
}
static u32 RingSizeOf(int i)
{
    return i == 0 ? VIEWER_RING0_SIZE : VIEWER_RINGN_SIZE;
}
static u8 g_send[VIEWER_MAX][VIEWER_SEND_MAX];
static AudioPacket g_audio[VIEWER_MAX];
static u8 alignas(0x1000) g_stacks[VIEWER_MAX][VIEWER_STACK_SIZE];
static u8 alignas(0x1000) g_inputStacks[VIEWER_MAX][0x6000 + LOGGING_STACK_BOOST];

static Mutex g_gate;
static bool g_usbActive;
static const int g_max = VIEWER_MAX;
static bool g_inited;

static atomic_int g_streamingCount;
static volatile u64 g_lastFrameTick;
static u64 g_videoTicket, g_videoWaitTicket;
static u32 g_videoWaitMask;
static u64 g_videoResyncNs;

static int ViewerIndex(const Viewer *v)
{
    return (int)(v - g_viewers);
}

int Viewers_Slot(const Viewer *v)
{
    return v->slot;
}
Thread *Viewers_Thread(Viewer *v)
{
    return &v->thread;
}
void *Viewers_StackBase(Viewer *v)
{
    return g_stacks[ViewerIndex(v)];
}
u32 Viewers_StackSize(void)
{
    return VIEWER_STACK_SIZE;
}
bool Viewers_ThreadStarted(const Viewer *v)
{
    return v->threadStarted;
}
void Viewers_MarkThreadStarted(Viewer *v)
{
    v->threadStarted = true;
}
int Viewers_Max(void)
{
    return g_max;
}

// pace raw capture to the fastest active viewer. A frame already popped
// for TCP may still be sending; allow one successor, but do not encode a backlog
// that would then need repeated IDRs. Slower secondary viewers retain their
// independent capacity/IDR recovery. No viewer here means the separate raw TCP
// consumer owns pacing through the capture ring instead.
bool Viewers_VideoCanProduce(void)
{
    bool active = false;
    for (int i = 0; i < VIEWER_MAX; ++i)
    {
        Viewer *v = &g_viewers[i];
        mutexLock(&v->lock);
        const bool streaming = v->streaming;
        const bool ready = streaming && v->q.count == 0;
        mutexUnlock(&v->lock);
        if (ready)
            return true;
        active |= streaming;
    }
    return !active;
}

bool Viewers_VideoActive(void)
{
    return atomic_load(&g_streamingCount) != 0;
}

// Direct network . Existing wire header and in-band SPS/PPS are retained.
// There is no capture FIFO or hub hop for this route. Each viewer owns its copy;
// a slow secondary viewer cannot retain an encoder staging buffer indefinitely.
bool Viewers_VideoSubmit(const void *data, u32 size, u64 timestamp)
{
    const u64 ticket = ++g_videoTicket;
    u32 mask = 0;
    bool resync = false;
    const bool idr = CaptureVideoHasIdr(data, size);
    PacketHeader header = {.Magic = STREAM_PACKET_HEADER,
                           .DataSize = size,
                           .Timestamp = timestamp,
                           .MetaData = PacketMeta_Type_Video | PacketMeta_Content_Data | PacketMeta_Content_MultiNal,
                           .ReplaySlot = 0xFF};
    const Part parts[] = {{&header, sizeof(header)}, {data, size}};
    for (int i = 0; i < VIEWER_MAX; ++i)
    {
        Viewer *v = &g_viewers[i];
        mutexLock(&v->lock);
        if (!v->streaming)
        {
            mutexUnlock(&v->lock);
            continue;
        }
        if (v->q.count != 0)
        {
            // The primary is completion-paced. Only a lagging secondary should
            // reach this bound: never skip a reference then send a dependent P.
            VqClear(&v->q);
            v->queuedTicket = 0;
            v->dropUntilIdr = true;
            resync = true;
        }
        if ((v->needIdr || v->dropUntilIdr) && !idr)
        {
            mutexUnlock(&v->lock);
            continue;
        }
        if (VqPush(&v->q, RingOf(i), parts, 2, sizeof(header) + size))
        {
            v->queuedTicket = ticket;
            v->needIdr = v->dropUntilIdr = false;
            mask |= 1u << i;
            condvarWakeOne(&v->cv);
        }
        else
        {
            v->dropUntilIdr = true;
            resync = true;
        }
        mutexUnlock(&v->lock);
    }
    g_videoWaitTicket = ticket;
    g_videoWaitMask = mask;
    g_lastFrameTick = svcGetSystemTick();
    // Bound exceptional secondary resyncs; never reproduce the age-reset storm.
    const u64 now = armTicksToNs(svcGetSystemTick());
    if (resync && (!g_videoResyncNs || now - g_videoResyncNs >= 1000000000ULL))
    {
        g_videoResyncNs = now;
        NxrpVideo_RequestKeyframe();
    }
    return Viewers_VideoActive();
}

bool Viewers_VideoWaitDelivery(void)
{
    // One outstanding ticket, as upstream's one outstanding payload. Allow the
    // next raw capture/VIC/encode to overlap this send; don't wait before capture.
    for (;;)
    {
        bool pending = false;
        int winner = -1;
        u64 first = UINT64_MAX;
        for (int i = 0; i < VIEWER_MAX; ++i)
        {
            if (!(g_videoWaitMask & (1u << i)))
                continue;
            Viewer *v = &g_viewers[i];
            mutexLock(&v->lock);
            if (v->streaming && !v->kick)
            {
                if (v->completedTicket >= g_videoWaitTicket && v->completedNs < first)
                {
                    winner = i;
                    first = v->completedNs;
                }
                pending |= v->queuedTicket == g_videoWaitTicket || v->sendingTicket == g_videoWaitTicket;
            }
            mutexUnlock(&v->lock);
        }
        if (winner >= 0)
        {
            return true;
        }
        if (!pending)
            return Viewers_VideoActive(); // cohort left; next AU starts/rejoins on IDR
        if (!Viewers_VideoActive())
            return false;
        svcSleepThread(1000000ULL);
    }
}

// ---- offering frames -----------------------------------------------------------------------------------

bool Viewers_StreamStale(void)
{
    if (atomic_load(&g_streamingCount) == 0)
        return false;
    return armTicksToNs(svcGetSystemTick() - g_lastFrameTick) > VIEWER_STALE_NS;
}

// ---- input -----------------------------------------------------------------------------------------------
// touch input, tunneled over the SAME punched socket the video already flows over (client -> console
// direction). Two packet kinds (touch 5 bytes, controller 17 bytes), told apart by the first byte; HOME is the 5-byte
// system packet. Non-blocking: costs at most SocketRecv's own EAGAIN sleep when nothing is pending. Accumulates partial
// packets across calls since TCP may deliver them split up.
// drains EVERYTHING that has arrived, not one packet per call (a client sends a controller packet on every
// stick/button change, up to ~60/s; handling one per pass built a backlog and "stuck buttons"). Button changes are
// applied immediately and in order; consecutive stick-only changes are coalesced into the newest one. : state
// is per viewer and the controller packets drive this viewer's own virtual pad (its slot).
static bool PollInput(Viewer *v)
{
    u8 pendingCtrl[CTRL_PACKET_SIZE] = {0};
    bool havePending = false;
    bool alive = true;

    for (int budget = 0; budget < 256; budget++)
    {
        u32 want = v->inHave ? NqInputSize(v->inBuf[0], v->deliveryFeedback) : TOUCH_PACKET_SIZE;
        if (!want)
            return false;
        s32 r = SocketTryRecv(v->socket, v->inBuf + v->inHave, want - v->inHave);
        if (r < 0)
        {
            v->inHave = 0;
            alive = false;
            break;
        }
        if (r == 0)
            break; // nothing more waiting

        v->inHave += (u32)r;
        want = NqInputSize(v->inBuf[0], v->deliveryFeedback);
        if (!want)
            return false;
        if (v->inBuf[0] == NQ_TYPE && v->inHave >= 4 && (v->inBuf[1] != 1 || v->inBuf[2] != NQ_SIZE || v->inBuf[3]))
            return false;
        if (v->inHave < want)
            continue;
        if (v->inBuf[0] == NQ_TYPE)
        {
            NqReport report;
            if (!NqDecode(v->inBuf, v->inHave, &report))
                return false;
            // Compute the shared ceiling without recursively taking this viewer lock.
            u32 viewers = 0, audio = 0;
            for (int i = 0; i < VIEWER_MAX; ++i)
            {
                Viewer *other = &g_viewers[i];
                mutexLock(&other->lock);
                if (other->streaming)
                {
                    ++viewers;
                    if (other->wantAudio)
                        ++audio;
                }
                mutexUnlock(&other->lock);
            }
            const u32 ceiling = NxrpHostVideoBudgetBps(viewers, audio);
            const u64 now_us = armTicksToNs(svcGetSystemTick()) / 1000;
            mutexLock(&v->lock);
            const bool accepted = !v->udpVideo && NqApply(&v->deliveryControl, &report, now_us, ceiling);
            mutexUnlock(&v->lock);
            if (!accepted)
            {
                v->inHave = 0;
                continue;
            } // stale/replayed report is not a media failure
            v->inHave = 0;
            continue;
        }

        if (v->inBuf[0] == CTRL_PACKET_TYPE)
        {
            u64 keys;
            memcpy(&keys, v->inBuf + 1, 8);
            keys &= 0xFFFFull;
            if (keys != v->lastKeys)
            {
                // a button changed: apply this (and only this) state now, keeping order
                ClientInputApplySlot(v->slot, v->inBuf, CTRL_PACKET_SIZE);
                v->lastKeys = keys;
                havePending = false;
            }
            else
            {
                memcpy(pendingCtrl, v->inBuf, CTRL_PACKET_SIZE); // stick-only: keep just the newest
                havePending = true;
            }
        }
        else
            ClientInputApplySlot(v->slot, v->inBuf, TOUCH_PACKET_SIZE);
        v->inHave = 0;
    }

    if (havePending)
        ClientInputApplySlot(v->slot, pendingCtrl, CTRL_PACKET_SIZE);
    return alive;
}

bool Viewers_ServiceInput(Viewer *v)
{
    if (v->kick || !PollInput(v))
        return false;
    ClientInputService();
    return true;
}

// Called from inside the socket send helper when a send is backed up: services the owning viewer's input so neither stalls for
// the duration of a multi-second frame, and reports peer closure. Any other socket is not a viewer's: nothing to do.
static bool ViewerSendHook(int socket)
{
    for (int i = 0; i < VIEWER_MAX; i++)
    {
        Viewer *v = &g_viewers[i];
        if (v->streaming && v->socket == socket)
            return Viewers_ServiceInput(v);
    }
    return true;
}

static void NxrpInputThread(void *arg)
{
    Viewer *v = arg;
    while (atomic_load(&v->inputRunning))
    {
        if (!Viewers_ServiceInput(v))
        {
            atomic_store(&v->inputAlive, false);
            break;
        }
        svcSleepThread(2000000ULL);
    }
}

bool Viewers_VideoStartInput(Viewer *v)
{
    atomic_store(&v->inputAlive, true);
    atomic_store(&v->inputRunning, true);
    const Result rc = threadCreate(&v->inputThread, NxrpInputThread, v, g_inputStacks[ViewerIndex(v)],
                                   sizeof(g_inputStacks[0]), 0x2C, 3);
    if (R_FAILED(rc))
    {
        atomic_store(&v->inputRunning, false);
        return false;
    }
    const Result start = threadStart(&v->inputThread);
    if (R_FAILED(start))
    {
        threadClose(&v->inputThread);
        atomic_store(&v->inputRunning, false);
        return false;
    }
    v->inputStarted = true;
    return true;
}

static bool NxrpSendAlive(void *arg)
{
    Viewer *v = arg;
    return !v->kick && atomic_load(&v->inputAlive);
}

bool Viewers_VideoInputAlive(Viewer *v)
{
    return NxrpSendAlive(v);
}

bool Viewers_VideoSend(Viewer *v, const void *wire, u32 size, bool video)
{
    const u64 start = armTicksToNs(svcGetSystemTick());
    const SocketMediaBudget budget = {DeliveryReserve, DeliveryRefund};
    u64 pacingNs = 0;
    const bool ok = SocketSendMediaPaced(v->socket, wire, size, NxrpSendAlive, v, &budget, &pacingNs);
    mutexLock(&g_deliveryBudgetLock);
    g_deliveryWaiting &= ~(1u << v->slot);
    mutexUnlock(&g_deliveryBudgetLock);
    const u64 end = armTicksToNs(svcGetSystemTick());
    // Real transport work only: deliberate pacing must not trigger congestion.
    const u64 elapsed = end - start;
    const u64 transport_us = (elapsed > pacingNs ? elapsed - pacingNs : 0) / 1000;
    u32 viewers = 0, audio = 0;
    for (int i = 0; i < VIEWER_MAX; ++i)
    {
        Viewer *other = &g_viewers[i];
        mutexLock(&other->lock);
        if (other->streaming)
        {
            ++viewers;
            if (other->wantAudio)
                ++audio;
        }
        mutexUnlock(&other->lock);
    }
    const u32 ceiling = NxrpHostVideoBudgetBps(viewers, audio);
    mutexLock(&v->lock);
    if (!v->udpVideo)
        NqLocal(&v->deliveryControl, end / 1000, transport_us > UINT32_MAX ? UINT32_MAX : (u32)transport_us, ceiling);
    if (video)
    {
        if (ok)
        {
            v->completedTicket = v->sendingTicket;
            v->completedNs = end;
        }
        v->sendingTicket = 0;
    }
    mutexUnlock(&v->lock);
    return ok;
}

bool Viewers_VideoSendUdp(Viewer *v, const void *wire, u32 size)
{
    u64 pacingNs = 0;
    // UDP media bypasses the shared TCP token ledger; it is paced by the UDPM sender itself.
    const bool ok = UdpmVideo_Send((const u8 *)wire, size, &pacingNs);
    const u64 end = armTicksToNs(svcGetSystemTick());
    mutexLock(&v->lock);
    if (ok)
    {
        v->completedTicket = v->sendingTicket;
        v->completedNs = end;
    }
    v->sendingTicket = 0;
    mutexUnlock(&v->lock);
    return ok;
}

// ---- places ------------------------------------------------------------------------------------------------
void Viewers_Init(void)
{
    if (g_inited)
        return;
    g_inited = true;
    NqBucketInit(&g_deliveryBucket, armTicksToNs(svcGetSystemTick()) / 1000, 9000000u);
    g_deliveryWaiting = 0;
    g_deliveryLastSlot = -1;
    for (int i = 0; i < VIEWER_MAX; i++)
    {
        g_viewers[i].slot = i;
        g_viewers[i].socket = SOCKET_INVALID;
        g_viewers[i].q.size = RingSizeOf(i);
    }
    SocketSendReadableHook = ViewerSendHook;

}

bool Viewers_UsbTryAcquire(void)
{
    Viewers_Init();
    mutexLock(&g_gate);
    bool ok = !g_usbActive;
    for (int i = 0; i < VIEWER_MAX && ok; i++)
        if (g_viewers[i].reserved)
            ok = false;
    if (ok)
        g_usbActive = true;
    mutexUnlock(&g_gate);
    return ok;
}

void Viewers_UsbRelease(void)
{
    mutexLock(&g_gate);
    g_usbActive = false;
    mutexUnlock(&g_gate);
}

void Viewers_KickAll(void)
{
    mutexLock(&g_gate);
    for (int i = 0; i < VIEWER_MAX; i++)
        if (g_viewers[i].reserved)
            g_viewers[i].kick = true;
    mutexUnlock(&g_gate);
}

Viewer *Viewers_Reserve(ViewerKind kind)
{
    Viewers_Init();
    Viewer *found = NULL;
    mutexLock(&g_gate);
    int used = 0;
    bool punching = false;
    for (int i = 0; i < VIEWER_MAX; i++)
    {
        if (g_viewers[i].reserved)
            used++;
        if (g_viewers[i].punchPhase)
            punching = true;
    }
    if (!g_usbActive && used < g_max && !(kind == ViewerKind_Punch && punching))
    {
        for (int i = 0; i < VIEWER_MAX; i++)
        {
            if (!g_viewers[i].reserved)
            {
                found = &g_viewers[i];
                found->reserved = true;
                found->kick = false;
                found->kind = kind;
                found->punchPhase = (kind == ViewerKind_Punch);
                found->streaming = false;
                break;
            }
        }
    }
    mutexUnlock(&g_gate);
    return found;
}

void Viewers_PunchPhaseDone(Viewer *v)
{
    mutexLock(&g_gate);
    v->punchPhase = false;
    mutexUnlock(&g_gate);
}

bool Viewers_PunchWaiting(void)
{
    bool waiting = false;
    mutexLock(&g_gate);
    for (int i = 0; i < VIEWER_MAX; i++)
        if (g_viewers[i].punchPhase)
            waiting = true;
    mutexUnlock(&g_gate);
    return waiting;
}

int Viewers_Count(void)
{
    int used = 0;
    mutexLock(&g_gate);
    for (int i = 0; i < VIEWER_MAX; i++)
        if (g_viewers[i].reserved)
            used++;
    mutexUnlock(&g_gate);
    return used;
}

void Viewers_Attach(Viewer *v, int socket, bool wantAudio, int audioBatching, bool opusAudio)
{
    const int index = ViewerIndex(v);
    // The hub applies SPS/PPS injection per viewer and always sends the data itself (never a replay marker), so neither
    // global option may fire inside CaptureReadVideo.

    v->socket = socket;
    v->wantAudio = wantAudio;
    v->batching = audioBatching;
    v->audioCursor = CaptureAudioCursorNow();
    OpusAudio_Destroy(v->opus);
    v->opus = NULL;
    PcmAccumReset(&v->pcm);
    if (wantAudio && opusAudio)
    {
        v->opus = OpusAudio_Create();
    }
    v->inHave = 0;
    v->deliveryFeedback = false;
    v->udpVideo = false;
    NqReset(&v->deliveryControl);
    v->lastKeys = 0;
    memset(&g_audio[index], 0, sizeof(g_audio[index]));

    mutexLock(&v->lock);
    VqClear(&v->q);
    v->needIdr = true;
    v->dropUntilIdr = false;
    v->queuedTicket = v->sendingTicket = v->completedTicket = v->completedNs = 0;
    v->streaming = true;
    mutexUnlock(&v->lock);

    atomic_fetch_add(&g_streamingCount, 1);
    {
        g_lastFrameTick = svcGetSystemTick();
        NxrpVideo_RequestKeyframe();
    }
}

void Viewers_Detach(Viewer *v)
{
    // Stop/join the sole reader before the socket or controller slot is released.
    if (v->inputStarted)
    {
        atomic_store(&v->inputRunning, false);
        JoinThread(&v->inputThread);
        v->inputStarted = false;
    }
    mutexLock(&v->lock);
    const bool was = v->streaming;
    v->streaming = false;
    v->deliveryFeedback = false;
    v->udpVideo = false;
    NqReset(&v->deliveryControl);
    VqClear(&v->q);
    v->queuedTicket = v->sendingTicket = 0;
    mutexUnlock(&v->lock);
    if (was)
    {
        atomic_fetch_sub(&g_streamingCount, 1);
    }
    v->socket = SOCKET_INVALID;
    OpusAudio_Destroy(v->opus);
    v->opus = NULL;
    if (was)
        ClientInputReleaseSlot(v->slot); // its pad and HOME (and the touchscreen if it held it)
}

void Viewers_Release(Viewer *v)
{
    Viewers_Detach(v);
    mutexLock(&g_gate);
    v->reserved = false;
    v->punchPhase = false;
    mutexUnlock(&g_gate);
}

bool Viewers_NextVideo(Viewer *v, const u8 **wire, u32 *wireSize, u32 waitMs)
{
    const int index = ViewerIndex(v);
    mutexLock(&v->lock);
    if (v->q.count == 0 && waitMs != 0)
        condvarWaitTimeout(&v->cv, &v->lock, (u64)waitMs * 1000000ULL);
    u32 len = 0;
    const bool ok = VqPop(&v->q, RingOf(index), g_send[index], &len);
    if ((ok))
    {
        v->sendingTicket = v->queuedTicket;
        v->queuedTicket = 0;
    }
    mutexUnlock(&v->lock);
    if (!ok)
        return false;
    *wire = g_send[index];
    *wireSize = len;
    return true;
}

bool Viewers_NextAudio(Viewer *v, const u8 **wire, u32 *wireSize)
{
    if (!v->wantAudio)
        return false;
    AudioPacket *pkt = &g_audio[ViewerIndex(v)];
    if (v->opus)
    {
        // Gather ring chunks until a whole 20 ms frame is there, then send it as one Opus packet.
        while (!PcmAccumHasFrame(&v->pcm))
        {
            if (!CaptureTryReadAudioFor(&v->audioCursor, v->batching, pkt))
                return false;
            PcmAccumAdd(&v->pcm, pkt->Data, pkt->Header.DataSize);
            pkt->Header.DataSize = 0;
        }
        const u64 timestamp = pkt->Header.Timestamp;
        UdpmVideo_NoteAudioPcm(PcmAccumFrame(&v->pcm), PCM_FRAME_BYTES);
        const u32 n = OpusAudio_Encode(v->opus, PcmAccumFrame(&v->pcm), pkt->Data, 1275u);
        PcmAccumPop(&v->pcm);
        if (!n)
            return false;
        pkt->Header.Magic = STREAM_PACKET_HEADER;
        pkt->Header.MetaData = PacketMeta_Type_Audio | PacketMeta_Content_Data | PacketMeta_Audio_Opus;
        pkt->Header.ReplaySlot = 0xFF;
        pkt->Header.DataSize = n;
        pkt->Header.Timestamp = timestamp;
        *wire = (const u8 *)pkt;
        *wireSize = sizeof(PacketHeader) + n;
        return true;
    }
    if (!CaptureTryReadAudioFor(&v->audioCursor, v->batching, pkt))
        return false;
    *wire = (const u8 *)pkt;
    *wireSize = sizeof(PacketHeader) + pkt->Header.DataSize;
    return true;
}
