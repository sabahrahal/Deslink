package com.desklink

import android.content.Context
import android.content.SharedPreferences

/**
 * Ajustes de la tablet. Solo lo que es de la tablet: la calidad, la resolución, los fps, el sonido y el táctil se
 * eligen en el PC, que es el que manda (docs/ajustes.md). La tablet le dice al PC lo que puede hacer ([hello]) y
 * enseña lo que el PC decide (SESSION_CONFIG).
 */
data class AppSettings(
    val showStats: Boolean = false,
    val autoConnect: Boolean = true,
) {
    companion object {
        /**
         * HELLO de esta tablet: su pantalla completa, su frecuencia y si decodifica HEVC. Los fps y la tasa van a 0:
         * los decide el PC.
         */
        fun hello(display: DisplaySpec, hevcSupported: Boolean) = Dlp.HelloConfig(
            width = display.width,
            height = display.height,
            fps = 0,
            bitrateMbps = 0,
            codec = if (hevcSupported) Dlp.CAN_DECODE_HEVC else Dlp.CAN_DECODE_H264_ONLY,
            densityDpi = display.densityDpi,
            refreshHz = display.refreshHz,
        )
    }
}

/** Pantalla de la tablet en horizontal (como se usa): lado largo x lado corto, en píxeles reales. */
data class DisplaySpec(val width: Int, val height: Int, val densityDpi: Int = 0, val refreshHz: Int = 60) {
    /**
     * Tamaño pedido al PC a [percent] % de la pantalla, en números pares y dentro de lo que acepta el host
     * (640x480 a 7680x4320). Si la escala deja un lado por debajo del mínimo se sube la escala, sin deformar.
     */
    fun scaled(percent: Int): Pair<Int, Int> {
        if (width <= 0 || height <= 0) return 0 to 0
        var scale = percent.coerceIn(10, 100) / 100.0
        scale = maxOf(scale, MIN_W.toDouble() / width, MIN_H.toDouble() / height)
        scale = minOf(scale, MAX_W.toDouble() / width, MAX_H.toDouble() / height)
        val w = (width * scale).toInt() and 1.inv()
        val h = (height * scale).toInt() and 1.inv()
        return w.coerceIn(MIN_W, MAX_W) to h.coerceIn(MIN_H, MAX_H)
    }

    companion object {
        const val MIN_W = 640
        const val MIN_H = 480
        const val MAX_W = 7680
        const val MAX_H = 4320

        /** Ordena a horizontal: el lado largo es el ancho. */
        fun landscape(a: Int, b: Int, densityDpi: Int, refreshHz: Int) =
            DisplaySpec(maxOf(a, b), minOf(a, b), densityDpi, refreshHz)
    }
}

/** Guarda los ajustes y el último PC en SharedPreferences. */
class SettingsStore(context: Context) {
    private val prefs: SharedPreferences = context.getSharedPreferences("desklink", Context.MODE_PRIVATE)

    fun load(): AppSettings {
        val d = AppSettings()
        return AppSettings(
            showStats = prefs.getBoolean(K_STATS, d.showStats),
            autoConnect = prefs.getBoolean(K_AUTO, d.autoConnect),
        )
    }

    fun save(s: AppSettings) {
        prefs.edit()
            .putBoolean(K_STATS, s.showStats)
            .putBoolean(K_AUTO, s.autoConnect)
            .apply()
    }

    /** Último PC al que se conectó: IP o [Link.CABLE_HOST]. */
    var lastHost: String?
        get() = prefs.getString(K_HOST, null)
        set(v) = prefs.edit().putString(K_HOST, v).apply()

    var lastHostName: String?
        get() = prefs.getString(K_HOST_NAME, null)
        set(v) = prefs.edit().putString(K_HOST_NAME, v).apply()

    /** Ya se vio la guía de inicio (o se saltó). Quien ya había conectado alguna vez no la ve de golpe. */
    var onboardingDone: Boolean
        get() = prefs.getBoolean(K_ONBOARDING, lastHost != null)
        set(v) = prefs.edit().putBoolean(K_ONBOARDING, v).apply()

    /** El usuario cerró el aviso «¿Necesitas más velocidad por USB?». */
    var usbTipDismissed: Boolean
        get() = prefs.getBoolean(K_USB_TIP, false)
        set(v) = prefs.edit().putBoolean(K_USB_TIP, v).apply()

    private companion object {
        const val K_HOST = "host" // la misma clave que usaban las versiones anteriores
        const val K_HOST_NAME = "host_name"
        const val K_STATS = "show_stats"
        const val K_AUTO = "auto_connect"
        const val K_ONBOARDING = "onboarding_done"
        const val K_USB_TIP = "usb_tip_dismissed"
    }
}
