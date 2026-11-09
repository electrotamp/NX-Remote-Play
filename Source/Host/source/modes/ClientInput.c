#include <stdio.h>
#include <string.h>
#include "ClientInput.h"

// Support-log line (defined in nxrp_video/applet_mitm_log.cpp, extern "C"); shows as "controllers: <line>" in
// support.txt.
void UnifiedSupportLine(const char *line);
#include "../capture.h"
#include "../hidplus/udp_manager.hpp"

// a physical tap lasted roughly 70-90ms in TouchTrace. Local timing was
// introduced to remove network latency from the gesture duration. Later tests still
// highlighted HOME tiles without selecting them, so duration alone was not the cause.
//
// Fix: decouple the console-reported touch duration from real network latency. The instant a
// press arrives for a NEW gesture, fire a locally-timed Start->Move->End sequence lasting
// ~80ms on the console itself (see FAST_TAP_* below and RefreshHeldTouch), matching real
// hardware timing - it does not wait for Android's actual release packet, which will almost
// always arrive after the synthetic tap has already completed. If a SECOND press arrives at a
// genuinely different position before that ~80ms window elapses, that means Android's own
// touch-slop dead zone (MainActivity.kt) has already decided this is a real drag, not tap
// jitter - switch to continuous drag mode instead, driven by real position updates until the
// real release arrives.
typedef enum
{
    TouchMode_Idle,
    TouchMode_FastTap,
    TouchMode_RealDrag = 3
} TouchMode;
static TouchMode g_touchMode = TouchMode_Idle;
static bool g_touchActive = false;
//  (multi-viewer): there is one touchscreen. The slot (viewer) that started the current gesture owns it
// until the gesture ends; touch from any other slot is ignored meanwhile. -1 = nobody.
static int g_touchOwner = -1;
static u16 g_lastTouchX = 0, g_lastTouchY = 0;
static u64 g_fastTapStartTick = 0;

static u64 g_lastSampleTick = 0;

static void SendTouchSample(u16 x, u16 y, u32 attributes)
{
    // TouchTrace showed zero delta_time in earlier synthetic samples. Supplying elapsed
    // time did not by itself make HOME select a tile; keep it for realistic timing.
    u64 now = svcGetSystemTick();
    u64 deltaTimeNs = (attributes & HidTouchAttribute_Start) ? 0 : armTicksToNs(now - g_lastSampleTick);
    g_lastSampleTick = now;

    HidTouchState state = {0};
    state.delta_time = deltaTimeNs;
    state.attributes = attributes;
    state.finger_id = 0;
    state.x = x;
    state.y = y;
    state.diameter_x = 10;
    state.diameter_y = 10;
    hiddbgSetTouchScreenAutoPilotState(&state, 1);
}

// HID derives Start/End from changes in the supplied finger list. Its Set handler
// overwrites the caller's attribute bits for a finger that remains present. Submit
// zero fingers while the override is active so HID generates a real End sample,
// then leave one short sampling window before clearing the override.
static void EndTouch(void)
{
    HidTouchState empty = {0};
    Result rc = hiddbgSetTouchScreenAutoPilotState(&empty, 0);
    if (R_SUCCEEDED(rc))
        svcSleepThread(20E+6);
    hiddbgUnsetTouchScreenAutoPilotState();
    g_touchActive = false;
    g_touchMode = TouchMode_Idle;
    g_touchOwner = -1;
}

