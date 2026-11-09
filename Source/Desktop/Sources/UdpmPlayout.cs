using System;
using NxRemotePlay.Client.Bridge;

namespace NxRemotePlay.Client.Sources;

// Receive-side timing shared with the Android client (udpm/PlayoutClock.kt, udpm/CongestionMeter.kt) and the NRO
// (Network/Playout.hpp); keep the constants and rules in step. Host and PC clocks are unsynchronised, so every delay
// below is the transit (arrival minus host timestamp) above the smallest transit of the last one to two windows.

// Adaptive presentation delay: a sample above the estimate raises it at once to 1.25x the sample; the estimate decays
// (x0.8) only after a quiet period without samples >= 0.8x. The target stays inside [lo, lo + band]; leaving it upward
// resets at once, downward at most once per second.
internal sealed class PresentationSchedule
{
    internal const long MaxExtraUs = 80_000;   // worst-case wait after arrival
    internal const long MaxJitterUs = 60_000;  // one stall cannot raise the delay without bound
    internal const long DecayQuietUs = 2_000_000;
    internal const long WarmupUs = 1_000_000;  // start-up bursts are not the path
    internal const long FloorWindowUs = 10_000_000;
    const long DecodeAllowanceUs = 4_000, BandUs = 6_000, MinHoldUs = 1_000_000;

    long firstArrival = long.MinValue, last = long.MinValue;
    long windowStart, windowMin = long.MaxValue, previousWindowMin = long.MaxValue;
    long highWater, lastHigh, target = -1, lastChange;

    public long TargetDelayUs => Math.Max(0, target);

    // Local time (same clock as arrivalUs) at which to hand the frame on: never before its arrival, never more than
    // MaxExtraUs after it, never before the previous frame (the queue keeps their order).
    public long PresentAt(long hostTimestampUs, long arrivalUs)
    {
        long present = Math.Max(OnFrame(hostTimestampUs, arrivalUs), arrivalUs);
        present = Math.Min(present, arrivalUs + MaxExtraUs);
        present = Math.Max(present, last);
        last = present;
        return present;
    }

    long OnFrame(long hostTimestampUs, long arrivalUs)
    {
        long transit = arrivalUs - hostTimestampUs;
        if (windowStart == 0) windowStart = arrivalUs;
        if (arrivalUs - windowStart >= FloorWindowUs)
        {
            previousWindowMin = windowMin;
            windowMin = long.MaxValue;
            windowStart = arrivalUs;
        }
        windowMin = Math.Min(windowMin, transit);
        long floor = Math.Min(windowMin, previousWindowMin);
        if (firstArrival == long.MinValue) firstArrival = arrivalUs;
        long sample = arrivalUs - firstArrival < WarmupUs ? 0 : Math.Min(transit - floor, MaxJitterUs);

        if (sample >= highWater * 8 / 10)
        {
            lastHigh = arrivalUs;
            if (sample > highWater) highWater = sample * 5 / 4;
        }
        else if (arrivalUs - lastHigh > DecayQuietUs)
        {
            highWater = highWater * 8 / 10;
            lastHigh = arrivalUs;
        }

        long lo = highWater + DecodeAllowanceUs, hi = lo + BandUs;
        if (target < lo || (target > hi && arrivalUs - lastChange >= MinHoldUs))
        {
            target = (lo + hi) / 2;
            lastChange = arrivalUs;
        }
        return hostTimestampUs + floor + target;
    }
}

// Tells the host how far behind its best case frames arrive (UDPM Congestion, every 250 ms), so it lowers the rate
// before a queue builds. Jitter raises the largest delay of a report, a path too slow for the sender also the smallest
// (a standing queue). A standing level that stays flat for 3 s is taken as a clock step.
internal sealed class CongestionMeter
{
    const long IntervalUs = 250_000, FloorWindowUs = 10_000_000, StandingUs = 50_000, RebaseAfterUs = 3_000_000, FlatUs = 30_000;

    long windowStart, windowMin = long.MaxValue, previousMin = long.MaxValue;
    long excessMin = long.MaxValue, excessMax, lastReport, maxExcessSeen;
    long standingSince, standingLow, standingHigh;
    uint frames, seq, lastNacks, lastUnderruns, sentReports, standingReports, rebases;

    public void OnFrame(long hostTimestampUs, long arrivalUs)
    {
        long transit = arrivalUs - hostTimestampUs;
        if (windowStart == 0) windowStart = arrivalUs;
        if (arrivalUs - windowStart >= FloorWindowUs)
        {
            previousMin = windowMin;
            windowMin = long.MaxValue;
            windowStart = arrivalUs;
        }
        windowMin = Math.Min(windowMin, transit);
        long excess = transit - Math.Min(windowMin, previousMin);
        frames++;
        excessMin = Math.Min(excessMin, excess);
        excessMax = Math.Max(excessMax, excess);
    }

    // The report for the interval that ends now, or null while none is due.
    public UdpmWire.Congestion? Report(long nowUs, uint nacksTotal, uint underrunsTotal)
    {
        if (lastReport == 0)
        {
            lastReport = nowUs; lastNacks = nacksTotal; lastUnderruns = underrunsTotal;
            return null;
        }
        if (nowUs - lastReport < IntervalUs) return null;
        long min = frames == 0 ? 0 : excessMin;
        var report = new UdpmWire.Congestion(seq++, Clamp((nowUs - lastReport) / 1000), frames, Clamp(min), Clamp(excessMax),
            nacksTotal - lastNacks, underrunsTotal - lastUnderruns);
        sentReports++;
        bool standing = frames > 0 && min >= StandingUs;
        if (standing) standingReports++;
        maxExcessSeen = Math.Max(maxExcessSeen, excessMax);
        TrackStanding(nowUs, standing, min);
        frames = 0; excessMin = long.MaxValue; excessMax = 0;
        lastReport = nowUs; lastNacks = nacksTotal; lastUnderruns = underrunsTotal;
        return report;
    }

    // One log fragment, then the counters start again.
    public string Summary()
    {
        string line = $"congestion reports={sentReports} standing={standingReports} maxDelay={maxExcessSeen / 1000}ms rebases={rebases}";
        sentReports = standingReports = rebases = 0;
        maxExcessSeen = 0;
        return line;
    }

    static uint Clamp(long v) => (uint)Math.Clamp(v, 0, int.MaxValue);

    void TrackStanding(long nowUs, bool standing, long level)
    {
        if (!standing) { standingSince = 0; return; }
        if (standingSince == 0) { standingSince = nowUs; standingLow = standingHigh = level; return; }
        standingLow = Math.Min(standingLow, level);
        standingHigh = Math.Max(standingHigh, level);
        if (standingHigh - standingLow >= FlatUs)
        {
            // Still moving (a queue filling or draining): measure again from here.
            standingSince = nowUs; standingLow = standingHigh = level;
        }
        else if (nowUs - standingSince >= RebaseAfterUs)
        {
            windowMin = previousMin = long.MaxValue;
            windowStart = 0; standingSince = 0; rebases++;
        }
    }
}
