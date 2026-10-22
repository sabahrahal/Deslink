package com.desklink

/**
 * Diferencia entre el reloj del PC y el de la tablet, para saber cuánto tarda un frame de punta a punta. Con cada
 * PING la tablet manda su hora; el PONG de un host nuevo trae la hora del PC al contestar (ver [Dlp.parsePongHostUs]).
 * Se supone que la ida y la vuelta tardan lo mismo, así que el error es como mucho medio RTT: de las últimas
 * [WINDOW] muestras se usa la de menor RTT, la que menos cola de WiFi tuvo.
 */
class ClockSync {
    private class Sample(val rttUs: Long, val offsetUs: Long)

    private val samples = ArrayDeque<Sample>()

    /** Hora del PC menos hora de la tablet, en µs; null hasta tener una muestra. */
    @Volatile var offsetUs: Long? = null
        private set

    /** Error máximo de [offsetUs]: medio RTT de la muestra usada. */
    @Volatile var errorUs: Long = -1
        private set

    /** [sentUs] y [receivedUs] en el reloj de la tablet; [hostUs] en el del PC. */
    @Synchronized
    fun onPong(sentUs: Long, hostUs: Long, receivedUs: Long) {
        val rtt = receivedUs - sentUs
        if (rtt < 0 || hostUs <= 0) return
        samples.addLast(Sample(rtt, hostUs - (sentUs + rtt / 2)))
        while (samples.size > WINDOW) samples.removeFirst()
        val best = samples.minBy { it.rttUs }
        offsetUs = best.offsetUs
        errorUs = best.rttUs / 2
    }

    /** Una hora del PC pasada al reloj de la tablet; null si aún no se sabe la diferencia. */
    fun toLocal(hostUs: Long): Long? = offsetUs?.let { hostUs - it }

    @Synchronized
    fun reset() {
        samples.clear()
        offsetUs = null
        errorUs = -1
    }

    companion object {
        const val WINDOW = 10
    }
}

/**
 * Dónde se va el tiempo de cada frame, desde que el PC lo manda al codificador hasta que se ve en la pantalla de la
 * tablet. Etapas (medias del último periodo, en µs):
 * - [Snapshot.arriveUs]: codificar en el PC, enviar y cruzar la red hasta tener el frame entero (necesita [ClockSync]).
 * - [Snapshot.holdUs]: el frame ya está entero pero espera a que se recupere uno anterior (reenvíos).
 * - [Snapshot.decodeUs]: dentro del decodificador de la tablet.
 * - [Snapshot.screenUs]: de salir del decodificador a encenderse en la pantalla (el siguiente refresco).
 * - [Snapshot.totalUs]: todo junto, de punta a punta (necesita [ClockSync]); [Snapshot.maxTotalUs] el peor frame.
 * Se llama desde el hilo del decodificador y se lee una vez por segundo desde otro: todo va sincronizado.
 */
class LatencyStats(private val clock: ClockSync) {
    class Snapshot(
        val frames: Int,
        val arriveUs: Long,
        val holdUs: Long,
        val decodeUs: Long,
        val screenUs: Long,
        val totalUs: Long,
        val maxTotalUs: Long,
    ) {
        val hasTotal get() = totalUs >= 0
        val hasScreen get() = screenUs >= 0

        /** Texto corto para las estadísticas de la tablet. */
        fun text(): String {
            if (frames == 0) return ""
            val parts = buildList {
                if (arriveUs >= 0) add("PC y red ${ms(arriveUs)}")
                if (holdUs > 0) add("esperar reenvíos ${ms(holdUs)}")
                add("decodificar ${ms(decodeUs)}")
                if (screenUs >= 0) add("pantalla ${ms(screenUs)}")
            }
            val total = if (hasTotal) "retardo ${ms(totalUs)} ms (peor ${ms(maxTotalUs)}): " else "retardo: "
            return total + parts.joinToString(" + ") { "$it ms" }
        }

        private fun ms(us: Long) = "%.1f".format(us / 1000.0)
    }

    private var frames = 0
    private var arriveSum = 0L
    private var arriveN = 0
    private var holdSum = 0L
    private var decodeSum = 0L
    private var screenSum = 0L
    private var screenN = 0
    private var totalSum = 0L
    private var totalN = 0
    private var maxTotal = 0L

    /**
     * Un frame que ya salió del decodificador. [hostUs] es su marca de tiempo (reloj del PC); el resto, en el reloj de
     * la tablet: [completeUs] cuando llegó su último paquete, [queuedUs] cuando entró al decodificador, [decodedUs]
     * cuando salió y [shownUs] cuando se encendió en pantalla (null si Android no lo dice).
     */
    @Synchronized
    fun record(hostUs: Long, completeUs: Long, queuedUs: Long, decodedUs: Long, shownUs: Long?) {
        frames++
        holdSum += (queuedUs - completeUs).coerceAtLeast(0)
        decodeSum += (decodedUs - queuedUs).coerceAtLeast(0)
        if (shownUs != null && shownUs >= decodedUs) {
            screenSum += shownUs - decodedUs
            screenN++
        }
        val sent = clock.toLocal(hostUs) ?: return
        // Con un reloj mal estimado (p. ej. el primer PONG llegó tarde) saldrían valores absurdos: se ignoran.
        val arrive = completeUs - sent
        if (arrive !in 0..MAX_PLAUSIBLE_US) return
        arriveSum += arrive
        arriveN++
        val end = if (shownUs != null && shownUs >= decodedUs) shownUs else decodedUs
        val total = end - sent
        totalSum += total
        totalN++
        if (total > maxTotal) maxTotal = total
    }

    /** Medias desde la última llamada; vuelve a empezar. */
    @Synchronized
    fun take(): Snapshot {
        val s = Snapshot(
            frames = frames,
            arriveUs = if (arriveN > 0) arriveSum / arriveN else -1,
            holdUs = if (frames > 0) holdSum / frames else 0,
            decodeUs = if (frames > 0) decodeSum / frames else 0,
            screenUs = if (screenN > 0) screenSum / screenN else -1,
            totalUs = if (totalN > 0) totalSum / totalN else -1,
            maxTotalUs = maxTotal,
        )
        frames = 0
        arriveSum = 0; arriveN = 0
        holdSum = 0; decodeSum = 0
        screenSum = 0; screenN = 0
        totalSum = 0; totalN = 0
        maxTotal = 0
        return s
    }

    private companion object {
        const val MAX_PLAUSIBLE_US = 5_000_000L
    }
}