static void ApplyTouchPacket(int slot, const u8 *packet)
{
    if (g_touchActive && g_touchOwner != slot)
        return; // another viewer is using the touchscreen
    // Avoid touch injection while final-visible capture briefly patches VI.
    if (Capture_IsViPatchBusy())
    {
        return;
    }

    bool pressed = packet[0] != 0;
    if (!pressed)
    {
        // FastTap mode: the local ~80ms sequence in RefreshHeldTouch is already driving this
        // gesture to completion (or already has) - this release arrived too late to matter,
        // which is the whole point. Only a genuine RealDrag needs to be ended HERE, at the
        // real moment the finger actually lifted.
        if (g_touchMode == TouchMode_RealDrag)
        {
            EndTouch();
        }
        return;
    }

    u16 x = (u16)(packet[1] | (packet[2] << 8));
    u16 y = (u16)(packet[3] | (packet[4] << 8));

    if (!g_touchActive)
    {
        // New gesture - always starts as a fast synthetic tap, timed locally.
        SendTouchSample(x, y, HidTouchAttribute_Start);
        g_touchActive = true;
        g_touchOwner = slot;
        g_touchMode = TouchMode_FastTap;
        g_fastTapStartTick = svcGetSystemTick();
    }
    else if (x != g_lastTouchX || y != g_lastTouchY)
    {
        // Position actually changed - Android's own touch-slop dead zone already decided
        // this is a real drag, not tap jitter. Promote out of FastTap mode if still in it.
        g_touchMode = TouchMode_RealDrag;
        SendTouchSample(x, y, 0);
    }
    // else: same position resent while already active - RefreshHeldTouch's periodic
    // resampling already covers this, nothing new to send.

    g_lastTouchX = x;
    g_lastTouchY = y;
}

// hardware capture (TouchTrace.c + hidGetGestureStates) proved the gesture
// recognizer DOES classify our synthetic tap correctly (Tap(5) -> Complete(1)), but skips
// the intermediate Touch(3) state a real tap always shows first - real hardware samples at
// ~4ms intervals (250Hz), ours were 20ms apart, 5x slower. If the recognizer batches
// whatever raw samples arrived since its last internal poll into one classification, our
// wider spacing could be why it jumps straight to the final result instead of surfacing
// Touch(3) first. Tightened to try to match real cadence and see if that changes the
// captured sequence - not yet proven this is the actual missing piece, just the most
// concrete remaining lead.
#define FAST_TAP_DURATION_NS (80ULL * 1000 * 1000)
#define FAST_TAP_STEP_NS (5ULL * 1000 * 1000)
#define TOUCH_REFRESH_NS (16ULL * 1000 * 1000)
static u64 g_lastTouchRefreshTick;

// a "hold the override open longer before releasing" mode (Cooldown)
// was tried and reverted - hardware proof showed the Tap(5) gesture sample only ever
// appeared IMMEDIATELY AFTER hiddbgUnsetTouchScreenAutoPilotState() was called, never during
// the hold itself, so extending the hold time before that call did nothing but delay when
// the same brief Tap->Idle window occurred - it never made that window any longer. Worse, it
// left g_touchActive true for longer, so a second real tap arriving during the extended
// window got misread as a drag continuation of the first (observed on hardware: a rapid
// second tap produced HidGestureType_Pan instead of a clean new tap). Back to releasing
// immediately once the local FastTap timer expires - proven not worse than this, and
// simpler.
static void RefreshHeldTouch(void)
{
    if (!g_touchActive)
        return;
    u64 now = svcGetSystemTick();

    if (g_touchMode == TouchMode_FastTap)
    {
        if (now - g_fastTapStartTick >= armNsToTicks(FAST_TAP_DURATION_NS))
        {
            EndTouch();
            return;
        }
        if (now - g_lastTouchRefreshTick >= armNsToTicks(FAST_TAP_STEP_NS))
        {
            g_lastTouchRefreshTick = now;
            SendTouchSample(g_lastTouchX, g_lastTouchY, 0);
        }
        return;
    }

    if (g_touchMode == TouchMode_RealDrag)
    {
        if (now - g_lastTouchRefreshTick >= armNsToTicks(TOUCH_REFRESH_NS))
        {
            g_lastTouchRefreshTick = now;
            SendTouchSample(g_lastTouchX, g_lastTouchY, 0);
        }
    }
}

// controller input, same punched socket, same client->console direction as touch.
// Touch packets start with pressed = 0/1, so any first byte >= 2 is unambiguous. Controller
// packet, 17 bytes little-endian: [0x02][keys:u64 HidNpadButton bits][lx:s16][ly:s16][rx:s16][ry:s16]
// (sticks in HID's +-32767 range, +Y up). The client sends the full state on every change, so
// a lost/late packet self-heals on the next one.

