package com.nxremoteplay.punchclient.udpm

import com.nxremoteplay.punchclient.NetworkStatus
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import kotlin.concurrent.thread

/**
 * NX-Remote-Play: receives the console's video over UDPM (see PROTOCOL.md / host UdpmVideo.c). One
 * thread owns the socket: Hello keepalive, reassembly + FEC, NACKs, keyframe requests and feedback.
 * Every complete video frame is the console's original wire packet (18-byte NxRemotePlay header +
 * payload) and goes to [onWire]; audio frames travel on the same socket as UDPM audio and go to
 * [onAudioWire]. [onEnd] fires once if the console goes silent for [silentLimitMs].
 */
class UdpmVideoChannel(
    private val console: InetAddress,
    private val port: Int,
    private val session: Int,
    private val onWire: (ByteArray) -> Unit,
    private val onEnd: (String) -> Unit,
    private val onLog: (String) -> Unit,
    private val onAudioWire: (ByteArray) -> Unit = {},
    private val silentLimitMs: Long = 15_000,
    private val natKey: ByteArray? = null,
    private val rendezvous: InetSocketAddress? = null,
    private val audioUnderruns: () -> Int = { 0 },
) {
    @Volatile
    var receivedMedia = false
        private set

    @Volatile private var running = false
    private var worker: Thread? = null
    @Volatile
    var playoutDelayUs = 0L
        private set

    fun start(): UdpmVideoChannel {
        running = true
        worker = thread(name = "udpm-video", isDaemon = true) { run() }
        return this
    }

    @Volatile private var keyframesWanted = 0

    /** Asks the console for a fresh keyframe (a restarted decoder needs one); repeated in case of loss. */
    fun requestKeyframe() {
        keyframesWanted = 3
    }

    fun stop() {
        running = false
        worker?.join(500)
        worker = null
    }

    private fun now() = System.nanoTime() / 1000

    private fun run() {
        var target = InetSocketAddress(console, port)
        var alt: InetSocketAddress? =
            null // rendezvous-discovered endpoint, tried IN ADDITION to the console address
        val sock =
            try {
                DatagramSocket().apply {
                    receiveBufferSize = 4 shl 20
                    soTimeout = 2
                }
            } catch (e: Exception) {
                onEnd("UDP socket failed: $e")
                return
            }
        fun send(b: ByteArray, to: InetSocketAddress = target) {
            try {
                sock.send(DatagramPacket(b, b.size, to))
            } catch (_: Exception) {}
        }
        // Same settings as the schedule the decoder uses, so the logged delay is the real one.
        val playout =
            PlayoutClock(
                decodeAllowanceUs = 4_000,
                bandUs = 6_000,
                maxJitterUs = PresentationSchedule.MAX_JITTER_US,
                floorWindowUs = PresentationSchedule.FLOOR_WINDOW_US,
                decayQuietUs = PresentationSchedule.SCHEDULE_DECAY_QUIET_US,
                warmupUs = PresentationSchedule.WARMUP_US,
            )
        val congestion = CongestionMeter()
        val rx =
            UdpmReceiver(UdpmProto.TYPE_VIDEO, session) { _, data, size, ts, _ ->
                playoutDelayUs =
                    playout.onFrame(ts, now()) - now() // logged; the decoder keeps its own schedule
                congestion.onFrame(ts, now())
                receivedMedia = true
                onWire(if (size == data.size) data else data.copyOf(size))
            }
        // Audio rides the same socket as its own UDPM stream (own frame ids, FEC and NACKs); a lost
        // audio frame is skipped after the hold time, never waited for.
        val rxAudio =
            UdpmReceiver(UdpmProto.TYPE_AUDIO, session) { _, data, size, _, _ ->
                onAudioWire(if (size == data.size) data else data.copyOf(size))
            }
        var audioNacks = 0
        val buf = ByteArray(UdpmProto.MAX_DATAGRAM + 64)
        val pkt = DatagramPacket(buf, buf.size)
        var lastHeard = now()
        var nextHello = 0L
        var nextFb = 0L
        var nextKey = 0L
        var nextLog = now() + 5_000_000
        var firstPacket = 0L
        var nextRegister = 0L
        var regSent = 0
        var regFail = 0
        var relayRx = 0
        var otherRx = 0
        var lastErr = ""
        val key = natKey?.takeIf { it.size == 16 }
        var fbSeq = 0
        var nacks = 0
        var keyReqs = 0
        onLog(
            "UDPM video: listening for console $console:$port session ${Integer.toHexString(session)}"
        )
        try {
            while (running) {
                val t0 = now()
                if (firstPacket == 0L && key != null && rendezvous != null && t0 >= nextRegister) {
                    try {
                        sock.send(
                            DatagramPacket(
                                UdpNat.packet(2, session, key).let { it },
                                28,
                                rendezvous,
                            )
                        )
                        regSent++
                    } catch (e: Exception) {
                        regFail++
                        lastErr = e.javaClass.simpleName + ":" + e.message
                    }
                    if (regSent == 1)
                        onLog(
                            "rendezvous: first registration sent to $rendezvous from local port ${sock.localPort}"
                        )
                    // Keep opening the peer pinhole from this very same media socket.
                    send(UdpNat.packet(4, session, key))
                    alt?.let { send(UdpNat.packet(4, session, key), it) }
                    nextRegister = t0 + 250_000
                }
                // Hello twice a second until video flows, then once a second (NAT keepalive +
                // liveness).
                if (t0 >= nextHello) {
                    send(UdpmProto.hello(session, fbSeq))
                    alt?.let { send(UdpmProto.hello(session, fbSeq), it) }
                    nextHello = t0 + if (firstPacket == 0L) 500_000 else 1_000_000
                }
                try {
                    sock.soTimeout = 2
                    while (true) {
                        pkt.length = buf.size
                        sock.receive(pkt)
                        if (
                            key != null &&
                                rendezvous != null &&
                                pkt.address == rendezvous.address &&
                                pkt.port == rendezvous.port
                        ) {
                            relayRx++
                            val peer = UdpNat.peer(buf, pkt.length, session, key)
                            if (peer != null) {
                                if (peer != target && peer != alt && firstPacket == 0L) {
                                    alt = peer
                                    onLog(
                                        "UDP rendezvous: extra endpoint $peer (keeping console address $target)"
                                    )
                                }
                                alt?.let {
                                    send(UdpNat.packet(4, session, key), it)
                                    send(UdpmProto.hello(session, fbSeq), it)
                                }
                            }
                            continue
                        }
                        val fromTarget = pkt.address == target.address && pkt.port == target.port
                        val a = alt
                        val fromAlt = a != null && pkt.address == a.address && pkt.port == a.port
                        if (!fromTarget && !fromAlt) {
                            otherRx++
                            continue
                        }
                        if (fromAlt && !fromTarget && firstPacket == 0L) {
                            onLog("UDP: first packet arrived from $a")
                            target = a!!
                            alt = null
                        } else if (fromTarget && firstPacket == 0L) alt = null
                        if (
                            key != null &&
                                UdpNat.matches(buf, pkt.length, session, key) &&
                                buf[4].toInt() == 4
                        ) {
                            send(UdpmProto.hello(session, fbSeq))
                            continue
                        }
                        val accepted = rx.push(buf, pkt.length, now()) || rxAudio.push(buf, pkt.length, now())
                        if (accepted) {
                            lastHeard = now()
                            if (firstPacket == 0L) {
                                firstPacket = lastHeard
                                onLog("UDPM video: first datagram received")
                            }
                        }
                        sock.soTimeout = 1
                    }
                } catch (_: SocketTimeoutException) {}
                val t = now()
                rx.poll(t)
                for (n in rx.collectNacks(t)) {
                    send(UdpmProto.nack(session, n))
                    nacks++
                }
                rxAudio.poll(t)
                for (n in rxAudio.collectNacks(t)) {
                    send(UdpmProto.nack(session, n))
                    audioNacks++
                }
                if (
                    rx.waitingKeyframe &&
                        firstPacket != 0L &&
                        t - firstPacket > 300_000 &&
                        t >= nextKey
                ) {
                    send(UdpmProto.keyframeRequest(session, rx.nextFrameId - 1))
                    keyReqs++
                    nextKey = t + 250_000
                }
                if (keyframesWanted > 0 && t >= nextKey) {
                    send(UdpmProto.keyframeRequest(session, rx.nextFrameId - 1))
                    keyReqs++
                    keyframesWanted--
                    nextKey = t + 100_000
                }
                val s = rx.stats
                if (congestion.due(t)) {
                    congestion.report(t, nacks, audioUnderruns())?.let {
                        send(UdpmProto.congestion(session, it))
                    }
                }
                if (t >= nextFb) {
                    send(
                        UdpmProto.feedback(
                            session,
                            Feedback(
                                fbSeq++,
                                rx.nextFrameId,
                                s.packetsReceived,
                                s.packetsLost,
                                s.shardsRecoveredFec,
                                s.framesDropped,
                                s.jitterUs.toInt(),
                                playout.targetDelayUs.toInt(),
                            ),
                        )
                    )
                    nextFb = t + 250_000
                }
                if (t >= nextLog) {
                    if (key != null)
                        onLog(
                            "rendezvous 5s: registrations sent=$regSent failed=$regFail relay_replies=$relayRx other_source_rx=$otherRx last_error=$lastErr"
                        )
                    onLog(
                        "UDPM video 5s: delivered=${s.framesDelivered} pkts=${s.packetsReceived} lost=${s.packetsLost} fec=${s.shardsRecoveredFec} " +
                            "nacks=$nacks keyreq=$keyReqs dropped=${s.framesDropped} skipped=${s.framesSkippedWaitingKeyframe} " +
                            "jitter=${s.jitterUs / 1000.0}ms playout=${playout.targetDelayUs / 1000.0}ms ${congestion.summary()} net=[${NetworkStatus.brief}]"
                    )
                    val a = rxAudio.stats
                    onLog(
                        "UDPM audio 5s: delivered=${a.framesDelivered} pkts=${a.packetsReceived} lost=${a.packetsLost} fec=${a.shardsRecoveredFec} nacks=$audioNacks dropped=${a.framesDropped}"
                    )
                    nextLog = t + 5_000_000
                }
                if (t - lastHeard > silentLimitMs * 1000) {
                    onEnd("UDP video silent for ${silentLimitMs / 1000} s")
                    break
                }
            }
        } catch (e: Exception) {
            onEnd("UDP video failed: ${e.javaClass.simpleName}: ${e.message}")
        } finally {
            sock.close()
            val s = rx.stats
            onLog(
                "UDPM video total: delivered=${s.framesDelivered} pkts=${s.packetsReceived} lost=${s.packetsLost} fec=${s.shardsRecoveredFec} dropped=${s.framesDropped} skipped=${s.framesSkippedWaitingKeyframe}"
            )
        }
    }
}
