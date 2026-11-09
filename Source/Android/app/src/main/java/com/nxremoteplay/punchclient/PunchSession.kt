package com.nxremoteplay.punchclient

import android.content.Context
import android.graphics.Bitmap
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull

sealed class ConnectState {
    object Idle : ConnectState()

    object Requesting : ConnectState() // waking the console / waiting for it to register

    object Punching : ConnectState() // address exchanged, opening the direct connection

    object Connected : ConnectState() // direct socket up; the player screen starts the stream

    data class Ended(val message: String) : ConnectState() // stream finished or connection dropped

    data class Error(val message: String) : ConnectState()
}

// The TCP hole-punch connector, moved out of the old test Activity  so the Compose UI
// can drive it. Protocol (unchanged, proven on hardware): UDP PUNCHWAKE to the Rendezvous
// server -> console picks it up on its next 30 s REFRESH and PUNCHHOSTs -> TCP PUNCHJOIN (retried
// while the host isn't ready) -> both sides connect() to each other from the same local port ->
// NxRemotePlay video handshake + stream over the punched socket, with touch/controller packets sent
// the
// other way on the same socket (5-byte touch, 17-byte controller; see PunchProbe.c).
class PunchSession(private val context: Context) {
    private val _state = MutableStateFlow<ConnectState>(ConnectState.Idle)
    val state: StateFlow<ConnectState> = _state

    private val _status = MutableStateFlow("")
    val status: StateFlow<String> = _status

    // Footer status: is the configured Rendezvous server answering? null = not checked yet.
    // Same trick as the NRO's footer: a REFRESH with a credential that can never be a real session
    // is a side-effect-free ping - the server always answers (OK / ERR NOTFOUND), so any reply
    // proves it is up. Re-reads the server from Prefs every cycle, so a change in Settings is
    // picked up without a restart.
    private val _serverOnline = MutableStateFlow<Boolean?>(null)
    val serverOnline: StateFlow<Boolean?> = _serverOnline

    // the "Server online/offline" footer probe. It used to start an endless loop in
    // init{} (one per PunchSession, never cancelled, a fresh socket + source port every 4-6 s),
    // so a recreated Activity multiplied it and the phone flooded the server's per-IP rate
    // limiter (which then dropped real host traffic). Now: exactly one loop, only while the app
    // is on screen (MainActivity onStart/onStop), one reused socket, 10 s cadence (5 s while
    // offline so recovery shows quickly), paused while a session is being set up or streaming.
    private val probeLock = Any()
    private var probeJob: kotlinx.coroutines.Job? = null

