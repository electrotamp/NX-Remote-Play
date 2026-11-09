package com.nxremoteplay.punchclient

/**
 *  A/V sync: keep the PCM queue ahead of the speaker near the user's audio-delay target. Pure,
 * JVM-testable.
 */
internal object AvSyncPolicy {
    enum class Action {
        KEEP,
        SKIP_PACKET
    }

    // Playing faster would change the sound, so a queue that has grown is trimmed by dropping an
    // arriving packet (about 43 ms) instead. The queue normally peaks at four packets (171 ms) right
    // after a write and packets can arrive back to back, so the margin must leave room for that:
    // 60 ms (tried first) dropped nearly all audio.
    const val SKIP_ABOVE_MS = 120.0 // drop an arriving packet above target + this (bounded latency)

    fun decide(queueMs: Double, targetMs: Int): Action =
        if (queueMs > targetMs + SKIP_ABOVE_MS) Action.SKIP_PACKET else Action.KEEP
}
