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
    external fun nativeSetGraphicsSwitches(
        reflectionCadence: Int,
        roadReflections: Boolean,
        particleDraw: Boolean,
        bloomGlow: Boolean,
        screenPostFx: Boolean,
        ultrawideHorPlus: Boolean,
        frameSkip60Hz: Boolean
    )
    external fun nativeShutdown()
}
