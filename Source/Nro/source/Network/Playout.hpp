#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include "../udpm/udpm_proto.h"

// Receive-side timing shared with the Android client (udpm/PlayoutClock.kt, udpm/CongestionMeter.kt); keep the
// constants and rules in step with it. Host and console clocks are unsynchronised, so every delay below is the
// transit (arrival minus host timestamp) above the smallest transit seen over the last one to two windows.
namespace Network::Punch
{
// Adaptive presentation delay: a sample above the estimate raises it at once to 1.25x the sample; the
// estimate decays (x0.8) only after a quiet period without samples >= 0.8x. The target stays inside
// [lo, lo + band]; leaving it upward resets at once, downward at most once per second.
class PresentationSchedule
{
  public:
    static constexpr int64_t MaxExtraUs = 80000;    // worst-case wait after arrival
    static constexpr int64_t MaxJitterUs = 60000;   // one stall cannot raise the delay without bound
    static constexpr int64_t DecayQuietUs = 2000000;
    static constexpr int64_t WarmupUs = 1000000;    // start-up bursts are not the path
    static constexpr int64_t FloorWindowUs = 10000000;

    // Local time (same clock as arrivalUs) at which to hand the frame on: never before its arrival, never
    // more than MaxExtraUs after it, never before the previous frame (the queue keeps their order).
    int64_t PresentAt(int64_t hostTimestampUs, int64_t arrivalUs)
    {
        const int64_t due = OnFrame(hostTimestampUs, arrivalUs);
        int64_t present = std::max(due, arrivalUs);
        present = std::min(present, arrivalUs + MaxExtraUs);
        present = std::max(present, last);
        last = present;
        return present;
    }

    int64_t TargetDelayUs() const
    {
        return std::max<int64_t>(0, target);
    }

  private:
    static constexpr int64_t DecodeAllowanceUs = 4000, BandUs = 6000, MinHoldUs = 1000000;
    static constexpr int64_t None = std::numeric_limits<int64_t>::max();
    int64_t firstArrival = std::numeric_limits<int64_t>::min(), last = std::numeric_limits<int64_t>::min();
    int64_t windowStart = 0, windowMin = None, previousWindowMin = None;
    int64_t highWater = 0, lastHigh = 0, target = -1, lastChange = 0;

    int64_t OnFrame(int64_t hostTimestampUs, int64_t arrivalUs)
    {
        const int64_t transit = arrivalUs - hostTimestampUs;
        if (!windowStart)
            windowStart = arrivalUs;
        if (arrivalUs - windowStart >= FloorWindowUs)
        {
            previousWindowMin = windowMin;
            windowMin = None;
            windowStart = arrivalUs;
        }
        windowMin = std::min(windowMin, transit);
        const int64_t floor = std::min(windowMin, previousWindowMin);
        if (firstArrival == std::numeric_limits<int64_t>::min())
            firstArrival = arrivalUs;
        const int64_t sample = arrivalUs - firstArrival < WarmupUs ? 0 : std::min(transit - floor, MaxJitterUs);

        if (sample >= highWater * 8 / 10)
        {
            lastHigh = arrivalUs;
            if (sample > highWater)
                highWater = sample * 5 / 4;
        }
        else if (arrivalUs - lastHigh > DecayQuietUs)
        {
            highWater = highWater * 8 / 10;
            lastHigh = arrivalUs;
        }

        const int64_t lo = highWater + DecodeAllowanceUs, hi = lo + BandUs;
        if (target < lo || (target > hi && arrivalUs - lastChange >= MinHoldUs))
        {
            target = (lo + hi) / 2;
            lastChange = arrivalUs;
        }
        return hostTimestampUs + floor + target;
    }
};

// Tells the host how far behind its best case frames arrive (UDPM Congestion, every 250 ms), so it lowers the
// rate before a queue builds. Jitter raises the largest delay of a report, a path too slow for the sender also
// the smallest (a standing queue). A standing level that stays flat for 3 s is taken as a clock step.
class CongestionMeter
{
  public:
    void OnFrame(int64_t hostTimestampUs, int64_t arrivalUs)
    {
        const int64_t transit = arrivalUs - hostTimestampUs;
        if (!windowStart)
            windowStart = arrivalUs;
        if (arrivalUs - windowStart >= FloorWindowUs)
        {
            previousMin = windowMin;
            windowMin = None;
            windowStart = arrivalUs;
        }
        windowMin = std::min(windowMin, transit);
        const int64_t excess = transit - std::min(windowMin, previousMin);
        ++frames;
        excessMin = std::min(excessMin, excess);
        excessMax = std::max(excessMax, excess);
    }

