#include "con_manager.hpp"
#include "udp_manager.hpp"
#include <mutex>
#include <array>

// Some of the code comes from hid-mitm

int FakeController::initialize(u16 conDeviceType)
{
    if (isInitialized)
        return 0;
    Result myResult;

    // Set the controller type to Pro-Controller, and set the npadInterfaceType.

    switch (conDeviceType)
    {
    case 1:
        controllerDevice.deviceType = HidDeviceType_FullKey3; // Pro Controller
        break;

    case 2:
        controllerDevice.deviceType = HidDeviceType_JoyLeft2; // Joy-Con Left
        break;

    case 3:
        controllerDevice.deviceType = HidDeviceType_JoyRight1; // Joy-Con Right
        break;
    }

    // Set the controller colors. The grip colors are for Pro-Controller on [9.0.0+].
    controllerDevice.singleColorBody = RGBA8_MAXALPHA(255, 153, 204);
    controllerDevice.singleColorButtons = RGBA8_MAXALPHA(0, 0, 0);
    if (conDeviceType == 1)
    {
        controllerDevice.colorLeftGrip = RGBA8_MAXALPHA(255, 0, 127);
        controllerDevice.colorRightGrip = RGBA8_MAXALPHA(255, 0, 127);
    }

    controllerDevice.npadInterfaceType = HidNpadInterfaceType_Bluetooth;

    // Setup example controller state.
    controllerState.battery_level = 4; // Set battery charge to full.

    if (conDeviceType == 1 || conDeviceType == 2)
    {
        controllerState.analog_stick_l.x = 0x0;
        controllerState.analog_stick_l.y = -0x0;
    }

    if (conDeviceType == 1 || conDeviceType == 3)
    {
        controllerState.analog_stick_r.x = 0x0;
        controllerState.analog_stick_r.y = -0x0;
    }

    myResult = hiddbgAttachHdlsVirtualDevice(&controllerHandle, &controllerDevice);
    if (R_FAILED(myResult))
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Failed connecting controller, attach rc=0x%x", (unsigned)myResult);
        printToFile(msg);
        return -1;
    }

    printToFile("Controller initialized!");
    isInitialized = true;
    return 0;
}

int FakeController::deInitialize()
{
    if (!isInitialized)
        return 0;
    Result myResult;

    controllerState = {0};
    hiddbgSetHdlsState(controllerHandle, &controllerState);

    myResult = hiddbgDetachHdlsVirtualDevice(controllerHandle);
    if (R_FAILED(myResult))
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Fatal Error while detaching controller, rc=0x%x", (unsigned)myResult);
        printToFile(msg);
    }
    controllerHandle = {0};
    controllerDevice = {0};

    isInitialized = false;

    return 0;
}

std::array<FakeController, 8> fakeControllerList;
u64 buttonPresses;

