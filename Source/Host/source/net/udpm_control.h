#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Two inputs set the video target:
// 1. Loss (UqApply): feedback counters are cumulative and only new, ordered reports advance a window.
//    Three consecutive >=1s windows with >=2 unrecoverable frames cut 15%.
// 2. Congestion (UqApplyCongestion): the client reports every ~250 ms how far behind its best case the
//    frames of the interval were (smallest and largest), and how often audio ran dry. Repaired loss shows
//    up as delay, not as lost frames, so this is what catches a full link: a standing queue (even the
//    earliest frame of the interval was late) means the sender is faster than the path. One bad report
//    cuts 25%. A queue keeps reporting for a while after the cut that fixes it, so a further cut needs
//    1 s plus the standing delay at the last cut to have passed, and a standing delay no smaller than at
//    that cut (the queue is not draining).
//    Cutting on every report while a 1.75 s queue drained took a game start to the floor.
//    A source switch (HOME <-> game) stalls the stream for a moment whatever the path does, so UqQuiet()
//    makes reports ignored for 6 s after one.
// After a loss cut, three clean windows recover 5% (at least 64kbps). After a congestion cut the rate is held
// for 6 s after the last congested report, then rises per two clean 1 s windows: 10% while below 80% of the
// limit, 5% above. The limit is 95% of the rate at the first cut of an episode (follow-up cuts while a queue
// drains do not lower it) and lasts until 30 s pass without congestion, so the rate settles below the path's
// limit instead of probing into it every few seconds. Missing feedback holds quality; the existing UDP
// liveness timeout still terminates an unresponsive session. The floor keeps the picture usable.
#define UQ_CEILING_BPS 7464000u
#define UQ_FLOOR_BPS 2000000u
#define UQ_EXPIRY_US 2000000ULL
// A report counts as congested when even its earliest frame came >50 ms after the best case the client
// has seen lately (a standing queue), or one frame came >300 ms after it (a stall), or audio ran dry
// twice. Ordinary jitter raises the largest excess but not the smallest, so it does not count.
#define UQ_CONG_STANDING_US 50000u
#define UQ_CONG_MIN_FRAMES 5u
#define UQ_CONG_SPIKE_US 300000u
#define UQ_CONG_UNDERRUNS 2u
#define UQ_CONG_CUT_PERCENT 25u
#define UQ_CONG_CUT_GAP_US 1000000ULL
#define UQ_CONG_HOLD_US 6000000ULL
#define UQ_CONG_QUIET_US 6000000ULL
#define UQ_CONG_LIMIT_US 30000000ULL
typedef struct
{
    uint64_t last_us, window_us;
    uint32_t seq, dropped, window_dropped, target_bps;
    uint32_t bad, good, reductions, recoveries;
    uint32_t limit_bps; // 95% of the rate at the first cut of the latest episode; 0 = none
    uint32_t cut_excess_us; // standing delay reported when the last cut was made
    uint64_t hold_until_us, last_cut_us, quiet_until_us, last_cong_us;
    uint32_t cong_seq, cong_cuts, cong_reports;
    bool seen, cong_seen;
} UqControl;
static inline void UqReset(UqControl *c)
{
    memset(c, 0, sizeof(*c));
    c->target_bps = UQ_CEILING_BPS;
}
static inline bool UqApply(UqControl *c, uint32_t seq, uint32_t dropped, uint64_t now)
{
    if (c->seen && (int32_t)(seq - c->seq) <= 0)
        return false;
    const bool baseline = !c->seen || now < c->last_us || now - c->last_us > UQ_EXPIRY_US;
    // Counter wrap is valid; a backwards/reset counter is a discontinuity.
    const uint32_t delta = dropped - c->dropped;
    if (baseline || delta > 0x7fffffffu)
    {
        c->window_us = now;
        c->window_dropped = 0;
        c->bad = c->good = 0;
    }
    else
    {
        if (UINT32_MAX - c->window_dropped < delta)
            c->window_dropped = UINT32_MAX;
        else
            c->window_dropped += delta;
        if (now - c->window_us >= 1000000ULL)
        {
            if (c->window_dropped >= 2)
            {
                c->good = 0;
                if (++c->bad >= 3)
                {
                    c->target_bps = c->target_bps / 100u * 85u;
                    if (c->target_bps < UQ_FLOOR_BPS)
                        c->target_bps = UQ_FLOOR_BPS;
                    c->bad = 0;
                    c->reductions++;
                }
            }
            else if (!c->window_dropped)
            {
                c->bad = 0;
                const bool afterCongestion = c->cong_cuts != 0;
                if (c->limit_bps && now - c->last_cong_us >= UQ_CONG_LIMIT_US)
                    c->limit_bps = 0; // the path has been clean for a while: allow the full rate again
                const uint32_t ceiling = c->limit_bps ? c->limit_bps : UQ_CEILING_BPS;
                if (now < c->hold_until_us)
                    c->good = 0; // congestion was reported lately: hold quality
                else if (++c->good >= (afterCongestion ? 2u : 3u))
                {
                    // far below the limit the path has room: climb faster
                    uint32_t step = c->target_bps / (afterCongestion && c->target_bps < ceiling / 5u * 4u ? 10u : 20u);
                    if (step < 64000u)
                        step = 64000u;
                    if (c->target_bps < ceiling)
                    {
                        c->recoveries++;
                        c->target_bps += step;
                        if (c->target_bps > ceiling)
                            c->target_bps = ceiling;
                    }
                    c->good = 0;
                }
            }
            else
                c->bad = c->good = 0;
            c->window_us = now;
            c->window_dropped = 0;
        }
    }
    c->seen = true;
    c->seq = seq;
    c->dropped = dropped;
    c->last_us = now;
    return true;
}

