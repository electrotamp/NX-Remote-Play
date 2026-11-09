package com.nxremoteplay.punchclient

import java.io.InputStream
import java.net.SocketTimeoutException

/** Runs on the existing TCP reader. Partial reads/timeouts keep audio serviced. */
internal object TcpMediaPump {
    fun readFully(
        input: InputStream,
        dest: ByteArray,
        offset: Int = 0,
        length: Int = dest.size - offset,
        tick: () -> Unit,
        cancelled: () -> Boolean
    ): Boolean {
        require(offset >= 0 && length >= 0 && offset + length <= dest.size)
        var position = offset
        val end = offset + length
        while (position < end && !cancelled()) {
            tick()
            val count =
                try {
                    input.read(dest, position, end - position)
                } catch (_: SocketTimeoutException) {
                    continue
                }
            if (count < 0) return false
            if (count > 0) position += count
        }
        return position == end && !cancelled()
    }

    // Audio never enters the video queue. Return wire bytes for the media owner's feedback.
    fun route(wire: ByteArray, audio: (ByteArray) -> Unit, video: (ByteArray) -> Unit): Int {
        require(wire.size >= 18)
        return if ((wire[16].toInt() and 3) == 2) {
            audio(wire.copyOfRange(18, wire.size))
            wire.size
        } else {
            video(wire)
            0
        }
    }
}