void apply_fake_con_state(struct input_message message)
{
    // Check if the magic is correct
    if (message.magic != INPUT_MSG_MAGIC)
        return;

    u16 conType;
    u64 keys;
    s32 joylx;
    s32 joyly;
    s32 joyrx;
    s32 joyry;

    for (s32 i = 0; i < message.con_count; i++)
    {
        switch (i)
        {
        case 0:
            conType = message.con_type;
            keys = message.keys;
            joylx = message.joy_l_x;
            joyly = message.joy_l_y;
            joyrx = message.joy_r_x;
            joyry = message.joy_r_y;
            break;
        case 1:
            conType = message.con_type2;
            keys = message.keys2;
            joylx = message.joy_l_x2;
            joyly = message.joy_l_y2;
            joyrx = message.joy_r_x2;
            joyry = message.joy_r_y2;
            break;
        case 2:
            conType = message.con_type3;
            keys = message.keys3;
            joylx = message.joy_l_x3;
            joyly = message.joy_l_y3;
            joyrx = message.joy_r_x3;
            joyry = message.joy_r_y3;
            break;
        case 3:
            conType = message.con_type4;
            keys = message.keys4;
            joylx = message.joy_l_x4;
            joyly = message.joy_l_y4;
            joyrx = message.joy_r_x4;
            joyry = message.joy_r_y4;
            break;
        case 4:
            conType = message.con_type5;
            keys = message.keys5;
            joylx = message.joy_l_x5;
            joyly = message.joy_l_y5;
            joyrx = message.joy_r_x5;
            joyry = message.joy_r_y5;
            break;
        case 5:
            conType = message.con_type6;
            keys = message.keys6;
            joylx = message.joy_l_x6;
            joyly = message.joy_l_y6;
            joyrx = message.joy_r_x6;
            joyry = message.joy_r_y6;
            break;
        case 6:
            conType = message.con_type7;
            keys = message.keys7;
            joylx = message.joy_l_x7;
            joyly = message.joy_l_y7;
            joyrx = message.joy_r_x7;
            joyry = message.joy_r_y7;
            break;
        case 7:
            conType = message.con_type8;
            keys = message.keys8;
            joylx = message.joy_l_x8;
            joyly = message.joy_l_y8;
            joyrx = message.joy_r_x8;
            joyry = message.joy_r_y8;
            break;
        }

        // If there is no controller connected, we have to initialize one
        if (!fakeControllerList[i].isInitialized && (conType > 0 && conType < 4))
        {
            fakeControllerList[i].initialize(conType);
        }
        // If there is a controller connected, but we changed the controller type to a non-existant one, we'll
        // disconnect it
        else if (fakeControllerList[i].isInitialized && (conType < 1 || conType > 3))
        {
            fakeControllerList[i].deInitialize();
            /*FakeController tempCon;
            fakeControllerList[i] = tempCon;*/
        }

        if (fakeControllerList[i].isInitialized)
        {
            FakeController &fc = fakeControllerList[i];
            fc.controllerState.buttons = keys;
            fc.controllerState.analog_stick_l.x = joylx;
            fc.controllerState.analog_stick_l.y = joyly;
            fc.controllerState.analog_stick_r.x = joyrx;
            fc.controllerState.analog_stick_r.y = joyry;
            Result myResult;
            // This function is causing all the issues in 12.0
            myResult = hiddbgSetHdlsState(fc.controllerHandle, &fc.controllerState);
            if (R_FAILED(myResult))
            {
                // the Switch can drop a virtual pad (observed with the Lite's own controls in use
                // alongside it); every later update then failed until the client disconnected, because the pad
                // stayed marked attached. Log the real result code and re-attach with back-off.
                fc.updateFailures++;
                if (fc.updateFailures == 1 || (fc.updateFailures % 64) == 0)
                {
                    char msg[120];
                    snprintf(
                        msg, sizeof(msg),
                        "Fatal Error while updating Controller State, slot=%d rc=0x%x handle=%x failures=%u reattaches=%u",
                        (int)i, (unsigned)myResult, (unsigned)fc.controllerHandle.handle, (unsigned)fc.updateFailures,
                        (unsigned)fc.reattachCount);
                    printToFile(msg);
                }
                const u64 nowNs = armTicksToNs(svcGetSystemTick());
                if (nowNs - fc.lastReattachNs >= 500000000ull)
                {
                    fc.lastReattachNs = nowNs;
                    fc.reattachCount++;
                    printToFile("Re-attaching virtual controller after failed update");
                    fc.deInitialize();
                    if (fc.initialize(conType) == 0)
                    {
                        fc.controllerState.buttons = keys;
                        fc.controllerState.analog_stick_l.x = joylx;
                        fc.controllerState.analog_stick_l.y = joyly;
                        fc.controllerState.analog_stick_r.x = joyrx;
                        fc.controllerState.analog_stick_r.y = joyry;
                        myResult = hiddbgSetHdlsState(fc.controllerHandle, &fc.controllerState);
                        if (R_SUCCEEDED(myResult))
                        {
                            char msg[96];
                            snprintf(msg, sizeof(msg), "Controller recovered after re-attach, failures=%u",
                                     (unsigned)fc.updateFailures);
                            printToFile(msg);
                        }
                    }
                }
            }
            else if (fc.updateFailures != 0)
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "Controller updates OK again after %u failures",
                         (unsigned)fc.updateFailures);
                printToFile(msg);
                fc.updateFailures = 0;
            }
        }
    }

    return;
}

// ---- Lite built-in controls mirrored to a virtual pad  --------------------------------
// A Switch Lite hands its built-in controls only to "handheld" input, and many games ignore handheld
// input once any external controller is connected, so the Lite's own buttons stop working as soon as a
// remote pad is attached. While a remote pad is attached we therefore read the handheld controls and
// re-send them through a second virtual Pro Controller, so a game sees two ordinary pads.
// Technique credit: PhantomCtrl by KazekiKitten (https://github.com/KazekiKitten/PhantomCrtl, AGPL-3.0).
// This is our own implementation of the same idea from the public libnx API; none of its code is used.
//
// Player numbers follow attach order (the probe lines in the log show the real numbers).
// AUTOMATIC ORDER (default, no /config/nxremoteplay/players.txt or "order=auto"): like a docked console, whoever
// is first gets Player 1. When a remote pad becomes active, if the Lite's own controls were used in the last
// LITE_RECENT_NS the Lite is Player 1 and the remote Player 2; otherwise the remote is Player 1 and the Lite
// joins as Player 2 the first time its controls are touched (so an idle Lite never takes a player slot).
// FIXED ORDER: players.txt with "remote=N lite=N" (1-4; "lite=0" = mirror off) keeps the earlier fixed behaviour:
// both pads attach at once in slot order, a slot nobody uses gets a neutral placeholder pad.
struct PadEntry
{
    u16 type;
    u64 keys;
    s32 lx, ly, rx, ry;
};

