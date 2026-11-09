package com.nxremoteplay.punchclient

import java.io.EOFException
import java.io.File
import java.io.FilterInputStream
import java.io.FilterOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

internal interface PendingLogExport {
    fun open(): OutputStream

    fun commit()

    fun abort()
}

internal fun interface LogExportDestination {
    fun create(displayName: String): PendingLogExport
}

/**
 * Streaming ZIP; no Android APIs, network access, private-log deletion, or whole-file allocation.
 */
internal object DiagnosticArchive {
    private val frameName = Regex("frame_timeline_(\\d+)\\.csv")
    private val ticket = Regex("(Server replied: OK \\S+ )[0-9a-fA-F]{12}(?=\\s|$)")

    fun publish(
        destination: LogExportDestination,
        appLog: File,
        frame: File,
        packet: File,
        version: String
    ): String {
        val session =
            frameName.matchEntire(frame.name)?.groupValues?.get(1)
                ?: error("Unexpected frame trace name")
        require(packet.name == "packet_timeline_$session.csv") { "Trace session mismatch" }
        require(appLog.name == "app_log.txt" && listOf(appLog, frame, packet).all { it.isFile }) {
            "Saved logs unavailable"
        }
        require(
            frame.canonicalFile.parentFile == appLog.canonicalFile.parentFile &&
                packet.canonicalFile.parentFile == appLog.canonicalFile.parentFile) {
                "Log directory mismatch"
            }
        return publishArchive(destination, appLog, frame, packet, version, session)
    }

    fun publishJoinFailure(
        destination: LogExportDestination,
        appLog: File,
        version: String,
        attempt: Long
    ): String {
        require(appLog.name == "app_log.txt" && appLog.isFile) { "Saved log unavailable" }
        require(attempt > 0) { "Invalid attempt identifier" }
        return publishArchive(destination, appLog, null, null, version, "join-failed-$attempt")
    }

    /** The "Export logs" button: the app log, the previous run's log and the newest stream traces, if any. */
    fun publishManual(
        destination: LogExportDestination,
        appLog: File,
        frame: File?,
        packet: File?,
        version: String,
        stamp: Long
    ): String {
        require(appLog.name == "app_log.txt" && appLog.isFile) { "Saved log unavailable" }
        return publishArchive(destination, appLog, frame, packet, version, "export-$stamp")
    }

    private fun publishArchive(
        destination: LogExportDestination,
        appLog: File,
        frame: File?,
        packet: File?,
        version: String,
        session: String
    ): String {
        val name = "NX-Remote-Play-logs-$session.zip"
        val pending = destination.create(name)
        try {
            pending.open().use { write(it, appLog, frame, packet, version, session) }
            pending.commit() // publish only AFTER ZIP and provider output are closed
        } catch (failure: Exception) {
            try {
                pending.abort()
            } catch (cleanup: Exception) {
                failure.addSuppressed(cleanup)
            }
            throw failure
        }
        return name
    }

    private fun write(
        output: OutputStream,
        appLog: File,
        frame: File?,
        packet: File?,
        version: String,
        session: String
    ) {
        // ZIP owns its compressor; the caller alone owns/closes the provider stream.
        val nonClosing =
            object : FilterOutputStream(output) {
                override fun write(buffer: ByteArray, offset: Int, length: Int) {
                    out.write(buffer, offset, length)
                }

                override fun close() {
                    flush()
                }
            }
        ZipOutputStream(nonClosing).use { zip ->
            val inventory = StringBuilder()
            fun entry(file: File, redact: Boolean) {
                val digest = MessageDigest.getInstance("SHA-256")
                var bytes = 0L
                fun emit(buffer: ByteArray, length: Int) {
                    zip.write(buffer, 0, length)
                    digest.update(buffer, 0, length)
                    bytes += length
                }
                zip.putNextEntry(ZipEntry(file.name))
                SnapshotInput(file.inputStream(), file.length()).use { source ->
                    if (redact) {
                        source.bufferedReader(Charsets.UTF_8).useLines { lines ->
                            lines.forEach { line ->
                                val safe =
                                    (ticket.replace(line, "$1[redacted-ticket]") + "\n")
                                        .toByteArray(Charsets.UTF_8)
                                emit(safe, safe.size)
                            }
                        }
                    } else {
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = source.read(buffer)
                            if (count < 0) break
                            emit(buffer, count)
                        }
                    }
                }
                zip.closeEntry()
                val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
                inventory.append("${file.name}\t$bytes\t$hash\n")
            }
            entry(appLog, true)
            File(appLog.parentFile, "app_log.prev.txt").takeIf { it.isFile }?.let { entry(it, true) }
            frame?.let { entry(it, false) }
            packet?.let { entry(it, false) }
            zip.putNextEntry(ZipEntry("manifest.txt"))
            zip.write(
                buildString {
                        append(
                            "NX-Remote-Play diagnostics export v1\napp_version=$version\nsession=$session\n")
                        append(
                            "App-owned logs only; Android system logcat and Switch logs are not included.\n")
                        append(
                            "app_log.txt is a best-effort history snapshot; connection tickets are redacted.\n")
                        append(
                            "CSV headers record bounded retention/evictions. Process termination can lose unsaved current traces.\n")
                        append(
                            "Private originals retained. Public exports remain until the user removes them.\n")
                        if (frame == null) {
                            append(
                                if (session.startsWith("join-failed"))
                                    "Join failed before streaming; no frame or packet traces were created.\n"
                                else "No frame or packet traces are included (no stream was traced).\n")
                        }
                        append("filename\tbytes\tsha256\n$inventory")
                    }
                    .toByteArray(Charsets.UTF_8))
            zip.closeEntry()
        }
    }

    /** Freeze the readable length even if another logger later appends to the history. */
    private class SnapshotInput(source: InputStream, private var remaining: Long) :
        FilterInputStream(source) {
        override fun read(): Int {
            if (remaining == 0L) return -1
            val value = super.read()
            if (value < 0) throw EOFException("Saved log truncated during export")
            remaining--
            return value
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            if (length == 0) return 0
            if (remaining == 0L) return -1
            val count = super.read(buffer, offset, minOf(length.toLong(), remaining).toInt())
            if (count < 0) throw EOFException("Saved log truncated during export")
            remaining -= count
            return count
        }
    }
}
