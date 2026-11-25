package com.ct2recomp.android

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Unrooted crash & ANR forensics (Android 11+):
 * Queries ActivityManager.getHistoricalProcessExitReasons() on startup and saves
 * native crash tombstones / ANR traces into user-accessible app files/logs/.
 */
object CrashLogs {
    private const val TAG = "CT2CrashLogs"
    private const val PREFS = "ct2_crash_prefs"
    private const val KEY_LAST_TS = "last_dumped_exit_ts"

    fun dumpRecentExitReasons(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return
        val baseDir = context.getExternalFilesDir(null) ?: context.filesDir
        val logsDir = File(baseDir, "logs").apply { mkdirs() }

        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        var lastDumpedTs = prefs.getLong(KEY_LAST_TS, 0L)
        var newestTs = lastDumpedTs

        val reasons = am.getHistoricalProcessExitReasons(context.packageName, 0, 10)
        val fmt = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US)

        for (info in reasons) {
            if (info.timestamp <= lastDumpedTs) continue
            if (info.reason != ApplicationExitInfo.REASON_CRASH_NATIVE &&
                info.reason != ApplicationExitInfo.REASON_CRASH &&
                info.reason != ApplicationExitInfo.REASON_ANR &&
                info.reason != ApplicationExitInfo.REASON_LOW_MEMORY
            ) {
                continue
            }

            val stamp = fmt.format(Date(info.timestamp))
            val outFile = File(logsDir, "exit_${stamp}_r${info.reason}.log")
            try {
                outFile.bufferedWriter().use { w ->
                    w.appendLine("timestamp=${info.timestamp} ($stamp)")
                    w.appendLine("reason=${info.reason} status=${info.status} importance=${info.importance}")
                    w.appendLine("pssKB=${info.pss} rssKB=${info.rss}")
                    w.appendLine("description=${info.description ?: "none"}")
                    w.appendLine("--- trace / tombstone ---")
                }
                info.traceInputStream?.use { traceIn ->
                    // On Android 12+, native tombstones may be binary protobuf; preserve alongside header.
                    val traceBytes = traceIn.readBytes()
                    if (traceBytes.isNotEmpty()) {
                        val isAscii = traceBytes.take(64).all { b ->
                            val c = b.toInt() and 0xFF
                            c == 9 || c == 10 || c == 13 || (c in 32..126)
                        }
                        if (isAscii) {
                            outFile.appendBytes(traceBytes)
                        } else {
                            val pbFile = File(logsDir, "tombstone_${stamp}.pb")
                            pbFile.writeBytes(traceBytes)
                            outFile.appendText("Binary protobuf tombstone saved to ${pbFile.name} (${traceBytes.size} bytes)\n")
                        }
                    }
                }
                Log.i(TAG, "Saved historical exit log: ${outFile.absolutePath}")
                if (info.timestamp > newestTs) newestTs = info.timestamp
            } catch (t: Throwable) {
                Log.w(TAG, "Failed writing exit log", t)
            }
        }

        if (newestTs > lastDumpedTs) {
            prefs.edit().putLong(KEY_LAST_TS, newestTs).apply()
        }
    }
}
