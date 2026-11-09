#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Local USB attachment lease. Server ownership is separate: prepare must be acknowledged
// before claiming the server route, and release/expiry never authorizes a legacy REGISTER.
// The caller serializes access and supplies a monotonic millisecond clock.
#define USB_DELEGATION_MAGIC 0x314C584Eu
#define USB_DELEGATION_MAX_MS 20000u
typedef struct
{
    uint64_t attachment, generation, deadline;
    bool prepared, committed;
} BridgeDelegation;
static inline bool BridgeDelegationActive(BridgeDelegation *lease, uint64_t now)
{
    if (lease->prepared && now >= lease->deadline)
        memset(lease, 0, sizeof(*lease));
    return lease->prepared;
}
// Operations: prepare=0, commit/heartbeat=1, release=2. A preparation is bound to
// a successful USB claim signature; the acknowledged server generation must advance.
static inline bool BridgeDelegationApply(BridgeDelegation *lease, uint64_t attachment, uint64_t signedGeneration,
                                         uint8_t operation, uint64_t generation, uint32_t milliseconds, uint64_t now)
{
    BridgeDelegationActive(lease, now);
    if (!attachment || !generation || operation > 2)
        return false;
    if (operation == 2)
    {
        if (milliseconds || !lease->prepared || lease->attachment != attachment || lease->generation != generation)
            return false;
        memset(lease, 0, sizeof(*lease));
        return true;
    }
    if (!milliseconds || milliseconds > USB_DELEGATION_MAX_MS || UINT64_MAX - now < milliseconds)
        return false;
    if (operation == 0)
    {
        if (lease->prepared || generation != signedGeneration)
            return false;
        *lease = (BridgeDelegation){attachment, generation, now + milliseconds, true, false};
        return true;
    }
    if (!lease->prepared || lease->attachment != attachment ||
        (lease->committed
             ? (generation != lease->generation && !(generation > lease->generation && signedGeneration != UINT64_MAX &&
                                                     generation == signedGeneration + 1))
             : generation <= lease->generation))
        return false;
    lease->generation = generation;
    lease->deadline = now + milliseconds;
    lease->committed = true;
    return true;
}

// Refresh only the local attachment deadline after a completed commit. This does
// not advance a server generation or create authority; callers use it solely while
// the already-authorized USB media path is demonstrably making progress.
static inline bool BridgeDelegationMaintain(BridgeDelegation *lease, uint64_t now, uint32_t milliseconds)
{
    if (!BridgeDelegationActive(lease, now) || !lease->committed || !milliseconds ||
        milliseconds > USB_DELEGATION_MAX_MS || UINT64_MAX - now < milliseconds)
        return false;
    lease->deadline = now + milliseconds;
    return true;
}
