#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "modes.h"
#include "UsbAuto.h"
#include "PunchProbe.h"
#include "PunchWake.h"
#include "ClientInput.h"
#include "UsbControl.h"
#include "BridgeOwner.h"
#include "BridgeDelegation.h"
#include "proto.h"
#include "../USB/Serial.h"
#include "../capture.h"

static atomic_bool g_active = false;
static atomic_bool g_stop = false;
static Thread g_thread;
static u8 alignas(0x1000) g_stack[0x4000 + LOGGING_STACK_BOOST];
static atomic_bool g_controls = false, g_readingControls = false;
static Thread g_inputThread;
static u8 alignas(0x1000) g_inputStack[0x4000 + LOGGING_STACK_BOOST];
static u64 g_controlSession;
static bool g_ownerEnabled;
static bool g_enrollEnabled; // first-use owner-key setup (operations 8/9) offered on this attachment
#define USB_ENROLL_MAGIC 0x3145584Eu
static Mutex g_ownerReplyMutex;
static bool g_ownerReplyReady;
static struct __attribute__((packed))
{
    PacketHeader header;
    u32 token, status;
    u8 proof[32], reserved[24];
} g_ownerReply;

//  USB attachment diagnostics. A Lite on USB stopped answering the desktop after a short capture
// stall and only recovered on a cable replug, and nothing in support.txt/log.txt showed why (LOG() goes to UDP only).
// These lines are written to support.txt as "usb: ..." and to UDP. Observation only: nothing here changes what the USB
// thread does.
void UnifiedUsbSupportLine(const char *line);
static void UsbSay(const char *fmt, ...)
{
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    UnifiedUsbSupportLine(line);
}

enum
{
    UsbStage_Hello,
    UsbStage_Request,
    UsbStage_Acquire,
    UsbStage_ModeSwitch,
    UsbStage_Handshake,
    UsbStage_AudioSend,
    UsbStage_CaptureRead,
    UsbStage_VideoSend,
    UsbStage_OwnerReply,
    UsbStage_Cleanup,
    UsbStage_Restore
};
static const char *const g_stageName[] = {"waiting for a USB host (hello)",
                                          "reading the host request",
                                          "acquiring USB from network viewers",
                                          "waiting for the mode switch",
                                          "sending the handshake",
                                          "sending audio",
                                          "reading capture video",
                                          "sending video",
                                          "sending the owner reply",
                                          "ending the session",
                                          "restoring the previous mode"};
static atomic_int g_stage = UsbStage_Hello;
static atomic_ullong g_stageTick;
static atomic_uint g_sessVideo, g_sessAudio;
static atomic_ullong g_sessStart, g_sessLastSend;
static bool g_backlightReported;
static int g_lastBacklight = -1;

static const char *UsbStateName(int state)
{
    switch (state)
    {
    case 0:
        return "detached";
    case 1:
        return "default";
    case 2:
        return "address";
    case 3:
        return "configured";
    case 4:
        return "suspended";
    default:
        return "unknown";
    }
}

// Marks the step the USB thread is about to enter. A step that took unusually long is reported when it ends; the
// watchdog reports one that never ends.
static void UsbStage(int stage)
{
    const u64 now = svcGetSystemTick();
    const int previous = atomic_load(&g_stage);
    const u64 started = atomic_load(&g_stageTick);
    if (started && previous != UsbStage_Hello)
    {
        const u64 ms = armTicksToNs(now - started) / 1000000ULL;
        const u64 limit = previous == UsbStage_CaptureRead ? 500 : previous == UsbStage_Request ? 1500 : 3000;
        if (ms > limit)
            UsbSay("'%s' took %u ms", g_stageName[previous], (unsigned)ms);
    }
    atomic_store(&g_stage, stage);
    atomic_store(&g_stageTick, now);
}

// 1 = screen on, 0 = off, -1 = unknown. The lbl session is opened and closed per query, like UtilSetConsoleScreenMode.
static int UsbScreenState(void)
{
    if (R_FAILED(lblInitialize()))
        return -1;
    LblBacklightSwitchStatus status = LblBacklightSwitchStatus_Disabled;
    int result =
        R_SUCCEEDED(lblGetBacklightSwitchStatus(&status)) ? (status == LblBacklightSwitchStatus_Enabled ? 1 : 0) : -1;
    lblExit();
    return result;
}