//  (multi-viewer): every remote source has its own slot and its own virtual pad. Slot 0 is the first
// transport (the USB hosting desktop, or the single client of an unbridged session); slots 1..3 are additional
// remote viewers. Each slot has independent pad / HOME state; hiddbg player order is decided in con_manager.cpp.
#define INPUT_SLOTS 4

//  (multi-viewer): several transports (one thread per unbridged viewer, the USB input thread) call in at
// once. Touch state and the per-slot pad state are plain globals, so every public entry point takes this lock.
// (Non-recursive: the internal *Locked helpers never take it again.)
static Mutex g_inputMutex;

// "system button" packet - the clients' menu (Home). Deliberately the SAME size as a touch
// packet (5 bytes) so PollClientInput's existing first read can never over-read into the next packet:
// [0x03][flags][0][0][0], flags bit0 = HOME held. Sent on press, refreshed every ~250 ms while held,
// and again on release; the host auto-releases after HOME_HOLD_TIMEOUT_NS without a refresh (and on
// disconnect) so a lost release can never leave HOME stuck. HDLS state bit 18 = HOME
// (HiddbgNpadButton_Home) - the same bit that opened the HOME overlay when a client's left stick
// was mis-forwarded on , so it is known to reach the real system.
#define HOME_KEY_BIT (1ull << 18)
#define HOME_HOLD_TIMEOUT_NS 1000000000ull
#define HOME_ATTACH_SETTLE_NS 500000000ull // wait after attaching the virtual pad before pressing HOME
#define HOME_MIN_HOLD_NS 150000000ull      // HOME is never down for less than this

typedef struct
{
    bool padAttached;
    bool homeHeld;
    bool homeViaOverride; // this slot's HOME is held through the hid:dbg HOME-button override (no pad bit)
    bool homeReleasePending;
    u64 homeAppliedTick;
    u64 homeTick;
    bool haveLastPad;
    struct input_message lastPadBase; // last real pad state (HOME bit excluded), re-applied when HOME changes
} PadSlot;
static PadSlot g_pads[INPUT_SLOTS];

// the on-screen HOME must ALWAYS work, whatever the console's own controllers are doing.
// HOME through a virtual pad only works when that pad is Player 1; on a regular console a wireless Joy-Con pair
// can hold P1, our pad becomes P2 and hid rejects every update (rc 0x1c24ca), so HOME is lost (hardware, :
// HOME worked with the Joy-Cons off). hid:dbg has a dedicated HOME-button override, cmd 111 SetHomeButtonAutoPilotState
// (u64, bit0) / cmd 112 Unset, independent of any npad. Hardware-proven on the Lite by capture research  .
// It is tried first; if the call fails (older firmware, command missing) the existing pad path below is used unchanged.
// Decided at run time, never by model. The override is global, so it is reference-counted across viewer slots.
static int g_homeOverrideHolders = 0;

static bool HomeOverrideSet(bool pressed)
{
    u64 state = pressed ? 1 : 0;
    Result rc = serviceDispatchIn(hiddbgGetServiceSession(), 111, state);
    if (R_FAILED(rc))
    {
        return false;
    }
    return true;
}

// A slot stops holding the override; the last holder to leave releases it (state 0, then Unset as  did).
static void HomeOverrideRelease(PadSlot *pad)
{
    if (!pad->homeViaOverride)
        return;
    pad->homeViaOverride = false;
    if (g_homeOverrideHolders > 0)
        g_homeOverrideHolders--;
    if (g_homeOverrideHolders == 0)
    {
        HomeOverrideSet(false);
        serviceDispatch(hiddbgGetServiceSession(), 112);
    }
}

