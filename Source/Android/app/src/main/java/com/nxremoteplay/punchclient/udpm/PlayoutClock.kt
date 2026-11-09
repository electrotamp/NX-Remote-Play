package com.nxremoteplay.punchclient.udpm

/**
 * Decides when a received frame is shown: at its host timestamp plus an adaptive delay, so bursty
 * arrival turns into even presentation. Rules recovered from Nintendo's GameShare client
 * (nerd::lsp::sched, firmware 22.5.0 splay):
 * - HighWaterMarkLatencyEstimator: a sample above the estimate raises it at once to 1.25x the
 *   sample; the estimate only decays (x0.8) after DECAY_QUIET_US without samples >= 0.8x.
 * - FastVideoSchedPolicy: target stays inside [lo, lo + band]; leaving it upward resets at once,
 *   downward at most once per minHoldUs. Host and phone clocks are unsynchronised, so "transit" =
 *   arrival - host timestamp includes an unknown constant offset. Using the minimum transit seen as
 *   the floor cancels it.
 *
 * [maxJitterUs] caps a single sample so one stall cannot raise the delay without bound, and
 * [floorWindowUs] makes the floor the minimum over the last one to two windows, so a slow drift
 * between the two clocks cannot push every frame late. Both default to "off". [decayQuietUs] is how
 * long the estimate waits without high samples before it decays, and samples in the first
 * [warmupUs] of the stream (start-up bursts, not the path) are ignored.
 */
class PlayoutClock(
    private val decodeAllowanceUs: Long = 8_000,
    private val bandUs: Long = 12_000,
    private val minHoldUs: Long = 1_000_000,
    private val maxJitterUs: Long = Long.MAX_VALUE,
    private val floorWindowUs: Long = Long.MAX_VALUE,
    private val decayQuietUs: Long = DECAY_QUIET_US,
    private val warmupUs: Long = 0,
) {
    private var firstArrivalUs = Long.MIN_VALUE
    private var floorTransit = Long.MAX_VALUE
    private var windowStartUs = 0L
    private var windowMin = Long.MAX_VALUE
    private var previousWindowMin = Long.MAX_VALUE
    private var highWater = 0L // jitter estimate above the floor
    private var lastHighUs = 0L
    private var target = -1L // current extra delay above the floor
    private var lastChangeUs = 0L

    val targetDelayUs
        get() = maxOf(0L, target)

    /** Feed every completed frame; returns the local time (us) at which to present it. */
    fun onFrame(hostTimestampUs: Long, arrivalUs: Long): Long {
        val transit = arrivalUs - hostTimestampUs
        if (floorWindowUs == Long.MAX_VALUE) {
            if (transit < floorTransit) floorTransit = transit
        } else {
            if (windowStartUs == 0L) windowStartUs = arrivalUs
            if (arrivalUs - windowStartUs >= floorWindowUs) {
                previousWindowMin = windowMin
                windowMin = Long.MAX_VALUE
                windowStartUs = arrivalUs
            }
            if (transit < windowMin) windowMin = transit
            floorTransit = minOf(windowMin, previousWindowMin)
        }
        if (firstArrivalUs == Long.MIN_VALUE) firstArrivalUs = arrivalUs
        val sample = if (arrivalUs - firstArrivalUs < warmupUs) 0L else minOf(transit - floorTransit, maxJitterUs)

        if (sample >= (highWater * 0.8).toLong()) {
            lastHighUs = arrivalUs
            if (sample > highWater) highWater = (sample * 1.25).toLong()
        } else if (arrivalUs - lastHighUs > decayQuietUs) {
            highWater = (highWater * 0.8).toLong()
            lastHighUs = arrivalUs
        }

        val lo = highWater + decodeAllowanceUs
        val hi = lo + bandUs
        if (target < lo) {
            target = (lo + hi) / 2
            lastChangeUs = arrivalUs
        } else if (target > hi && arrivalUs - lastChangeUs >= minHoldUs) {
            target = (lo + hi) / 2
            lastChangeUs = arrivalUs
        }

        return hostTimestampUs + floorTransit + target
    }

    companion object {
        const val DECAY_QUIET_US = 5_000_000L
    }
}

/**
 * Turns [PlayoutClock] into the time a decoded frame is shown, with a hard bound: never earlier than
 * its arrival, never later than [maxExtraUs] after it (give or take a microsecond), and always after
 * the previous frame.
 * A late frame is shown at once, so a stall is a short freeze and not a lasting delay.
 */
class PresentationSchedule(
    private val maxExtraUs: Long = MAX_EXTRA_US,
    private val clock: PlayoutClock =
        PlayoutClock(
            decodeAllowanceUs = 4_000,
            bandUs = 6_000,
            maxJitterUs = MAX_JITTER_US,
            floorWindowUs = FLOOR_WINDOW_US,
            decayQuietUs = SCHEDULE_DECAY_QUIET_US,
            warmupUs = WARMUP_US,
        ),
) {
    private var lastUs = Long.MIN_VALUE

    /** Local time (microseconds, same clock as [arrivalUs]) at which to show the frame. */
    fun presentAt(hostTimestampUs: Long, arrivalUs: Long): Long {
        val due = clock.onFrame(hostTimestampUs, arrivalUs)
        var present = maxOf(due, arrivalUs)
        present = minOf(present, arrivalUs + maxExtraUs)
        present = maxOf(present, lastUs + 1) // strictly increasing: it is the decoder timestamp
        lastUs = present
        return present
    }

    companion object {
        /**
         * Worst-case extra delay over the moment a frame arrives. The delay follows the measured jitter
         * (a few ms on good Wi-Fi), so this cap only matters on bursty links: 40 ms let every LTE hole
         * of 50-150 ms show as a stutter; 80 ms trades a little latency for smoothness,
         * the way the TCP path's buffering did.
         */
        const val MAX_EXTRA_US = 80_000L
        const val MAX_JITTER_US = 60_000L
        /**
         * One start-up burst and one 17 ms blip held the Wi-Fi delay at 34-82 ms with ~1 ms network
         * jitter: the estimate now starts after the first second and decays after 2 s
         * without high samples instead of 5 s.
         */
        const val SCHEDULE_DECAY_QUIET_US = 2_000_000L
        const val WARMUP_US = 1_000_000L
        const val FLOOR_WINDOW_US = 10_000_000L
    }
}
