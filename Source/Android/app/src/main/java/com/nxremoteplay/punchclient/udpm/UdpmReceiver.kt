package com.nxremoteplay.punchclient.udpm

/**
 * Kotlin port of udpm_receiver.c (same rules, same tests): reassembles FEC blocks, rebuilds lost
 * shards, delivers frames in order, and after an unrecoverable loss withholds P-frames until the
 * next keyframe so the decoder never sees a broken reference chain. Not thread-safe: drive it from
 * the single UDP receive thread.
 */
class UdpmReceiver(
    private val streamType: Int,
    private val session: Int,
    private val onFrame:
        (frameId: Int, data: ByteArray, size: Int, timestampUs: Long, flags: Int) -> Unit,
) {
    var holdUs = 40_000L
    var nackAfterUs = 8_000L

    class Stats {
        var packetsReceived = 0
        var shardsRecoveredFec = 0
        var framesDelivered = 0
        var framesDropped = 0
        var framesSkippedWaitingKeyframe = 0
        var firstSeq = 0
        var highestSeq = 0
        var seqSeen = false
        var jitterUs = 0L
        val packetsExpected
            get() = if (seqSeen) highestSeq - firstSeq + 1 else 0

        val packetsLost
            get() = maxOf(0, packetsExpected - packetsReceived)
    }

    val stats = Stats()

    private class Block(val k: Int, val m: Int, val shardSize: Int, var lastUs: Long) {
        val buf = ByteArray((k + m) * shardSize)
        val present = BooleanArray(k + m)
        var have = 0
        var done = false
        var nacks = 0
    }

    private class Frame(
        val id: Int,
        val size: Int,
        val blockCount: Int,
        val flags: Int,
        val timestampUs: Long
    ) {
        val blocks = arrayOfNulls<Block>(blockCount)
        var blocksDone = 0
        var completeUs = 0L
        val complete
            get() = blocksDone == blockCount
    }

    private val frames = HashMap<Int, Frame>()
    private var nextId = 0
    private var started = false
    var waitingKeyframe = streamType == UdpmProto.TYPE_VIDEO
        private set

    val nextFrameId
        get() = nextId

    private var transitPrev = 0L
    private var transitInit = false

    private fun diff(a: Int, b: Int) = a - b // wrapping int arithmetic, same as the C seqDiff

    fun push(dg: ByteArray, len: Int, nowUs: Long): Boolean {
        val h = ShardHeader.parse(dg, len) ?: return false
        if (h.type != streamType || h.session != session) return false
        if (!started) {
            nextId = h.frameId
            started = true
        }
        // Loss statistics count every valid datagram, including parity that arrives after its frame
        // was already delivered; only exact repeats of a tracked shard are taken back out.
        if (!stats.seqSeen) {
            stats.firstSeq = h.packetSeq
            stats.highestSeq = h.packetSeq
            stats.seqSeen = true
        } else if (diff(h.packetSeq, stats.highestSeq) > 0) stats.highestSeq = h.packetSeq
        stats.packetsReceived++
        if (diff(h.frameId, nextId) < 0) return false
        while (diff(h.frameId, nextId) >= WINDOW) { // fell far behind: give up on the oldest
            frames.remove(nextId)
            stats.framesDropped++
            if (streamType == UdpmProto.TYPE_VIDEO) waitingKeyframe = true
            nextId++
        }
        val f =
            frames.getOrPut(h.frameId) {
                val transit = nowUs - h.timestampUs
                if (transitInit)
                    stats.jitterUs += (Math.abs(transit - transitPrev) - stats.jitterUs) / 16
                transitPrev = transit
                transitInit = true
                Frame(
                    h.frameId,
                    h.frameSize,
                    h.blockCount,
                    h.flags and UdpmProto.FLAG_KEYFRAME,
                    h.timestampUs)
            }
        if (f.size != h.frameSize || f.blockCount != h.blockCount) return false
        val b =
            f.blocks[h.blockIndex]
                ?: Block(h.dataShards, h.parityShards, h.shardSize, nowUs).also {
                    f.blocks[h.blockIndex] = it
                }
        if (b.k != h.dataShards || b.m != h.parityShards || b.shardSize != h.shardSize) return false
        if (b.present[h.shardIndex]) {
            stats.packetsReceived--
            return true
        }
        b.present[h.shardIndex] = true
        b.lastUs = nowUs
        if (b.done) return true
        System.arraycopy(dg, UdpmProto.HEADER_SIZE, b.buf, h.shardIndex * b.shardSize, b.shardSize)
        b.have++
        if (b.have >= b.k) {
            val rebuilt = UdpmFec.decode(b.buf, b.present, b.k, b.m, b.shardSize)
            if (rebuilt >= 0) {
                stats.shardsRecoveredFec += rebuilt
                b.done = true
                if (++f.blocksDone == f.blockCount) f.completeUs = nowUs
            }
        }
        poll(nowUs)
        return true
    }

    /** Delivers or drops frames whose wait expired. Call every few ms even without traffic. */
    fun poll(nowUs: Long) {
        if (!started) return
        while (true) {
            val f = frames[nextId]
            if (f != null && f.complete) {
                deliver(f)
                frames.remove(nextId)
                nextId++
                continue
            }
            var expired = false
            for (i in 1 until WINDOW) {
                val n = frames[nextId + i] ?: continue
                if (n.complete) {
                    expired = nowUs - n.completeUs >= holdUs
                    break
                }
            }
            if (!expired) return
            frames.remove(nextId)
            stats.framesDropped++
            if (streamType == UdpmProto.TYPE_VIDEO) waitingKeyframe = true
            nextId++
        }
    }

    private fun deliver(f: Frame) {
        val key = f.flags and UdpmProto.FLAG_KEYFRAME != 0
        if (waitingKeyframe && !key) {
            stats.framesSkippedWaitingKeyframe++
            return
        }
        waitingKeyframe = false
        val out = ByteArray(f.size)
        val blockBytes = f.blocks[0]!!.k * f.blocks[0]!!.shardSize
        var off = 0
        for (b in 0 until f.blockCount) {
            val want = minOf(blockBytes, f.size - b * blockBytes)
            System.arraycopy(f.blocks[b]!!.buf, 0, out, off, want)
            off += want
        }
        stats.framesDelivered++
        onFrame(f.id, out, f.size, f.timestampUs, f.flags)
    }

    /** Blocks still short of k shards after nackAfterUs (twice at most per block). */
    fun collectNacks(nowUs: Long, max: Int = 16): List<Nack> {
        val out = ArrayList<Nack>()
        for (f in frames.values) for ((bi, b) in f.blocks.withIndex()) {
            if (out.size >= max) return out
            // Only once the block has gone quiet: a paced block legitimately takes ~20 ms to
            // arrive.
            if (b == null ||
                b.done ||
                b.nacks >= 2 ||
                nowUs - b.lastUs < nackAfterUs * (b.nacks + 1))
                continue
            var lo = 0L
            var hi = 0L
            for (s in 0 until b.k + b.m) if (!b.present[s]) {
                if (s < 64) lo = lo or (1L shl s) else hi = hi or (1L shl (s - 64))
            }
            b.nacks++
            out.add(Nack(f.id, bi, streamType, lo, hi))
        }
        return out
    }

    companion object {
        const val WINDOW = 32
    }
}
