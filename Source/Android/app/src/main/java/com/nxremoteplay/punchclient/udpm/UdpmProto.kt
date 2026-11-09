package com.nxremoteplay.punchclient.udpm

// Kotlin mirror of Source/Host/source/udpm/udpm_proto.h. Little-endian throughout.
object UdpmProto {
    const val MAGIC = 0xD7
    const val VERSION = 1
    const val HEADER_SIZE = 36
    const val MAX_SHARD_SIZE = 1400
    const val MAX_DATAGRAM = HEADER_SIZE + MAX_SHARD_SIZE
    const val MAX_DATA_SHARDS = 64
    const val MAX_PARITY_SHARDS = 32

    const val TYPE_VIDEO = 1
    const val TYPE_AUDIO = 2
    const val TYPE_PROBE = 3
    const val TYPE_HELLO = 0x10
    const val TYPE_NACK = 0x11
    const val TYPE_FEEDBACK = 0x12
    const val TYPE_CONGESTION = 0x14
    const val TYPE_KEYFRAME_REQUEST = 0x13

    const val FLAG_KEYFRAME = 1
    const val FLAG_PARITY = 2

    const val NACK_SIZE = 32
    const val FEEDBACK_SIZE = 40
    const val CONGESTION_SIZE = 40
    const val KEYFRAME_REQUEST_SIZE = 12
    const val HELLO_SIZE = 12

    fun u8(b: ByteArray, o: Int) = b[o].toInt() and 0xFF

    fun u16(b: ByteArray, o: Int) = u8(b, o) or (u8(b, o + 1) shl 8)

    fun u32(b: ByteArray, o: Int) = (u16(b, o).toLong() or (u16(b, o + 2).toLong() shl 16)).toInt()

    fun u64(b: ByteArray, o: Int) =
        (u32(b, o).toLong() and 0xFFFFFFFFL) or (u32(b, o + 4).toLong() shl 32)

    fun put32(b: ByteArray, o: Int, v: Int) {
        for (i in 0 until 4) b[o + i] = (v ushr (8 * i)).toByte()
    }

    fun put64(b: ByteArray, o: Int, v: Long) {
        for (i in 0 until 8) b[o + i] = (v ushr (8 * i)).toByte()
    }

    private fun prefix(type: Int, session: Int, size: Int): ByteArray {
        val b = ByteArray(size)
        b[0] = MAGIC.toByte()
        b[1] = VERSION.toByte()
        b[2] = type.toByte()
        put32(b, 4, session)
        return b
    }


    fun hello(session: Int, nonce: Int) =
        prefix(TYPE_HELLO, session, HELLO_SIZE).also { put32(it, 8, nonce) }

    fun keyframeRequest(session: Int, lastGoodFrameId: Int) =
        prefix(TYPE_KEYFRAME_REQUEST, session, KEYFRAME_REQUEST_SIZE).also {
            put32(it, 8, lastGoodFrameId)
        }

    fun nack(session: Int, n: Nack) =
        prefix(TYPE_NACK, session, NACK_SIZE).also {
            put32(it, 8, n.frameId)
            it[12] = n.blockIndex.toByte()
            it[13] = n.streamType.toByte()
            put64(it, 16, n.missingLo)
            put64(it, 24, n.missingHi)
        }

    /** Body: seq, interval ms, frames, smallest and largest delay, nacks, audio underruns, 0 (all u32 LE). */
    fun congestion(session: Int, c: Congestion) =
        prefix(TYPE_CONGESTION, session, CONGESTION_SIZE).also {
            val v =
                intArrayOf(
                    c.seq,
                    c.intervalMs,
                    c.frames,
                    c.excessMinUs,
                    c.excessMaxUs,
                    c.nacks,
                    c.audioUnderruns,
                    0)
            for (i in v.indices) put32(it, 8 + 4 * i, v[i])
        }

    fun feedback(session: Int, f: Feedback) =
        prefix(TYPE_FEEDBACK, session, FEEDBACK_SIZE).also {
            val v =
                intArrayOf(
                    f.seq,
                    f.highestFrameId,
                    f.packetsReceived,
                    f.packetsLost,
                    f.shardsRecoveredFec,
                    f.framesDropped,
                    f.jitterUs,
                    f.playoutDelayUs)
            for (i in v.indices) put32(it, 8 + 4 * i, v[i])
        }
}

data class ShardHeader(
    val type: Int,
    val flags: Int,
    val session: Int,
    val packetSeq: Int,
    val frameId: Int,
    val frameSize: Int,
    val blockIndex: Int,
    val blockCount: Int,
    val shardIndex: Int,
    val dataShards: Int,
    val parityShards: Int,
    val shardSize: Int,
    val timestampUs: Long,
) {
    companion object {
        /** Same validation as UdpmReadShardHeader; null for anything malformed or foreign. */
        fun parse(b: ByteArray, len: Int): ShardHeader? {
            if (len < UdpmProto.HEADER_SIZE ||
                UdpmProto.u8(b, 0) != UdpmProto.MAGIC ||
                UdpmProto.u8(b, 1) != UdpmProto.VERSION)
                return null
            val type = UdpmProto.u8(b, 2)
            if (type != UdpmProto.TYPE_VIDEO &&
                type != UdpmProto.TYPE_AUDIO &&
                type != UdpmProto.TYPE_PROBE)
                return null
            val h =
                ShardHeader(
                    type,
                    UdpmProto.u8(b, 3),
                    UdpmProto.u32(b, 4),
                    UdpmProto.u32(b, 8),
                    UdpmProto.u32(b, 12),
                    UdpmProto.u32(b, 16),
                    UdpmProto.u8(b, 20),
                    UdpmProto.u8(b, 21),
                    UdpmProto.u8(b, 22),
                    UdpmProto.u8(b, 23),
                    UdpmProto.u8(b, 24),
                    UdpmProto.u16(b, 26),
                    UdpmProto.u64(b, 28),
                )
            val total = h.dataShards + h.parityShards
            if (h.blockCount == 0 || h.blockIndex >= h.blockCount) return null
            if (h.dataShards == 0 ||
                h.dataShards > UdpmProto.MAX_DATA_SHARDS ||
                h.parityShards > UdpmProto.MAX_PARITY_SHARDS)
                return null
            if (h.shardIndex >= total || h.shardSize == 0 || h.shardSize > UdpmProto.MAX_SHARD_SIZE)
                return null
            if (len != UdpmProto.HEADER_SIZE + h.shardSize) return null
            if (((h.flags and UdpmProto.FLAG_PARITY) != 0) != (h.shardIndex >= h.dataShards))
                return null
            val maxFrame = h.blockCount.toLong() * UdpmProto.MAX_DATA_SHARDS * h.shardSize
            if (h.frameSize <= 0 || h.frameSize.toLong() > maxFrame) return null
            return h
        }
    }
}

data class Nack(
    val frameId: Int,
    val blockIndex: Int,
    val streamType: Int,
    val missingLo: Long,
    val missingHi: Long
)

data class Congestion(
    val seq: Int,
    val intervalMs: Int,
    val frames: Int,
    val excessMinUs: Int,
    val excessMaxUs: Int,
    val nacks: Int,
    val audioUnderruns: Int,
)

data class Feedback(
    val seq: Int,
    val highestFrameId: Int,
    val packetsReceived: Int,
    val packetsLost: Int,
    val shardsRecoveredFec: Int,
    val framesDropped: Int,
    val jitterUs: Int,
    val playoutDelayUs: Int,
)
