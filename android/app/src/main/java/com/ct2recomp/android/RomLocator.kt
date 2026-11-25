package com.ct2recomp.android

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import android.util.Log
import java.io.File

/**
 * Resolves the user's Crazy Taxi 3: High Roller (Original Xbox) XISO/XBE from:
 *  1) Direct sdcard directory: /sdcard/crazy taxi (and case variants)
 *  2) App external files directory: /sdcard/Android/data/com.ct2recomp.android/files/rom
 *  3) Persisted Android SAF URI (detached fd passed directly to native XISO RomVfs)
 */
object RomLocator {
    private const val TAG = "CT3RomLocator"
    private const val PREFS_NAME = "ct3_rom_prefs"
    private const val KEY_SAF_URI = "saf_rom_uri"

    private val SUPPORTED_EXTS = setOf("iso", "xiso", "xbe")

    fun hasAllFilesAccess(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            true
        }
    }

    fun candidateDirectories(context: Context): List<File> {
        val extRoot = Environment.getExternalStorageDirectory()
        val list = mutableListOf(
            File("/sdcard/crazy taxi"),
            File(extRoot, "crazy taxi"),
            File(extRoot, "Crazy Taxi"),
            File(extRoot, "crazy_taxi"),
            File(extRoot, "ct3"),
        )
        context.getExternalFilesDir(null)?.let { list.add(File(it, "rom")) }
        return list.distinctBy { it.absolutePath }
    }

    fun findDirectSdcardRom(context: Context): File? {
        var bestFile: File? = null
        var bestScore = 0

        for (dir in candidateDirectories(context)) {
            if (!dir.exists() || !dir.isDirectory || !dir.canRead()) continue
            dir.walkTopDown().maxDepth(3).forEach { f ->
                if (!f.isFile || !f.canRead()) return@forEach
                val ext = f.extension.lowercase()
                if (ext !in SUPPORTED_EXTS) return@forEach

                var score = when (ext) {
                    "iso", "xiso" -> 100
                    "xbe" -> 90
                    else -> 0
                }
                val lower = f.name.lowercase()
                if ("taxi 3" in lower || "taxi3" in lower || "high roller" in lower || lower == "default.xbe") {
                    score += 60
                }
                if (score > bestScore) {
                    bestScore = score
                    bestFile = f
                }
            }
        }
        return bestFile
    }

    fun saveSafUri(context: Context, uri: Uri) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_SAF_URI, uri.toString())
            .apply()
    }

    fun loadSavedSafUri(context: Context): Uri? {
        val raw = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(KEY_SAF_URI, null)
        return raw?.let { Uri.parse(it) }
    }

    fun openSafRomIntoNative(context: Context, uri: Uri): Boolean {
        return try {
            val displayName = queryDisplayName(context, uri) ?: "Crazy Taxi 3 - High Roller (USA).iso"
            val pfd = context.contentResolver.openFileDescriptor(uri, "r") ?: return false
            val fd = pfd.detachFd()
            pfd.close()
            NativeBridge.nativeOpenRomFd(fd, displayName)
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to open SAF ROM uri=$uri", t)
            false
        }
    }

    private fun queryDisplayName(context: Context, uri: Uri): String? {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) return cursor.getString(idx)
                }
            }
        return uri.lastPathSegment
    }
}
