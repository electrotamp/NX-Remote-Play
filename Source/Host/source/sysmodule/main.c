#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <switch.h>
#include <string.h>

// Platform startup hooks for the capture and transport core.

#include "../core.h"
#include "../modes/modes.h"
#include "../capture.h"
#include "../nxrp_video.h"
#include "unified_entry.h"

// Buffers are static; allocator hooks share libstratosphere's heap with C dependencies.
#include "../ipc/ipc.h"

#include "../net/sockets.h"

static Result fsInitResult;
static FsFileSystem sdCard;

// __nx_applet_type and __nx_fs_num_sessions: now defined by libstratosphere's own shim
// (see the note above __libnx_initheap used to be here).
u32 __nx_fsdev_direntry_cache_size = 1;

// Static work buffer for hiddbg's virtual controller session (see the hiddbgAttachHdlsWorkBuffer
// call below) - statically allocated because this build runs with the heap disabled.
static HiddbgHdlsSessionId g_hidplusSessionId;
static u8 alignas(0x1000) g_hidplusWorkMem[0x1000];

// Renamed from the libnx-startup-invoked __appInit: libstratosphere's own shim
// (init_libnx_shim.os.horizon.cpp) already defines __appInit/__appExit/main itself and
// calls ams::init::InitializeSystemModule()/ams::Main()/ams::init::FinalizeSystemModule()
// in their place - unified_entry.cpp calls these three renamed functions from there.
// Bodies are otherwise unchanged from the original sysmodule.
void NxRemotePlay_AppInit(void)
{
    //  (35): removed. This was an unconditional, undocumented 20-second sleep at
    // the very start of boot2, present in both this tree's source and the real TCP transport/
    // capture research trees it was copied from, with no comment anywhere explaining what race or
    // resource it was waiting out. The ~2-minute boot this caused (stacked with
    // NvencProbe's own added work) is why the real-frame-capture work tonight (HOME not yet
    // presenting, or already idle, by the time our probes ran - entries (30)/(34)) wasn't
    // working. Scoped to nxremoteplay only - the real TCP transport/capture research sources this
    // was copied from are untouched, per this tree's own standing isolation rule.

    Result rc;

    // LOG() (with FILE_LOGGING) is a raw printf() to stdout - stdout is only
    // redirected to /nxremoteplay_log.txt inside CoreInit() below (see core.c's
    // freopen call). Calling LOG() any earlier than that writes to an
    // unbound stdout on a console-less sysmodule, which crashes inside
    // newlib's vfprintf. So nothing before CoreInit() returns may LOG().
    rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc))
    {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));

        setsysExit();
    }

    if (R_FAILED(rc))
        fatalThrow(ERR_INIT_FAILED);

    rc = CoreInit();
    if (R_FAILED(rc))
        fatalThrow(rc);

    // Ignore erorrs, at most we won't be able to read the default mode
    // We use this directly because we don't want to depend on fsdev which requires malloc
    fsInitResult = fsOpenSdCardFileSystem(&sdCard);

    // From here down: services needed by the merged-in sys-hidplus input forwarding
    // (see source/hidplus). This runs a separate UDP-driven thread that injects a
    // virtual controller via hiddbg - it does not touch NxRemotePlay's own capture/streaming
    // code at all, it's just sharing the process. Socket init is NOT repeated here,
    // sys-hidplus's raw BSD socket calls work fine against the session NxRemotePlay's own
    // SocketInit() (called from CoreInit above) already opened.
    // fatalThrow() shows whatever Result it's given directly on the crash screen (as
    // module/description), with no register or backtrace capture (it's a deliberate
    // call, not a real CPU exception) - so throwing our own placeholder code here would
    // make every failure in this block indistinguishable on the crash screen. Throwing
    // the real rc instead makes the crash screen itself identify which call failed.
    // Optional controller and screenshot-service initialization must fail without taking
    // down video/audio streaming. Log the result and disable the unavailable feature.
    rc = hidInitialize();

    rc = hiddbgInitialize();

    // hiddbgAttachHdlsWorkBuffer needs a work buffer as of libnx's 13.0.0+ API (older
    // sys-hidplus builds used a zero-argument version of this call). NxRemotePlay's build
    // disables the heap entirely (see USE_HEAP above / __libnx_aligned_alloc), so this
    // must be a statically allocated buffer rather than aligned_alloc, matching the
    // pattern NxRemotePlay's own code already uses for its stream stack areas below.
    rc = hiddbgAttachHdlsWorkBuffer(&g_hidplusSessionId, g_hidplusWorkMem, sizeof(g_hidplusWorkMem));

    // Keep the screenshot service available for final-visible capture.
    rc = capsscInitialize();

}

