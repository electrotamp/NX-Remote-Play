package com.nxremoteplay.punchclient

/** One audio owner. Retains partial writes; fixed capacity, no silent discard. */
internal class PendingPcm(private val capacity: Int = 32768) {
    private val data = ByteArray(capacity)
    private var head = 0
    var pending = 0
        private set

    var received = 0L
        private set

    var written = 0L
        private set

    var shutdown = 0L
        private set

    init {
        require(capacity > 0 && capacity % 4 == 0)
    }

    fun append(src: ByteArray, size: Int) {
        require(size >= 0 && size <= src.size && size % 4 == 0)
        check(size <= capacity - pending) {
            "PCM pending buffer full; ending stream explicitly"
        }
        val tail = (head + pending) % capacity
        val first = minOf(size, capacity - tail)
        src.copyInto(data, tail, 0, first)
        if (first < size) src.copyInto(data, 0, first, size)
        pending += size
        received += size
    }

    /** Backpressure the audio producer, not the UDP video decoder. No larger queue. */
    fun admit(
        src: ByteArray,
        size: Int,
        drainAudio: () -> Unit,
        now: () -> Long = System::nanoTime,
        pause: () -> Unit = { Thread.sleep(2) },
        cancelled: () -> Boolean = { false }
    ): Boolean {
        require(size in 0..minOf(src.size, capacity) && size % 4 == 0)
        val start = now()
        while (true) {
            if (cancelled()) return false
            drainAudio()
            if (size <= capacity - pending) {
                append(src, size)
                return true
            }
            check(now() - start < 250_000_000L) {
                "PCM output stalled for 250 ms; pending=$pending; no audio discarded"
            }
            pause()
        }
    }

    fun drain(write: (ByteArray, Int, Int) -> Int) {
        // At most two contiguous portions; no spin on zero/partial writes.
        repeat(2) {
            if (pending == 0) return
            val n = minOf(pending, capacity - head)
            val accepted = write(data, head, n)
            check(accepted >= 0 && accepted <= n) { "PCM write failed: $accepted" }
            head = (head + accepted) % capacity
            pending -= accepted
            written += accepted
            check(accepted % 4 == 0) { "PCM write unaligned: $accepted; ending stream" }
            if (accepted < n) return
        }
    }

    fun close() {
        shutdown += pending
        pending = 0
        head = 0
    }
}
