package com.nxremoteplay.punchclient.udpm

/**
 * Tells the host how far behind its best case frames are arriving, so it can lower the video rate
 * before a queue builds up. The host and phone clocks are unsynchronised, so "delay" is the transit
 * (arrival minus host timestamp) above the smallest transit of the last one to two windows.
 *
 * Ordinary jitter raises the largest delay of a report but not the smallest; a path that is too slow
 * for the sender raises the smallest too (a standing queue). A clock step looks the same for a
 * while, so a standing level that stays flat for [rebaseAfterUs] is taken as the new normal.
 */
class CongestionMeter(
    private val intervalUs: Long = 250_000,
    private val floorWindowUs: Long = 10_000_000,
    private val standingUs: Long = 50_000,
    private val rebaseAfterUs: Long = 3_000_000,
    private val flatUs: Long = 30_000,
) {
    private var windowStartUs = 0L
    private var windowMin = Long.MAX_VALUE
    private var previousMin = Long.MAX_VALUE
    private var frames = 0
    private var excessMin = Long.MAX_VALUE
    private var excessMax = 0L
    private var lastReportUs = 0L
    private var seq = 0
    private var lastNacks = 0
    private var lastUnderruns = 0
    private var standingSinceUs = 0L
    private var standingLow = 0L
    private var standingHigh = 0L
    private var sentReports = 0
    private var standingReports = 0
    private var maxExcessSeenUs = 0L
    private var rebases = 0

    fun onFrame(hostTimestampUs: Long, arrivalUs: Long) {
        val transit = arrivalUs - hostTimestampUs
        if (windowStartUs == 0L) windowStartUs = arrivalUs
        if (arrivalUs - windowStartUs >= floorWindowUs) {
            previousMin = windowMin
            windowMin = Long.MAX_VALUE
            windowStartUs = arrivalUs
        }
        if (transit < windowMin) windowMin = transit
        val excess = transit - minOf(windowMin, previousMin)
        frames++
        if (excess < excessMin) excessMin = excess
        if (excess > excessMax) excessMax = excess
    }

    fun due(nowUs: Long) = lastReportUs == 0L || nowUs - lastReportUs >= intervalUs

    /** The report for the interval that ends now, or null if none is due yet. */
    fun report(nowUs: Long, nacksTotal: Int, underrunsTotal: Int): Congestion? {
        if (lastReportUs == 0L) {
            lastReportUs = nowUs
            lastNacks = nacksTotal
            lastUnderruns = underrunsTotal
            return null
        }
        if (nowUs - lastReportUs < intervalUs) return null
        val min = if (frames == 0) 0L else excessMin
        val report =
            Congestion(
                seq++,
                ((nowUs - lastReportUs) / 1000).toInt(),
                frames,
                min.coerceIn(0, Int.MAX_VALUE.toLong()).toInt(),
                excessMax.coerceIn(0, Int.MAX_VALUE.toLong()).toInt(),
                nacksTotal - lastNacks,
                underrunsTotal - lastUnderruns,
            )
        sentReports++
        if (frames > 0 && min >= standingUs) standingReports++
        if (excessMax > maxExcessSeenUs) maxExcessSeenUs = excessMax
        trackStanding(nowUs, frames > 0 && min >= standingUs, min)
        frames = 0
        excessMin = Long.MAX_VALUE
        excessMax = 0L
        lastReportUs = nowUs
        lastNacks = nacksTotal
        lastUnderruns = underrunsTotal
        return report
    }

    private fun trackStanding(nowUs: Long, standing: Boolean, level: Long) {
        if (!standing) {
            standingSinceUs = 0L
            return
        }
        if (standingSinceUs == 0L) {
            standingSinceUs = nowUs
            standingLow = level
            standingHigh = level
            return
        }
        standingLow = minOf(standingLow, level)
        standingHigh = maxOf(standingHigh, level)
        if (standingHigh - standingLow >= flatUs) {
            // The level is still moving (a queue filling or draining): measure again from here.
            standingSinceUs = nowUs
            standingLow = level
            standingHigh = level
        } else if (nowUs - standingSinceUs >= rebaseAfterUs) {
            windowMin = Long.MAX_VALUE
            previousMin = Long.MAX_VALUE
            windowStartUs = 0L
            standingSinceUs = 0L
            rebases++
        }
    }

    /** One line for the log, then the counters start again. */
    fun summary(): String {
        val line =
            "congestion reports=$sentReports standing=$standingReports " +
                "maxDelay=${maxExcessSeenUs / 1000}ms rebases=$rebases"
        sentReports = 0
        standingReports = 0
        maxExcessSeenUs = 0L
        rebases = 0
        return line
    }
}
