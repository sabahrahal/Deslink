package com.desklink

import android.app.Activity
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.DisplayMetrics

/** Datos de la tablet que el PC necesita para ajustar el monitor virtual. */
object Device {
    /** Pantalla completa en píxeles reales (sin restar barras ni recortes), en horizontal. */
    @Suppress("DEPRECATION")
    fun display(activity: Activity): DisplaySpec {
        val (w, h) = if (Build.VERSION.SDK_INT >= 30) {
            val b = activity.windowManager.maximumWindowMetrics.bounds
            b.width() to b.height()
        } else {
            val m = DisplayMetrics()
            activity.windowManager.defaultDisplay.getRealMetrics(m)
            m.widthPixels to m.heightPixels
        }
        val d = activity.windowManager.defaultDisplay
        val refresh = d.supportedModes.maxOfOrNull { it.refreshRate } ?: d.refreshRate
        return DisplaySpec.landscape(w, h, activity.resources.displayMetrics.densityDpi, Math.round(refresh))
    }

    /** ¿Hay decodificador HEVC? Se mira una vez: no cambia mientras la app vive. */
    val hevcSupported: Boolean by lazy {
        try {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
                !info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_HEVC, ignoreCase = true) }
            }
        } catch (_: Exception) {
            true
        }
    }

    /**
     * ¿Le conviene H.264 aunque tenga HEVC? En MediaTek (Lenovo Idea Tab Pro, mt8792, a 2944x1840) HEVC tardaba 13-15 ms
     * por frame en decodificar y H.264 8,7-9,4 ms, con la calidad igual de buena a la tasa automática.
     */
    val prefersH264: Boolean by lazy {
        try {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
                !info.isEncoder && isMediaTek(info.name) &&
                    info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_HEVC, ignoreCase = true) }
            }
        } catch (_: Exception) {
            false
        }
    }

    fun isMediaTek(decoderName: String): Boolean =
        decoderName.startsWith("c2.mtk.", ignoreCase = true) || decoderName.startsWith("omx.mtk.", ignoreCase = true)
}