static void ApplyControllerPacket(int slot, const u8 *p)
{
    PadSlot *pad = &g_pads[slot];
    struct input_message m = {0};
    m.magic = INPUT_MSG_MAGIC;
    m.con_count = 1;
    m.con_type = 1; // Pro Controller
    memcpy(&m.keys, p + 1, 8);
    // ONLY the real buttons (bits 0-15: A..D-pad). In the HDLS state, bits 18 and 19 mean HOME and
    // CAPTURE (HiddbgNpadButton_Home/Capture) - but in libnx's HidNpadButton (what a client's padGetButtons()
    // returns) the same bits are the LEFT STICK pushed right / down. Passing them through made moving
    // the left stick trigger screenshots and the HOME overlay on the host . The host derives
    // stick directions from the analog values itself, so nothing is lost.
    m.keys &= 0xFFFFull;
    s16 v[4];
    memcpy(v, p + 9, 8);
    m.joy_l_x = v[0];
    m.joy_l_y = v[1];
    m.joy_r_x = v[2];
    m.joy_r_y = v[3];
    pad->lastPadBase = m;
    pad->haveLastPad = true;
    if (pad->homeHeld && !pad->homeViaOverride)
        m.keys |= HOME_KEY_BIT; // a HOME hold from the menu stays pressed while other buttons change
    if (!pad->padAttached)
    {
        // shows in support.txt which slot's pad the console received its first controller packet for
        char line[64];
        snprintf(line, sizeof(line), "pad slot %d: first controller packet", slot);
        UnifiedSupportLine(line);
    }
    ApplyRelayHidPlusStateSlot(slot, m);
    pad->padAttached = true;
}

// HOME press/release from a client menu (SYS_PACKET_TYPE). A repeat of the current state only refreshes
// the failsafe timer; a change re-applies the last real pad state with HOME set/cleared.
static void ApplyHomeState(int slot, bool held)
{
    PadSlot *pad = &g_pads[slot];
    const u64 now = svcGetSystemTick();
    pad->homeTick = now;

    struct input_message m = {0};
    if (pad->haveLastPad)
        m = pad->lastPadBase;
    m.magic = INPUT_MSG_MAGIC;
    m.con_count = 1;
    m.con_type = 1; // Pro Controller

    if (held)
    {
        pad->homeReleasePending = false; // a new press cancels a release that was still being deferred
        if (pad->homeHeld)
            return; // refresh only
        // Preferred: the dedicated HOME override (works whoever holds Player 1). Another slot may already hold it.
        if (g_homeOverrideHolders > 0 || HomeOverrideSet(true))
        {
            g_homeOverrideHolders++;
            pad->homeViaOverride = true;
            pad->homeHeld = true;
            pad->homeAppliedTick = svcGetSystemTick();
            pad->homeTick = pad->homeAppliedTick;
            return;
        }
        // HARDWARE FINDING : a HOME set in the SAME instant the virtual pad is attached is lost
        // (autopilot's HOME works because its pad is attached first). With the pad already attached (e.g.
        // the client sent controller state) a press works immediately; otherwise attach a neutral pad,
        // give hid time to bring it up, then press.
        if (!pad->padAttached)
        {
            ApplyRelayHidPlusStateSlot(slot, m); // neutral state, no HOME yet
            pad->padAttached = true;
            svcSleepThread(HOME_ATTACH_SETTLE_NS);
        }
        pad->homeHeld = true;
        m.keys |= HOME_KEY_BIT;
        ApplyRelayHidPlusStateSlot(slot, m);
        pad->homeAppliedTick = svcGetSystemTick();
        pad->homeTick = pad->homeAppliedTick;
        return;
    }

    if (!pad->homeHeld)
        return;
    // The release packet can already be queued behind the settle delay above (a tap is only ~150 ms long on
    // the client); never let HOME be down for less than HOME_MIN_HOLD_NS of real time or hid may miss it.
    if (now - pad->homeAppliedTick < armNsToTicks(HOME_MIN_HOLD_NS))
    {
        pad->homeReleasePending = true;
        return;
    }
    pad->homeHeld = false;
    pad->homeReleasePending = false;
    if (pad->homeViaOverride)
    {
        HomeOverrideRelease(pad); // no pad update: HOME never rode on the pad
        return;
    }
    ApplyRelayHidPlusStateSlot(slot, m);
}