// Runs on the USB input thread (always alive, wakes every 10-50 ms): bus state changes, a USB thread stuck in one step,
// the screen switching on/off during a session, and a once-a-minute session heartbeat.
static void UsbWatchdog(void)
{
    static u64 lastPoll, lastBacklight, lastBeat, reportedTick;
    static int lastState = -1, reportedMark;
    const u64 now = svcGetSystemTick();
    if (now - lastPoll < armNsToTicks(1000000000ULL))
        return;
    lastPoll = now;

    UsbState state;
    if (R_SUCCEEDED(usbDsGetState(&state)) && (int)state != lastState)
    {
        UsbSay("bus state %s -> %s", lastState < 0 ? "start" : UsbStateName(lastState), UsbStateName((int)state));
        lastState = (int)state;
    }

    const int stage = atomic_load(&g_stage);
    const u64 tick = atomic_load(&g_stageTick);
    if (stage != UsbStage_Hello && tick)
    {
        if (tick != reportedTick)
        {
            reportedTick = tick;
            reportedMark = 0;
        }
        const u64 secs = armTicksToNs(now - tick) / 1000000000ULL;
        static const int marks[] = {3, 10, 30, 60, 120, 300};
        for (unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
            if ((int)secs >= marks[i] && reportedMark < marks[i])
            {
                reportedMark = marks[i];
                UsbSay("USB thread has been '%s' for %u s (screen %d)", g_stageName[stage], (unsigned)secs,
                       UsbScreenState());
                break;
            }
    }

    if (atomic_load(&g_active))
    {
        if (now - lastBacklight >= armNsToTicks(5000000000ULL))
        {
            lastBacklight = now;
            const int screen = UsbScreenState();
            if (!g_backlightReported || screen != g_lastBacklight)
            {
                UsbSay("screen %s during the USB session", screen == 1 ? "on" : screen == 0 ? "OFF" : "unknown");
                g_backlightReported = true;
                g_lastBacklight = screen;
            }
        }
        if (now - lastBeat >= armNsToTicks(60000000000ULL))
        {
            lastBeat = now;
            const u64 last = atomic_load(&g_sessLastSend);
            UsbSay("session %u s: video %u audio %u, last media sent %u ms ago, stage '%s'",
                   (unsigned)(armTicksToNs(now - atomic_load(&g_sessStart)) / 1000000000ULL), atomic_load(&g_sessVideo),
                   atomic_load(&g_sessAudio), (unsigned)(last ? armTicksToNs(now - last) / 1000000ULL : 0),
                   g_stageName[stage]);
        }
    }
    else
    {
        g_backlightReported = false;
        lastBeat = now;
    }
}

static bool SendOwnerReply(void)
{
    // Only the media thread writes IN transfers. Do not interleave an input-thread reply
    // inside a video packet; keep one bounded mailbox and reject competing sign requests.
    mutexLock(&g_ownerReplyMutex);
    bool ok = true;
    if (g_ownerReplyReady)
    {
        ok = UsbStreamingSend(&g_ownerReply, sizeof(g_ownerReply));
        memset(&g_ownerReply, 0, sizeof(g_ownerReply));
        g_ownerReplyReady = false;
    }
    mutexUnlock(&g_ownerReplyMutex);
    return ok;
}

static void UsbInputThread(void *arg)
{
    (void)arg;
    while (true)
    {
        UsbWatchdog();
        if (!atomic_load(&g_controls))
        {
            svcSleepThread(10000000ULL);
            continue;
        }
        atomic_store(&g_readingControls, true);
        if (!atomic_load(&g_controls))
        {
            atomic_store(&g_readingControls, false);
            continue;
        }
        UsbControlFrame frame;
        size_t have = 0;
        bool sequenced = false;
        //  (multi-viewer): slot 0 is the hosting desktop's pad, 1..USB_VIEWER_SLOTS are remote viewers'
        // pads; each has its own held state and one-second stale-input watchdog.
        bool held[USB_VIEWER_SLOTS + 1] = {false};
        u64 lastInput[USB_VIEWER_SLOTS + 1];
        for (int slot = 0; slot <= USB_VIEWER_SLOTS; slot++)
            lastInput[slot] = svcGetSystemTick();
        u32 sequence = 0;
        UsbOwnerAssembly assembly = {0};
        while (atomic_load(&g_controls))
        {
            UsbWatchdog();
            have += usbSerialRead((u8 *)&frame + have, sizeof(frame) - have, 50000000ULL);
            if (!atomic_load(&g_controls))
                break;
            if (have == sizeof(frame))
            {
                have = 0;
                if (UsbControlValid(&frame) && frame.session == g_controlSession &&
                    (!sequenced || UsbControlSequenceForward(frame.sequence, sequence)))
                {
                    sequence = frame.sequence;
                    sequenced = true;
                    if (frame.operation == 6)
                    {
                        if (g_ownerEnabled && UsbOwnerAppend(&assembly, frame.payload, frame.length, svcGetSystemTick(),
                                                             armNsToTicks(2000000000ULL)) == 1)
                        {
                            mutexLock(&g_ownerReplyMutex);
                            if (!g_ownerReplyReady)
                            {
                                memset(&g_ownerReply, 0, sizeof(g_ownerReply));
                                g_ownerReply.header.Magic = USB_OWNER_MAGIC;
                                g_ownerReply.header.DataSize = 64;
                                g_ownerReply.token = assembly.token;
                                g_ownerReply.status =
                                    BridgeOwner_Sign(assembly.bytes, assembly.total, g_ownerReply.proof);
                                g_ownerReplyReady = true;
                            }
                            mutexUnlock(&g_ownerReplyMutex);
                            memset(&assembly, 0, sizeof(assembly));
                        }
                    }
                    else if (frame.operation == 8 || frame.operation == 9)
                    {
                        // First-use owner-key setup. Reply token = the frame's own sequence number.
                        if (g_enrollEnabled)
                        {
                            mutexLock(&g_ownerReplyMutex);
                            bool available = !g_ownerReplyReady;
                            mutexUnlock(&g_ownerReplyMutex);
                            if (available)
                            {
                                u8 fingerprint[32];
                                u32 status = frame.operation == 8
                                                 ? BridgeOwner_StageKey(frame.payload, frame.length, fingerprint)
                                                 : BridgeOwner_CommitKey(frame.payload, frame.length, fingerprint);
                                mutexLock(&g_ownerReplyMutex);
                                memset(&g_ownerReply, 0, sizeof(g_ownerReply));
                                g_ownerReply.header.Magic = USB_ENROLL_MAGIC;
                                g_ownerReply.header.DataSize = 64;
                                g_ownerReply.token = frame.sequence;
                                g_ownerReply.status = status;
                                memcpy(g_ownerReply.proof, fingerprint, 32);
                                g_ownerReplyReady = true;
                                mutexUnlock(&g_ownerReplyMutex);
                                if (frame.operation == 9 && status == 0)
                                {
                                    // The console now holds a key: signing and delegation work for the rest of this
                                    // attachment.
                                    g_ownerEnabled = true;
                                    g_enrollEnabled = false;
                                }
                                memset(fingerprint, 0, sizeof(fingerprint));
                            }
                        }
                    }
                    else if (frame.operation == 7)
                    {
                        if (g_ownerEnabled)
                        {
                            mutexLock(&g_ownerReplyMutex);
                            bool available = !g_ownerReplyReady;
                            mutexUnlock(&g_ownerReplyMutex);
                            if (available)
                            {
                                // Quiescence can wait for a network poll. Never hold the media
                                // mailbox mutex across it: local capture continues until the ACK.
                                u8 acknowledgement[32];
                                u32 status = BridgeOwner_Delegate(frame.payload, frame.length, g_controlSession,
                                                                  acknowledgement);
                                mutexLock(&g_ownerReplyMutex);
                                memset(&g_ownerReply, 0, sizeof(g_ownerReply));
                                g_ownerReply.header.Magic = USB_DELEGATION_MAGIC;
                                g_ownerReply.header.DataSize = 64;
                                g_ownerReply.token = OwnerU32(frame.payload);
                                g_ownerReply.status = status;
                                memcpy(g_ownerReply.proof, acknowledgement, 32);
                                g_ownerReplyReady = true;
                                mutexUnlock(&g_ownerReplyMutex);
                            }
                        }
                    }
                    else
                    {
                        const int slot = frame.slot;
                        lastInput[slot] = svcGetSystemTick();
                        if (frame.operation == 5)
                        {
                            ClientInputReleaseSlot(slot);
                            held[slot] = false;
                        }
                        else if (frame.operation != 4)
                        {
                            ClientInputApplySlot(slot, frame.payload, frame.length);
                            held[slot] = true;
                        }
                    }
                }
            }
            ClientInputService();
            for (int slot = 0; slot <= USB_VIEWER_SLOTS; slot++)
            {
                if (held[slot] && svcGetSystemTick() - lastInput[slot] > armNsToTicks(1000000000ULL))
                {
                    ClientInputReleaseSlot(slot);
                    held[slot] = false;
                }
            }
        }
        ClientInputReleaseAll();
        atomic_store(&g_readingControls, false);
    }
}

static bool SendCapabilities(bool ownerRequested)
{
    UsbCapabilities caps = {0};
    caps.magic = USB_CAPABILITY_MARKER;
    caps.version = 1;
    caps.size = sizeof(caps);
    caps.capabilities = USB_CAP_IDENTITY | USB_CAP_INPUT | USB_CAP_INPUT_TIMEOUT;
    caps.slots = 1; // legacy byte: older desktops require exactly 1
    caps.capabilities |= USB_CAP_VIEWER_SLOTS;
    caps.reserved[0] = USB_VIEWER_SLOTS; // extra per-viewer pads (slots 1..n)
    g_controlSession = svcGetSystemTick();
    if (g_controlSession == 0)
        g_controlSession = 1;
    caps.session = g_controlSession;
    PunchWake_GetLocalIdentity(caps.id, sizeof(caps.id), caps.password, sizeof(caps.password));
    g_ownerEnabled = ownerRequested && BridgeOwner_Ready(caps.id, caps.padding);
    // No usable key yet and the desktop asked for owner mode: offer to install one (see BridgeOwner_StageKey).
    g_enrollEnabled = ownerRequested && !g_ownerEnabled && BridgeOwner_CanEnroll();
    if (g_enrollEnabled)
        caps.capabilities |= USB_CAP_OWNER_ENROLL;
    if (g_ownerEnabled)
        caps.capabilities |= USB_CAP_OWNER_SIGNER;
    if (g_ownerEnabled && PunchWake_DirectOwnershipEnabled())
        caps.capabilities |= USB_CAP_OWNER_DELEGATION;
    PunchWake_GetServer(caps.server, sizeof(caps.server));
    strncpy(caps.serial, NxRemotePlayBeacon + NxRemotePlayBeaconLen - sizeof(SetSysSerialNumber),
            sizeof(caps.serial) - 1);
    return UsbStreamingSend(&caps, sizeof(caps));
}

static void UsbModeExit(void)
{
    atomic_store(&g_stop, true);
    // The USB service stays enumerated, but its capture must finish before
    // another mode resets shared packet buffers or client options.
    while (atomic_load(&g_active))
        svcSleepThread(10000000ULL);
}

// The persistent worker owns USB I/O. Selecting USB through the NRO still
// works, but no second set of streaming threads is created.
const StreamMode USB_MODE = {UsbModeExit};

static bool RequestValid(const struct ProtoHandshakeRequest *request)
{
    return request->Magic == PROTO_REQUEST_MAGIC && memcmp(&request->ProtoVer, NXREMOTEPLAY_PROTOCOL_VERSION, 2) == 0 &&
           (request->MetaFlags & (ProtoMeta_Video | ProtoMeta_Audio)) != 0;
}

static void Reject(void)
{
    ProtoHandshakeResponse response = {0};
    response.Code = Handshake_UnknownFailure;
    response.MemoryPools.QueryResult = UINT32_MAX;
    UsbStreamingSend(&response, sizeof(response));
}

static void UsbAutoThread(void *arg)
{
    (void)arg;
    u32 helloFailures = 0;
    u64 helloFirstFailure = 0;
    while (true)
    {
        UsbStage(UsbStage_Hello);
        if (usbSerialWrite(PROTO_HANDSHAKE_HELLO, sizeof(PROTO_HANDSHAKE_HELLO), 1000000000ULL) !=
            sizeof(PROTO_HANDSHAKE_HELLO))
        {
            if (helloFailures++ == 0)
            {
                helloFirstFailure = svcGetSystemTick();
                UsbSay("no USB host is reading yet (hello not accepted); this is normal without a cable");
            }
            svcSleepThread(100000000ULL);
            continue;
        }
        if (helloFailures)
        {
            UsbSay("a USB host answered after %u unanswered hellos (%u s)", (unsigned)helloFailures,
                   (unsigned)(armTicksToNs(svcGetSystemTick() - helloFirstFailure) / 1000000000ULL));
            helloFailures = 0;
        }
        struct ProtoHandshakeRequest request;
        UsbStage(UsbStage_Request);
        if (usbSerialRead(&request, sizeof(request), 1000000000ULL) != sizeof(request))
        {
            UsbSay("the USB host took the hello but sent no request within 1 s");
            continue;
        }
        // Validate without touching global capture options owned by a live
        // network session. An occupied session gets a bounded refusal.
        if (!RequestValid(&request) || CoreModeSwitchPending())
        {
            UsbSay("refused a USB request: %s",
                   CoreModeSwitchPending() ? "a mode switch is pending" : "invalid request");
            Reject();
            continue;
        }
        // a USB host wins over network viewers. They are disconnected (they may reconnect by themselves or
        // manually once USB is released; while USB is attached new viewers are refused). Bounded to 2.5 s so the
        // handshake never stalls: if a session has not ended by then the host gets a refusal and its next attempt finds
        // the console free.
        UsbStage(UsbStage_Acquire);
        if (!PunchProbe_TryAcquireUsb())
        {
            UsbSay("USB host connected while network viewers are active; disconnecting them");
            const u64 evictStart = svcGetSystemTick();
            bool acquired = false;
            while (armTicksToNs(svcGetSystemTick() - evictStart) < 2500000000ULL)
            {
                PunchProbe_EvictViewersForUsb();
                svcSleepThread(100000000ULL);
                if (PunchProbe_TryAcquireUsb())
                {
                    acquired = true;
                    break;
                }
            }
            if (!acquired)
            {
                UsbSay("network viewers did not end within 2.5 s; refusing this attempt");
                Reject();
                continue;
            }
        }

        u32 previousMode = GetCurrentMode();
        if (previousMode != TYPE_MODE_USB)
            SetModeID(TYPE_MODE_USB);
        UsbStage(UsbStage_ModeSwitch);
        u64 transitionStart = svcGetSystemTick();
        while (CoreModeSwitchPending() && svcGetSystemTick() - transitionStart < armNsToTicks(15000000000ULL))
            svcSleepThread(10000000ULL);
        if (CoreModeSwitchPending() || GetCurrentMode() != TYPE_MODE_USB)
        {
            UsbSay("the mode switch to USB did not complete in 15 s; refusing");
            Reject();
            PunchProbe_ReleaseUsb();
            if (GetCurrentMode() == TYPE_MODE_USB && previousMode != TYPE_MODE_USB)
                SetModeID(previousMode);
            continue;
        }

        atomic_store(&g_stop, false);
        g_ownerEnabled = false;
        g_enrollEnabled = false;
        g_ownerReplyReady = false;
        BridgeOwner_ResetAttachment();
        atomic_store(&g_active, true);
        // Recheck after publishing active: a concurrent NRO mode change either
        // cancels us here or waits in UsbModeExit until cleanup has completed.
        bool accepted = !CoreModeSwitchPending() && GetCurrentMode() == TYPE_MODE_USB && !atomic_load(&g_stop);
        ProtoParsedHandshake parsed = {0};
        UsbStage(UsbStage_Handshake);
        if (accepted)
        {
            parsed = ProtoHandshake(ProtoHandshakeAccept_Any, (u8 *)&request, sizeof(request));
            bool extended = (request.FeatureFlags & ExtraFeatureFlags_UsbControl) &&
                            !(request.FeatureFlags & ExtraFeatureFlags_MemoryDiag);
            if (extended && parsed.Result.Code == Handshake_Ok)
                parsed.Result.MemoryPools.QueryResult = USB_CAPABILITY_MARKER;
            accepted = UsbStreamingSend(&parsed.Result, sizeof(parsed.Result)) && parsed.Result.Code == Handshake_Ok;
            if (accepted && extended)
                accepted = SendCapabilities((request.FeatureFlags & ExtraFeatureFlags_UsbOwner) != 0);
            atomic_store(&g_controls, accepted && extended);
        }
        else
            Reject();
        const char *endReason = "the USB host went away";
        if (!accepted)
            UsbSay("the handshake was not accepted (code %d)", (int)parsed.Result.Code);
        if (accepted)
        {
            UsbSay("session start: video %d audio %d controls %d (previous mode %u)", parsed.RequestedVideo ? 1 : 0,
                   parsed.RequestedAudio ? 1 : 0, atomic_load(&g_controls) ? 1 : 0, previousMode);
            atomic_store(&g_sessVideo, 0);
            atomic_store(&g_sessAudio, 0);
            atomic_store(&g_sessStart, svcGetSystemTick());
            atomic_store(&g_sessLastSend, 0);
            if (parsed.RequestedVideo)
                CaptureVideoConnected();
            if (parsed.RequestedAudio)
                CaptureAudioConnected();
            u64 lastSend = svcGetSystemTick();
            while (!atomic_load(&g_stop))
            {
                bool success = true;
                bool sentMedia = false;
                if (parsed.RequestedAudio)
                    for (int i = 0; i < 8 && CaptureTryReadAudio(); i++)
                    {
                        UsbStage(UsbStage_AudioSend);
                        success = UsbStreamingSend(&APkt, APkt.Header.DataSize + sizeof(PacketHeader));
                        if (!success)
                        {
                            endReason = "an audio send failed";
                            break;
                        }
                        lastSend = svcGetSystemTick();
                        atomic_fetch_add(&g_sessAudio, 1);
                        atomic_store(&g_sessLastSend, lastSend);
                        sentMedia = true;
                    }
                if (!success || atomic_load(&g_stop))
                {
                    if (success)
                        endReason = "the USB mode was stopped";
                    break;
                }
                UsbStage(UsbStage_CaptureRead);
                bool haveVideo = parsed.RequestedVideo && CaptureReadVideo();
                if (haveVideo)
                {
                    UsbStage(UsbStage_VideoSend);
                    if (atomic_load(&g_stop) || !UsbStreamingSend(&VPkt, VPkt.Header.DataSize + sizeof(PacketHeader)))
                    {
                        endReason = atomic_load(&g_stop) ? "the USB mode was stopped" : "a video send failed";
                        break;
                    }
                    lastSend = svcGetSystemTick();
                    atomic_fetch_add(&g_sessVideo, 1);
                    atomic_store(&g_sessLastSend, lastSend);
                    sentMedia = true;
                }
                // A healthy attachment keeps its local control suspension alive without
                // a recurring USB delegation command. A stalled send receives no refresh.
                if (sentMedia)
                    BridgeOwner_MaintainAttachment();
                // Signed server renewals still need their proof reply, but serving it after
                // media prevents takeover bookkeeping from preempting the live pump.
                UsbStage(UsbStage_OwnerReply);
                if (!SendOwnerReply())
                {
                    endReason = "the owner reply send failed";
                    break;
                }
                if (svcGetSystemTick() - lastSend > armNsToTicks(5000000000ULL))
                {
                    endReason = "no media could be sent for 5 s";
                    break;
                }
                svcSleepThread(1000000ULL);
            }
            UsbStage(UsbStage_Cleanup);
            UsbSay("session end after %u s: %s (video %u, audio %u, last media sent %u ms ago)",
                   (unsigned)(armTicksToNs(svcGetSystemTick() - atomic_load(&g_sessStart)) / 1000000000ULL), endReason,
                   atomic_load(&g_sessVideo), atomic_load(&g_sessAudio),
                   (unsigned)(armTicksToNs(svcGetSystemTick() - lastSend) / 1000000ULL));
        }
        UsbStage(UsbStage_Cleanup);
        atomic_store(&g_controls, false);
        while (atomic_load(&g_readingControls))
            svcSleepThread(1000000ULL);
        g_ownerEnabled = false;
        g_enrollEnabled = false;
        g_ownerReplyReady = false;
        memset(&g_ownerReply, 0, sizeof(g_ownerReply));
        BridgeOwner_ResetAttachment();
        ProtoClientGlobalStateDisconnected();
        PunchProbe_ReleaseUsb();
        atomic_store(&g_active, false);
        UsbSay("released local USB capture; ready for the next host");
        if (!CoreModeSwitchPending() && GetCurrentMode() == TYPE_MODE_USB && previousMode != TYPE_MODE_USB)
            SetModeID(previousMode);
        // Do not negotiate a new source while restoration still owns buffers.
        UsbStage(UsbStage_Restore);
        while (CoreModeSwitchPending())
            svcSleepThread(10000000ULL);
    }
}

void UsbAuto_Start(void)
{
    mutexInit(&g_ownerReplyMutex);
    Result rc = UsbStreamingInitialize();
    if (R_FAILED(rc))
    {
        return;
    }
    LaunchThread(&g_inputThread, UsbInputThread, NULL, g_inputStack, sizeof(g_inputStack), 0x2C);
    LaunchThread(&g_thread, UsbAutoThread, NULL, g_stack, sizeof(g_stack), 0x2C);
}