#define LITE_RECENT_NS 30000000000ull      // Lite counted as "in use" this long after its last input
#define LITE_STICK_DEADZONE 8000

static Mutex pkgMutex;
static struct input_message fakeConsState;

static bool g_auto = true;
static int g_liteSlot = 2;                    // fixed mode: 1-4, 0 = mirror disabled
static int g_remoteSlot = 1;                  // fixed mode: player place of remote slot 0 (1-4)
static volatile u64 g_liteLastActiveTick = 0; // 0 = never
//  (multi-viewer): up to REMOTE_SLOTS remote sources at once (slot 0 = the first transport / hosting
// desktop, 1..3 = extra viewers), each with its own virtual pad. g_remotePos[slot] is that pad's player place 1-4
// (0 = none free). Places are kept while a pad exists and freed when its source leaves, like controllers on a console;
// the Switch numbers players by attach order, so the places only decide which pad index a source drives.
#define REMOTE_SLOTS 4
#define PLAYER_PLACES 4
static PadEntry g_remotes[REMOTE_SLOTS]; // type 0 = slot has no pad attached
static int g_remotePos[REMOTE_SLOTS];
// HOME works no matter which player asks. A running single-player game only accepts pad 1 (a viewer's
// own pad is rejected with rc 0x1c24ca), so HOME is never sent on the asking viewer's own pad: the HOME bit is stripped
// from every remote pad and applied to the pad at player place 1 instead.
#define PAD_HOME_BIT (1ull << 18)                  // HDLS HOME (HiddbgNpadButton_Home)
#define HOME_STANDIN_SETTLE_NS 500000000ull        // a HOME set in the instant a pad attaches is lost (hardware, )
static bool g_homeReq[REMOTE_SLOTS]; // remote slots that currently hold HOME
static bool g_standIn = false;       // place 1 was empty: a neutral stand-in pad carries HOME instead
static u64 g_standInTick = 0;
static bool g_standInHomeOn = false;
static int g_litePos = 0;          // auto order: player place of the Lite's pad once it joined, 0 = not on a pad
static bool g_liteNoPlace = false; // auto order: the Lite wanted a place but all four were taken (logged once)
static PadEntry g_lite = {1, 0, 0, 0, 0, 0};
static bool g_mirrorReady = false; // handheld reading initialised
static bool g_mirrorThreadStarted = false;
static u32 g_planCount = 0; // pads currently attached by the plan
static bool g_styleLogPending = false;
static u64 g_planAppliedTick = 0;
static bool g_probeRemoteDone = false;
static bool g_probeLiteDone = false;
static u64 g_remoteKeysSince = 0, g_liteKeysSince = 0;

// NO AUTO REGISTRATION in Change Grip/Order. While the system Controller applet (program
// 0100000000001003, HOME > Controllers > Change Grip/Order, also opened by games) is running, no virtual pad is
// attached on its own: every pad is detached when the applet opens and a viewer's pad attaches only when that viewer
// presses a button (a button already held when the applet opened must be released first), taking the lowest free player
// place in press order. The Lite's own-control mirror pad stays off meanwhile (the applet reads the real Lite). When
// the applet closes, viewers that never pressed are attached in slot order.
#define CONTROLLER_APPLET_TID 0x0100000000001003ull
#define ORDER_POLL_NS 250000000ull
#define PAD_PRESS_MASK 0xFFFFull                   // A..D-pad; HOME is stripped and sticks can drift
static bool g_orderMode = false;
static bool g_liteArmed = false;        // order mode: the Lite's buttons were seen released since the applet opened
static bool g_orderArmed[REMOTE_SLOTS]; // order mode: slot's buttons were seen released since the applet opened
static bool g_pmReady = false;
static bool g_pmTried = false;
static u64 g_orderPollTick = 0;

static void EnsureOrderWatch()
{
    if (g_pmTried)
        return;
    g_pmTried = true;
    const Result rc = pmdmntInitialize();
    g_pmReady = R_SUCCEEDED(rc);
    char msg[80];
    snprintf(msg, sizeof(msg), "LocalMirror: controller-applet watch pm:dmnt rc=0x%x", (unsigned)rc);
    printToFile(msg);
}

