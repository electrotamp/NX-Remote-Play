package com.nxremoteplay.punchclient

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.widget.Toast
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/** Starts only after streaming cleanup. Does not replace 's synchronous session logger. */
internal object BaselinePublicLogs {
    private val busy = AtomicBoolean(false)

    fun save(context: Context, frame: File? = null, packet: File? = null, manual: Boolean = false) {
        val app = context.applicationContext
        val main = Handler(Looper.getMainLooper())
        fun notice(text: String) {
            main.post { Toast.makeText(app, text, Toast.LENGTH_LONG).show() }
        }
        if (Build.VERSION.SDK_INT < 29) {
            notice("Log export needs Android 10 or newer. Private logs kept.")
            return
        }
        if (!busy.compareAndSet(false, true)) {
            notice("Log export busy. Private logs kept.")
            return
        }
        Thread(
                {
                    try {
                        val resolver = app.contentResolver
                        val destination = LogExportDestination { name ->
                            val values =
                                ContentValues().apply {
                                    put(MediaStore.MediaColumns.DISPLAY_NAME, name)
                                    put(MediaStore.MediaColumns.MIME_TYPE, "application/zip")
                                    put(
                                        MediaStore.MediaColumns.RELATIVE_PATH,
                                        Environment.DIRECTORY_DOWNLOADS + "/NX-Remote-Play/")
                                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                                }
                            val uri =
                                resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                                    ?: error("Downloads entry unavailable")
                            object : PendingLogExport {
                                override fun open() =
                                    resolver.openOutputStream(uri, "w")
                                        ?: error("Downloads output unavailable")

                                override fun commit() {
                                    check(
                                        resolver.update(
                                            uri,
                                            ContentValues().apply {
                                                put(MediaStore.MediaColumns.IS_PENDING, 0)
                                            },
                                            null,
                                            null) > 0)
                                }

                                override fun abort() {
                                    resolver.delete(uri, null, null)
                                }
                            }
                        }
                        @Suppress("DEPRECATION")
                        val version =
                            app.packageManager
                                .getPackageInfo(app.packageName, 0)
                                .versionName
                                .orEmpty()
                        val appLog = File(app.filesDir, "app_log.txt")
                        if (manual) {
                            // Newest stream traces that have both halves, if any.
                            val newest =
                                app.filesDir
                                    .listFiles { f ->
                                        f.name.startsWith("frame_timeline_") && f.extension == "csv"
                                    }
                                    ?.sortedByDescending { it.lastModified() }
                                    ?.firstOrNull {
                                        File(it.parentFile, it.name.replace("frame_timeline_", "packet_timeline_"))
                                            .isFile
                                    }
                            DiagnosticArchive.publishManual(
                                destination,
                                appLog,
                                newest,
                                newest?.let {
                                    File(it.parentFile, it.name.replace("frame_timeline_", "packet_timeline_"))
                                },
                                version,
                                System.currentTimeMillis())
                        } else if (frame != null && packet != null) {
                            DiagnosticArchive.publish(destination, appLog, frame, packet, version)
                        } else {
                            DiagnosticArchive.publishJoinFailure(
                                destination, appLog, version, System.currentTimeMillis())
                        }
                        notice("Logs saved to Downloads/NX-Remote-Play")
                    } catch (ex: Exception) {
                        android.util.Log.w("PunchClient", "media-trace post-stream log export failed", ex)
                        notice("Couldn't save logs to Downloads. Private logs kept.")
                    } finally {
                        busy.set(false)
                    }
                },
                "NX-BaselineExport")
            .start()
    }
}
