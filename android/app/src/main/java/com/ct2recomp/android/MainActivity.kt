package com.ct2recomp.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.graphics.Typeface
import android.util.Log
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity(), SurfaceHolder.Callback {
    companion object {
        private const val TAG = "CT2MainActivity"
    }

    private lateinit var surfaceView: SurfaceView
    private var hudText: TextView? = null
    private var romOverlay: LinearLayout? = null
    private var statusText: TextView? = null
    private var romLoaded = false

    // 1 Hz UI-thread telemetry HUD updater (zero per-frame JNI calls).
    private val uiHandler = Handler(Looper.getMainLooper())
    private val hudUpdater = object : Runnable {
        override fun run() {
            hudText?.text = NativeBridge.nativeGetDiagStatus()
            uiHandler.postDelayed(this, 1000L)
        }
    }

    private val pickRomLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri != null) {
                try {
                    contentResolver.takePersistableUriPermission(
                        uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                    )
                } catch (_: SecurityException) {
                    // Best-effort persistence
                }
                RomLocator.saveSafUri(this, uri)
                if (RomLocator.openSafRomIntoNative(this, uri)) {
                    onRomMounted("SAF: ${uri.lastPathSegment}")
                } else {
                    statusText?.text = "Gagal membuka ROM dari SAF. Pilih file Crazy Taxi 3 (.iso/.xiso/default.xbe)."
                }
            }
        }

    // Push battery temperature only when Android broadcasts a change (zero per-frame JNI calls).
    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val tenthsC = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            if (tenthsC > 0) {
                NativeBridge.nativeSetBatteryTempC(tenthsC / 10.0f)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // 1. Capture any crash/ANR tombstones from prior runs on unrooted devices.
        CrashLogs.dumpRecentExitReasons(this)

        // 2. Initialize native host + Vulkan renderer + hang watchdog.
        val filesDir = (getExternalFilesDir(null) ?: filesDir).absolutePath
        val cacheDir = (externalCacheDir ?: cacheDir).absolutePath
        NativeBridge.nativeInit(filesDir, cacheDir)

        // 3. Build fullscreen Vulkan surface + lightweight ROM prompt overlay if needed.
        val root = FrameLayout(this)
        surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
        }
        root.addView(surfaceView)

        val hud = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 11f
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundColor(0x99000000.toInt())
            setPadding(20, 16, 20, 16)
            text = "Initializing Vulkan + Crazy Taxi 3 XBE runtime..."
        }
        hudText = hud
        root.addView(
            hud,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START
            ).apply {
                setMargins(16, 16, 16, 16)
            }
        )

        val overlay = buildRomOverlay()
        romOverlay = overlay
        root.addView(overlay)
        setContentView(root)

        ContextCompat.registerReceiver(
            this,
            batteryReceiver,
            IntentFilter(Intent.ACTION_BATTERY_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        tryAutoLoadRom()
    }

    private fun buildRomOverlay(): LinearLayout {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(0xCC111111.toInt())
            setPadding(48, 48, 48, 48)
        }

        val title = TextView(this).apply {
            text = "Crazy Taxi 3: High Roller (Xbox XBE -> Vulkan ARM64)"
            textSize = 20f
            setTextColor(0xFFFFFFFF.toInt())
            gravity = Gravity.CENTER
        }
        val status = TextView(this).apply {
            text = "Mencari ROM di /sdcard/crazy taxi ..."
            textSize = 14f
            setTextColor(0xFFDDDDDD.toInt())
            gravity = Gravity.CENTER
            setPadding(0, 16, 0, 24)
        }
        statusText = status

        val grantSdcardBtn = Button(this).apply {
            text = "Izinkan Akses /sdcard/crazy taxi"
            setOnClickListener {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val intent = Intent(
                        Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:$packageName")
                    )
                    startActivity(intent)
                } else {
                    tryAutoLoadRom()
                }
            }
        }

        val pickSafBtn = Button(this).apply {
            text = "Pilih ROM via File Picker (SAF)"
            setOnClickListener {
                pickRomLauncher.launch(arrayOf("*/*"))
            }
        }

        box.addView(title)
        box.addView(status)
        box.addView(grantSdcardBtn)
        box.addView(pickSafBtn)
        return box
    }

    private fun tryAutoLoadRom() {
        if (romLoaded) return

        // Priority 1: Direct /sdcard/crazy taxi (and variants)
        val direct = RomLocator.findDirectSdcardRom(this)
        if (direct != null && NativeBridge.nativeOpenRomPath(direct.absolutePath)) {
            onRomMounted(direct.absolutePath)
            return
        }

        // Priority 2: Previously selected SAF URI
        val savedUri = RomLocator.loadSavedSafUri(this)
        if (savedUri != null && RomLocator.openSafRomIntoNative(this, savedUri)) {
            onRomMounted("SAF: ${savedUri.lastPathSegment}")
            return
        }

        val hint = if (!RomLocator.hasAllFilesAccess()) {
            "ROM belum bisa dibaca dari /sdcard/crazy taxi (izin All Files belum aktif).\n" +
                "Aktifkan izin atau pilih file Crazy Taxi 3 (.iso/.xiso/default.xbe) via SAF."
        } else {
            "Tidak ditemukan file .iso/.xiso/default.xbe di /sdcard/crazy taxi.\n" +
                "Letakkan Crazy Taxi 3 - High Roller (USA).iso di /sdcard/crazy taxi atau pilih via SAF."
        }
        statusText?.text = hint
        Log.i(TAG, hint)
    }

    private fun onRomMounted(label: String) {
        romLoaded = true
        romOverlay?.visibility = LinearLayout.GONE
        Log.i(TAG, "ROM mounted: $label")
    }

    override fun onResume() {
        super.onResume()
        NativeBridge.nativeSetForeground(true)
        uiHandler.removeCallbacks(hudUpdater)
        uiHandler.post(hudUpdater)
        if (!romLoaded) {
            tryAutoLoadRom()
        }
    }

    override fun onPause() {
        uiHandler.removeCallbacks(hudUpdater)
        NativeBridge.nativeSetForeground(false)
        super.onPause()
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(batteryReceiver)
        } catch (_: IllegalArgumentException) {
            // Already unregistered
        }
        NativeBridge.nativeShutdown()
        super.onDestroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativeBridge.nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeBridge.nativeSetSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativeBridge.nativeSetSurface(null)
    }
}
