package app.extendesk

/**
 * Perfiles de batería. Cada uno fija los límites que se piden al host (fps, Mbps y escala de la resolución
 * codificada) y lo que la tablet hace por su lado (brillo máximo, modo del WiFi, rendimiento sostenido).
 * Los valores del host son los mismos que `dlp::ProfileDefaults` en protocol/src/packet.cpp.
 *
 * [MINIMO] no se elige a mano: es el último escalón del modo «adaptar a la carga» cuando el cable da poco.
 */
enum class PowerProfile(
    val id: String,
    val label: String,
    /** Número del perfil en QUALITY_REQUEST. */
    val wire: Int,
    /** 0 = sin límite (lo que diga el host, 60 fps de serie). */
    val maxFps: Int,
    /** 0 = sin límite (lo que diga el host, 50 Mbps de serie). */
    val maxBitrateMbps: Int,
    val scalePercent: Int,
    /** Brillo máximo de la ventana (0..1), solo si el usuario lo tiene más alto y en manual; null = no se toca. */
    val brightnessCap: Float?,
    /** WiFi sin ahorro de energía (menos latencia, más consumo). Por cable no se usa nunca. */
    val lowLatencyWifi: Boolean,
    /** Pide a Android fijar la CPU/GPU en una frecuencia sostenible. */
    val sustainedPerformance: Boolean,
    /**
     * Fija el modo de pantalla con la menor frecuencia que acomoda los fps del stream. Si no, la mayor que los acomoda
     * sin saltos (60 fps → 120 Hz en una pantalla de 144 Hz): cada frame espera menos al siguiente refresco.
     */
    val lowRefreshMode: Boolean,
    /** Decodificador a su velocidad máxima (menos retardo, más consumo); si no, a la justa para los fps. */
    val fastDecode: Boolean,
) {
    MAXIMA("maxima", "Máxima calidad", 0, 0, 0, 100, null, true, true, false, true),
    EQUILIBRADO("equilibrado", "Equilibrado", 1, 60, 30, 100, null, true, false, false, true),
    AHORRO("ahorro", "Ahorro", 2, 30, 12, 75, 0.5f, false, false, true, false),
    MINIMO("minimo", "Mínimo", 3, 30, 8, 60, 0.35f, false, false, true, false);

    val selectable: Boolean get() = this != MINIMO

    /** Un escalón menos de consumo (MINIMO es el último). */
    fun lower(): PowerProfile = values().getOrElse(ordinal + 1) { this }

    /** Un escalón más de calidad (MAXIMA es el último). */
    fun higher(): PowerProfile = values().getOrElse(ordinal - 1) { this }

    /** Resumen corto para las estadísticas, p. ej. «Ahorro · 30 fps · 12 Mbps · 75 %». */
    fun summary(): String {
        val fps = if (maxFps > 0) "$maxFps fps" else "fps del PC"
        val mbps = if (maxBitrateMbps > 0) "$maxBitrateMbps Mbps" else "Mbps del PC"
        return "$label · $fps · $mbps · $scalePercent %"
    }

    companion object {
        /** Claves de [Prefs] que comparten la pantalla de ajustes y el stream. */
        const val PREF_KEY = "power_profile"   // "ahorro" | "equilibrado" | "maxima"
        const val PREF_ADAPT = "power_adapt"   // Boolean: adaptar a la carga por USB (de serie, sí)

        /** Recomendado: calidad de sobra para escritorio y, con «adaptar a la carga», la batería no baja por USB. */
        val DEFAULT = EQUILIBRADO
        const val DEFAULT_ADAPT = true

        /** Perfil por su número en el protocolo (la calidad que elige el PC: 0 alta, 1 equilibrada, 2 ahorro). */
        fun fromWire(wire: Int): PowerProfile = values().firstOrNull { it.wire == wire } ?: DEFAULT

        fun fromId(id: String?): PowerProfile = values().firstOrNull { it.id == id && it.selectable } ?: DEFAULT

        /**
         * La menor frecuencia de pantalla que muestra [fps] sin saltos: un múltiplo exacto (30 fps → 60 Hz) o, si
         * no hay ninguno, la menor que llegue. Null si ninguna llega.
         */
        fun pickRefreshRate(rates: List<Float>, fps: Int): Float? {
            if (fps <= 0) return null
            val enough = rates.filter { it >= fps - 0.5f }
            val multiple = enough.filter {
                val k = Math.round(it / fps)
                k >= 1 && kotlin.math.abs(it - k * fps) < 0.6f
            }
            return multiple.minOrNull() ?: enough.minOrNull()
        }

        /**
         * La mayor frecuencia de pantalla que muestra [fps] sin saltos (un múltiplo exacto: 60 fps → 120 Hz, no 144, que
         * repetiría unos frames 2 veces y otros 3). Si no hay ninguna, la mayor. Null sin modos o con [fps] <= 0.
         */
        fun pickFastRefreshRate(rates: List<Float>, fps: Int): Float? {
            if (fps <= 0 || rates.isEmpty()) return null
            val multiple = rates.filter {
                val k = Math.round(it / fps)
                k >= 1 && kotlin.math.abs(it - k * fps) < 0.6f
            }
            return multiple.maxOrNull() ?: rates.maxOrNull()
        }
    }
}
