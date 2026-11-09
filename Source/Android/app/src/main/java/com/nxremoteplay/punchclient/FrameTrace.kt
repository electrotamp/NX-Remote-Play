package com.nxremoteplay.punchclient

import android.os.SystemClock
import java.util.concurrent.atomic.AtomicLong

/** Read-only timing counters for a short latency diagnosis. No frame is delayed or dropped here. */
internal class FrameTrace(private val log: (String) -> Unit) {
    private val startedNs = SystemClock.elapsedRealtimeNanos()
    private var windowNs = startedNs
    private var lastReceiveNs = 0L
    private var lastOutputNs = 0L
    private var received = 0
    private var queued = 0
    private var output = 0
    private var queueDepthMax = 0
    private var unavailable = 0
    private var maxReceiveGapNs = 0L
    private var minReceiveGapNs = Long.MAX_VALUE
    private var maxOutputGapNs = 0L
    private var firstOutput = false
    private var firstSps = false
    private var firstPps = false
    private var firstIdr = false
    private val rendered = AtomicLong(0)
    private val lastRenderNs = AtomicLong(0)
    private val maxRenderGapNs = AtomicLong(0)

    @Synchronized fun receive(data: ByteArray, packetNumber: Int, queueDepth: Int = 0) {
        val now = SystemClock.elapsedRealtimeNanos()
        if (lastReceiveNs != 0L) {
            val gap = now - lastReceiveNs
            if (gap >= 0) {
                maxReceiveGapNs = maxOf(maxReceiveGapNs, gap)
                minReceiveGapNs = minOf(minReceiveGapNs, gap)
            }
        }
        lastReceiveNs = now
        received++
        queueDepthMax = maxOf(queueDepthMax, queueDepth)
        if ((!firstSps || !firstPps || !firstIdr) && packetNumber <= 1800) {
            val mask = nalMask(data)
            if (!firstSps && mask and 1 != 0) {
                firstSps = true
                log("FrameTrace: first SPS at packet $packetNumber after ${ms(now - startedNs)} ms")
            }
            if (!firstPps && mask and 2 != 0) {
                firstPps = true
                log("FrameTrace: first PPS at packet $packetNumber after ${ms(now - startedNs)} ms")
            }
            if (!firstIdr && mask and 4 != 0) {
                firstIdr = true
                log("FrameTrace: first IDR at packet $packetNumber after ${ms(now - startedNs)} ms")
            }
        }
    }

    @Synchronized fun queued() {
        queued++
    }

    @Synchronized fun inputUnavailable() {
        unavailable++
    }

    @Synchronized fun output(packetNumber: Int) {
        val now = SystemClock.elapsedRealtimeNanos()
        if (lastOutputNs != 0L && now >= lastOutputNs)
            maxOutputGapNs = maxOf(maxOutputGapNs, now - lastOutputNs)
        lastOutputNs = now
        output++
        if (!firstOutput) {
            firstOutput = true
            log(
                "FrameTrace: first decoder output after ${ms(now - startedNs)} ms, received packet $packetNumber"
            )
        }
    }

    // MediaCodec invokes this on a dedicated HandlerThread, separate from input and UI.
    fun rendered(presentationNanoTime: Long) {
        val now =
            if (presentationNanoTime > 0) presentationNanoTime
            else SystemClock.elapsedRealtimeNanos()
        val previous = lastRenderNs.getAndSet(now)
        if (previous > 0 && now >= previous)
            maxRenderGapNs.accumulateAndGet(now - previous) { old, value -> maxOf(old, value) }
        rendered.incrementAndGet()
    }

    @Synchronized fun tick() {
        val now = SystemClock.elapsedRealtimeNanos()
        if (now - windowNs < 1_000_000_000L) return
        log(
            "FrameTrace: window=${ms(now-windowNs)}ms rx=$received rxGapMin=${if (minReceiveGapNs == Long.MAX_VALUE) 0 else ms(minReceiveGapNs)}ms rxGapMax=${ms(maxReceiveGapNs)}ms queueDepthMax=$queueDepthMax queued=$queued decoded=$output renderCallbacks=${rendered.getAndSet(0)} renderGapMax=${ms(maxRenderGapNs.getAndSet(0))}ms inputUnavailable=$unavailable decodeGapMax=${ms(maxOutputGapNs)}ms"
        )
        windowNs = now
        received = 0
        queueDepthMax = 0
        queued = 0
        output = 0
        unavailable = 0
        maxReceiveGapNs = 0
        minReceiveGapNs = Long.MAX_VALUE
        maxOutputGapNs = 0
    }

    private fun ms(nanos: Long) = "%.2f".format(java.util.Locale.US, nanos / 1_000_000.0)

    private fun nalMask(data: ByteArray): Int {
        var mask = 0
        var i = 0
        while (i + 4 < data.size) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte()) {
                val start =
                    when {
                        data[i + 2] == 1.toByte() -> i + 3
                        i + 4 < data.size &&
                            data[i + 2] == 0.toByte() &&
                            data[i + 3] == 1.toByte() -> i + 4
                        else -> -1
                    }
                if (start >= 0 && start < data.size) {
                    when (data[start].toInt() and 0x1f) {
                        7 -> mask = mask or 1
                        8 -> mask = mask or 2
                        5 -> mask = mask or 4
                    }
                    i = start
                }
            }
            i++
        }
        return mask
    }
}