static bool ControllerAppletOpen()
{
    if (!g_pmReady)
        return false;
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, CONTROLLER_APPLET_TID));
}

static Thread g_mirrorThread;
alignas(0x1000) static u8 g_mirrorStack[0x2000];

static void MirrorThread(void *);

static void StartMirrorThread()
{
    if (g_mirrorThreadStarted)
        return;
    g_mirrorThreadStarted = true;
    Result rc = threadCreate(&g_mirrorThread, MirrorThread, NULL, g_mirrorStack, sizeof(g_mirrorStack), 0x30, 3);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_mirrorThread);
    char msg[64];
    snprintf(msg, sizeof(msg), "LocalMirror: thread start rc=0x%x", (unsigned)rc);
    printToFile(msg);
}

// liteSlot/remoteSlot < 0 on both = automatic order.
extern "C" void LocalMirror_Configure(int liteSlot, int remoteSlot)
{
    char msg[96];
    if (liteSlot < 0 && remoteSlot < 0)
    {
        g_auto = true;
        printToFile("LocalMirror: config order=auto (first to press is Player 1)");
        StartMirrorThread();
        return;
    }
    g_auto = false;
    if (remoteSlot < 1 || remoteSlot > 4)
        remoteSlot = 1;
    if (liteSlot < 0 || liteSlot > 4)
        liteSlot = 2;
    if (liteSlot == remoteSlot)
        liteSlot = (remoteSlot == 1) ? 2 : 1;
    g_remoteSlot = remoteSlot;
    g_liteSlot = liteSlot;
    snprintf(msg, sizeof(msg), "LocalMirror: config fixed remote=P%d lite=%d (0 = off)", remoteSlot, liteSlot);
    printToFile(msg);
}

static void SetEntry(struct input_message &m, int i, const PadEntry &e)
{
    switch (i)
    {
    case 0:
        m.con_type = e.type;
        m.keys = e.keys;
        m.joy_l_x = e.lx;
        m.joy_l_y = e.ly;
        m.joy_r_x = e.rx;
        m.joy_r_y = e.ry;
        break;
    case 1:
        m.con_type2 = e.type;
        m.keys2 = e.keys;
        m.joy_l_x2 = e.lx;
        m.joy_l_y2 = e.ly;
        m.joy_r_x2 = e.rx;
        m.joy_r_y2 = e.ry;
        break;
    case 2:
        m.con_type3 = e.type;
        m.keys3 = e.keys;
        m.joy_l_x3 = e.lx;
        m.joy_l_y3 = e.ly;
        m.joy_r_x3 = e.rx;
        m.joy_r_y3 = e.ry;
        break;
    case 3:
        m.con_type4 = e.type;
        m.keys4 = e.keys;
        m.joy_l_x4 = e.lx;
        m.joy_l_y4 = e.ly;
        m.joy_r_x4 = e.rx;
        m.joy_r_y4 = e.ry;
        break;
    }
}

static bool AnyRemoteOn()
{
    for (int i = 0; i < REMOTE_SLOTS; i++)
        if (g_remotes[i].type != 0)
            return true;
    return false;
}

// Player place of the Lite's mirrored pad, 0 = it has none (mirror off, no remote, or not joined yet).
static int LitePlace()
{
    if (!AnyRemoteOn() || !g_mirrorReady)
        return 0;
    // while the Controller applet is open the Lite is a participant like a viewer: its mirror pad is off
    // until its own buttons are pressed (see MirrorThread), then g_litePos holds the place it took. (Fixed order keeps
    // the old behaviour: off meanwhile.)
    if (g_orderMode)
        return g_auto ? g_litePos : 0;
    return g_auto ? g_litePos : g_liteSlot;
}

static bool PlaceTaken(int place, int exceptRemote)
{
    for (int i = 0; i < REMOTE_SLOTS; i++)
        if (i != exceptRemote && g_remotes[i].type != 0 && g_remotePos[i] == place)
            return true;
    if (LitePlace() == place)
        return true;
    // fixed order reserves the configured place for remote slot 0 even while that slot is idle
    if (!g_auto && !g_orderMode && exceptRemote != 0 && place == g_remoteSlot)
        return true;
    return false;
}

static int LowestFreePlace(int exceptRemote)
{
    for (int place = 1; place <= PLAYER_PLACES; place++)
        if (!PlaceTaken(place, exceptRemote))
            return place;
    return 0;
}

static void ApplyPlanLocked();

