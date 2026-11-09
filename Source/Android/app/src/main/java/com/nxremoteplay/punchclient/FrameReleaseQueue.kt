package com.nxremoteplay.punchclient

import java.util.ArrayDeque

/**
 * Keeps decoded buffers bounded and hands out one picture per display refresh, oldest first. Two
 * pictures often fall due in the same refresh at 60 fps on a 60 Hz screen; showing the newer one and
 * discarding the older one turned ordinary arrival jitter into skipped frames (about 4% of them on
 * Wi-Fi), so the older one is shown and the newer one a refresh later. A picture is discarded only
 * when it is more than [lateDropUs] (two 60 Hz refreshes) past its time and a newer one is already due,
 * which bounds how far the picture can fall behind after a stall.
 */
internal class FrameReleaseQueue(
    private val capacity: Int = 6,
    private val lateDropUs: Long = LATE_DROP_US,
) {
    data class Frame(val index: Int, val ptsUs: Long)
    data class Release(val frame: Frame?, val discarded: List<Frame>)

    private val frames = ArrayDeque<Frame>()

    init {
        require(capacity > 0)
    }

    @Synchronized
    fun add(index: Int, ptsUs: Long): Frame? {
        val overflow = if (frames.size == capacity) frames.removeFirst() else null
        frames.addLast(Frame(index, ptsUs))
        return overflow
    }

    @Synchronized
    fun takeDue(nowUs: Long): Release {
        val discarded = mutableListOf<Frame>()
        while (frames.size >= 2) {
            val first = frames.first()
            val second = frames.elementAt(1)
            if (second.ptsUs > nowUs || nowUs - first.ptsUs <= lateDropUs) break
            discarded.add(frames.removeFirst())
        }
        val selected = if (frames.peekFirst()?.let { it.ptsUs <= nowUs } == true) frames.removeFirst() else null
        return Release(selected, discarded)
    }

    @Synchronized
    fun clear() = frames.clear()

    companion object {
        const val LATE_DROP_US = 34_000L
    }
}