    // The report for the interval that ends now; false while none is due.
    bool Report(int64_t nowUs, uint32_t nacksTotal, uint32_t underrunsTotal, UdpmCongestion &out)
    {
        if (!lastReport)
        {
            lastReport = nowUs;
            lastNacks = nacksTotal;
            lastUnderruns = underrunsTotal;
            return false;
        }
        if (nowUs - lastReport < IntervalUs)
            return false;
        const int64_t min = frames ? excessMin : 0;
        out = UdpmCongestion{seq++,
                             Clamp((nowUs - lastReport) / 1000),
                             frames,
                             Clamp(min),
                             Clamp(excessMax),
                             nacksTotal - lastNacks,
                             underrunsTotal - lastUnderruns,
                             0};
        ++sentReports;
        const bool standing = frames && min >= StandingUs;
        if (standing)
            ++standingReports;
        maxExcessSeen = std::max(maxExcessSeen, excessMax);
        TrackStanding(nowUs, standing, min);
        frames = 0;
        excessMin = None;
        excessMax = 0;
        lastReport = nowUs;
        lastNacks = nacksTotal;
        lastUnderruns = underrunsTotal;
        return true;
    }

    // One log fragment, then the counters start again.
    std::string Summary()
    {
        std::string line = "congestion reports=" + std::to_string(sentReports) + " standing=" +
                           std::to_string(standingReports) + " maxDelay=" + std::to_string(maxExcessSeen / 1000) +
                           "ms rebases=" + std::to_string(rebases);
        sentReports = standingReports = rebases = 0;
        maxExcessSeen = 0;
        return line;
    }

  private:
    static constexpr int64_t IntervalUs = 250000, FloorWindowUs = 10000000, StandingUs = 50000;
    static constexpr int64_t RebaseAfterUs = 3000000, FlatUs = 30000;
    static constexpr int64_t None = std::numeric_limits<int64_t>::max();
    int64_t windowStart = 0, windowMin = None, previousMin = None;
    int64_t excessMin = None, excessMax = 0, lastReport = 0, maxExcessSeen = 0;
    int64_t standingSince = 0, standingLow = 0, standingHigh = 0;
    uint32_t frames = 0, seq = 0, lastNacks = 0, lastUnderruns = 0;
    uint32_t sentReports = 0, standingReports = 0, rebases = 0;

    static uint32_t Clamp(int64_t v)
    {
        return uint32_t(std::clamp<int64_t>(v, 0, std::numeric_limits<int32_t>::max()));
    }

    void TrackStanding(int64_t nowUs, bool standing, int64_t level)
    {
        if (!standing)
        {
            standingSince = 0;
            return;
        }
        if (!standingSince)
        {
            standingSince = nowUs;
            standingLow = standingHigh = level;
            return;
        }
        standingLow = std::min(standingLow, level);
        standingHigh = std::max(standingHigh, level);
        if (standingHigh - standingLow >= FlatUs)
        {
            // Still moving (a queue filling or draining): measure again from here.
            standingSince = nowUs;
            standingLow = standingHigh = level;
        }
        else if (nowUs - standingSince >= RebaseAfterUs)
        {
            windowMin = previousMin = None;
            windowStart = 0;
            standingSince = 0;
            ++rebases;
        }
    }
};
} // namespace Network::Punch