// One congestion report. Returns false for an old or repeated sequence number.
static inline bool UqApplyCongestion(UqControl *c, uint32_t seq, uint32_t interval_ms, uint32_t frames,
                                     uint32_t excess_min_us, uint32_t excess_max_us, uint32_t audio_underruns,
                                     uint64_t now)
{
    if (c->cong_seen && (int32_t)(seq - c->cong_seq) <= 0)
        return false;
    c->cong_seen = true;
    c->cong_seq = seq;
    c->cong_reports++;
    if (interval_ms == 0 || interval_ms > 2000)
        return true; // not a usable interval
    if (now < c->quiet_until_us)
        return true; // the stream was just restarted by a source switch
    const bool standing = frames >= UQ_CONG_MIN_FRAMES && excess_min_us >= UQ_CONG_STANDING_US;
    if (!(standing || excess_max_us >= UQ_CONG_SPIKE_US || audio_underruns >= UQ_CONG_UNDERRUNS))
        return true;
    const bool newEpisode = !c->last_cong_us || now - c->last_cong_us >= UQ_CONG_HOLD_US;
    c->hold_until_us = now + UQ_CONG_HOLD_US;
    c->last_cong_us = now;
    c->good = 0;
    // Frames reported now were sent one queue-delay ago, so a cut shows only after that delay has passed.
    if (!newEpisode && c->last_cut_us && now >= c->last_cut_us &&
        (now - c->last_cut_us < UQ_CONG_CUT_GAP_US + c->cut_excess_us || excess_min_us < c->cut_excess_us))
        return true; // the last cut cannot show yet, or the queue is draining
    if (newEpisode || !c->limit_bps)
    {
        c->limit_bps = c->target_bps / 100u * 95u;
        if (c->limit_bps < UQ_FLOOR_BPS)
            c->limit_bps = UQ_FLOOR_BPS;
    }
    c->cut_excess_us = excess_min_us;
    c->target_bps = c->target_bps / 100u * (100u - UQ_CONG_CUT_PERCENT);
    if (c->target_bps < UQ_FLOOR_BPS)
        c->target_bps = UQ_FLOOR_BPS;
    c->last_cut_us = now;
    c->cong_cuts++;
    return true;
}

// The video source was switched (HOME <-> game): the restart stalls frames for a moment, which is not the path.
static inline void UqQuiet(UqControl *c, uint64_t now)
{
    c->quiet_until_us = now + UQ_CONG_QUIET_US;
}
