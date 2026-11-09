package com.nxremoteplay.punchclient

/** Bounded read-completion statistics, not packet ingress or physical presentation. */
internal class DeliveryFeedback {
    companion object {
        const val FEATURE = 0x10
        const val ACK = 0x3346514e
    }

    private var sequence = 0
    private var lastReport = 0L
    private var lastVideo = 0L
    private var firstVideo = 0L
    private var echo = 0L
    private var bytes = 0L
    private var videos = 0L
    private var gapUs = 0L
    private var processingUs = 0L
    private val histogram = LongArray(2049)
    private var intervals = 0L
    private var above50 = 0L
    private var above150 = 0L
    private var maxGap = 0L

    fun packet(size: Int) {
        bytes += size.toLong() + 18
    }

    fun audioWireBytes(size: Long) {
        require(size >= 0)
        bytes += size
    }

    fun video(timestamp: Long, now: Long) {
        if (timestamp > 0) echo = timestamp
        if (firstVideo == 0L) firstVideo = now
        if (lastVideo > 0 && now >= lastVideo) {
            val us = (now - lastVideo) / 1000
            histogram[minOf(2048L, (us + 999) / 1000).toInt()]++
            intervals++
            if (us > 50_000) above50++
            if (us > 150_000) above150++
            maxGap = maxOf(maxGap, us)
            gapUs = maxOf(gapUs, us)
        }
        lastVideo = now
        videos++
    }

    fun handled(durationNs: Long) {
        processingUs = maxOf(processingUs, (durationNs.coerceAtLeast(0) / 1000))
    }

    fun report(now: Long): ByteArray? {
        if (echo <= 0 || (lastReport > 0 && now - lastReport < 250_000_000)) return null
        sequence++
        if (sequence == 0) sequence = 1
        val p = ByteArray(32)
        p[0] = 0x7e
        p[1] = 1
        p[2] = 32
        fun u32(offset: Int, value: Long) {
            for (i in 0..3) p[offset + i] = (value ushr (8 * i)).toByte()
        }
        u32(4, sequence.toLong())
        u32(8, echo)
        u32(12, echo ushr 32)
        u32(16, videos)
        u32(20, bytes.coerceAtMost(0xffffffffL))
        u32(24, gapUs.coerceAtMost(0xffffffffL))
        u32(28, processingUs.coerceAtMost(0xffffffffL))
        bytes = 0
        gapUs = 0
        processingUs = 0
        lastReport = now
        return p
    }

    fun summary(): String {
        val rank = (intervals * 99 + 99) / 100
        var seen = 0L
        var bucket = 0
        for (i in histogram.indices) {
            seen += histogram[i]
            if (seen >= rank) {
                bucket = i
                break
            }
        }
        val upper = if (bucket == 2048) "overflow>2047000" else (bucket * 1000).toString()
        return "arrivals: scope=whole_stream_read_completion intervals=$intervals videos=$videos durationMs=${(lastVideo-firstVideo).coerceAtLeast(0)/1_000_000} p99UpperUs=$upper maxUs=$maxGap gt50ms=$above50 gt150ms=$above150; includes transitions, not physical display"
    }
}