void NxRemotePlay_AppExit(void)
{

    capsscExit();

    // hiddbgAttachHdlsWorkBuffer() in appInit claims a singleton session from the hid
    // sysmodule - per devkitPro's own hdls example, hiddbgReleaseHdlsWorkBuffer()/
    // hiddbgExit() "must be run at some point before exiting". We weren't calling
    // either, so every exit (even a clean one) left that session claimed until reboot,
    // which made every subsequent launch attempt fail at hiddbgAttachHdlsWorkBuffer().
    Result hidplusRc = hiddbgReleaseHdlsWorkBuffer(g_hidplusSessionId);
    (void)hidplusRc;
    hiddbgExit();

    hidExit();
    SocketDeinit();
    if (R_SUCCEEDED(fsInitResult))
        fsFsClose(&sdCard);
    fsExit();
    smExit();
}

// Player order for the remote pad and the Lite's mirrored built-in controls: automatic
// (whoever presses first is Player 1). See hidplus/con_manager.cpp.
extern void LocalMirror_Configure(int liteSlot, int remoteSlot);

// Session pairing and transport; shared by manual and rendezvous wake requests.
#include "../modes/PunchProbe.h"
// from modes/PunchWake.c - always-on UDP CHECKIN trigger, replaces the manual Y-hold
#include "../modes/PunchWake.h"
#include "../modes/UsbAuto.h"
#include "../modes/BridgeOwner.h"

// from source/hidplus/udp_manager.cpp (compiled as C++, but declared with C linkage
// there via extern "C", so a plain forward declaration here is enough)
extern void networkThread(void *arg);

static Thread hidplus_thread;
static alignas(0x1000) u8 hidplus_thread_stack[0x1000];

// Avoid persistent outbound relay registration in this sysmodule: prior testing found
// console sleep/restart failures that were not fixed by timing or polling changes.
// Keep signaling resource lifetimes bounded; do not restore a held-open outbound
// relay path without evidence resolving those failures.

void NxRemotePlay_Main(void)
{

    // Runs independently of the streaming mode below - forwards controller input via
    // sys-hidplus's UDP protocol (port 8000) regardless of whether video/audio goes
    // out over USB or network. See source/hidplus for the merged-in implementation.
    Result hidplusRc =
        threadCreate(&hidplus_thread, networkThread, NULL, hidplus_thread_stack, sizeof(hidplus_thread_stack), 0x30, 3);
    if (R_SUCCEEDED(hidplusRc))
    {
        Result startRc = threadStart(&hidplus_thread);
        (void)startRc;
    }

    LocalMirror_Configure(-1, -1);

    // The console's Session ID/Password client (register/refresh/wake, LAN identity) - see PunchWake.h. It always
    // starts; the server comes from the NRO's /config/nxremoteplay/server.txt or the built-in default, and the
    // on/off choice is the Host screen's toggle (/config/nxremoteplay/hosting_enabled.txt).
    // PunchWake_Start reads the server back from PunchProbe_GetRelayConfig, so the watcher must be started first.
    PunchProbe_StartButtonWatcher(&sdCard);
    BridgeOwner_Initialize(&sdCard);
    PunchWake_Start(&sdCard);

    NxrpVideo_Start();

    UsbAuto_Start();
    IpcThread();
}