// Failsafe + deferred release: runs every loop pass.
static void RefreshHomeHold(int slot)
{
    PadSlot *pad = &g_pads[slot];
    if (!pad->homeHeld)
        return;
    const u64 now = svcGetSystemTick();
    if (pad->homeReleasePending && now - pad->homeAppliedTick >= armNsToTicks(HOME_MIN_HOLD_NS))
    {
        ApplyHomeState(slot, false);
        return;
    }
    if (now - pad->homeTick >= armNsToTicks(HOME_HOLD_TIMEOUT_NS))
    {
        pad->homeReleasePending = false;
        pad->homeAppliedTick = 0; // long past: no minimum-hold deferral
        ApplyHomeState(slot, false);
    }
}

// Detaches the slot's virtual pad so a dropped connection can never leave buttons held or a phantom
// controller connected.
static void ReleaseControllerPad(int slot)
{
    PadSlot *pad = &g_pads[slot];
    if (!pad->padAttached)
        return;
    struct input_message m = {0};
    m.magic = INPUT_MSG_MAGIC;
    m.con_count = 1;
    m.con_type = 0;
    ApplyRelayHidPlusStateSlot(slot, m);
    pad->padAttached = false;
    {
        char line[64];
        snprintf(line, sizeof(line), "pad slot %d: released", slot);
        UnifiedSupportLine(line);
    }
}

bool ClientInputApplySlot(int slot, const u8 *packet, size_t size)
{
    if (slot < 0 || slot >= INPUT_SLOTS)
        return false;
    bool ok = true;
    mutexLock(&g_inputMutex);
    if (size == CTRL_PACKET_SIZE && packet[0] == CTRL_PACKET_TYPE)
        ApplyControllerPacket(slot, packet);
    else if (size == TOUCH_PACKET_SIZE && packet[0] == SYS_PACKET_TYPE)
        ApplyHomeState(slot, (packet[1] & 1) != 0);
    else if (size == TOUCH_PACKET_SIZE && packet[0] <= 1)
        ApplyTouchPacket(slot, packet);
    else
        ok = false;
    mutexUnlock(&g_inputMutex);
    return ok;
}

void ClientInputService(void)
{
    mutexLock(&g_inputMutex);
    RefreshHeldTouch();
    for (int slot = 0; slot < INPUT_SLOTS; slot++)
        RefreshHomeHold(slot);
    mutexUnlock(&g_inputMutex);
}

// Everything one slot holds: its pad and HOME, plus the touchscreen if that slot owns it.
static void ReleaseSlotLocked(int slot)
{
    if (!g_touchActive || g_touchOwner == slot)
    {
        hiddbgUnsetTouchScreenAutoPilotState();
        g_touchActive = false;
        g_touchMode = TouchMode_Idle;
        g_touchOwner = -1;
    }
    PadSlot *pad = &g_pads[slot];
    HomeOverrideRelease(pad); // a dropped connection must never leave the HOME override held
    pad->homeHeld = false;
    pad->homeReleasePending = false;
    pad->haveLastPad = false;
    ReleaseControllerPad(slot);
}

void ClientInputReleaseSlot(int slot)
{
    if (slot < 0 || slot >= INPUT_SLOTS)
        return;
    mutexLock(&g_inputMutex);
    ReleaseSlotLocked(slot);
    mutexUnlock(&g_inputMutex);
}

void ClientInputReleaseAll(void)
{
    mutexLock(&g_inputMutex);
    hiddbgUnsetTouchScreenAutoPilotState();
    g_touchActive = false;
    g_touchMode = TouchMode_Idle;
    g_touchOwner = -1;
    for (int slot = 0; slot < INPUT_SLOTS; slot++)
        ReleaseSlotLocked(slot);
    mutexUnlock(&g_inputMutex);
}
