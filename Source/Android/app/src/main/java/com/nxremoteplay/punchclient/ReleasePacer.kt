package com.nxremoteplay.punchclient

import android.media.MediaCodec
import android.os.Handler
import android.os.HandlerThread
import android.view.Choreographer
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread

/**
 * Hands decoded pictures to the display one per refresh. Releasing a picture when its packet happens to
 * be handled (or at a render time the platform does not honour) lets two pictures land in the same
 * refresh and costs the one before it (measured on the S24+: about 10-16% of pictures never reached the
 * screen). Here a thread takes pictures out of the decoder as soon as they are ready, and a Choreographer
 * callback releases the newest due picture at each display refresh. Future pictures stay queued until
 * their presentation time; obsolete decoded pictures are discarded after a network stall. [maxHeld]
 * bounds codec buffers retained while waiting for their deadlines.
 *
 * [onDecoded] runs on the drain thread for every decoded picture and returns false for one that must not
 * be shown.
 */
internal class ReleasePacer(
    private val codec: () -> MediaCodec,
    maxHeld: Int = 6,
    private val onDecoded: (ptsUs: Long) -> Boolean,
) {
    private val held = FrameReleaseQueue(maxHeld)
    private val releasedCount = AtomicInteger()
    private val droppedCount = AtomicInteger()
    private val failure = AtomicReference<MediaCodec.CodecException?>(null)
    @Volatile private var running = true
    private val releaseThread = HandlerThread("NX-Release").apply { start() }
    private var choreographer: Choreographer? = null

    private val frameCallback =
        object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (!running) return
                try {
                    val release = held.takeDue(frameTimeNanos / 1000)
                    for (old in release.discarded) {
                        codec().releaseOutputBuffer(old.index, false)
                        droppedCount.incrementAndGet()
                    }
                    val next = release.frame
                    if (next != null) {
                        codec().releaseOutputBuffer(next.index, true)
                        releasedCount.incrementAndGet()
                    }
                } catch (ex: MediaCodec.CodecException) {
                    failure.compareAndSet(null, ex)
                } catch (_: IllegalStateException) {
                    held.clear() // the codec was reset or stopped; its buffers are gone
                }
                choreographer?.postFrameCallback(this)
            }
        }

    private val drainThread =
        thread(name = "NX-Drain", isDaemon = true) {
            val info = MediaCodec.BufferInfo()
            while (running) {
                try {
                    val c = codec()
                    val index = c.dequeueOutputBuffer(info, 2_000)
                    if (index >= 0) {
                        if (onDecoded(info.presentationTimeUs)) {
                            held.add(index, info.presentationTimeUs)?.let { old ->
                                c.releaseOutputBuffer(old.index, false)
                                droppedCount.incrementAndGet()
                            }
                        } else c.releaseOutputBuffer(index, false)
                    }
                } catch (ex: MediaCodec.CodecException) {
                    failure.compareAndSet(null, ex)
                    Thread.sleep(20)
                } catch (_: IllegalStateException) {
                    Thread.sleep(5)
                }
            }
        }

    init {
        Handler(releaseThread.looper).post {
            val c = Choreographer.getInstance()
            choreographer = c
            c.postFrameCallback(frameCallback)
        }
    }

    val released: Int get() = releasedCount.get()
    val dropped: Int get() = droppedCount.get()

    /** A codec error seen on the drain or release thread, handed to the stream loop (it restarts the decoder). */
    fun takeFailure(): MediaCodec.CodecException? = failure.getAndSet(null)

    /** Forget waiting pictures; call before the codec is reset, because their buffer indices die with it. */
    fun clear() = held.clear()

    fun stop() {
        running = false
        drainThread.join(500)
        releaseThread.quitSafely()
        held.clear()
    }
}