// pkgMutex must be held. Enters/leaves order mode (see the comment above CONTROLLER_APPLET_TID).
static void SetOrderModeLocked(bool on)
{
    if (on == g_orderMode)
        return;
    g_orderMode = on;
    if (on)
    {
        for (int slot = 0; slot < REMOTE_SLOTS; slot++)
        {
            g_remotePos[slot] = 0;
            g_orderArmed[slot] =
                (g_remotes[slot].keys & PAD_PRESS_MASK) == 0; // armed unless a button is held right now
        }
        g_litePos = 0;
        g_liteNoPlace = false;
        g_liteArmed = false;
        g_standIn = false;
        g_standInHomeOn = false;
        printToFile("LocalMirror: Controller applet open - virtual pads detached until each viewer presses a button");
    }
    else
    {
        for (int slot = 0; slot < REMOTE_SLOTS; slot++)
            if (g_remotes[slot].type != 0 && g_remotePos[slot] == 0)
                g_remotePos[slot] = (!g_auto && slot == 0) ? g_remoteSlot : LowestFreePlace(slot);
        printToFile("LocalMirror: Controller applet closed - pads that did not press are attached in slot order");
    }
    ApplyPlanLocked();
}

// pkgMutex must be held.
static void ApplyPlanLocked()
{
    struct input_message m;
    memset(&m, 0, sizeof(m));
    m.magic = INPUT_MSG_MAGIC;

    const bool remoteOn = AnyRemoteOn();
    const int litePlace = LitePlace();
    const bool liteOn = litePlace > 0;
    int count;
    if (!remoteOn)
    {
        // every entry stays type 0, which detaches whatever the plan had attached
        count = g_planCount ? (int)g_planCount : 1;
        g_standIn = false;
        g_standInHomeOn = false;
    }
    else
    {
        count = 0;
        for (int slot = 0; slot < REMOTE_SLOTS; slot++)
            if (g_remotes[slot].type != 0 && g_remotePos[slot] > count)
                count = g_remotePos[slot];
        if (liteOn && litePlace > count)
            count = litePlace;
        bool homeAny = false;
        for (int slot = 0; slot < REMOTE_SLOTS; slot++)
            if (g_remotes[slot].type != 0 && g_homeReq[slot])
                homeAny = true;
        for (int place = 1; place <= count; place++)
        {
            // fixed order keeps the neutral placeholder for a place nobody uses; automatic order simply
            // leaves it empty (type 0), so a controller that left stays gone until someone takes its place
            PadEntry e = g_auto ? PadEntry{0, 0, 0, 0, 0, 0} : PadEntry{1, 0, 0, 0, 0, 0};
            for (int slot = 0; slot < REMOTE_SLOTS; slot++)
                if (g_remotes[slot].type != 0 && g_remotePos[slot] == place)
                    e = g_remotes[slot];
            if (liteOn && place == litePlace)
                e = g_lite;
            if (place == 1)
            {
                if (!homeAny)
                {
                    g_standIn = false;
                    g_standInHomeOn = false;
                }
                else if (e.type != 0)
                {
                    e.keys |= PAD_HOME_BIT;
                    g_standIn = false;
                    g_standInHomeOn = false;
                }
                else
                {
                    // place 1 is empty (its source left): attach a neutral stand-in first, HOME follows once it has
                    // settled (the mirror thread re-plans when the settle time has passed)
                    e = PadEntry{1, 0, 0, 0, 0, 0};
                    const u64 now = svcGetSystemTick();
                    if (!g_standIn)
                    {
                        g_standIn = true;
                        g_standInTick = now;
                        g_standInHomeOn = false;
                    }
                    if (armTicksToNs(now - g_standInTick) >= HOME_STANDIN_SETTLE_NS)
                    {
                        e.keys |= PAD_HOME_BIT;
                        g_standInHomeOn = true;
                    }
                }
            }
            SetEntry(m, place - 1, e);
        }
        // pads above the new highest place (a viewer with a high place left) must still be visited to detach
        if ((int)g_planCount > count)
            count = (int)g_planCount;
    }
    m.con_count = (u16)count;
    apply_fake_con_state(m);

    if (remoteOn && g_planCount == 0)
    {
        g_styleLogPending = true;
        g_planAppliedTick = svcGetSystemTick();
    }
    if (remoteOn)
    {
        int highest = 0;
        for (int slot = 0; slot < REMOTE_SLOTS; slot++)
            if (g_remotes[slot].type != 0 && g_remotePos[slot] > highest)
                highest = g_remotePos[slot];
        if (liteOn && litePlace > highest)
            highest = litePlace;
        g_planCount = (u32)highest;
    }
    else
        g_planCount = 0;
}

