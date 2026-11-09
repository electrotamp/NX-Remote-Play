#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define NQ_ACK 0x3346514eu
#define NQ_TYPE 0x7eu
#define NQ_SIZE 32u
#define NQ_EXPIRY_US 2000000ULL
#define NQ_MIN_VIDEO_BPS 128000u
#define NQ_BURST_BYTES 8192u
#define NQ_GRANT_BYTES 4096u

typedef struct
{
    uint32_t sequence, gap_us;
    uint64_t echo_us;
} NqReport;
typedef struct
{
    uint64_t last_us, last_echo_us, reduction_us;
    uint32_t sequence, target_bps, baseline_us, bad, good, local_bad;
} NqControl;
static inline uint32_t NqLoad32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline bool NqDecode(const uint8_t *p, uint32_t n, NqReport *r)
{
    if (n != NQ_SIZE || p[0] != NQ_TYPE || p[1] != 1 || p[2] != NQ_SIZE || p[3])
        return false;
    r->sequence = NqLoad32(p + 4);
    r->echo_us = NqLoad32(p + 8) | ((uint64_t)NqLoad32(p + 12) << 32);
    r->gap_us = NqLoad32(p + 24);
    return r->sequence != 0;
}
static inline uint32_t NqInputSize(uint8_t type, bool negotiated)
{
    return type == NQ_TYPE ? (negotiated ? NQ_SIZE : 0u) : type == 2 ? 17u : 5u;
}
static inline void NqReset(NqControl *c)
{
    memset(c, 0, sizeof(*c));
}
static inline uint32_t NqClamp(uint32_t value, uint32_t ceiling)
{
    if (value < NQ_MIN_VIDEO_BPS)
        value = NQ_MIN_VIDEO_BPS;
    return value > ceiling ? ceiling : value;
}
static inline bool NqFresh(const NqControl *c, uint64_t now)
{
    return c->last_us && now >= c->last_us && now - c->last_us <= NQ_EXPIRY_US;
}
// Queue-inclusive echo age uses the HOST clock on both ends of the subtraction.
// It is neither path RTT nor physical screen latency. Reader gap is not ingress.
static inline bool NqApply(NqControl *c, const NqReport *r, uint64_t now, uint32_t ceiling)
{
    if (!r->echo_us || r->echo_us > now || now - r->echo_us > 30000000ULL)
        return false;
    if (c->sequence && (int32_t)(r->sequence - c->sequence) <= 0)
        return false;
    if (c->last_echo_us && r->echo_us < c->last_echo_us)
        return false;
    const uint32_t age = (uint32_t)(now - r->echo_us);
    const bool fresh = NqFresh(c, now);
    if (!fresh)
    {
        c->baseline_us = age;
        c->bad = c->good = 0;
        if (!c->target_bps)
            c->target_bps = ceiling;
    }
    if (age < c->baseline_us)
        c->baseline_us = age;
    c->sequence = r->sequence;
    c->last_echo_us = r->echo_us;
    c->last_us = now;
    c->target_bps = NqClamp(c->target_bps, ceiling);
    const uint32_t excess = age - c->baseline_us;
    const bool congested = excess > 100000u || r->gap_us > 150000u;
    if (congested)
    {
        c->good = 0;
        if (c->bad < 3)
            ++c->bad;
        if (c->bad >= 2 && (!c->reduction_us || now - c->reduction_us >= 750000ULL))
        {
            c->target_bps =
                NqClamp(c->target_bps / (excess > 400000u ? 2u : 4u) * (excess > 400000u ? 1u : 3u), ceiling);
            c->reduction_us = now;
            c->bad = 0;
        }
    }
    else
    {
        c->bad = 0;
        if (c->good < 8)
            ++c->good;
        if (c->good >= 8 && (!c->reduction_us || now - c->reduction_us >= 2000000ULL))
        {
            const uint32_t step = c->target_bps / 20u > 64000u ? c->target_bps / 20u : 64000u;
            c->target_bps = NqClamp(c->target_bps + step, ceiling);
            c->good = 0;
        }
    }
    return true;
}
// No receive feedback: transport-call time (excluding deliberate pacing) is a
// local fallback signal, NOT a link capacity estimate or user input latency.
static inline void NqLocal(NqControl *c, uint64_t now, uint32_t network_us, uint32_t ceiling)
{
    if (NqFresh(c, now))
        return;
    if (!c->target_bps)
        c->target_bps = ceiling;
    c->target_bps = NqClamp(c->target_bps, ceiling);
    if (network_us > 50000u)
    {
        if (c->local_bad < 2)
            ++c->local_bad;
        if (c->local_bad >= 2 && (!c->reduction_us || now - c->reduction_us >= 750000ULL))
        {
            c->target_bps = NqClamp(c->target_bps / 4u * 3u, ceiling);
            c->reduction_us = now;
            c->local_bad = 0;
            c->good = 0;
        }
    }
    else
    {
        c->local_bad = 0;
        if (c->good < 120)
            ++c->good;
        if (c->good >= 120 && now >= c->reduction_us && now - c->reduction_us >= 3000000ULL)
        {
            c->target_bps = NqClamp(c->target_bps + 64000u, ceiling);
            c->good = 0;
        }
    }
}

typedef struct
{
    uint64_t updated_us, credit, accepted;
    uint32_t rate_bps;
} NqBucket;
static inline void NqBucketInit(NqBucket *b, uint64_t now, uint32_t rate)
{
    b->updated_us = now;
    b->credit = (uint64_t)NQ_BURST_BYTES * 1000000ULL;
    b->accepted = 0;
    b->rate_bps = rate;
}
static inline void NqBucketUpdate(NqBucket *b, uint64_t now, uint32_t rate)
{
    if (now >= b->updated_us)
    {
        uint64_t elapsed = now - b->updated_us;
        if (elapsed > 1000000ULL)
            elapsed = 1000000ULL;
        b->credit += elapsed * (b->rate_bps / 8u);
        const uint64_t limit = (uint64_t)NQ_BURST_BYTES * 1000000ULL;
        if (b->credit > limit)
            b->credit = limit;
        b->updated_us = now;
    }
    b->rate_bps = rate;
}
static inline uint32_t NqBucketGrant(NqBucket *b, uint32_t wanted)
{
    const uint32_t amount = wanted < NQ_GRANT_BYTES ? wanted : NQ_GRANT_BYTES;
    if (b->credit < (uint64_t)amount * 1000000ULL)
        return 0;
    b->credit -= (uint64_t)amount * 1000000ULL;
    return amount;
}
static inline void NqBucketRefund(NqBucket *b, uint32_t unused, uint32_t accepted)
{
    b->credit += (uint64_t)unused * 1000000ULL;
    const uint64_t limit = (uint64_t)NQ_BURST_BYTES * 1000000ULL;
    if (b->credit > limit)
        b->credit = limit;
    b->accepted += accepted;
}