    fun startServerProbe() {
        synchronized(probeLock) {
            if (probeJob?.isActive == true) return
            probeJob =
                CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
                    var socket: DatagramSocket? = null
                    try {
                        while (true) {
                            val busy =
                                _state.value is ConnectState.Requesting ||
                                    _state.value is ConnectState.Punching ||
                                    _state.value is ConnectState.Connected
                            if (!busy) {
                                if (socket == null || socket.isClosed) socket = DatagramSocket()
                                val ok = probeServer(socket)
                                _serverOnline.value = ok
                                if (!ok) { // a socket from before a Wi-Fi/cellular switch can stay
                                    // dead: rebuild it
                                    socket.close()
                                    socket = null
                                }
                            }
                            delay(if (_serverOnline.value == true) 10_000 else 5_000)
                        }
                    } finally {
                        socket?.close()
                    }
                }
        }
    }

    fun stopServerProbe() {
        synchronized(probeLock) {
            probeJob?.cancel()
            probeJob = null
        }
    }

    private fun probeServer(s: DatagramSocket): Boolean {
        val server = Prefs.parseServerAddress(Prefs.getServerAddress(context)) ?: return false
        return try {
            s.soTimeout = 2_000
            val msg = "REFRESH __PROBE__ __PROBE__\n".toByteArray(Charsets.US_ASCII)
            s.send(DatagramPacket(msg, msg.size, InetSocketAddress(server.first, server.second)))
            val buf = ByteArray(64)
            s.receive(DatagramPacket(buf, buf.size))
            true
        } catch (_: Exception) {
            false
        }
    }

    // Set by the player view. Called on the main thread.
    @Volatile var onJpegFrame: ((Bitmap) -> Unit)? = null
    @Volatile var onRealVideoFrame: (() -> Unit)? = null

    private val main = Handler(Looper.getMainLooper())
    private val io = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    // One FIFO writer preserves down/up ordering even when IO workers are scheduled differently.
    private val inputQueue = kotlinx.coroutines.channels.Channel<Pair<Socket, ByteArray>>(128)
    init {
        io.launch {
            for ((target, bytes) in inputQueue) {
                if (target !== socket || target.isClosed) continue
                try {
                    target.getOutputStream().write(bytes)
                } catch (_: Exception) {
                    try {
                        target.close()
                    } catch (_: Exception) {}
                }
            }
        }
    }
    private var job: kotlinx.coroutines.Job? = null
    private var boundCellular = false

    @Volatile private var socket: Socket? = null
    @Volatile private var videoClient: NxRemotePlayVideoClient? = null

    // connection ticket from "PUNCHJOIN2" ("OK <host> <ticket>"), sent to the console
    // in the
    // Reserved bytes of the video handshake request so it accepts this join even when NAT changed
    // its source
    // address (a join from inside the console's own house). Null when the server/console is older.
    @Volatile private var connTicket: ByteArray? = null

    fun setForeground(active: Boolean) {
        if (!active) {
            physicalPad = PadState()
            touchPad = PadState()
            sendMergedPad()
            sendSystemButtons(false)
            sendTouch(false, 0, 0)
        }
    }

    private val logFile
        get() = File(context.filesDir, "app_log.txt")

    private val bearer = NetworkBearer(context, this::log)

    fun log(line: String) {
        android.util.Log.i("PunchClient", line)
        try {
            // Keep the log bounded: past 2 MB it becomes app_log.prev.txt (the previous one is replaced).
            if (logFile.length() > 2L * 1024 * 1024) {
                val prev = File(context.filesDir, "app_log.prev.txt")
                prev.delete()
                logFile.renameTo(prev)
            }
            logFile.appendText(
                SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(Date()) + " " + line + "\n")
        } catch (_: Exception) {}
        // log lines are diagnostics only (app_log.txt / logcat). What the user sees is
        // set explicitly
        // via _status.value with plain wording - raw server replies such as ERR NOTFOUND are normal
        // while the
        // Switch is waking and must never be shown as if something were wrong.
    }

    // ---------------------------------------------------------------- connect

    fun connect(host: String, port: Int, id: String, password: String, forceCellular: Boolean) {
        if (_state.value is ConnectState.Requesting ||
            _state.value is ConnectState.Punching ||
            _state.value is ConnectState.Connected)
            return
        _state.value = ConnectState.Requesting
        _status.value = "Connecting..."
        log("=== Join $id via $host:$port (forceCellular=$forceCellular) ===")

        job = io.launch {
            val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            try {
                if (forceCellular) {
                    _status.value = "Connecting over mobile data..."
                    log("Requesting a cellular network and binding this process to it...")
                    val net = withTimeoutOrNull(10_000) { requestCellularNetwork(cm) }
                    if (net == null) {
                        fail(
                            "Couldn't get a cellular network within 10s (no SIM/data, or airplane mode?)")
                        return@launch
                    }
                    cm.bindProcessToNetwork(net)
                    boundCellular = true
                    log("Process bound to cellular network $net.")
                }
                bearer.start()
                // Same network? Search the local network first (no server needed, and the only
                // thing that works when phone and console share one router). Skipped when the
                // user forces cellular - that's an explicit "go through the internet" test.
                if (!forceCellular) {
                    _status.value = "Looking for the Switch on this network..."
                    log("LAN: looking for $id on the local network...")
                    when (val lan = tryLan(id, password)) {
                        is LanResult.Found -> {
                            socket = lan.socket
                            _state.value = ConnectState.Connected
                            return@launch
                        }
                        is LanResult.WrongPassword -> {
                            fail(
                                "A Switch with that Session ID is on this network, but the password was wrong.")
                            return@launch
                        }
                        LanResult.Busy -> {
                            fail("The console is already connected to another device.")
                            return@launch
                        }
                        LanResult.None -> {
                            log("LAN: no console answered - trying the server")
                            _status.value = "Contacting the server..."
                        }
                    }
                }

                val localPort = (20000..59999).random()
                val punched = doPunch(host, port, id, password, localPort)
                if (punched != null) {
                    socket = punched
                    _state.value = ConnectState.Connected
                } else {
                    unbindCellular(cm)
                }
            } catch (ex: Exception) {
                log("Connect failed: ${ex.javaClass.simpleName}: ${ex.message}")
                fail("Connection failed: ${ex.message ?: ex.javaClass.simpleName}")
                unbindCellular(cm)
            } finally {
                if (_state.value is ConnectState.Error) {
                    unbindCellular(cm)
                    main.post { BaselinePublicLogs.save(context) }
                }
            }
        }
    }

    // ---------------------------------------------------------------- LAN discovery
    // client -> UDP broadcast :9913 "NXLAN1 DISCOVER <id>"; the console answers
    // "NXLAN1 HERE <id> <port>" and listens on that TCP port for ~8 s; then
    // "NXLAN1 CHALLENGE <nonce>" / "NXLAN1 AUTH <clientNonce> <sha256hex>" proves the password
    // without sending it: sha256 = SHA-256(password ":" hostNonce ":" clientNonce). See
    // PunchProbe.c (LanHost) on the sysmodule for the host side.
    private sealed class LanResult {
        class Found(val socket: Socket) : LanResult()

        object WrongPassword : LanResult()

        object Busy : LanResult()

        object None : LanResult()
    }

    private fun broadcastTargets(): List<InetAddress> {
        val out = mutableListOf(InetAddress.getByName("255.255.255.255"))
        try {
            @Suppress("DEPRECATION")
            val dhcp =
                (context.applicationContext.getSystemService(Context.WIFI_SERVICE)
                        as? android.net.wifi.WifiManager)
                    ?.dhcpInfo
            if (dhcp != null && dhcp.netmask != 0) {
                // DhcpInfo ints are little-endian IPv4 (first octet in the low byte).
                val bcast = (dhcp.ipAddress and dhcp.netmask) or dhcp.netmask.inv()
                out.add(
                    InetAddress.getByAddress(
                        byteArrayOf(
                            (bcast and 0xFF).toByte(),
                            ((bcast shr 8) and 0xFF).toByte(),
                            ((bcast shr 16) and 0xFF).toByte(),
                            ((bcast shr 24) and 0xFF).toByte())))
            }
        } catch (_: Exception) {}
        return out
    }

    // every other address of this phone's /24 - used when broadcasts get no answer
    // (some routers/APs filter or don't forward broadcast frames, unicast still works).
    private fun sweepTargets(): List<InetAddress> {
        val out = mutableListOf<InetAddress>()
        try {
            @Suppress("DEPRECATION")
            val dhcp =
                (context.applicationContext.getSystemService(Context.WIFI_SERVICE)
                        as? android.net.wifi.WifiManager)
                    ?.dhcpInfo
            if (dhcp != null && dhcp.ipAddress != 0) {
                val ip = dhcp.ipAddress // little-endian IPv4: first octet in the low byte
                val self = (ip shr 24) and 0xFF
                for (h in 1..254) {
                    if (h == self) continue
                    out.add(
                        InetAddress.getByAddress(
                            byteArrayOf(
                                (ip and 0xFF).toByte(),
                                ((ip shr 8) and 0xFF).toByte(),
                                ((ip shr 16) and 0xFF).toByte(),
                                h.toByte())))
                }
            }
        } catch (_: Exception) {}
        return out
    }

    private fun tryLan(id: String, password: String): LanResult {
        var hostAddr: InetAddress? = null
        var hostPort = 0
        var busy = false
        val udp = DatagramSocket()
        try {
            udp.broadcast = true
            udp.soTimeout = 250
            val msg = "NXLAN1 DISCOVER $id\n".toByteArray(Charsets.US_ASCII)
            val buf = ByteArray(96)

            fun tryReceive() {
                try {
                    val p = DatagramPacket(buf, buf.size)
                    udp.receive(p)
                    val reply = String(p.data, 0, p.length, Charsets.US_ASCII).trim()
                    if (reply == "NXLAN1 BUSY $id") busy = true
                    val prefix = "NXLAN1 HERE $id "
                    if (reply.startsWith(prefix)) {
                        val port = reply.substring(prefix.length).trim().toIntOrNull()
                        if (port != null && port in 1..65535) {
                            hostAddr = p.address
                            hostPort = port
                        }
                    }
                } catch (_: SocketTimeoutException) {}
            }

            // Phase 1: broadcasts (~1.5 s)
            val targets = broadcastTargets()
            var round = 0
            while (round < 6 && hostAddr == null && !busy) {
                round++
                for (t in targets) {
                    try {
                        udp.send(DatagramPacket(msg, msg.size, t, 9913))
                    } catch (_: Exception) {}
                }
                tryReceive()
            }

            // Phase 2: nobody answered the broadcast - ask every address of our /24 directly (~1.5
            // s)
            if (hostAddr == null && !busy) {
                val sweep = sweepTargets()
                if (sweep.isNotEmpty()) {
                    log("LAN: no broadcast answer - sweeping ${sweep.size} local addresses")
                    for (t in sweep) {
                        try {
                            udp.send(DatagramPacket(msg, msg.size, t, 9913))
                        } catch (_: Exception) {}
                    }
                    val end = System.currentTimeMillis() + 1_500
                    while (hostAddr == null &&
                        !busy &&
                        System.currentTimeMillis() < end) tryReceive()
                }
            }
        } catch (ex: Exception) {
            log("LAN: discovery failed: ${ex.message}")
            return LanResult.None
        } finally {
            udp.close()
        }
        if (busy) return LanResult.Busy
        val addr = hostAddr ?: return LanResult.None

        log("LAN: console answered at ${addr.hostAddress}:$hostPort")
        _status.value = "Found the Switch on this network. Connecting..."
        val s = Socket()
        try {
            s.connect(InetSocketAddress(addr, hostPort), 5_000)
            val challenge = readLine(s, 5_000)
            if (challenge == null ||
                !challenge.startsWith("NXLAN1 CHALLENGE ") ||
                challenge.length != 17 + 16) {
                log("LAN: unexpected challenge '$challenge'")
                s.close()
                return LanResult.None
            }
            val hostNonce = challenge.substring(17)
            val nonceBytes = ByteArray(8).also { java.security.SecureRandom().nextBytes(it) }
            val clientNonce = nonceBytes.joinToString("") { "%02x".format(it) }
            val digest =
                java.security.MessageDigest.getInstance("SHA-256")
                    .digest("$password:$hostNonce:$clientNonce".toByteArray(Charsets.US_ASCII))
            val hash = digest.joinToString("") { "%02x".format(it) }
            writeLine(s, "NXLAN1 AUTH $clientNonce $hash")
            val verdict = readLine(s, 5_000)
            if (verdict != "NXLAN1 OK") {
                s.close()
                return LanResult.WrongPassword
            }
            log("LAN: authenticated")
            return LanResult.Found(s)
        } catch (ex: Exception) {
            log("LAN: connection failed: ${ex.message}")
            try {
                s.close()
            } catch (_: Exception) {}
            return LanResult.None
        }
    }

    private fun fail(message: String) {
        log(message)
        bearer.stop()
        _state.value = ConnectState.Error(message)
    }

    private fun unbindCellular(cm: ConnectivityManager) {
        if (boundCellular) {
            cm.bindProcessToNetwork(null)
            boundCellular = false
            log("Process unbound from cellular.")
        }
    }

    private suspend fun requestCellularNetwork(cm: ConnectivityManager): Network? =
        suspendCancellableCoroutine { cont ->
            val request =
                NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
                    .build()
            val callback =
                object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(network: Network) {
                        if (cont.isActive) cont.resumeWith(Result.success(network))
                    }
                }
            cm.requestNetwork(request, callback)
            cont.invokeOnCancellation { cm.unregisterNetworkCallback(callback) }
        }

    // Single UDP datagram - harmless ERR NOTFOUND if the console isn't registered under this id.
    private fun sendPunchWake(host: String, port: Int, id: String, password: String): String {
        val s = DatagramSocket()
        return try {
            s.soTimeout = 3_000
            val msg = "PUNCHWAKE $id $password\n".toByteArray(Charsets.US_ASCII)
            s.send(DatagramPacket(msg, msg.size, InetSocketAddress(host, port)))
            val buf = ByteArray(64)
            val reply = DatagramPacket(buf, buf.size)
            s.receive(reply)
            String(reply.data, 0, reply.length, Charsets.US_ASCII).trim()
        } catch (ex: Exception) {
            "no reply (${ex.message})"
        } finally {
            s.close()
        }
    }

    // Returns the punched socket, or null after setting an Error state.
    private suspend fun doPunch(
        host: String,
        port: Int,
        id: String,
        password: String,
        localPort: Int
    ): Socket? {
        _status.value = "Waking the Switch..."
        log("Requesting the console wake up (PUNCHWAKE)...")
        val wakeReply = sendPunchWake(host, port, id, password)
        log("PUNCHWAKE: server replied '$wakeReply'")
        if (wakeReply == "ERR NOTFOUND") {
            fail(
                "No console is registered with that Session ID and Password. Check them on the Switch's Host screen.")
            return null
        }

        // The console picks the wake up on its next REFRESH (<= 30 s), then registers for a punch:
        // PUNCHJOIN answers ERR NOTFOUND until it has. Retry for comfortably longer than that.
        val joinDeadline = System.currentTimeMillis() + 45_000
        var reply: String? = null
        var attempt = 0
        var useTicketJoin =
            true // PUNCHJOIN2 first; an older server answers ERR BADCMD and we retry with PUNCHJOIN
        connTicket = null
        while (System.currentTimeMillis() < joinDeadline) {
            attempt++
            val relay = Socket()
            try {
                relay.reuseAddress = true
                relay.bind(InetSocketAddress(localPort))
                relay.connect(InetSocketAddress(host, port), 10_000)
                writeLine(
                    relay, (if (useTicketJoin) "PUNCHJOIN2" else "PUNCHJOIN") + " $id $password")
                reply = readLine(relay, 10_000)
            } catch (ex: Exception) {
                relay.close()
                fail("Couldn't reach the server: ${ex.message}")
                return null
            }
            relay.close() // free the local port for the punch (or the next retry)

            if (useTicketJoin && reply == "ERR BADCMD") {
                useTicketJoin = false
                log("Server does not know PUNCHJOIN2 - joining the old way (no connection ticket)")
                continue
            }
            if (reply != null && reply != "ERR NOTFOUND") break
            _status.value = "Waiting for the Switch to respond..."
            log("Attempt $attempt: server replied '$reply' - host not ready yet, retrying...")
            delay(1_500)
        }

        log("Server replied: ${redactConnectionTicketReply(reply)}")
        if (reply == null || !reply.startsWith("OK ")) {
            fail(
                "The console didn't answer. Make sure it's on with Hosting enabled, and not already connected to another device.")
            return null
        }

        val replyTokens = reply.substring(3).trim().split(' ').filter { it.isNotEmpty() }
        val hostAddr = replyTokens.firstOrNull() ?: ""
        val ticketToken = replyTokens.getOrNull(1)
        connTicket =
            if (ticketToken != null &&
                ticketToken.length == 12 &&
                ticketToken.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' })
                ByteArray(6) { i -> ticketToken.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
            else null
        log(
            if (connTicket != null)
                "Connection ticket received (the console accepts this join from any network address)"
            else
                "No connection ticket (older server or console: the console checks the source address)")
        val colon = hostAddr.lastIndexOf(':')
        val hostIp = if (colon > 0) hostAddr.substring(0, colon) else null
        val hostPort = if (colon > 0) hostAddr.substring(colon + 1).toIntOrNull() else null
        if (hostIp == null || hostPort == null) {
            fail("Couldn't parse the console's address '$hostAddr'.")
            return null
        }

        _state.value = ConnectState.Punching
        _status.value = "Connecting to the Switch..."
        log("Punching toward $hostIp:$hostPort from local port $localPort...")
        val punch = Socket()
        try {
            punch.reuseAddress = true
            punch.bind(InetSocketAddress(localPort))
            punch.connect(InetSocketAddress(hostIp, hostPort), 15_000)
            // Controller snapshots are tiny and time-sensitive. Do not let TCP's
            // small-write batching delay them behind a pending acknowledgement.
            punch.tcpNoDelay = true
            log("PUNCH SUCCEEDED - direct connection to $hostIp:$hostPort established.")
            return punch
        } catch (ex: SocketTimeoutException) {
            punch.close()
            fail(
                "Couldn't connect to the Switch. The network in use may be blocking direct connections - try a different network or mobile data.")
        } catch (ex: Exception) {
            punch.close()
            log("Direct connection error: ${ex.javaClass.simpleName}: ${ex.message}") // technical
            // detail
            // stays in
            // the log
            fail(
                "Couldn't connect to the Switch. The network in use may be blocking direct connections - try a different network or mobile data.")
        }
        return null
    }

    private fun writeLine(s: Socket, line: String) {
        s.getOutputStream().write((line + "\n").toByteArray(Charsets.US_ASCII))
        s.getOutputStream().flush()
    }

    private fun readLine(s: Socket, timeoutMs: Int): String? {
        s.soTimeout = timeoutMs
        val input = s.getInputStream()
        val buf = ByteArrayOutputStream()
        try {
            while (true) {
                val b = input.read()
                if (b < 0)
                    return if (buf.size() == 0) null else buf.toString(Charsets.US_ASCII.name())
                if (b == '\n'.code) return buf.toString(Charsets.US_ASCII.name()).trimEnd('\r')
                buf.write(b)
            }
        } catch (_: Exception) {
            return null
        }
    }

    // ---------------------------------------------------------------- streaming

    // Called by the player screen once its video Surface exists. Runs the blocking read/decode loop
    // on an IO thread (it must NOT run on the UI thread - see NxRemotePlayVideoClient / fullscreen
    // drop fix).
    fun startStream(surface: Surface) {
        val sock = socket ?: return
        if (videoClient != null) return
        val client =
            NxRemotePlayVideoClient(
                connTicket = connTicket,
                onFeedback = { bytes ->
                    // Same bounded FIFO and sole writer as ordered controller packets.
                    if (sock === socket && !sock.isClosed)
                        inputQueue.trySend(sock to bytes).isSuccess
                    else false
                },
                traceFile =
                    File(context.filesDir, "frame_timeline_${System.currentTimeMillis()}.csv"),
                onLog = { line -> main.post { log(line) } },
                onJpegFrame = { bmp -> main.post { onJpegFrame?.invoke(bmp) } },
                onRealVideoFrame = { main.post { onRealVideoFrame?.invoke() } },
                // FIFO behind original logger callbacks; exporter starts after media cleanup only.
                onSavedTraces = { frame, packet ->
                    main.post { BaselinePublicLogs.save(context, frame, packet) }
                },
                udpVideo = Prefs.getUdpVideo(context),
                audioDelayMs = Prefs.getAudioDelayMs(context),
                upscaleMode = Prefs.getUpscaleMode(context),
                assets = context.assets,
                udpRendezvous =
                    Prefs.parseServerAddress(Prefs.getServerAddress(context))?.let {
                        InetSocketAddress.createUnresolved(it.first, it.second)
                    }, // resolved off the UI thread (hostname default)
            )
        videoClient = client
        io.launch {
            try {
                client.streamFrom(sock, surface)
                main.post {
                    if (_state.value is ConnectState.Connected)
                        _state.value = ConnectState.Ended("The connection ended.")
                }
            } catch (ex: Exception) {
                log("Stream failed: ${ex.javaClass.simpleName}: ${ex.message}")
                main.post {
                    if (_state.value is ConnectState.Connected)
                        _state.value = ConnectState.Ended("The connection dropped.")
                }
            } finally {
                closeSocket()
                videoClient = null
                val cm =
                    context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
                unbindCellular(cm)
            }
        }
    }

    private fun closeSocket() {
        bearer.stop()
        lastPad = null
        physicalPad = PadState()
        touchPad = PadState()
        try {
            socket?.close()
        } catch (_: Exception) {}
        socket = null
    }

    // Leaves the session: stops the stream, closes the socket, back to Idle.
    fun reset() {
        job?.cancel()
        videoClient?.stop()
        closeSocket()
        _state.value = ConnectState.Idle
        _status.value = ""
    }

    fun dispose() {
        reset()
        inputQueue.close()
        io.cancel()
    }

    // ---------------------------------------------------------------- input

    private fun sendRaw(bytes: ByteArray) {
        val sock = socket ?: return
        if (!inputQueue.trySend(sock to bytes).isSuccess) {
            // End a stalled connection rather than drop a release or grow an unbounded queue.
            try {
                sock.close()
            } catch (_: Exception) {}
        }
    }

    // Console-native 1280x720 pixel coordinates (PunchProbe.c ApplyTouchPacket).
    fun sendTouch(pressed: Boolean, x: Int, y: Int) {
        sendRaw(
            byteArrayOf(
                if (pressed) 1 else 0,
                (x and 0xFF).toByte(),
                ((x shr 8) and 0xFF).toByte(),
                (y and 0xFF).toByte(),
                ((y shr 8) and 0xFF).toByte(),
            ))
    }

    // system button from the floating menu. 5 bytes (same size as a touch packet):
    // [0x03][flags, bit0 = HOME held][0][0][0] - see PunchProbe.c SYS_PACKET_TYPE. The console
    // auto-releases a HOME that never gets its release, so a lost packet can't leave it stuck.
    fun sendSystemButtons(homeHeld: Boolean) {
        sendRaw(byteArrayOf(0x03, if (homeHeld) 1 else 0, 0, 0, 0))
    }

    // A normal HOME tap: press, ~150 ms, release.
    fun sendHomeTap() {
        io.launch {
            sendSystemButtons(true)
            delay(150)
            sendSystemButtons(false)
        }
    }

    // HidNpadButton bits + s16 sticks (PunchProbe.c ApplyControllerPacket).
    private var lastPad: ByteArray? = null

    private data class PadState(
        val keys: Long = 0,
        val lx: Int = 0,
        val ly: Int = 0,
        val rx: Int = 0,
        val ry: Int = 0
    )

    private var physicalPad = PadState()
    private var touchPad = PadState()

    fun sendPad(keys: Long, lx: Int, ly: Int, rx: Int, ry: Int) {
        physicalPad = PadState(keys, lx, ly, rx, ry)
        sendMergedPad()
    }

    fun sendTouchPad(keys: Long, lx: Int, ly: Int, rx: Int, ry: Int) {
        touchPad = PadState(keys, lx, ly, rx, ry)
        sendMergedPad()
    }

    private fun sendMergedPad() {
        val keys = physicalPad.keys or touchPad.keys
        val leftTouch = touchPad.lx != 0 || touchPad.ly != 0
        val rightTouch = touchPad.rx != 0 || touchPad.ry != 0
        val lx = if (leftTouch) touchPad.lx else physicalPad.lx
        val ly = if (leftTouch) touchPad.ly else physicalPad.ly
        val rx = if (rightTouch) touchPad.rx else physicalPad.rx
        val ry = if (rightTouch) touchPad.ry else physicalPad.ry
        val b = ByteBuffer.allocate(17).order(ByteOrder.LITTLE_ENDIAN)
        b.put(0x02)
        b.putLong(
            keys and 0xFFFFL) // real buttons only - bits 18/19 mean HOME/CAPTURE on the host (see
        // PunchProbe.c)
        b.putShort(lx.toShort())
        b.putShort(ly.toShort())
        b.putShort(rx.toShort())
        b.putShort(ry.toShort())
        val pkt = b.array()
        if (lastPad?.contentEquals(pkt) == true) return
        lastPad = pkt
        sendRaw(pkt)
    }
}
