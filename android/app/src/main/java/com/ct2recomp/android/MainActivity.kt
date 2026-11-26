package com.ct2recomp.android

import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import kotlin.math.roundToInt

class MainActivity : ComponentActivity(), SurfaceHolder.Callback {
    companion object {
        private const val TAG = "CT2MainActivity"
        private const val PREFS_NAME = "ct3_settings"

        // Xbox XINPUT_GAMEPAD button bits
        private const val BTN_DPAD_UP = 0x0001
        private const val BTN_DPAD_DOWN = 0x0002
        private const val BTN_DPAD_LEFT = 0x0004
        private const val BTN_DPAD_RIGHT = 0x0008
        private const val BTN_START = 0x0010
        private const val BTN_BACK = 0x0020
    }

    private lateinit var surfaceView: SurfaceView
    private var hudText: TextView? = null
    private var romOverlay: LinearLayout? = null
    private var touchOverlay: FrameLayout? = null
    private var touchLeftCluster: View? = null
    private var touchRightCluster: View? = null
    private var settingsOverlay: FrameLayout? = null
    private var statusText: TextView? = null
    private var romLoaded = false
    private var surfaceReady = false
    private var hudVisible = true

    // Persisted runtime & graphics settings (live-applied where supported)
    private var cfgRenderScalePct = 100  // 50, 75, 100, 125, 150, 200
    private var cfgVkApiMode = 0         // 0=Auto(1.3), 1=1.2, 2=1.1
    private var cfgRenderPath = 0        // 0=Auto, 1=DynRender, 2=CachedRP
    private var cfgVsync = true
    private var cfgFpsCap = 60           // 60, 0(Unl), 30
    private var cfgShadows = true
    private var cfgFog = true
    private var cfgBloom = true
    private var cfgScreenFilter = 0      // 0=Off, 1=Sharpen, 2=Vivid, 3=Scanlines, 4=CRT, 5=HDR
    private var cfgAntiAliasing = 0      // 0=Off, 2=FXAA
    private var cfgShaderPreload = true
    private var cfgShowTouch = true
    private var cfgTouchOpacityPct = 78  // 100, 78, 50, 25

    // Touch controller state
    private var touchButtons = 0
    private var touchLx = 0
    private var touchLy = 0
    private var touchA = 0
    private var touchB = 0
    private var touchX = 0
    private var touchY = 0
    private var touchBlack = 0
    private var touchWhite = 0
    private var touchLt = 0
    private var touchRt = 0

    // Hardware controller state
    private var hwButtons = 0
    private var hwLx = 0
    private var hwLy = 0
    private var hwRx = 0
    private var hwRy = 0
    private var hwA = 0
    private var hwB = 0
    private var hwX = 0
    private var hwY = 0
    private var hwBlack = 0
    private var hwWhite = 0
    private var hwLt = 0
    private var hwRt = 0

