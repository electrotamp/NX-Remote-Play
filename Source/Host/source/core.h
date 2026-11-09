#pragma once
#include <switch.h>
#include "modes/defines.h"

#define LOGGING_STACK_BOOST 0

#define R_RET_ON_FAIL(x) do { Result rc = x; if (R_FAILED(rc)) return rc; } while (0)
#define R_THROW(x) do { Result r = x; if (R_FAILED(r)) { fatalThrow(r); }  } while(0)

extern char NxRemotePlayBeacon[];
extern int NxRemotePlayBeaconLen;

Result CoreInit();

void LaunchThread(Thread *t, ThreadFunc f, void *arg, void *stackLocation, u32 stackSize, u32 prio);

void JoinThread(Thread *t);

void SetModeID(u32 mode);

u32 GetCurrentMode();
bool CoreModeSwitchPending(void);

#ifdef __cplusplus
extern "C"
{
#endif
    void NetworkSupportLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