// Logs which player number (npad No1-No4) shows the given buttons: tells us how the Switch numbered the pads.
static void ProbeNumbers(const char *who, u64 keys)
{
    u64 seen[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++)
    {
        HidNpadFullKeyState st;
        if (hidGetNpadStatesFullKey((HidNpadIdType)(HidNpadIdType_No1 + i), &st, 1) > 0)
            seen[i] = st.buttons & 0xFFFFull;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "LocalMirror: probe %s sent buttons=%x; seen No1=%x No2=%x No3=%x No4=%x", who,
             (unsigned)keys, (unsigned)seen[0], (unsigned)seen[1], (unsigned)seen[2], (unsigned)seen[3]);
    printToFile(msg);
}

static void MirrorThread(void *)
{
    hidInitializeNpad();
    // An earlier, narrower registration (5 ids, Handheld|FullKey only) was tried against a 30 s system stall on one console;
    // the stall had another cause, so every standard style is registered again:
    // All standard styles: Pro, Handheld, paired Joy-Cons, and individual Joy-Cons, on all 8 players + handheld.
    const HidNpadIdType ids[] = {HidNpadIdType_Handheld,
                                 HidNpadIdType_No1,
                                 HidNpadIdType_No2,
                                 HidNpadIdType_No3,
                                 HidNpadIdType_No4,
                                 static_cast<HidNpadIdType>(4),
                                 static_cast<HidNpadIdType>(5),
                                 static_cast<HidNpadIdType>(6),
                                 static_cast<HidNpadIdType>(7)};
    Result rc1 = hidSetSupportedNpadIdType(ids, 9);
    // SystemExt is the style Atmosphere reads held buttons with when it decides on a launch override.
    Result rc2 = hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt);
    char msg[160];
    snprintf(msg, sizeof(msg), "LocalMirror: npad init supportedIds rc=0x%x styleSet rc=0x%x", (unsigned)rc1,
             (unsigned)rc2);
    printToFile(msg);

    bool loggedFirstRead = false;
    bool wasRemoteOn = false;
    u64 lastRemoteRefreshTick = 0;
    mutexLock(&pkgMutex);
    g_mirrorReady = true;
    mutexUnlock(&pkgMutex);

    EnsureOrderWatch();

    while (true)
    {
        const u64 pollNow = svcGetSystemTick();
        if (g_pmReady && (g_orderPollTick == 0 || armTicksToNs(pollNow - g_orderPollTick) >= ORDER_POLL_NS))
        {
            g_orderPollTick = pollNow;
            const bool appletOpen = ControllerAppletOpen();
            mutexLock(&pkgMutex);
            SetOrderModeLocked(appletOpen);
            mutexUnlock(&pkgMutex);
        }
        const bool remoteOn = AnyRemoteOn();
        if (!g_auto && !remoteOn)
        {
            svcSleepThread(100000000ull); // fixed order, nobody controlling remotely: nothing to do
            continue;
        }
        // automatic order keeps watching the Lite's controls at a low rate so it knows who is "first"
        svcSleepThread(remoteOn ? 5000000ull : 50000000ull);

        HidNpadHandheldState st = {};
        size_t n = hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &st, 1);
        // remote maintenance must run even on a regular/docked Switch,
        // which need not have any Handheld samples. Missing samples are not
        // synthetic releases of the Lite's previously observed controls.
        if (n > 0 && !loggedFirstRead)
        {
            loggedFirstRead = true;
            printToFile("LocalMirror: first handheld state read");
        }

        mutexLock(&pkgMutex);
        PadEntry e = g_lite;
        mutexUnlock(&pkgMutex);
        if (n > 0)
        {
            e.type = 1;
            e.keys = st.buttons & 0xFFFFull; // A..D-pad only; HDLS bits 18/19 are HOME/CAPTURE, not stick directions
            e.lx = st.analog_stick_l.x;
            e.ly = st.analog_stick_l.y;
            e.rx = st.analog_stick_r.x;
            e.ry = st.analog_stick_r.y;
        }
        const bool inUse =
            n > 0 && (e.keys != 0 || e.lx > LITE_STICK_DEADZONE || e.lx < -LITE_STICK_DEADZONE ||
                      e.ly > LITE_STICK_DEADZONE || e.ly < -LITE_STICK_DEADZONE || e.rx > LITE_STICK_DEADZONE ||
                      e.rx < -LITE_STICK_DEADZONE || e.ry > LITE_STICK_DEADZONE || e.ry < -LITE_STICK_DEADZONE);
        const u64 now = svcGetSystemTick();
        if (inUse)
            g_liteLastActiveTick = now;

        if (!remoteOn)
        {
            wasRemoteOn = false;
            lastRemoteRefreshTick = 0;
            continue;
        }

        mutexLock(&pkgMutex);
        if (AnyRemoteOn())
        {
            if (!wasRemoteOn)
            {
                wasRemoteOn = true;
                g_probeRemoteDone = g_probeLiteDone = false;
            }
            bool changed = false;
            if (g_auto && g_orderMode && g_litePos == 0 && !g_liteNoPlace && n > 0)
            {
                // order mode: the Lite registers by pressing a button (after releasing whatever it held when the applet
                // opened), taking the lowest free place in press order, exactly like a viewer
                if (e.keys == 0)
                    g_liteArmed = true;
                else if (g_liteArmed)
                {
                    const int place = LowestFreePlace(-1);
                    if (place > 0)
                    {
                        g_litePos = place;
                        changed = true;
                        snprintf(msg, sizeof(msg),
                                 "LocalMirror: order mode: Lite pressed a button, takes player place P%d", place);
                        printToFile(msg);
                    }
                    else
                    {
                        g_liteNoPlace = true;
                        printToFile(
                            "LocalMirror: order mode: Lite pressed a button but all four player places are taken");
                    }
                }
            }
            if (g_auto && !g_orderMode && g_litePos == 0 && !g_liteNoPlace && inUse)
            {
                // the Lite's controls were touched after a remote joined: it takes the lowest free player place
                const int place = LowestFreePlace(-1);
                if (place > 0)
                {
                    g_litePos = place;
                    changed = true;
                    snprintf(msg, sizeof(msg), "LocalMirror: Lite controls joined as P%d", place);
                    printToFile(msg);
                }
                else
                {
                    g_liteNoPlace = true;
                    printToFile("LocalMirror: Lite controls not mirrored - all four player places are taken");
                }
            }
            if (e.keys != g_lite.keys || e.lx != g_lite.lx || e.ly != g_lite.ly || e.rx != g_lite.rx ||
                e.ry != g_lite.ry)
            {
                g_lite = e;
                changed = true;
            }
            if (g_standIn && !g_standInHomeOn && armTicksToNs(now - g_standInTick) >= HOME_STANDIN_SETTLE_NS)
                changed = true; // the neutral pad 1 stand-in has settled: re-plan so HOME follows it
            // Keep active virtual pads serviced independently of handheld
            // changes. Use the canonical plan (including releases/HOME), never
            // a saved packet that could resurrect an older button state.
            if (changed || lastRemoteRefreshTick == 0 || armTicksToNs(now - lastRemoteRefreshTick) >= 16000000ull)
            {
                ApplyPlanLocked();
                lastRemoteRefreshTick = now;
            }

            // probe: once a button has been held a moment, log which player number (No1-No4) shows it
            u64 rk = 0;
            for (int slot = 0; slot < REMOTE_SLOTS; slot++)
                if (g_remotes[slot].type != 0)
                {
                    rk = g_remotes[slot].keys & 0xFFFFull;
                    break;
                }
            if (rk == 0)
                g_remoteKeysSince = 0;
            else if (!g_remoteKeysSince)
                g_remoteKeysSince = now;
            if (e.keys == 0 || LitePlace() == 0)
                g_liteKeysSince = 0;
            else if (!g_liteKeysSince)
                g_liteKeysSince = now;
            if (!g_probeRemoteDone && g_remoteKeysSince && armTicksToNs(now - g_remoteKeysSince) > 40000000ull)
            {
                g_probeRemoteDone = true;
                ProbeNumbers("remote", rk);
            }
            if (!g_probeLiteDone && g_liteKeysSince && armTicksToNs(now - g_liteKeysSince) > 40000000ull)
            {
                g_probeLiteDone = true;
                ProbeNumbers("lite", e.keys);
            }
            if (g_styleLogPending && armTicksToNs(svcGetSystemTick() - g_planAppliedTick) > 1500000000ull)
            {
                g_styleLogPending = false;
                snprintf(
                    msg, sizeof(msg), "LocalMirror: npad styles No1=%x No2=%x No3=%x No4=%x Handheld=%x",
                    (unsigned)hidGetNpadStyleSet(HidNpadIdType_No1), (unsigned)hidGetNpadStyleSet(HidNpadIdType_No2),
                    (unsigned)hidGetNpadStyleSet(HidNpadIdType_No3), (unsigned)hidGetNpadStyleSet(HidNpadIdType_No4),
                    (unsigned)hidGetNpadStyleSet(HidNpadIdType_Handheld));
                printToFile(msg);
            }
        }
        mutexUnlock(&pkgMutex);
    }
}

