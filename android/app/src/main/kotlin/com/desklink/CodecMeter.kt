package com.desklink

import android.content.Context

/**
 * Lo que esta tablet tarda de verdad en decodificar cada codec, aprendido de las sesiones: milisegundos por frame y
 * por megapíxel (para comparar sesiones a resoluciones distintas), en media móvil. Con los dos codecs medidos se pide
 * el más rápido; sin medidas manda la regla fija de [Device.prefersH264]. Si solo se conoce uno, la siguiente sesión
 * prueba el otro una vez, para tener con qué comparar.
 */
class CodecMeter(context: Context) {
    private val prefs = context.getSharedPreferences(PowerProfile.PREFS, Context.MODE_PRIVATE)

    fun msPerMp(codec: Int): Double? =
        prefs.getFloat(key(codec), -1f).takeIf { it > 0f }?.toDouble()

    /** Una muestra de un segundo de sesión: [decodeMs] de media por frame a [width]x[height] con [codec]. */
    fun record(codec: Int, width: Int, height: Int, decodeMs: Double) {
        val sample = normalize(width, height, decodeMs) ?: return
        val old = msPerMp(codec)
        val samples = prefs.getInt(countKey(codec), 0)
        prefs.edit()
            .putFloat(key(codec), blend(old, sample).toFloat())
            .putInt(countKey(codec), samples + 1)
            .apply()
    }

    fun samples(codec: Int): Int = prefs.getInt(countKey(codec), 0)

    /** ¿Pedir H.264 aunque haya HEVC? */
    fun prefersH264(default: Boolean): Boolean = decide(
        msPerMp(Dlp.CODEC_H264).takeIf { samples(Dlp.CODEC_H264) >= MIN_SAMPLES },
        msPerMp(Dlp.CODEC_HEVC).takeIf { samples(Dlp.CODEC_HEVC) >= MIN_SAMPLES },
        default,
    )

    companion object {
        /** Segundos de sesión con un codec antes de fiarse de su media. */
        const val MIN_SAMPLES = 20

        /** H.264 tiene que ser al menos un 15 % más rápido: a igual tasa HEVC se ve algo mejor. */
        const val H264_MARGIN = 0.85

        private fun key(codec: Int) = "decode_ms_mp_$codec"
        private fun countKey(codec: Int) = "decode_n_$codec"

        fun normalize(width: Int, height: Int, decodeMs: Double): Double? {
            if (width <= 0 || height <= 0 || decodeMs <= 0.0 || decodeMs > 500.0) return null
            return decodeMs / (width.toDouble() * height / 1e6)
        }

        /** Media móvil lenta: una sesión rara (un juego, la tablet caliente) no cambia la elección de golpe. */
        fun blend(old: Double?, sample: Double): Double = if (old == null) sample else old * 0.95 + sample * 0.05

        fun decide(h264: Double?, hevc: Double?, default: Boolean): Boolean = when {
            h264 != null && hevc != null -> h264 < hevc * H264_MARGIN
            h264 != null -> false  // falta HEVC: se prueba una vez
            hevc != null -> true   // falta H.264: se prueba una vez
            else -> default
        }
    }
}
