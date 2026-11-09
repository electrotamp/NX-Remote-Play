package com.nxremoteplay.punchclient

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Decodes the console's Opus game audio (PacketMeta_Audio_Opus: 48 kHz stereo, 20 ms packets) with the
 * phone's own Opus decoder. The output is the same 16-bit PCM the playback path already takes.
 */
internal class OpusAudioDecoder private constructor(private val codec: MediaCodec) {
    private val info = MediaCodec.BufferInfo()
    private var ptsUs = 0L

    /** Feeds one Opus packet; passes on every PCM chunk the codec has ready (the last one may come with the next packet). */
    fun decode(packet: ByteArray, size: Int, sink: (ByteArray, Int) -> Unit) {
        val index = codec.dequeueInputBuffer(5_000)
        if (index >= 0) {
            val input = codec.getInputBuffer(index) ?: return
            input.clear()
            input.put(packet, 0, size)
            codec.queueInputBuffer(index, 0, size, ptsUs, 0)
            ptsUs += FRAME_US
        }
        while (true) {
            val out = codec.dequeueOutputBuffer(info, 0)
            if (out >= 0) {
                val buffer = codec.getOutputBuffer(out)
                if (buffer != null && info.size > 0) {
                    val pcm = ByteArray(info.size)
                    buffer.position(info.offset)
                    buffer.get(pcm)
                    sink(pcm, pcm.size)
                }
                codec.releaseOutputBuffer(out, false)
            } else if (out == MediaCodec.INFO_TRY_AGAIN_LATER) {
                break
            }
        }
    }

    fun close() {
        try {
            codec.stop()
        } catch (_: Exception) {}
        try {
            codec.release()
        } catch (_: Exception) {}
    }

    companion object {
        const val MIME = "audio/opus"
        const val SAMPLE_RATE = 48_000
        const val CHANNELS = 2
        private const val FRAME_US = 20_000L
        private const val PRE_ROLL_NS = 80_000_000L

        /** The 19-byte OpusHead the decoder is configured with (no pre-skip, no gain, mapping family 0). */
        fun head(): ByteArray =
            ByteBuffer.allocate(19)
                .order(ByteOrder.LITTLE_ENDIAN)
                .put("OpusHead".toByteArray(Charsets.US_ASCII))
                .put(1) // version
                .put(CHANNELS.toByte())
                .putShort(0) // pre-skip
                .putInt(SAMPLE_RATE)
                .putShort(0) // output gain
                .put(0) // channel mapping family
                .array()

        private fun longLe(value: Long): ByteArray =
            ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array()

        private fun format(): MediaFormat =
            MediaFormat.createAudioFormat(MIME, SAMPLE_RATE, CHANNELS).apply {
                setByteBuffer("csd-0", ByteBuffer.wrap(head()))
                setByteBuffer("csd-1", ByteBuffer.wrap(longLe(0)))
                setByteBuffer("csd-2", ByteBuffer.wrap(longLe(PRE_ROLL_NS)))
            }

        /** True when this phone has an Opus decoder; only then does the app ask the console for Opus. */
        fun supported(): Boolean =
            try {
                MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(format()) != null
            } catch (_: Exception) {
                false
            }

        fun create(): OpusAudioDecoder? =
            try {
                val codec = MediaCodec.createDecoderByType(MIME)
                codec.configure(format(), null, null, 0)
                codec.start()
                OpusAudioDecoder(codec)
            } catch (_: Exception) {
                null
            }
    }
}