// Thread-safe entry point for feeding a REMOTE source's pad state in from somewhere other than
// networkThread's own polling loop below - used by ClientInput.c (the punched/USB clients' controller
// packets). con_type 0 detaches that source's pad. `slot` 0 is the first transport (and what the older single-pad
// callers mean); 1..3 are additional remote viewers , each with its own virtual pad. While any remote pad
// exists the Lite's own controls are mirrored to a further pad (see above).
extern "C" void ApplyRelayHidPlusStateSlot(int slot, struct input_message message)
{
    if (slot < 0 || slot >= REMOTE_SLOTS)
        return;
    mutexLock(&pkgMutex);
    if (message.magic == INPUT_MSG_MAGIC && message.con_count >= 1)
    {
        const bool wasOnSlot = g_remotes[slot].type != 0;
        const bool anyBefore = AnyRemoteOn();
        char msg[96];
        if (message.con_type >= 1 && message.con_type <= 3)
        {
            g_remotes[slot].type = message.con_type;
            g_homeReq[slot] = (message.keys & PAD_HOME_BIT) != 0;
            g_remotes[slot].keys =
                message.keys & ~PAD_HOME_BIT; // HOME rides on the pad at player place 1 (see ApplyPlanLocked)
            g_remotes[slot].lx = message.joy_l_x;
            g_remotes[slot].ly = message.joy_l_y;
            g_remotes[slot].rx = message.joy_r_x;
            g_remotes[slot].ry = message.joy_r_y;
            if (!wasOnSlot)
            {
                EnsureOrderWatch();
                SetOrderModeLocked(ControllerAppletOpen());
                if (g_orderMode)
                {
                    g_remotePos[slot] = 0; // registers only when this viewer presses a button
                    g_orderArmed[slot] = false;
                }
                else if (!anyBefore && g_auto)
                {
                    // who is first? the Lite, if its controls were used recently
                    const u64 last = g_liteLastActiveTick;
                    const bool liteFirst =
                        g_mirrorReady && last != 0 && armTicksToNs(svcGetSystemTick() - last) < LITE_RECENT_NS;
                    g_litePos = liteFirst ? 1 : 0;
                    g_liteNoPlace = false;
                    g_remotePos[slot] = liteFirst ? 2 : 1;
                    printToFile(liteFirst ? "LocalMirror: auto order Lite=P1 remote=P2"
                                          : "LocalMirror: auto order remote=P1, Lite joins as P2 when used");
                }
                else if (!g_auto && slot == 0)
                {
                    g_remotePos[0] = g_remoteSlot; // fixed order keeps the configured place for the first transport
                }
                else
                {
                    g_remotePos[slot] = LowestFreePlace(slot);
                    snprintf(msg, sizeof(msg), "LocalMirror: remote slot %d takes player place P%d%s", slot,
                             g_remotePos[slot], g_remotePos[slot] == 0 ? " (none free - no pad)" : "");
                    printToFile(msg);
                }
            }
            if (g_orderMode && g_remotePos[slot] == 0)
            {
                // order mode: a viewer registers by pressing a button, after having released whatever it held at applet
                // start
                const bool pressed = (message.keys & PAD_PRESS_MASK) != 0;
                if (!pressed)
                    g_orderArmed[slot] = true;
                else if (g_orderArmed[slot])
                {
                    g_remotePos[slot] = LowestFreePlace(slot);
                    snprintf(msg, sizeof(msg),
                             "LocalMirror: order mode: remote slot %d pressed a button, takes player place P%d%s", slot,
                             g_remotePos[slot], g_remotePos[slot] == 0 ? " (none free - no pad)" : "");
                    printToFile(msg);
                    if (g_remotePos[slot] == 0)
                        g_orderArmed[slot] = false; // log once per press when all four places are taken
                }
            }
        }
        else
        {
            g_remotes[slot].type = 0;
            g_homeReq[slot] = false;
            g_remotePos[slot] = 0;
            if (!AnyRemoteOn())
            {
                g_litePos = 0;
                g_liteNoPlace = false;
            }
            if (wasOnSlot)
            {
                snprintf(msg, sizeof(msg), "LocalMirror: remote slot %d released its pad", slot);
                printToFile(msg);
            }
        }
        ApplyPlanLocked();

        // this worker also maintains remote pads when local mirroring is off.
        if (AnyRemoteOn())
            StartMirrorThread();
    }
    mutexUnlock(&pkgMutex);
}

void networkThread(void *_)
{
    struct input_message temporal_pkg;
    printToFile("Starting Network Loop Thread!");
    while (true)
    {
        int poll_res = poll_udp_input(&temporal_pkg);
        mutexLock(&pkgMutex);

        if (poll_res == 0)
        {
            fakeConsState = temporal_pkg;
            apply_fake_con_state(fakeConsState);
        }
        else
        {
            fakeConsState.magic = 0;
            svcSleepThread(1e+7l);
        }
        mutexUnlock(&pkgMutex);

        svcSleepThread(-1);
    }
}
