// Keep an attached process running while reading its presented buffers.
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet
{

struct DebugPumpStats
{
    u32 wakes;     /* times the handle was signalled */
    u32 events;    /* debug events drained, all types */
    u32 created;   /* DebugEvent_CreateThread */
    u32 exited;    /* DebugEvent_ExitThread */
    u32 other;     /* CreateProcess / ExitProcess / Exception */
    u32 faults;    /* real exceptions, left to the game (not marked handled) */
    u32 continues; /* successful ContinueDebugEvent calls */
    u32 busy;      /* ContinueDebugEvent said Busy: an event came in between */
    u32 failed;    /* any other ContinueDebugEvent failure */
    u32 last_fail_rc;
    u32 per_core[4]; /* which core each continue ran on */
    u64 max_hold_ns; /* longest wake -> continued, i.e. our share of a break */
    bool gone;       /* the game exited or we were detached */
};

/* Starts pumping `dbg`, which must already be drained and continued once
 * (the attach burst). Call from the thread that owns the handle. */
void DebugPumpStart(::ams::svc::Handle dbg);

/* Stops pumping and returns once the pump no longer touches the handle
 * (at most ~50 ms). Must run BEFORE the handle is closed. No-op if the
 * pump was never started. */
void DebugPumpStop();

/* True once the game has exited (or the handle stopped working). A read
 * failing after this is the game closing, not a fault. */
bool DebugPumpTargetGone();

void DebugPumpGetStats(DebugPumpStats *out);

/* One line: "debug events N (+created/-exited/other), continued C ..." */
void DebugPumpLogStats(const char *who);

} // namespace ams::mitm::applet
