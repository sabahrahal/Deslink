package app.extendesk

/** Cómo va la batería durante el stream (para el indicador). */
enum class ChargeState {
    /** Aún no hay cifra fiable. */
    MEASURING,
    /** Sin cable: se gasta batería. */
    ON_BATTERY,
    /** Con cable y la batería sube (o se mantiene). */
    CHARGING,
    /** Con cable pero baja: se está bajando la calidad para compensar. */
    ADAPTING,
    /** Con cable, baja y ya no queda nada que bajar (o «adaptar» está apagado): hay que avisar. */
    DRAINING,
}

/**
 * Modo «adaptar a la carga». Recibe cada pocos segundos la potencia neta medida en la batería y decide el
 * perfil efectivo: con cable, si la batería baja se pasa al perfil inferior; si sobra margen durante un rato
 * se vuelve a subir, sin pasar del que eligió el usuario ([base]) ni volver enseguida a uno que ya descargó.
 * Sin cable se usa [base], y un escalón menos con la batería baja. Lógica pura (sin Android) para probarla.
 */
class ChargeGovernor(private val base: PowerProfile, private val adapt: Boolean) {
    var current: PowerProfile = base
        private set
    var state: ChargeState = ChargeState.MEASURING
        private set
    /** Último valor neto usado (mW, positivo = sube). */
    var lastNetMw: Double? = null
        private set

    private var plugged: Boolean? = null
    private var lastChangeMs = 0L
    private var surplusSinceMs = -1L
    private var drainSinceMs = -1L
    private var peakPercent = -1
    /** Perfil que descargó la batería con cable y desde cuándo: no se vuelve a él durante [BLOCK_MS]. */
    private var blocked: PowerProfile? = null
    private var blockedAtMs = 0L

    /**
     * [netMw] null = aún sin medida. Devuelve true si cambió [current] (hay que mandarlo al host y
     * reiniciar la ventana de medida, que mezclaría consumo de antes y de después).
     */
    fun update(nowMs: Long, isPlugged: Boolean, netMw: Double?, percent: Int): Boolean {
        lastNetMw = netMw
        if (plugged != isPlugged) {
            // Al enchufar o desenchufar se parte del perfil del usuario y se deja asentar la medida.
            plugged = isPlugged
            lastChangeMs = nowMs
            surplusSinceMs = -1
            drainSinceMs = -1
            peakPercent = percent
            blocked = null
            val target = if (isPlugged) base else batteryTarget(percent)
            state = ChargeState.MEASURING
            return setCurrent(target)
        }
        if (!isPlugged) {
            state = if (netMw == null) ChargeState.MEASURING else ChargeState.ON_BATTERY
            val target = batteryTarget(percent)
            // Bajar con la batería baja sí; volver a subir solo si se recupera de verdad (histéresis de 5 %).
            if (target.ordinal > current.ordinal || (target.ordinal < current.ordinal && percent >= LOW_BATTERY_PERCENT + 5)) {
                lastChangeMs = nowMs
                return setCurrent(target)
            }
            return false
        }

        if (percent > peakPercent) peakPercent = percent
        // Si no hay cifra, que el porcentaje baje con el cable también cuenta como descarga.
        val draining = when {
            // Con la batería llena algunos aparatos la dejan bajar un poco antes de rellenar: eso no es descarga.
            netMw != null -> netMw < -DRAIN_MW && (percent < FULL_PERCENT || netMw < -FULL_DRAIN_MW)
            else -> percent in 0 until peakPercent
        }
        val surplus = netMw != null && netMw > SURPLUS_MW
        if (draining) {
            if (drainSinceMs < 0) drainSinceMs = nowMs
            surplusSinceMs = -1
        } else {
            drainSinceMs = -1
            if (surplus) {
                if (surplusSinceMs < 0) surplusSinceMs = nowMs
            } else {
                surplusSinceMs = -1
            }
        }
        val settled = nowMs - lastChangeMs >= SETTLE_MS

        if (netMw == null && !draining) {
            if (state != ChargeState.ADAPTING && state != ChargeState.DRAINING) state = ChargeState.MEASURING
            return false
        }
        if (draining) {
            if (adapt && current != PowerProfile.MINIMO) {
                state = ChargeState.ADAPTING
                // Asentada la medida tras el último cambio y la descarga mantenida unos segundos (no un pico).
                if (!settled || nowMs - drainSinceMs < CONFIRM_MS) return false
                blocked = current
                blockedAtMs = nowMs
                lastChangeMs = nowMs
                drainSinceMs = -1
                peakPercent = percent
                return setCurrent(current.lower())
            }
            // No queda nada que bajar (o «adaptar» está apagado): se avisa si la descarga se mantiene.
            if (nowMs - drainSinceMs >= DRAIN_WARN_MS) state = ChargeState.DRAINING
            return false
        }

        state = ChargeState.CHARGING
        if (adapt && current != base && surplusSinceMs >= 0 && nowMs - surplusSinceMs >= RAISE_AFTER_MS && settled) {
            val next = current.higher()
            val stillBlocked = next == blocked && nowMs - blockedAtMs < BLOCK_MS
            if (!stillBlocked && next.ordinal >= base.ordinal) {
                lastChangeMs = nowMs
                surplusSinceMs = -1
                return setCurrent(next)
            }
        }
        return false
    }

    private fun batteryTarget(percent: Int): PowerProfile =
        if (adapt && percent in 0..LOW_BATTERY_PERCENT) base.lower() else base

    private fun setCurrent(p: PowerProfile): Boolean {
        if (p == current) return false
        current = p
        return true
    }

    /** Texto del indicador, p. ej. «Cargando +2,1 W» o «Se descarga con el cable −0,6 W». */
    fun indicator(): String {
        val w = lastNetMw?.let { " ${"%+.1f".format(it / 1000)} W" } ?: ""
        val profile = if (current != base) " · bajado a ${current.label}" else ""
        return when (state) {
            ChargeState.MEASURING -> "Midiendo consumo…"
            ChargeState.ON_BATTERY -> "Batería · consumo neto$w$profile"
            ChargeState.CHARGING -> "Cargando · neto$w$profile"
            ChargeState.ADAPTING -> "Se descarga con el cable$w · bajando calidad$profile"
            ChargeState.DRAINING -> "Se descarga pese al cable$w$profile"
        }
    }

    companion object {
        /** Por debajo de −0,15 W la batería baja (el ruido de la medida ronda ±0,1 W). */
        const val DRAIN_MW = 150.0
        /** Margen para volver a subir de perfil: el escalón siguiente gasta como mucho ~1 W más (estimado). */
        const val SURPLUS_MW = 1_500.0
        /** Tras un cambio, la medida necesita ~20 s de ventana más el retardo del suavizado. */
        const val SETTLE_MS = 40_000L
        const val RAISE_AFTER_MS = 120_000L
        const val BLOCK_MS = 15 * 60_000L
        const val DRAIN_WARN_MS = 30_000L
        const val CONFIRM_MS = 10_000L
        const val FULL_PERCENT = 98
        const val FULL_DRAIN_MW = 1_000.0
        const val LOW_BATTERY_PERCENT = 20
    }
}
