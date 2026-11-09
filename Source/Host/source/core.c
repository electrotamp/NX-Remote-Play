#include <string.h>

#include "core.h"
#include "modes/defines.h"
#include "modes/modes.h"
#include "capture.h"

#include "net/sockets.h"

#define PLACEHOLDER_SERIAL "Unknown serial\0\0\0\0\0\0\0\0\0"
_Static_assert(sizeof(PLACEHOLDER_SERIAL) == sizeof(SetSysSerialNumber));

char NxRemotePlayBeacon[] = "NxRemotePlay"
                            "|" NXREMOTEPLAY_VERSION_STRING "|" NXREMOTEPLAY_PROTOCOL_VERSION "|" PLACEHOLDER_SERIAL;
int NxRemotePlayBeaconLen = sizeof(NxRemotePlayBeacon);

Result CoreInit()
{
    Result rc = setsysInitialize();

    // Ignore error, we'll just use a placeholder serial
    if (R_SUCCEEDED(rc))
    {
        SetSysSerialNumber serial;
        Result rc2 = setsysGetSerialNumber(&serial);

        if (R_SUCCEEDED(rc2))
        {
            memcpy(NxRemotePlayBeacon + NxRemotePlayBeaconLen - sizeof(SetSysSerialNumber), serial.number,
                   sizeof(SetSysSerialNumber));
            NxRemotePlayBeacon[NxRemotePlayBeaconLen - 1] = '\0';
        }

        setsysExit();
    }

    rc = CaptureInitialize();
    if (R_FAILED(rc))
        return rc;


    SocketInit();

    return 0;
}

atomic_bool IsThreadRunning = false;

void LaunchThread(Thread *t, ThreadFunc f, void *arg, void *stackLocation, u32 stackSize, u32 prio)
{
    Result rc = threadCreate(t, f, arg, stackLocation, stackSize, prio, 3);
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = threadStart(t);
    if (R_FAILED(rc))
        fatalThrow(rc);
}

void JoinThread(Thread *t)
{
    if (t->handle == INVALID_HANDLE)
        return;

    Result rc = threadWaitForExit(t);
    if (R_FAILED(rc))
    {
        fatalThrow(rc);
    }

    rc = threadClose(t);
    if (R_FAILED(rc))
    {
        fatalThrow(rc);
    }

    t->handle = INVALID_HANDLE;
}

const StreamMode *CurrentMode = NULL;

// Mode switching is complicated because the streaming thread can get stuck indefinitely waiting for grc when a game is
// not running So we fake mode switching: when the user puts in a request a thread starts and waits for the streaming
// thread to exit the request can be changed at any time while this thread is waiting, once the streaming threads
// actually exit the new request is processed
static u8 alignas(0x1000) ModeSwitchingStackArea[0x2000 + LOGGING_STACK_BOOST];
static atomic_bool IsModeSwitchPending = false;
static Thread ModeSwitchThread;
static Mutex ModeSwitchingMutex;
static const StreamMode *SwitchModeTarget = NULL;

bool CoreModeSwitchPending(void)
{
    return atomic_load(&IsModeSwitchPending);
}

static const StreamMode *GetUserVisibleMode()
{
    mutexLock(&ModeSwitchingMutex);

    const StreamMode *ret = CurrentMode;

    if (IsModeSwitchPending)
        ret = SwitchModeTarget;

    mutexUnlock(&ModeSwitchingMutex);
    return ret;
}

static void EnterTargetMode()
{
    mutexLock(&ModeSwitchingMutex);

    if (CurrentMode)
        fatalThrow(ERR_MAIN_UNEXPECTED_MODE);

    CurrentMode = SwitchModeTarget;
    SwitchModeTarget = NULL;

    if (CurrentMode)
        IsThreadRunning = true;

    IsModeSwitchPending = false;

    mutexUnlock(&ModeSwitchingMutex);
}

static void ExitCurrentMode()
{
    if (CurrentMode)
    {
        IsThreadRunning = false;

        CurrentMode->ExitFn();
        CurrentMode = NULL;
    }
}

static void SwitchModesThreadMain(void *)
{
    // This will take forever
    ExitCurrentMode();

    // This is fast
    EnterTargetMode();
}

static void SwitchModes(const StreamMode *mode)
{
    mutexLock(&ModeSwitchingMutex);

    SwitchModeTarget = mode;
    if (!IsModeSwitchPending)
    {
        IsModeSwitchPending = true;

        if (ModeSwitchThread.handle)
        {
            // Dispose the previous mode switch thread. By now it shouldn't be running anymore so this won't block
            JoinThread(&ModeSwitchThread);
        }

        // If there is a mode active, we use a thread to wait for it to exit
        if (CurrentMode)
        {
            LaunchThread(&ModeSwitchThread, SwitchModesThreadMain, NULL, ModeSwitchingStackArea,
                         sizeof(ModeSwitchingStackArea), 0x2C);
        }
        else // Otherwise we just do it directly
        {
            mutexUnlock(&ModeSwitchingMutex);
            SwitchModesThreadMain(NULL);
            return;
        }
    }

    mutexUnlock(&ModeSwitchingMutex);
}

u32 GetCurrentMode()
{
    const StreamMode *mode = GetUserVisibleMode();

    if (mode == NULL)
        return TYPE_MODE_NULL;
    else if (mode == &USB_MODE)
        return TYPE_MODE_USB;
    else
        fatalThrow(ERR_MAIN_UNKMODE);
}

void SetModeID(u32 mode)
{
    switch (mode)
    {
    case TYPE_MODE_USB:
        SwitchModes(&USB_MODE);
        break;
    case TYPE_MODE_NULL:
        SwitchModes(NULL);
        break;
    default:
        fatalThrow(ERR_MAIN_UNKMODESET);
    }
}
