package com.nxremoteplay.punchclient

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build

/** What the session log records about the network a session runs on. */
data class BearerFacts(
    val transports: List<String>,
    val validated: Boolean,
    val metered: Boolean,
    val roaming: Boolean,
    val downKbps: Int,
    val upKbps: Int,
    val signalDbm: Int?,
) {
    val tag: String
        get() = if (transports.isEmpty()) "OTHER" else transports.joinToString("+")

    /** Fields whose change gets its own log line; signal and link rate drift constantly. */
    val identity: String
        get() = "$tag/$validated/$metered/$roaming"

    fun describe(): String = buildString {
        append(tag)
        append(" validated=").append(yesNo(validated))
        append(" metered=").append(yesNo(metered))
        if ("CELLULAR" in transports) append(" roaming=").append(yesNo(roaming))
        if (downKbps > 0 || upKbps > 0) append(" link=${downKbps}/${upKbps}kbps")
        if (signalDbm != null) append(" signal=${signalDbm}dBm")
    }

    /**
     * Short form for the periodic lines. The link figures are Android's estimate, not a
     * measurement.
     */
    fun brief(): String = buildString {
        append(tag)
        if (signalDbm != null) append(" ${signalDbm}dBm")
        if (downKbps > 0 || upKbps > 0) append(" ${downKbps / 1000}/${upKbps / 1000}Mbps")
    }

    private fun yesNo(value: Boolean) = if (value) "yes" else "no"
}

/** Latest network summary, read by the periodic stream lines. */
object NetworkStatus {
    @Volatile var brief: String = "unknown"
}

/**
 * Writes the network a session runs on (Wi-Fi, mobile data, ...) to the session log when it starts
 * and whenever the network changes, and keeps [NetworkStatus] current for the periodic lines.
 */
class NetworkBearer(context: Context, private val log: (String) -> Unit) {
    private val manager =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private var callback: ConnectivityManager.NetworkCallback? = null
    @Volatile private var lastIdentity: String? = null

    fun start() {
        stop()
        val bound = manager.boundNetworkForProcess
        val first = factsOf(manager.getNetworkCapabilities(bound ?: manager.activeNetwork))
        log(
            "network: ${first?.describe() ?: "none (no active network)"}${if (bound != null) " (process bound)" else ""}"
        )
        publish(first)
        val monitor =
            object : ConnectivityManager.NetworkCallback() {
                override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
                    if (bound != null && network != bound) return
                    val facts = factsOf(caps)
                    val changed = facts?.identity != lastIdentity
                    publish(facts)
                    if (changed) log("network changed: ${facts?.describe() ?: "none"}")
                }

                override fun onLost(network: Network) {
                    if (bound != null && network != bound) return
                    publish(null)
                    log("network lost")
                }
            }
        try {
            if (bound != null) {
                val request =
                    NetworkRequest.Builder()
                        .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
                        .build()
                manager.registerNetworkCallback(request, monitor)
            } else {
                manager.registerDefaultNetworkCallback(monitor)
            }
            callback = monitor
        } catch (ex: RuntimeException) {
            log("network: change monitor unavailable (${ex.javaClass.simpleName})")
        }
    }

    fun stop() {
        val monitor = callback ?: return
        callback = null
        try {
            manager.unregisterNetworkCallback(monitor)
        } catch (_: RuntimeException) {}
        NetworkStatus.brief = "none"
    }

    private fun publish(facts: BearerFacts?) {
        lastIdentity = facts?.identity
        NetworkStatus.brief = facts?.brief() ?: "none"
    }

    private fun factsOf(caps: NetworkCapabilities?): BearerFacts? {
        if (caps == null) return null
        val signal =
            if (
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
                    caps.signalStrength != NetworkCapabilities.SIGNAL_STRENGTH_UNSPECIFIED
            )
                caps.signalStrength
            else null
        return BearerFacts(
            transports = TRANSPORTS.filter { caps.hasTransport(it.first) }.map { it.second },
            validated = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
            metered = !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED),
            roaming =
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
                    !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING),
            downKbps = caps.linkDownstreamBandwidthKbps,
            upKbps = caps.linkUpstreamBandwidthKbps,
            signalDbm = signal,
        )
    }

    private companion object {
        val TRANSPORTS =
            listOf(
                NetworkCapabilities.TRANSPORT_WIFI to "WIFI",
                NetworkCapabilities.TRANSPORT_CELLULAR to "CELLULAR",
                NetworkCapabilities.TRANSPORT_ETHERNET to "ETHERNET",
                NetworkCapabilities.TRANSPORT_BLUETOOTH to "BLUETOOTH",
                NetworkCapabilities.TRANSPORT_VPN to "VPN",
            )
    }
}
