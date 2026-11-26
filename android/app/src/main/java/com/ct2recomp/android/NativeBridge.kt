package com.ct2recomp.android

import android.view.Surface

object NativeBridge {
    init {
        System.loadLibrary("main")
    }

    external fun nativeInit(filesDir: String, cacheDir: String): Boolean
    external fun nativeOpenRomPath(romPath: String): Boolean
    external fun nativeOpenRomFd(fd: Int, displayName: String): Boolean
    external fun nativeSetSurface(surface: Surface?)
    external fun nativeSetBatteryTempC(tempC: Float)
    external fun nativeSetForeground(foreground: Boolean)
    external fun nativeGetDiagStatus(): String
    external fun nativeSetPadState(
        isHardware: Boolean,
        buttons: Int,
        lx: Int,
        ly: Int,
        rx: Int,
        ry: Int,
        a: Int,
        b: Int,
        x: Int,
        y: Int,
        black: Int,
        white: Int,
        lt: Int,
        rt: Int
    )
    external fun nativeSetGraphicsSwitches(
        reflectionCadence: Int,
        roadReflections: Boolean,
        particleDraw: Boolean,
        bloomGlow: Boolean,
        screenPostFx: Boolean,
        ultrawideHorPlus: Boolean,
        frameSkip60Hz: Boolean
    )
    external fun nativeApplyRuntimeSettings(
        renderScalePct: Int,
        vkApiMode: Int,
        renderPath: Int,
        vsync: Boolean,
        fpsCap: Int,
        shadows: Boolean,
        fog: Boolean,
        bloom: Boolean,
        screenFilter: Int,
        antiAliasing: Int,
        shaderPreload: Boolean
    )
    external fun nativeShutdown()
}
