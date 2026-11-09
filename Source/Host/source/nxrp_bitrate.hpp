#pragma once
#include <cstdint>

// Feedback over complete hardware-encoded access units, including IDR headers.
// This targets average video bitrate; it does not pace TCP or discard P frames.
class NxrpBitrate
{
  public:
    static constexpr uint32_t TargetBps = 10000000;
    static constexpr uint8_t InitialQp = 30;
    // The 1 s window below reacted only after a busy scene had already queued 0.5-0.9 s of video
    // on the path (run 1793507133929: 10-12 Mbps against a 7.464 Mbps budget, QP moved 1-3 steps a
    // second). StepBurst caps the short-term rate instead.
    static constexpr uint64_t BurstWindowNs = 250000000ULL;
    static constexpr uint32_t BurstMinFrames = 5;
    static constexpr unsigned BurstMaxSteps = 6; // six QP steps halve the rate
    // P frames carry this share of the budget; one IDR in 60 costs about 4.6 P frames
    static constexpr unsigned PFrameSharePercent = 93;
    explicit NxrpBitrate(uint64_t now_ns)
        : start_ns(now_ns), burst_start_ns(now_ns)
    {
    }
    void SetDemandFps(uint32_t fps)
    {
        demand_fps = fps < 1 ? 1 : fps > 60 ? 60 : fps;
    }
    void SetTargetBps(uint32_t target)
    {
        target_bps = target < 128000u ? 128000u : target > TargetBps ? TargetBps : target;
    }
    uint32_t VideoTargetBps() const
    {
        return target_bps;
    }
    void Add(uint32_t bytes, bool idr = false)
    {
        window_bytes += bytes;
        ++frames;
        if (!idr)
        {
            burst_bytes += bytes;
            ++burst_frames;
        }
    }
    // Raises QP at once when the last 250 ms of P frames ran more than 10% above their share of the
    // budget, by as many steps as bring that rate back to the budget. Recovery stays with Step().
    bool StepBurst(uint64_t now_ns, uint8_t current_qp, uint8_t *next_qp)
    {
        if (now_ns < burst_start_ns)
        {
            burst_start_ns = now_ns;
            burst_bytes = burst_frames = 0;
            return false;
        }
        const uint64_t elapsed = now_ns - burst_start_ns;
        if (elapsed < BurstWindowNs)
            return false;
        const uint64_t bytes = burst_bytes;
        const uint32_t count = burst_frames;
        burst_start_ns = now_ns;
        burst_bytes = burst_frames = 0;
        if (count < BurstMinFrames)
            return false;
        // slow delivery must not hide oversized frames, as in Step()
        const uint64_t wall_rate = bytes * 8000000000ULL / elapsed;
        const uint64_t projected = bytes * 8ULL * demand_fps / count;
        uint64_t rate = wall_rate > projected ? wall_rate : projected;
        if (rate > burst_peak_bps)
            burst_peak_bps = rate > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rate);
        const uint64_t budget = static_cast<uint64_t>(target_bps) * PFrameSharePercent / 100;
        if (rate <= budget * 11 / 10)
            return false;
        unsigned steps = 0;
        while (steps < BurstMaxSteps && rate > budget)
        {
            rate = rate * 8909 / 10000; // one QP step scales the output by 2^(-1/6)
            ++steps;
        }
        const unsigned next = current_qp + steps > 44 ? 44u : current_qp + steps;
        *next_qp = static_cast<uint8_t>(next);
        if (next == current_qp)
            return false;
        ++burst_cuts;
        burst_cut_in_window = true;
        return true;
    }
    uint32_t LastBurstCuts() const
    {
        return last_burst_cuts;
    }
    uint32_t LastBurstPeakBps() const
    {
        return last_burst_peak_bps;
    }
    bool Step(uint64_t now_ns, uint8_t current_qp, uint8_t *next_qp, uint32_t *bps)
    {
        if (now_ns <= start_ns || now_ns - start_ns < 1000000000ULL || frames < 5)
            return false;
        const uint64_t rate = window_bytes * 8000000000ULL / (now_ns - start_ns);
        *bps = static_cast<uint32_t>(rate > UINT32_MAX ? UINT32_MAX : rate);
        // Do not let slow delivery hide oversized frames:100KB at10fps is
        // only8Mbps, but those frames would need48Mbps at60fps.
        const uint64_t projected = window_bytes * 8ULL * demand_fps / frames;
        const uint64_t pressure = rate > projected ? rate : projected;
        unsigned next = current_qp;
        if (pressure > target_bps * 14ULL / 10)
            next += 3;
        else if (pressure > target_bps * 11ULL / 10)
            next += 1;
        else if (pressure < target_bps * 85ULL / 100 && next > 24)
            --next;
        // this window already holds the frames StepBurst reacted to: its average would count them twice
        if (burst_cut_in_window)
            next = current_qp;
        if (next > 44)
            next = 44;
        if (next < 24)
            next = 24;
        *next_qp = static_cast<uint8_t>(next);
        start_ns = now_ns;
        window_bytes = frames = 0;
        last_burst_cuts = burst_cuts;
        last_burst_peak_bps = burst_peak_bps;
        burst_cuts = burst_peak_bps = 0;
        burst_cut_in_window = false;
        return true;
    }

  private:
    uint64_t start_ns, window_bytes = 0;
    uint32_t frames = 0;
    uint32_t demand_fps = 60;
    uint32_t target_bps = TargetBps;
    uint64_t burst_start_ns, burst_bytes = 0;
    uint32_t burst_frames = 0;
    uint32_t burst_cuts = 0, burst_peak_bps = 0, last_burst_cuts = 0, last_burst_peak_bps = 0;
    bool burst_cut_in_window = false;
};