    // 1 Hz UI-thread telemetry HUD updater (zero per-frame JNI calls).
    private val uiHandler = Handler(Looper.getMainLooper())
    private val hudUpdater = object : Runnable {
        override fun run() {
            if (hudVisible) {
                hudText?.text = NativeBridge.nativeGetDiagStatus()
            }
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

        CrashLogs.dumpRecentExitReasons(this)

        val filesDir = (getExternalFilesDir(null) ?: filesDir).absolutePath
        val cacheDir = (externalCacheDir ?: cacheDir).absolutePath
        loadSettings()
        NativeBridge.nativeInit(filesDir, cacheDir)
        pushSettingsToNative()

        val root = FrameLayout(this)
        surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
        }
        root.addView(surfaceView)

        val controls = buildTouchControls()
        touchOverlay = controls
        controls.visibility = View.GONE
        root.addView(
            controls,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )

        val hud = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 10.5f
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundColor(0x88000000.toInt())
            setPadding(16, 12, 16, 12)
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
                setMargins(16, 12, 16, 12)
            }
        )

        val overlay = buildRomOverlay()
        romOverlay = overlay
        root.addView(overlay)

        val settingsPanel = buildSettingsOverlay()
        settingsOverlay = settingsPanel
        settingsPanel.visibility = View.GONE
        root.addView(
            settingsPanel,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )
        applyTouchSettingsUi()
        hudText?.visibility = if (hudVisible) View.VISIBLE else View.GONE

        setContentView(root)

        ContextCompat.registerReceiver(
            this,
            batteryReceiver,
            IntentFilter(Intent.ACTION_BATTERY_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).roundToInt()

    @SuppressLint("ClickableViewAccessibility")
    private fun makePadButton(
        label: String,
        bgArgb: Int = 0x55222222,
        wDp: Int = 62,
        hDp: Int = 52,
        onState: (Boolean) -> Unit
    ): TextView {
        val bg = GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(12).toFloat()
            setColor(bgArgb)
            setStroke(dp(1), 0x88FFFFFF.toInt())
        }
        return TextView(this).apply {
            text = label
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(0xDDFFFFFF.toInt())
            gravity = Gravity.CENTER
            background = bg
            layoutParams = LinearLayout.LayoutParams(dp(wDp), dp(hDp)).apply {
                setMargins(dp(5), dp(5), dp(5), dp(5))
            }
            setOnTouchListener { v, ev ->
                when (ev.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                        v.alpha = 1.0f
                        onState(true)
                        pushTouchPad()
                        true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                        v.alpha = 0.78f
                        onState(false)
                        pushTouchPad()
                        true
                    }
                    else -> true
                }
            }
            alpha = 0.78f
        }
    }

    private fun buildTouchControls(): FrameLayout {
        val layer = FrameLayout(this)

        // Top-center utility bar: BACK / START / HUD toggle
        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        topBar.addView(makePadButton("BACK", 0x55333333, 64, 38) { down ->
            touchButtons = if (down) (touchButtons or BTN_BACK) else (touchButtons and BTN_BACK.inv())
        })
        topBar.addView(makePadButton("START", 0x66AA6600, 74, 38) { down ->
            touchButtons = if (down) (touchButtons or BTN_START) else (touchButtons and BTN_START.inv())
        })
        val hudBtn = TextView(this).apply {
            text = "HUD"
            textSize = 12f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(0xDDFFFFFF.toInt())
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = dp(12).toFloat()
                setColor(0x55333333)
                setStroke(dp(1), 0x88FFFFFF.toInt())
            }
            layoutParams = LinearLayout.LayoutParams(dp(58), dp(38)).apply {
                setMargins(dp(5), dp(5), dp(5), dp(5))
            }
            setOnClickListener {
                hudVisible = !hudVisible
                hudText?.visibility = if (hudVisible) View.VISIBLE else View.GONE
            }
        }
        topBar.addView(hudBtn)
        val setBtn = TextView(this).apply {
            text = "SET"
            textSize = 12f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(0xFFFFFFFF.toInt())
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = dp(12).toFloat()
                setColor(0x772266AA)
                setStroke(dp(1), 0xAAFFFFFF.toInt())
            }
            layoutParams = LinearLayout.LayoutParams(dp(58), dp(38)).apply {
                setMargins(dp(5), dp(5), dp(5), dp(5))
            }
            setOnClickListener {
                val panel = settingsOverlay ?: return@setOnClickListener
                panel.visibility = if (panel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
            }
        }
        topBar.addView(setBtn)
        layer.addView(
            topBar,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END
            ).apply {
                setMargins(dp(12), dp(10), dp(16), 0)
            }
        )

        // Bottom-left: D-Pad Up/Down (menu & gear) + Steer Left/Right (analog + D-pad)
        val leftCluster = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
        }
        val upRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        upRow.addView(makePadButton("▲", 0x55222222, 58, 44) { down ->
            touchButtons = if (down) (touchButtons or BTN_DPAD_UP) else (touchButtons and BTN_DPAD_UP.inv())
            touchLy = if (down) 32767 else 0
        })
        upRow.addView(makePadButton("▼", 0x55222222, 58, 44) { down ->
            touchButtons = if (down) (touchButtons or BTN_DPAD_DOWN) else (touchButtons and BTN_DPAD_DOWN.inv())
            touchLy = if (down) -32767 else 0
        })
        val steerRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        steerRow.addView(makePadButton("◀", 0x66224466, 78, 66) { down ->
            touchButtons = if (down) (touchButtons or BTN_DPAD_LEFT) else (touchButtons and BTN_DPAD_LEFT.inv())
            touchLx = if (down) -32767 else 0
        })
        steerRow.addView(makePadButton("▶", 0x66224466, 78, 66) { down ->
            touchButtons = if (down) (touchButtons or BTN_DPAD_RIGHT) else (touchButtons and BTN_DPAD_RIGHT.inv())
            touchLx = if (down) 32767 else 0
        })
        leftCluster.addView(upRow)
        leftCluster.addView(steerRow)
        touchLeftCluster = leftCluster
        layer.addView(
            leftCluster,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.START
            ).apply {
                setMargins(dp(18), 0, 0, dp(16))
            }
        )

        // Bottom-right: Classic Xbox ABXY diamond + BRAKE (LT) / GAS (RT) pedals
        val rightCluster = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
        }
        val abxyTop = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        abxyTop.addView(makePadButton("Y", 0x66998811, 56, 48) { down -> touchY = if (down) 255 else 0 })

        val abxyMid = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        abxyMid.addView(makePadButton("X", 0x66225599, 56, 48) { down -> touchX = if (down) 255 else 0 })
        val spacer = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(44), dp(1))
        }
        abxyMid.addView(spacer)
        abxyMid.addView(makePadButton("B", 0x66992222, 56, 48) { down -> touchB = if (down) 255 else 0 })

        val abxyBot = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        abxyBot.addView(makePadButton("A", 0x66228833, 56, 48) { down -> touchA = if (down) 255 else 0 })

        val pedals = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        pedals.addView(makePadButton("LT", 0x77883322, 78, 52) { down -> touchLt = if (down) 255 else 0 })
        pedals.addView(makePadButton("RT", 0x77228844, 86, 52) { down -> touchRt = if (down) 255 else 0 })

        rightCluster.addView(abxyTop)
        rightCluster.addView(abxyMid)
        rightCluster.addView(abxyBot)
        rightCluster.addView(pedals)
        touchRightCluster = rightCluster
        layer.addView(
            rightCluster,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.END
            ).apply {
                setMargins(0, 0, dp(18), dp(16))
            }
        )

        return layer
    }

    private fun pushTouchPad() {
        NativeBridge.nativeSetPadState(
            false,
            touchButtons,
            touchLx, touchLy, 0, 0,
            touchA, touchB, touchX, touchY,
            touchBlack, touchWhite, touchLt, touchRt
        )
    }

    private fun pushHwPad() {
        NativeBridge.nativeSetPadState(
            true,
            hwButtons,
            hwLx, hwLy, hwRx, hwRy,
            hwA, hwB, hwX, hwY,
            hwBlack, hwWhite, hwLt, hwRt
        )
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val src = event.source
        val isGamepad = (src and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (src and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        if (isGamepad) {
            val down = event.action == KeyEvent.ACTION_DOWN
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A -> { hwA = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_B -> { hwB = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_X -> { hwX = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_Y -> { hwY = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_L1 -> { hwWhite = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_R1 -> { hwBlack = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_L2 -> { hwLt = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_R2 -> { hwRt = if (down) 255 else 0; pushHwPad(); return true }
                KeyEvent.KEYCODE_BUTTON_START -> {
                    hwButtons = if (down) (hwButtons or BTN_START) else (hwButtons and BTN_START.inv())
                    pushHwPad(); return true
                }
                KeyEvent.KEYCODE_BUTTON_SELECT -> {
                    hwButtons = if (down) (hwButtons or BTN_BACK) else (hwButtons and BTN_BACK.inv())
                    pushHwPad(); return true
                }
                KeyEvent.KEYCODE_DPAD_UP -> {
                    hwButtons = if (down) (hwButtons or BTN_DPAD_UP) else (hwButtons and BTN_DPAD_UP.inv())
                    pushHwPad(); return true
                }
                KeyEvent.KEYCODE_DPAD_DOWN -> {
                    hwButtons = if (down) (hwButtons or BTN_DPAD_DOWN) else (hwButtons and BTN_DPAD_DOWN.inv())
                    pushHwPad(); return true
                }
                KeyEvent.KEYCODE_DPAD_LEFT -> {
                    hwButtons = if (down) (hwButtons or BTN_DPAD_LEFT) else (hwButtons and BTN_DPAD_LEFT.inv())
                    pushHwPad(); return true
                }
                KeyEvent.KEYCODE_DPAD_RIGHT -> {
                    hwButtons = if (down) (hwButtons or BTN_DPAD_RIGHT) else (hwButtons and BTN_DPAD_RIGHT.inv())
                    pushHwPad(); return true
                }
            }
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        if ((ev.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK &&
            ev.action == MotionEvent.ACTION_MOVE
        ) {
            hwLx = (ev.getAxisValue(MotionEvent.AXIS_X) * 32767f).roundToInt().coerceIn(-32768, 32767)
            hwLy = (-ev.getAxisValue(MotionEvent.AXIS_Y) * 32767f).roundToInt().coerceIn(-32768, 32767)
            hwRx = (ev.getAxisValue(MotionEvent.AXIS_Z) * 32767f).roundToInt().coerceIn(-32768, 32767)
            hwRy = (-ev.getAxisValue(MotionEvent.AXIS_RZ) * 32767f).roundToInt().coerceIn(-32768, 32767)
            val lt = maxOf(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER), ev.getAxisValue(MotionEvent.AXIS_BRAKE))
            val rt = maxOf(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS))
            hwLt = (lt * 255f).roundToInt().coerceIn(0, 255)
            hwRt = (rt * 255f).roundToInt().coerceIn(0, 255)
            pushHwPad()
            return true
        }
        return super.dispatchGenericMotionEvent(ev)
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
        if (romLoaded || !surfaceReady) return

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
        romOverlay?.visibility = View.GONE
        touchOverlay?.visibility = View.VISIBLE
        applyTouchSettingsUi()
        Log.i(TAG, "ROM mounted: $label")
    }

    private fun loadSettings() {
        val sp = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        cfgRenderScalePct = sp.getInt("render_scale_pct", 100)
        cfgVkApiMode = sp.getInt("vk_api_mode", 0)
        cfgRenderPath = sp.getInt("render_path", 0)
        cfgVsync = sp.getBoolean("vsync", true)
        cfgFpsCap = sp.getInt("fps_cap", 60)
        cfgShadows = sp.getBoolean("shadows", true)
        cfgFog = sp.getBoolean("fog", true)
        cfgBloom = sp.getBoolean("bloom", true)
        cfgScreenFilter = sp.getInt("screen_filter", 0)
        cfgAntiAliasing = sp.getInt("anti_aliasing", 0)
        cfgShaderPreload = sp.getBoolean("shader_preload", true)
        cfgShowTouch = sp.getBoolean("show_touch", true)
        cfgTouchOpacityPct = sp.getInt("touch_opacity_pct", 78)
        hudVisible = sp.getBoolean("hud_visible", true)
    }

    private fun saveSettings() {
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putInt("render_scale_pct", cfgRenderScalePct)
            .putInt("vk_api_mode", cfgVkApiMode)
            .putInt("render_path", cfgRenderPath)
            .putBoolean("vsync", cfgVsync)
            .putInt("fps_cap", cfgFpsCap)
            .putBoolean("shadows", cfgShadows)
            .putBoolean("fog", cfgFog)
            .putBoolean("bloom", cfgBloom)
            .putInt("screen_filter", cfgScreenFilter)
            .putInt("anti_aliasing", cfgAntiAliasing)
            .putBoolean("shader_preload", cfgShaderPreload)
            .putBoolean("show_touch", cfgShowTouch)
            .putInt("touch_opacity_pct", cfgTouchOpacityPct)
            .putBoolean("hud_visible", hudVisible)
            .apply()
    }

    private fun pushSettingsToNative() {
        NativeBridge.nativeApplyRuntimeSettings(
            cfgRenderScalePct,
            cfgVkApiMode,
            cfgRenderPath,
            cfgVsync,
            cfgFpsCap,
            cfgShadows,
            cfgFog,
            cfgBloom,
            cfgScreenFilter,
            cfgAntiAliasing,
            cfgShaderPreload
        )
    }

    private fun applyTouchSettingsUi() {
        val vis = if (cfgShowTouch) View.VISIBLE else View.GONE
        val alpha = (cfgTouchOpacityPct / 100f).coerceIn(0.15f, 1.0f)
        touchLeftCluster?.visibility = vis
        touchRightCluster?.visibility = vis
        touchLeftCluster?.alpha = alpha
        touchRightCluster?.alpha = alpha
    }

    private fun buildSettingsOverlay(): FrameLayout {
        val backdrop = FrameLayout(this).apply {
            setBackgroundColor(0xAA000000.toInt())
            isClickable = true
            isFocusable = true
            setOnClickListener { visibility = View.GONE }
        }

        val cardBg = GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(16).toFloat()
            setColor(0xEE141A24.toInt())
            setStroke(dp(1), 0x8866AACC.toInt())
        }
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = cardBg
            setPadding(dp(16), dp(14), dp(16), dp(14))
            isClickable = true
        }

        // Header
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val title = TextView(this).apply {
            text = "Crazy Taxi 3 Settings"
            textSize = 17f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(0xFFFFFFFF.toInt())
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        val closeBtn = TextView(this).apply {
            text = "CLOSE"
            textSize = 12f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(0xFFFFFFFF.toInt())
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = dp(10).toFloat()
                setColor(0xAA993333.toInt())
            }
            setPadding(dp(14), dp(7), dp(14), dp(7))
            setOnClickListener { backdrop.visibility = View.GONE }
        }
        header.addView(title)
        header.addView(closeBtn)
        card.addView(header)

        val subtitle = TextView(this).apply {
            text = "Opsi bertanda (restart) berlaku saat app dibuka ulang."
            textSize = 11f
            setTextColor(0xFF9FB4CC.toInt())
            setPadding(0, dp(4), 0, dp(10))
        }
        card.addView(subtitle)

        val list = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        fun addSettingRow(label: String, valueText: () -> String, onCycle: () -> Unit) {
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, dp(5), 0, dp(5))
            }
            val lbl = TextView(this).apply {
                text = label
                textSize = 13.5f
                setTextColor(0xFFE6EEF8.toInt())
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            }
            val valBtn = TextView(this).apply {
                text = valueText()
                textSize = 12.5f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xFFFFFFFF.toInt())
                gravity = Gravity.CENTER
                minWidth = dp(140)
                setPadding(dp(12), dp(8), dp(12), dp(8))
                background = GradientDrawable().apply {
                    shape = GradientDrawable.RECTANGLE
                    cornerRadius = dp(10).toFloat()
                    setColor(0xFF23384E.toInt())
                    setStroke(dp(1), 0x8888BBEE.toInt())
                }
                setOnClickListener {
                    onCycle()
                    text = valueText()
                    saveSettings()
                    pushSettingsToNative()
                    applyTouchSettingsUi()
                    hudText?.visibility = if (hudVisible) View.VISIBLE else View.GONE
                }
            }
            row.addView(lbl)
            row.addView(valBtn)
            list.addView(row)
        }

        val resSteps = intArrayOf(50, 75, 100, 125, 150, 200)
        addSettingRow("Internal Resolution", {
            val w = (640 * cfgRenderScalePct) / 100
            val h = (480 * cfgRenderScalePct) / 100
            "${cfgRenderScalePct}% (${w}x${h})"
        }) {
            val idx = resSteps.indexOf(cfgRenderScalePct).let { if (it < 0) 2 else it }
            cfgRenderScalePct = resSteps[(idx + 1) % resSteps.size]
        }

        addSettingRow("Vulkan API (restart)", {
            when (cfgVkApiMode) {
                1 -> "Vulkan 1.2"
                2 -> "Vulkan 1.1"
                else -> "Auto (1.3)"
            }
        }) { cfgVkApiMode = (cfgVkApiMode + 1) % 3 }

        addSettingRow("Render Path (restart)", {
            when (cfgRenderPath) {
                1 -> "Dynamic Render"
                2 -> "Cached RP"
                else -> "Auto"
            }
        }) { cfgRenderPath = (cfgRenderPath + 1) % 3 }

        addSettingRow("VSync (Swapchain)", { if (cfgVsync) "ON (FIFO)" else "OFF (Unlocked)" }) {
            cfgVsync = !cfgVsync
        }

        val fpsSteps = intArrayOf(60, 0, 30)
        addSettingRow("FPS Cap", { if (cfgFpsCap == 0) "Unlimited" else "$cfgFpsCap FPS" }) {
            val idx = fpsSteps.indexOf(cfgFpsCap).let { if (it < 0) 0 else it }
            cfgFpsCap = fpsSteps[(idx + 1) % fpsSteps.size]
        }

        addSettingRow("Shadows", { if (cfgShadows) "ON" else "OFF" }) { cfgShadows = !cfgShadows }
        addSettingRow("Fog", { if (cfgFog) "ON" else "OFF" }) { cfgFog = !cfgFog }
        addSettingRow("Bloom / Glow", { if (cfgBloom) "ON" else "OFF" }) { cfgBloom = !cfgBloom }

        val filterNames = arrayOf("Off", "Sharpen", "Vivid", "Scanlines", "CRT", "HDR")
        addSettingRow("Screen Filter", { filterNames[cfgScreenFilter.coerceIn(0, filterNames.lastIndex)] }) {
            cfgScreenFilter = (cfgScreenFilter + 1) % filterNames.size
        }

        addSettingRow("Anti-Aliasing", { if (cfgAntiAliasing == 2) "FXAA" else "OFF" }) {
            cfgAntiAliasing = if (cfgAntiAliasing == 2) 0 else 2
        }

        addSettingRow("Shader Preload", { if (cfgShaderPreload) "ON" else "OFF" }) {
            cfgShaderPreload = !cfgShaderPreload
        }

        addSettingRow("Touch Controls", { if (cfgShowTouch) "SHOW" else "HIDE" }) {
            cfgShowTouch = !cfgShowTouch
        }

        val opSteps = intArrayOf(100, 78, 50, 25)
        addSettingRow("Touch Opacity", { "${cfgTouchOpacityPct}%" }) {
            val idx = opSteps.indexOf(cfgTouchOpacityPct).let { if (it < 0) 1 else it }
            cfgTouchOpacityPct = opSteps[(idx + 1) % opSteps.size]
        }

        addSettingRow("Performance HUD", { if (hudVisible) "ON" else "OFF" }) {
            hudVisible = !hudVisible
        }

        val scroll = ScrollView(this).apply {
            addView(list)
        }
        card.addView(
            scroll,
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        )

        val dm = resources.displayMetrics
        val cardW = (dm.widthPixels * 0.62f).roundToInt().coerceIn(dp(340), dp(520))
        val cardH = (dm.heightPixels * 0.86f).roundToInt()
        backdrop.addView(
            card,
            FrameLayout.LayoutParams(cardW, cardH, Gravity.CENTER)
        )
        return backdrop
    }

    override fun onResume() {
        super.onResume()
        NativeBridge.nativeSetForeground(true)
        uiHandler.removeCallbacks(hudUpdater)
        uiHandler.post(hudUpdater)
        if (!romLoaded && surfaceReady) {
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
        surfaceReady = true
        NativeBridge.nativeSetSurface(holder.surface)
        if (!romLoaded) {
            tryAutoLoadRom()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceReady = true
        NativeBridge.nativeSetSurface(holder.surface)
        if (!romLoaded) {
            tryAutoLoadRom()
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        NativeBridge.nativeSetSurface(null)
    }
}
