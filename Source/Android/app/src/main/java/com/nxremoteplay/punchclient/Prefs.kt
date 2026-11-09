package com.nxremoteplay.punchclient

import android.content.Context

// Persist the server, last-used session credentials and force-cellular preference locally.
object Prefs {
    private const val FILE = "punch_client_prefs"
    private const val KEY_SERVER = "rendezvous_server"
    private const val KEY_USE_DEFAULT = "use_default_server"
    private const val KEY_LAST_ID = "last_session_id"
    private const val KEY_LAST_PASSWORD = "last_session_password"
    private const val KEY_FORCE_CELLULAR = "force_cellular"
    private const val KEY_UDP_VIDEO = "udp_video"
    private const val KEY_AUDIO_DELAY_MS = "audio_delay_ms"
    private const val KEY_UPSCALE = "picture_upscale"

    // The Rendezvous server this project's own hardware testing runs against.
    const val DefaultServer = "play.nxremoteplay.com"

    private fun p(context: Context) = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    // Server selection: a toggle. ON = the default server (play.nxremoteplay.com, port 7921);
    // OFF = the user's own server, any IPv4 address or DNS name with an optional :port (7921 when
    // omitted),
    // used exactly as typed. KEY_SERVER keeps the custom text (and migrates older saved servers).
    fun getCustomServer(context: Context): String = p(context).getString(KEY_SERVER, "") ?: ""

    fun setCustomServer(context: Context, value: String) {
        p(context).edit().putString(KEY_SERVER, value).apply()
    }

    fun getUseDefaultServer(context: Context): Boolean {
        val sp = p(context)
        if (sp.contains(KEY_USE_DEFAULT)) return sp.getBoolean(KEY_USE_DEFAULT, true)
        // Older versions stored only one server string: a non-default one means "custom".
        val saved = getCustomServer(context).trim()
        return saved.isEmpty() || isDefaultServerText(saved)
    }

    fun setUseDefaultServer(context: Context, value: Boolean) {
        p(context).edit().putBoolean(KEY_USE_DEFAULT, value).apply()
    }

    /**
     * The server the app actually uses right now: default when the toggle is ON, otherwise the
     * custom text.
     */
    fun getServerAddress(context: Context): String =
        if (getUseDefaultServer(context)) DefaultServer
        else getCustomServer(context).trim().ifEmpty { DefaultServer }

    fun isDefaultServerText(value: String): Boolean =
        parseServerAddress(value)?.let {
            it.first.equals(DefaultServer, ignoreCase = true) && it.second == DefaultPort
        } == true

    fun getLastSessionId(context: Context): String = p(context).getString(KEY_LAST_ID, "") ?: ""

    fun getLastSessionPassword(context: Context): String =
        p(context).getString(KEY_LAST_PASSWORD, "") ?: ""

    fun setLastSession(context: Context, id: String, password: String) {
        p(context).edit().putString(KEY_LAST_ID, id).putString(KEY_LAST_PASSWORD, password).apply()
    }

    fun getForceCellular(context: Context): Boolean =
        p(context).getBoolean(KEY_FORCE_CELLULAR, false)

    // NX-Remote-Play: ask the console for video over UDPM (it falls back to TCP by itself).
    fun getUdpVideo(context: Context): Boolean = p(context).getBoolean(KEY_UDP_VIDEO, true)

    fun setUdpVideo(context: Context, value: Boolean) {
        p(context).edit().putBoolean(KEY_UDP_VIDEO, value).apply()
    }

    //  A/V sync: PCM allowed to sit queued ahead of the speaker (ms). Default 100; presets in
    // Settings > Testing.
    val AudioDelayPresetsMs = listOf(40, 70, 100, 150, 200, 300)

    fun getAudioDelayMs(context: Context): Int =
        p(context).getInt(KEY_AUDIO_DELAY_MS, 100).coerceIn(20, 500)

    fun setAudioDelayMs(context: Context, value: Int) {
        p(context).edit().putInt(KEY_AUDIO_DELAY_MS, value.coerceIn(20, 500)).apply()
    }

    // picture upscaling: 0 Off (default), 1 FSR (AMD FidelityFX Super Resolution 1), 2
    // Cubic (lighter). Applies from the next stream.
    val UpscaleNames = listOf("Off", "FSR", "Cubic")

    fun getUpscaleMode(context: Context): Int = p(context).getInt(KEY_UPSCALE, 0).coerceIn(0, 2)

    fun setUpscaleMode(context: Context, value: Int) {
        p(context).edit().putInt(KEY_UPSCALE, value.coerceIn(0, 2)).apply()
    }

    fun setForceCellular(context: Context, value: Boolean) {
        p(context).edit().putBoolean(KEY_FORCE_CELLULAR, value).apply()
    }

    // Floating stream-menu circle position, as fractions (0..1) of the free travel area so it
    // survives
    // rotation / a different screen. Default: near the top-right corner.
    fun getMenuPos(context: Context): Pair<Float, Float> =
        p(context).getFloat("menu_x", 0.97f) to p(context).getFloat("menu_y", 0.05f)

    fun setMenuPos(context: Context, x: Float, y: Float) {
        p(context).edit().putFloat("menu_x", x).putFloat("menu_y", y).apply()
    }

    const val DefaultPort = 7921

    // Custom server box: accepts "host", "host:port", or a URL-ish "scheme://host:port/" (the
    // scheme and any path are ignored). The port defaults to 7921. Returns null if malformed.
    fun parseServerAddress(value: String): Pair<String, Int>? {
        var v = value.trim()
        val scheme = v.indexOf("://")
        if (scheme >= 0) v = v.substring(scheme + 3)
        val slash = v.indexOf('/')
        if (slash >= 0) v = v.substring(0, slash)
        if (v.isBlank() || v.any { it.isWhitespace() }) return null
        val colon = v.lastIndexOf(':')
        if (colon < 0) return v to DefaultPort
        if (colon == 0 || colon == v.length - 1) return null
        val host = v.substring(0, colon)
        val port = v.substring(colon + 1).toIntOrNull() ?: return null
        if (port !in 1..65535) return null
        return host to port
    }
}
