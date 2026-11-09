package com.nxremoteplay.punchclient

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import com.nxremoteplay.punchclient.udpm.PresentationSchedule
import java.io.File
import java.io.InputStream
import java.net.Socket

// NxRemotePlay TCP-bridge media client. Wire format is
// exactly stock NxRemotePlay's TCP_MODE protocol (protocol.md, proto.h, capture.h in the
// NxRemotePlay-Combined source tree) - this class doesn't know or care that the underlying
// socket came from a TCP hole-punch rather than a normal LAN connect; the handshake and
// streaming protocol are identical either way.
class NxRemotePlayVideoClient(
    // 6-byte connection ticket from PUNCHJOIN2 (null = none); goes into the Reserved
    // bytes of the
    // handshake request so the console accepts the connection even when NAT changed its source
    // address.
    private val connTicket: ByteArray? = null,
    private val onFeedback: (ByteArray) -> Boolean = { false },
    private val onLog: (String) -> Unit,
    // HOME/applet JPEG-fallback frames (this codebase's PacketMeta_Content_Screenshot) are
    // NOT H264 - MediaCodec can't touch them. Decoded here (off the UI thread - JPEG decode
    // is not free at up to ~30fps) and handed to the caller to display however it renders
    // stills; onRealVideoFrame fires once real H264 data resumes so the caller can switch
    // back. Both deliberately fire on every applicable packet rather than only on a detected
    // transition - cheap to call repeatedly, and the caller decides whether/how to no-op an
    // already-current mode.
    private val onJpegFrame: (Bitmap) -> Unit,
    private val onRealVideoFrame: () -> Unit,
    private val traceFile: File? = null,
    private val onSavedTraces: (File, File) -> Unit = { _, _ -> },
    // NX-Remote-Play: ask for video over UDPM (falls back to TCP video if not offered or no UDP
    // path).
    private val udpVideo: Boolean = false,
    private val udpRendezvous: java.net.InetSocketAddress? = null,
    //  A/V sync: how much PCM may sit queued ahead of the speaker (ms). Video is shown on
    // arrival, so a deeper
    // audio queue plays sound later than the picture; the user tunes this (Settings > Testing >
    // Audio delay).
    private val audioDelayMs: Int = 100,
    // 0 = decoder renders straight into the surface (as before), 1 = FSR, 2 = Cubic,
    // both through UpscaleRenderer
    private val upscaleMode: Int = 0,
    private val assets: android.content.res.AssetManager? = null,
) {

    // The server may be a hostname (default play.nxremoteplay.com): resolve it here, on the stream
    // thread,
    // never on the UI thread that constructs this client.
    private val rendezvousAddress: java.net.InetSocketAddress? by lazy {
        udpRendezvous?.let { a ->
            if (!a.isUnresolved) a
            else
                try {
                    java.net.InetSocketAddress(a.hostString, a.port).takeIf { !it.isUnresolved }
                } catch (e: Exception) {
                    onLog("UDP rendezvous name lookup failed: ${e.javaClass.simpleName}")
                    null
                }
        }
    }

    companion object {
        private const val HELLO_SIZE = 16
        private const val HANDSHAKE_REQUEST_SIZE = 16
        private const val HANDSHAKE_RESPONSE_SIZE = 72
        private const val PACKET_HEADER_SIZE = 18
        private const val HANDSHAKE_OK = 6

        // ProtoMeta_Video
        private const val META_FLAGS_VIDEO = 0x01
        // ProtoMetaVideo_InjectPPSSPS - required, see protocol.md: without this the stream
        // can never be decoded at all since GRC only emits real SPS/PPS once, at game start.
        private const val VIDEO_FLAGS_INJECT_PPS_SPS = 0x02
        // ProtoMeta_Audio. Audio is raw 16-bit little-endian PCM, 48 kHz stereo (capture.h AbufSz).
        private const val META_FLAGS_AUDIO = 0x02
        private const val AUDIO_BATCHING = 1
        private const val AUDIO_SAMPLE_RATE = 48000
        private const val AUDIO_BYTES_PER_SEC = AUDIO_SAMPLE_RATE * 4
        // NX-Remote-Play (host UdpmVideo.h): FeatureFlags bit, offer in response bytes 8..15.
        private const val UDPM_FEATURE = 0x20
        private const val OPUS_FEATURE = 0x80 // can decode Opus game audio
        private val END_OF_STREAM = ByteArray(0)

        private const val OPUS_PACKET = 0x80 // PacketMeta_Audio_Opus in the packet's meta byte

        /** Frames still waiting behind the one being decoded before it is only decoded, not shown. */
        private const val BACKLOG_SKIP_FRAMES = 2
    }

    // One audio owner: existing TCP reader in negotiated UDP mode, media reader otherwise.
    // Fixed PCM capacity; nonblocking AudioTrack writes with bounded producer backpressure.
    private var audioTrack: AudioTrack? = null
    private var audioPlaying = false
    private var audioPackets = 0
    private var audioWritten = 0L
    private var audioDropped = 0L
    private var pendingPcm = PendingPcm()

    private fun drainAudio() {
        val t = audioTrack ?: return
        pendingPcm.drain { data, offset, size ->
            t.write(data, offset, size, AudioTrack.WRITE_NON_BLOCKING)
        }
        audioWritten = pendingPcm.written
        if (
            !audioPlaying && audioWritten >= maxOf(8192, AUDIO_BYTES_PER_SEC * audioDelayMs / 1000)
        ) {
            t.play()
            audioPlaying = true
            onLog("Audio: playback started")
        }
    }

    // A stream track starts only once its buffer is full (start threshold = capacity). The sync policy
    // stops writing at target + SKIP_ABOVE_MS, which can be a little short of full, so the track
    // would stay silent with its head at 0 (seen on hardware and reproduced on an emulator).
    // Start at the target instead.
    private fun lowerStartThreshold(t: AudioTrack) {
        val frames = minOf(AUDIO_SAMPLE_RATE * audioDelayMs / 1000, t.bufferCapacityInFrames)
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            t.startThresholdInFrames = frames
        } else {
            // No threshold API: a buffer of twice the target still fills below the skip line.
            t.bufferSizeInFrames = minOf(t.bufferCapacityInFrames, frames * 2)
        }
        onLog(
            "Audio: start threshold " +
                "${if (android.os.Build.VERSION.SDK_INT >= 31) t.startThresholdInFrames else t.bufferSizeInFrames}" +
                " frames (target $audioDelayMs ms)"
        )
    }

    private fun startAudio() {
        pendingPcm = PendingPcm()
        try {
            val minBuf =
                AudioTrack.getMinBufferSize(
                    AUDIO_SAMPLE_RATE,
                    AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT,
                )
            val bufBytes = maxOf(minBuf, AUDIO_BYTES_PER_SEC / 4) // ~250 ms cushion against jitter
            audioTrack =
                AudioTrack.Builder()
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                            .build()
                    )
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(AUDIO_SAMPLE_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .build()
                    )
                    .setBufferSizeInBytes(bufBytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build()
            lowerStartThreshold(audioTrack!!)
            onLog("Audio: AudioTrack ready (buffer $bufBytes bytes, min $minBuf)")
        } catch (ex: Exception) {
            audioTrack = null
            onLog("Audio: couldn't start playback - ${ex.javaClass.simpleName}: ${ex.message}")
        }
    }

    //  A/V sync. Audio ahead of the speaker = PCM written to the AudioTrack but not yet played,
    // plus PCM still in
    // PendingPcm. Video is rendered on arrival, so this queue IS the audio-behind-picture delay. A
    // TCP burst deepens it
    // and, because the host produces audio in real time, it never drains by itself. Hold it near
    // the target: skip an arriving packet once the queue passes target + 120 ms (AvSyncPolicy). Never play faster.
    private var avSkippedBytes = 0L
    private var avFirstNs = 0L
    private var avLastNs = 0L
    private var avGapMaxNs = 0L
    private var avReportNs = 0L
    private var avQueueMaxMs = 0.0
    private var avUnderruns0 = 0

    private fun audioQueueMs(t: AudioTrack): Double {
        val head = t.playbackHeadPosition.toLong() and 0xFFFFFFFFL
        val frames = audioWritten / 4 - head + pendingPcm.pending / 4
        return frames * 1000.0 / AUDIO_SAMPLE_RATE
    }

    private fun avSyncTick(t: AudioTrack) {
        val now = System.nanoTime()
        if (avFirstNs == 0L) {
            avFirstNs = now
            avLastNs = now
            avReportNs = now
        }
        avGapMaxNs = maxOf(avGapMaxNs, now - avLastNs)
        avLastNs = now
        if (audioPlaying) {
            val q = audioQueueMs(t)
            if (q > avQueueMaxMs) avQueueMaxMs = q
        }
        if (now - avReportNs >= 5_000_000_000L) {
            val under = t.underrunCount
            val q = if (audioPlaying) audioQueueMs(t) else 0.0
            val elapsedMs = (now - avFirstNs) / 1_000_000.0
            val suppliedMs = (pendingPcm.received + avSkippedBytes) * 1000.0 / AUDIO_BYTES_PER_SEC
            onLog(
                "audio sync: target=${audioDelayMs}ms queue=%.0fms max=%.0fms pending=%dms underruns=%d(+%d) gapMax=%.0fms behind=%.0fms skipped=%dms head=%d state=%d"
                    .format(
                        q,
                        avQueueMaxMs,
                        pendingPcm.pending * 1000 / AUDIO_BYTES_PER_SEC,
                        under,
                        under - avUnderruns0,
                        avGapMaxNs / 1_000_000.0,
                        elapsedMs - suppliedMs,
                        avSkippedBytes * 1000 / AUDIO_BYTES_PER_SEC,
                        t.playbackHeadPosition.toLong() and 0xFFFFFFFFL,
                        t.playState,
                    )
            )
            avReportNs = now
            avGapMaxNs = 0
            avQueueMaxMs = q
            avUnderruns0 = under
        }
    }

    private var opusDecoder: OpusAudioDecoder? = null
    private var opusFailed = false

    // Raw PCM goes straight to playback; an Opus packet (PacketMeta_Audio_Opus) is decoded first.
    private fun onAudioPayload(opus: Boolean, data: ByteArray, size: Int = data.size) {
        if (!opus) {
            playAudio(data, size)
            return
        }
        var decoder = opusDecoder
        if (decoder == null && !opusFailed) {
            decoder = OpusAudioDecoder.create()
            opusDecoder = decoder
            if (decoder == null) {
                opusFailed = true
                onLog("Audio: the Opus decoder could not start; audio is dropped")
            } else onLog("Audio: Opus (48 kHz stereo, 20 ms packets)")
        }
        decoder?.decode(data, size) { pcm, n -> playAudio(pcm, n) }
    }

    private fun playAudio(data: ByteArray, size: Int) {
        val t = audioTrack ?: return
        try {
            drainAudio()
            if (
                audioPlaying &&
                    AvSyncPolicy.decide(audioQueueMs(t), audioDelayMs) ==
                        AvSyncPolicy.Action.SKIP_PACKET
            ) {
                avSkippedBytes +=
                    size // bounded latency: drop this packet, counted and logged, never silent
                avSyncTick(t)
                return
            }
            if (!pendingPcm.admit(data, size, ::drainAudio, cancelled = { !running })) return
            drainAudio()
            // Start once the target is queued (at least one packet) so a normal jitter blip doesn't
            // underrun immediately.
            if (
                !audioPlaying &&
                    audioWritten >= maxOf(size, AUDIO_BYTES_PER_SEC * audioDelayMs / 1000)
            ) {
                t.play()
                audioPlaying = true
                onLog("Audio: playback started")
            }
            avSyncTick(t)
            audioPackets++
            if (audioPackets <= 3 || audioPackets % 500 == 0) {
                val under = t.underrunCount
                onLog(
                    "Audio: packet $audioPackets ($size bytes), written=$audioWritten dropped=$audioDropped underruns=$under pending=${pendingPcm.pending} received=${pendingPcm.received}"
                )
            }
        } catch (ex: Exception) {
            onLog("Audio: write failed - ${ex.javaClass.simpleName}: ${ex.message}")
            throw ex
        }
    }

    private fun stopAudio() {
        opusDecoder?.close()
        opusDecoder = null
        opusFailed = false
        pendingPcm.close()
        if (pendingPcm.received > 0)
            onLog(
                "PCM final accepted=${pendingPcm.received} written=${pendingPcm.written} shutdown=${pendingPcm.shutdown} pending=${pendingPcm.pending}; no silent partial-write discard"
            )
        try {
            audioTrack?.stop()
        } catch (_: Exception) {}
        try {
            audioTrack?.release()
        } catch (_: Exception) {}
        audioTrack = null
        audioPlaying = false
        audioPackets = 0
        audioWritten = 0
        audioDropped = 0
        avSkippedBytes = 0
        avFirstNs = 0
        avLastNs = 0
        avGapMaxNs = 0
        avReportNs = 0
        avQueueMaxMs = 0.0
        avUnderruns0 = 0
    }

    @Volatile private var running = false

    fun stop() {
        running = false
    }

    // Blocks the calling thread for the whole streaming session. PunchSession.startStream
    // runs it on Dispatchers.IO so socket reads and decoding never block controller UI events.
    fun streamFrom(socket: Socket, surface: Surface) {
        // found via adb logcat + the console's own UDP log side-by-side - the
        // console can detect a broken connection (its own send() fails with a reset/broken
        // pipe) and close its socket while Android's READ side never gets an EOF or RST at
        // all (an asymmetric TCP failure - the RST that caused the console's write to fail
        // doesn't necessarily reach back to this side, especially over a cellular NAT path).
        // Without a timeout, input.read() then blocks forever waiting for data that will
        // never arrive - no crash, no log line, just a permanently stuck stream and a
        // disabled Test Punch button. A bounded read turns that into a SocketTimeoutException,
        // which the catch block below now handles exactly like any other stream-ending error.
        socket.soTimeout = 15_000
        val input = socket.getInputStream()
        val output = socket.getOutputStream()

        val hello = ByteArray(HELLO_SIZE)
        if (!readFully(input, hello)) {
            onLog("Video: never received hello from console")
            return
        }
        onLog("Video: hello = ${String(hello, Charsets.US_ASCII).trimEnd('\u0000')}")

        val request = ByteArray(HANDSHAKE_REQUEST_SIZE)
        // Magic 0xAAAAAAAA (PROTO_REQUEST_MAGIC) - same byte repeated 4x, byte order is a
        // non-issue.
        request[0] = 0xAA.toByte()
        request[1] = 0xAA.toByte()
        request[2] = 0xAA.toByte()
        request[3] = 0xAA.toByte()
        // ProtoVer - compared via raw memcmp against "03" on the console side (proto.c), not
        // parsed as an integer, so this just needs to be the literal ASCII bytes.
        request[4] = '0'.code.toByte()
        request[5] = '3'.code.toByte()
        // also ask for audio on this same connection (console interleaves packets
        // tagged PacketMeta_Type_Audio; a console that predates audio rejects with a non-OK code).
        request[6] = (META_FLAGS_VIDEO or META_FLAGS_AUDIO).toByte()
        request[7] = VIDEO_FLAGS_INJECT_PPS_SPS.toByte()
        request[8] = AUDIO_BATCHING.toByte() // batches of (n+1) x 0x1000-byte PCM chunks per packet
        request[9] =
            (DeliveryFeedback.FEATURE or
                    (if (udpVideo) UDPM_FEATURE or (if (rendezvousAddress != null) 0x40 else 0)
                    else 0) or
                    (if (OpusAudioDecoder.supported()) OPUS_FEATURE else 0))
                .toByte() // Ticket bytes10..15 stay unchanged.
        // bytes 10-15 (Reserved): the connection ticket when we have one, otherwise 0
        if (connTicket != null && connTicket.size == 6) {
            System.arraycopy(connTicket, 0, request, 10, 6)
            onLog("Video: connection ticket included in the handshake request")
        }

        output.write(request)
        output.flush()

        val response = ByteArray(HANDSHAKE_RESPONSE_SIZE)
        if (!readFully(input, response)) {
            onLog("Video: never received handshake response from console")
            return
        }

        val code = leU32(response, 0)
        if (code != HANDSHAKE_OK) {
            onLog("Video: handshake rejected, code=$code")
            return
        }

        onLog("Video: handshake OK - starting decoder")
        val delivery = DeliveryFeedback()
        val feedbackEnabled = leU32(response, 4) == DeliveryFeedback.ACK
        onLog(
            "delivery feedback negotiated=$feedbackEnabled; bounded PCM retry; unchanged codec/source modes"
        )
        // NX-Remote-Play: MemoryPools.ApplicationSize = bit63 | port<<32 | session token when UDP
        // video is offered.
        var udpOffer = 0L
        if (udpVideo && feedbackEnabled)
            for (i in 0..7) udpOffer = udpOffer or ((response[8 + i].toLong() and 0xFF) shl (8 * i))
        val queue: java.util.concurrent.ArrayBlockingQueue<ByteArray>? =
            if (udpOffer < 0) java.util.concurrent.ArrayBlockingQueue(512) else null
        var udpChannel: com.nxremoteplay.punchclient.udpm.UdpmVideoChannel? = null
        var tcpReader: Thread? = null
        var udpAudioThread: Thread? = null
        val audioWireBytes = java.util.concurrent.atomic.AtomicLong(0)
        val startTransports: () -> Unit = {
            if (queue != null) {
                val port = ((udpOffer ushr 32) and 0xFFFF).toInt()
                val token = udpOffer.toInt()
                val natKey =
                    if (leU32(response, 32) == com.nxremoteplay.punchclient.udpm.UdpNat.MAGIC)
                        response.copyOfRange(16, 32)
                    else null
                if (natKey != null)
                    onLog("UDP rendezvous capability negotiated; direct UDP path will be verified")
                onLog(
                    "NX-Remote-Play: console offered UDP media on port $port; video and audio arrive over UDPM, input stays on TCP"
                )
                // Liveness now comes from the UDP channel (15 s silence ends it); TCP may idle
                // between audio packets.
                socket.soTimeout = 20 // timeout services audio without losing a partial TCP record
                // Audio arrives as UDPM audio frames on the same socket. Its own thread owns playback so a full
                // PCM queue can never hold up the video; the oldest audio is dropped if it falls behind.
                val udpAudioQueue = java.util.concurrent.ArrayBlockingQueue<ByteArray>(200)
                udpChannel =
                    com.nxremoteplay.punchclient.udpm
                        .UdpmVideoChannel(
                            socket.inetAddress,
                            port,
                            token,
                            onWire = { wire ->
                                if (!queue.offer(wire))
                                    onLog("UDPM video: decoder queue full, frame dropped")
                            },
                            onAudioWire = { wire ->
                                if (!udpAudioQueue.offer(wire)) {
                                    udpAudioQueue.poll()
                                    udpAudioQueue.offer(wire)
                                }
                            },
                            onEnd = { why ->
                                onLog("UDPM video: $why")
                                queue.offer(END_OF_STREAM)
                            },
                            onLog = onLog,
                            natKey = natKey,
                            rendezvous = rendezvousAddress,
                            audioUnderruns = { audioTrack?.underrunCount ?: 0 },
                        )
                        .start()
                // The TCP connection only carries input and the console's liveness now; media is UDP.
                tcpReader =
                    Thread(
                            {
                                val h = ByteArray(PACKET_HEADER_SIZE)
                                try {
                                    while (running) {
                                        if (
                                            !TcpMediaPump.readFully(input, h, tick = {}) {
                                                !running
                                            }
                                        )
                                            break
                                        val size = leU32(h, 4)
                                        if (
                                            h[0] != 0xCC.toByte() ||
                                                h[1] != 0xCC.toByte() ||
                                                h[2] != 0xCC.toByte() ||
                                                h[3] != 0xCC.toByte() ||
                                                size <= 0 ||
                                                size > 0x54000
                                        ) {
                                            onLog("Video: bad TCP packet (magic/size), stopping")
                                            break
                                        }
                                        val wire = ByteArray(PACKET_HEADER_SIZE + size)
                                        System.arraycopy(h, 0, wire, 0, PACKET_HEADER_SIZE)
                                        if (
                                            !TcpMediaPump.readFully(
                                                input,
                                                wire,
                                                offset = PACKET_HEADER_SIZE,
                                                length = size,
                                                tick = {},
                                            ) {
                                                !running
                                            }
                                        )
                                            break
                                        TcpMediaPump.route(
                                            wire,
                                            audio = {}, // audio no longer travels over TCP
                                            video = { queue.put(it) },
                                        )
                                    }
                                } catch (ex: Exception) {
                                    if (running)
                                        onLog(
                                            "TCP/audio reader ended: ${ex.javaClass.simpleName}: ${ex.message}"
                                        )
                                } finally {
                                    queue.offer(END_OF_STREAM)
                                }
                            },
                            "nx-tcp-reader",
                        )
                        .apply {
                            isDaemon = true
                            start()
                        }
                udpAudioThread =
                    Thread(
                            {
                                try {
                                    while (running) {
                                        val wire = udpAudioQueue.poll(10, java.util.concurrent.TimeUnit.MILLISECONDS)
                                        if (wire == null) {
                                            drainAudio()
                                            continue
                                        }
                                        if (wire.size <= PACKET_HEADER_SIZE) continue
                                        val meta = wire[16].toInt() and 0xFF
                                        if ((meta and 0x03) != 0x02) continue
                                        audioWireBytes.addAndGet(wire.size.toLong())
                                        onAudioPayload(
                                            (meta and OPUS_PACKET) != 0,
                                            wire.copyOfRange(PACKET_HEADER_SIZE, wire.size),
                                        )
                                    }
                                } catch (ex: Exception) {
                                    if (running)
                                        onLog(
                                            "UDP audio thread ended: ${ex.javaClass.simpleName}: ${ex.message}"
                                        )
                                } finally {
                                    stopAudio() // release on its owner; never race a writer with
                                    // AudioTrack.release
                                }
                            },
                            "nx-udp-audio",
                        )
                        .apply {
                            isDaemon = true
                            start()
                        }
                onLog(
                    "audio: UDPM audio thread owns PCM; fixed 32768-byte pending buffer; video decode never waits for it"
                )
            } else if (udpVideo)
                onLog("NX-Remote-Play: UDP video not offered by the console; video stays on TCP")
        }

        var decoder = MediaCodec.createDecoderByType("video/avc")
        // Width/height are informational for the decoder's initial allocation - the real
        // SPS in the stream (injected inline, Annex-B start codes, see capture.c) is what
        // actually configures the decode geometry; NxRemotePlay's console-side capture is always
        // 1280x720 in this codebase's GRC configuration.
        val format = MediaFormat.createVideoFormat("video/avc", 1280, 720)
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                val supported =
                    decoder.codecInfo
                        .getCapabilitiesForType("video/avc")
                        .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
                if (supported) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                onLog(
                    "Video: low-latency decoder mode ${if (supported) "enabled" else "unavailable"}"
                )
            } catch (ex: Exception) {
                onLog("Video: low-latency decoder check unavailable (${ex.javaClass.simpleName})")
            }
        }
        var upscaler: UpscaleRenderer? = null
        if (upscaleMode > 0) {
            try {
                upscaler = UpscaleRenderer(surface, upscaleMode, assets, onLog)
            } catch (t: Throwable) {
                onLog(
                    "Video: picture upscaling unavailable (${t.javaClass.simpleName}: ${t.message}) - the plain surface is used"
                )
            }
        }
        try {
            decoder.configure(format, upscaler?.inputSurface ?: surface, null, 0)
            decoder.start()
        } catch (t: Throwable) {
            upscaler?.release()
            throw t
        }

        onLog(
            "playback: negotiated progress feedback and bounded PCM retry; presentation deadlines up to ${PresentationSchedule.MAX_EXTRA_US / 1000} ms after arrival (follows measured jitter), one picture per refresh in order, a picture >${FrameReleaseQueue.LATE_DROP_US / 1000} ms late is skipped"
        )

        val frameTrace = FrameTrace(onLog)
        val schedule = PresentationSchedule()
        // Frames that were decoded (the decoder needs every one) but not shown because newer frames
        // were already waiting; keyed by their input timestamp.
        val notShown: MutableSet<Long> = java.util.Collections.synchronizedSet(HashSet<Long>())
        val staleCount = java.util.concurrent.atomic.AtomicInteger(0)
        val decodedCount = java.util.concurrent.atomic.AtomicInteger(0)
        val latestReceived = java.util.concurrent.atomic.AtomicInteger(1)
        val traceThread = HandlerThread("NX-FrameTrace").apply { start() }
        fun attachRenderListener() {
            try {
                decoder.setOnFrameRenderedListener(
                    { _, _, presentationNanoTime -> frameTrace.rendered(presentationNanoTime) },
                    Handler(traceThread.looper),
                )
            } catch (ex: Exception) {
                onLog("FrameTrace: render callback unavailable (${ex.javaClass.simpleName})")
            }
        }
        attachRenderListener()

        // Decoded pictures go to the display one per refresh (ReleasePacer); a picture that was decoded
        // only as a reference for later ones (a backlog frame) is dropped instead of shown.
        val pacer =
            ReleasePacer(
                codec = { decoder },
                onDecoded = { ptsUs ->
                    val show = !notShown.remove(ptsUs)
                    if (!show) staleCount.incrementAndGet()
                    decodedCount.incrementAndGet()
                    frameTrace.output(latestReceived.get())
                    show
                },
            )

        // After a non-transient codec error every later call fails, so without this the picture
        // stays frozen while the stream and audio carry on (seen on hardware).
        var decoderRestarts = 0
        fun restartDecoder() {
            if (decoderRestarts >= 10) return
            decoderRestarts++
            try {
                pacer.clear()
                decoder.reset()
                decoder.configure(format, upscaler?.inputSurface ?: surface, null, 0)
                decoder.start()
                attachRenderListener()
                notShown.clear()
                udpChannel?.requestKeyframe()
                onLog("Video: decoder restarted (#$decoderRestarts), keyframe requested")
            } catch (ex: Exception) {
                onLog("Video: decoder restart failed - ${ex.javaClass.simpleName}: ${ex.message}")
            }
        }

        running = true
        val header = ByteArray(PACKET_HEADER_SIZE)
        var framesReceived = 0
        var decodedFrames = 0
        var decodeErrorLogged = false
        var showingJpeg = false

        try {
            stopAudio()
            startAudio()
            startTransports()
            while (running) {
                if (tcpReader == null) drainAudio()
                delivery.audioWireBytes(audioWireBytes.getAndSet(0))
                if (feedbackEnabled) delivery.report(System.nanoTime())?.let { onFeedback(it) }
                val queued: ByteArray? =
                    if (queue != null)
                        queue.poll(20, java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue
                    else null
                if (queued === END_OF_STREAM) {
                    onLog(
                        "Video: stream ended after $framesReceived packets received, ${decodedFrames + decodedCount.get()} actually decoded"
                    )
                    break
                }
                if (queued != null) {
                    if (queued.size < PACKET_HEADER_SIZE) continue
                    System.arraycopy(queued, 0, header, 0, PACKET_HEADER_SIZE)
                } else if (!readFully(input, header)) {
                    onLog(
                        "Video: connection ended after $framesReceived packets received, ${decodedFrames + decodedCount.get()} actually decoded"
                    )
                    break
                }


                if (
                    header[0] != 0xCC.toByte() ||
                        header[1] != 0xCC.toByte() ||
                        header[2] != 0xCC.toByte() ||
                        header[3] != 0xCC.toByte()
                ) {
                    onLog("Video: bad packet magic, stopping (no resync implemented yet)")
                    break
                }

                val dataSize = leU32(header, 4)
                if (
                    dataSize <= 0 ||
                        dataSize > 0x54000 ||
                        (queued != null && queued.size != PACKET_HEADER_SIZE + dataSize)
                ) {
                    onLog("Video: implausible DataSize=$dataSize, stopping")
                    break
                }

                val data =
                    if (queued != null) queued.copyOfRange(PACKET_HEADER_SIZE, queued.size)
                    else ByteArray(dataSize)
                if (queued == null && !readFully(input, data)) {
                    onLog(
                        "Video: connection ended mid-frame after $framesReceived packets received"
                    )
                    break
                }

                val metaData = header[16].toInt() and 0xFF
                delivery.packet(dataSize)
                val processingBegin = System.nanoTime()
                // PacketMeta_Type_Audio = BIT(1): raw PCM, not video - hand to the AudioTrack.
                if ((metaData and 0x03) == 0x02) {
                    onAudioPayload((metaData and OPUS_PACKET) != 0, data, dataSize)
                    delivery.handled(System.nanoTime() - processingBegin)
                    continue
                }
                // PacketMeta_Content_Mask bits: only feed real H264 data payloads to the
                // decoder - it can't touch JPEG or a replay/error marker. UseNalHash was
                // never requested above, so Replay packets should never arrive, but check
                // anyway rather than assume the console honored that.
                val isReplay = (metaData and 0x08) != 0 // PacketMeta_Content_Replay = BIT(3)
                val isError = (metaData and 0x20) != 0 // PacketMeta_Content_Error = BIT(5)
                val isScreenshot =
                    (metaData and 0x40) != 0 // PacketMeta_Content_Screenshot = BIT(6)

                if (isReplay || isError) {
                    if (isError) onLog("Video: error packet, metaData=0x${metaData.toString(16)}")
                    continue
                }

                val currentUdpChannel = udpChannel
                if (currentUdpChannel != null && !currentUdpChannel.receivedMedia) {
                    onLog("transport: TCP fallback confirmed; remote UDP test NOT passed")
                    currentUdpChannel.stop()
                    udpChannel = null
                }
                if (isScreenshot) {
                    // This codebase's HOME/applet JPEG-fallback path (~30fps raw caps:sc
                    // capture, see capture.c/home_capture.c) - a plain JPEG still, not H264.
                    // Decoding it into the actual MediaCodec decoder would be garbage in,
                    // garbage out at best. Previously this whole branch just skipped these
                    // packets outright, which is why HOME never showed anything even once
                    // the console-side fallback was working correctly.
                    val bitmap = BitmapFactory.decodeByteArray(data, 0, dataSize)
                    if (bitmap != null) {
                        onJpegFrame(bitmap)
                        showingJpeg = true
                        decodedFrames++
                    } else {
                        onLog("Video: JPEG-fallback packet failed to decode ($dataSize bytes)")
                    }
                    framesReceived++
                    if (framesReceived <= 4 || framesReceived % 60 == 0) {
                        onLog(
                            "Video: received packet $framesReceived ($dataSize bytes, JPEG fallback), decoder output so far: ${decodedFrames + decodedCount.get()}"
                        )
                    }
                    continue
                }

                // The UI only needs a notification when leaving HOME fallback.
                // Posting one main-thread task for every game frame (now ~60/s)
                // competes with controller events without changing the surface.
                if (showingJpeg) {
                    showingJpeg = false
                    onRealVideoFrame()
                }

                frameTrace.receive(data, framesReceived + 1, queue?.size ?: 0)
                delivery.video(baselineRawTimestamp(header), System.nanoTime())

                latestReceived.set(framesReceived + 1)
                pacer.takeFailure()?.let { ex ->
                    onLog(
                        "Video: codec error - transient=${ex.isTransient} recoverable=${ex.isRecoverable} " +
                            "code=${ex.errorCode} diagnostic=${ex.diagnosticInfo}"
                    )
                    if (!ex.isTransient) restartDecoder()
                }

                // a decoder/surface failure (e.g. the SurfaceView's surface being
                // destroyed by the fullscreen view reparenting) must not end the whole stream -
                // skip the frame and keep the connection (and JPEG fallback frames) alive.
                try {
                    val inputIndex = decoder.dequeueInputBuffer(10_000)
                    if (inputIndex >= 0) {
                        val inputBuffer = decoder.getInputBuffer(inputIndex)
                        inputBuffer?.clear()
                        inputBuffer?.put(data)
                        // The input timestamp carries the moment to show this frame, so each
                        // frame keeps its own time through the decoder (see PresentationSchedule).
                        val hostTimestampUs = baselineRawTimestamp(header)
                        val arrivalUs = processingBegin / 1000
                        val presentUs =
                            if (hostTimestampUs > 0) schedule.presentAt(hostTimestampUs, arrivalUs)
                            else arrivalUs
                        // After a network stall the backlog arrives at once. Showing all of it would
                        // fast-forward the picture, so only the newest frames of a backlog are shown.
                        if ((queue?.size ?: 0) >= BACKLOG_SKIP_FRAMES) {
                            if (notShown.size > 256) notShown.clear()
                            notShown.add(presentUs)
                        }
                        decoder.queueInputBuffer(inputIndex, 0, dataSize, presentUs, 0)
                        frameTrace.queued()
                    } else frameTrace.inputUnavailable()

                    // Outputs are taken out of the decoder by ReleasePacer's own thread.
                } catch (ex: MediaCodec.CodecException) {
                    onLog(
                        "Video: codec error - transient=${ex.isTransient} recoverable=${ex.isRecoverable} " +
                            "code=${ex.errorCode} diagnostic=${ex.diagnosticInfo}"
                    )
                    if (!ex.isTransient) restartDecoder()
                } catch (ex: Exception) {
                    if (!decodeErrorLogged) {
                        decodeErrorLogged = true
                        onLog(
                            "Video: decoder error, skipping frame(s), stream kept open - ${ex.javaClass.simpleName}: ${ex.message}"
                        )
                    }
                }

                framesReceived++
                delivery.handled(System.nanoTime() - processingBegin)
                frameTrace.tick()
                if (framesReceived <= 4 || framesReceived % 60 == 0) {
                    onLog(
                        "Video: received packet $framesReceived ($dataSize bytes), decoder output so far: ${decodedFrames + decodedCount.get()}"
                    )
                }
            }
        } catch (ex: Exception) {
            // found via adb logcat pull (crash buffer) after repeated reports of
            // the whole app closing itself - java.net.SocketException ("Connection reset",
            // e.g. when the console's side of the connection drops during a HOME/applet
            // transition like opening Settings) and android.media.MediaCodec$CodecException
            // ("rendering to non-initialized(obsolete) surface") were both being thrown here
            // and propagating all the way up through MainActivity.doPunch, which also had no
            // catch - and since the whole call chain runs on Dispatchers.Main (see doPunch's
            // launch site), any uncaught exception here was a guaranteed app crash, not just a
            // dropped connection. Treat it exactly like a graceful stream end.
            onLog("Video: exception in stream loop - ${ex.javaClass.simpleName}: ${ex.message}")
        } finally {
            running = false
            udpChannel?.stop()
            udpAudioThread?.let { audio ->
                audio.interrupt()
                audio.join(1000)
            }
            tcpReader?.let { reader ->
                try {
                    socket.shutdownInput()
                } catch (_: Exception) {}
                reader.interrupt()
                reader.join(1000)
                if (reader.isAlive)
                    onLog("TCP reader cleanup pending after the stream ended")
            }
            delivery.audioWireBytes(audioWireBytes.getAndSet(0))
            onLog(delivery.summary())
            pacer.stop()
            if (staleCount.get() > 0)
                onLog("Video: ${staleCount.get()} frames decoded but not shown (newer frames were waiting)")
            onLog("Video: display pacing released ${pacer.released} pictures, dropped ${pacer.dropped} that piled up behind newer ones")
            if (tcpReader == null) stopAudio()
            try {
                decoder.stop()
                decoder.release()
            } catch (_: Exception) {}
            upscaler?.release() // after the decoder: nothing renders into its surface any more
            traceThread.quitSafely()
            // New IO/export starts only after the original  media cleanup above.
            try {
                traceFile?.let { file ->
                    val packet =
                        File(
                            file.parentFile,
                            file.name.replace("frame_timeline_", "packet_timeline_"),
                        )
                    file.bufferedWriter().use {
                        BaselineMediaTrace.writeDisabled(it, true)
                    }
                    packet.bufferedWriter().use {
                        BaselineMediaTrace.writeDisabled(it, false)
                    }
                    val old =
                        file.parentFile
                            ?.listFiles { f ->
                                f.name.startsWith("frame_timeline_") && f.extension == "csv"
                            }
                            ?.filter { f ->
                                try {
                                    f.bufferedReader().use {
                                        it.readLine()?.startsWith("# media-trace") == true
                                    }
                                } catch (_: Exception) {
                                    false
                                }
                            }
                            ?.sortedByDescending { it.name }
                            .orEmpty()
                    old.drop(3).forEach {
                        File(it.parentFile, it.name.replace("frame_timeline_", "packet_timeline_"))
                            .delete()
                        it.delete()
                    }
                    onLog(
                        "Baseline media-trace: saved ${file.name}; playback ended"
                    )
                    onSavedTraces(file, packet)
                }
            } catch (ex: Exception) {
                onLog("Baseline media-trace: trace save unavailable - ${ex.javaClass.simpleName}")
            }
        }
    }

    private fun baselineRawTimestamp(header: ByteArray): Long {
        var value = 0L
        for (i in 0..7) value = value or ((header[8 + i].toLong() and 255) shl (8 * i))
        return value
    }

    private fun readFully(input: InputStream, buffer: ByteArray): Boolean =
        readFully(input, buffer, 0, buffer.size)

    private fun readFully(input: InputStream, buffer: ByteArray, start: Int, length: Int): Boolean {
        var offset = start
        val end = start + length
        while (offset < end) {
            val r = input.read(buffer, offset, end - offset)
            if (r < 0) return false
            offset += r
        }
        return true
    }

    private fun leU32(buffer: ByteArray, offset: Int): Int =
        (buffer[offset].toInt() and 0xFF) or
            ((buffer[offset + 1].toInt() and 0xFF) shl 8) or
            ((buffer[offset + 2].toInt() and 0xFF) shl 16) or
            ((buffer[offset + 3].toInt() and 0xFF) shl 24)
}
