package com.nxremoteplay.punchclient

import android.util.Log
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONArray

/**
 * Asks GitHub for the newest published release and compares its tag with this build's version.
 * The desktop app, the Switch app and the web page make the same check (COMPATIBILITY.md "Update
 * check").
 */
object UpdateCheck {
    const val ReleasesPage = "https://github.com/ElectroTamp/NX-Remote-Play/releases"
    const val ReleasesPageLabel = "github.com/ElectroTamp/NX-Remote-Play/releases"
    private const val ReleasesApi =
        "https://api.github.com/repos/ElectroTamp/NX-Remote-Play/releases?per_page=10"

    sealed interface State {
        data object NotChecked : State
        data object Checking : State
        data object UpToDate : State
        data class Available(val version: String) : State
        data object Failed : State
    }

    /** The start-up announcement was shown or dismissed in this process. */
    @Volatile var popupShown = false

    private val mutableState = MutableStateFlow<State>(State.NotChecked)
    val state: StateFlow<State> = mutableState

    fun statusText(s: State): String =
        when (s) {
            State.Checking -> "Checking..."
            State.UpToDate -> "Up to date"
            is State.Available -> "Update available: ${s.version}"
            State.Failed -> "Could not check"
            State.NotChecked -> "Not checked"
        }

    /** Starts one background check; a call while a check is running does nothing. */
    @Synchronized
    fun start() {
        if (mutableState.value == State.Checking) return
        mutableState.value = State.Checking
        thread(name = "update-check", isDaemon = true) { mutableState.value = run() }
    }

    private fun run(): State =
        try {
            val connection = URL(ReleasesApi).openConnection() as HttpURLConnection
            connection.connectTimeout = 10_000
            connection.readTimeout = 10_000
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            connection.setRequestProperty(
                "User-Agent", "NX-Remote-Play-Android/" + BuildConfig.VERSION_NAME)
            val body =
                try {
                    if (connection.responseCode != 200)
                        throw java.io.IOException("HTTP ${connection.responseCode}")
                    connection.inputStream.bufferedReader().use { it.readText() }
                } finally {
                    connection.disconnect()
                }
            val releases = JSONArray(body)
            val tags =
                (0 until releases.length())
                    .map { releases.getJSONObject(it) }
                    .filter { !it.optBoolean("draft") }
                    .map { it.optString("tag_name") }
            val newest = newestTag(tags)
            if (newest != null && (compare(newest, BuildConfig.VERSION_NAME) ?: 0) > 0)
                State.Available(newest.trimStart('v', 'V'))
            else State.UpToDate
        } catch (e: Exception) {
            Log.w("UpdateCheck", "update check failed: $e")
            State.Failed
        }

    fun newestTag(tags: List<String>): String? =
        tags.filter { compare(it, "0.0.0") != null }.reduceOrNull { a, b -> if (compare(b, a)!! > 0) b else a }

    /**
     * Semantic version order (MAJOR.MINOR.PATCH, then a pre-release such as -beta.2 sorts before
     * the release). A leading "v" and any "+build" suffix are ignored. Null when either side is not
     * a version.
     */
    fun compare(a: String, b: String): Int? {
        val (coreA, preA) = parse(a) ?: return null
        val (coreB, preB) = parse(b) ?: return null
        for (i in 0 until 3) if (coreA[i] != coreB[i]) return coreA[i].compareTo(coreB[i]).sign()
        if (preA.isEmpty() || preB.isEmpty())
            return if (preA.size == preB.size) 0 else if (preA.isEmpty()) 1 else -1
        for (i in 0 until minOf(preA.size, preB.size)) {
            val numA = preA[i].toLongOrNull()
            val numB = preB[i].toLongOrNull()
            val order =
                when {
                    numA != null && numB != null -> numA.compareTo(numB)
                    numA != null -> -1
                    numB != null -> 1
                    else -> preA[i].compareTo(preB[i])
                }
            if (order != 0) return order.sign()
        }
        return preA.size.compareTo(preB.size).sign()
    }

    private fun Int.sign() = Integer.signum(this)

    private fun parse(text: String): Pair<List<Long>, List<String>>? {
        var s = text.trim().removePrefix("v").removePrefix("V").substringBefore('+')
        var pre = emptyList<String>()
        val dash = s.indexOf('-')
        if (dash >= 0) {
            pre = s.substring(dash + 1).split('.')
            s = s.substring(0, dash)
        }
        val parts = s.split('.')
        if (parts.size != 3 || pre.any { it.isEmpty() }) return null
        val core = parts.map { p -> if (p.isNotEmpty() && p.all { it in '0'..'9' }) p.toLongOrNull() else null }
        if (core.any { it == null }) return null
        return core.map { it!! } to pre
    }
}
