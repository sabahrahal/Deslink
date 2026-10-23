package app.extendesk

import kotlin.math.roundToInt

/**
 * Colchón de sonido entre la red y el altavoz, en trozos de 10 ms. Lo llenan los paquetes que llegan (a veces
 * a ráfagas, desordenados o tarde) y lo vacía la tarjeta de sonido de la tablet a su propio ritmo:
 *
 * - Espera a tener [target] trozos antes de empezar a sonar.
 * - Si falta un trozo pero ya llegaron los siguientes, se da por perdido ([Kind.LOST]: se rellena con silencio).
 * - Si se vacía, rellena con silencio sin avanzar, esperando al trozo que llega tarde, y sube [target] 10 ms;
 *   tras 20 s sin cortes lo baja 10 ms. Si el PC deja de enviar (no suena nada) para del todo ([Kind.IDLE]).
 * - El reloj del PC y el de la tablet nunca van exactos: si el colchón medio crece o mengua, pide reproducir
 *   un 1 % más rápido o más lento ([Out.adjust]) para volver al objetivo sin cortes.
 *
 * Se llama desde dos hilos (red y sonido): todo va sincronizado. Los tiempos los pasa quien llama, en ms.
 */
class AudioJitterBuffer(
    private val minTarget: Int,
    private val maxTarget: Int = 12,
) {
    enum class Kind { IDLE, FRAME, LOST }

    /** [adjust]: -1 = reproducir un poco más rápido (sobra colchón), +1 = más lento (falta), 0 = normal. */
    class Out(val kind: Kind, val data: ByteArray? = null, val adjust: Int = 0)

    private val frames = HashMap<Long, ByteArray>()
    private var next = -1L
    private var newest = -1L
    private var playing = false
    private var bufferingSinceMs = -1L
    private var misses = 0
    private var avgDepth = 0.0
    private var lastTargetChangeMs = 0L

    @Volatile var target = minTarget
        private set
    @Volatile var underruns = 0L
        private set
    @Volatile var lost = 0L
        private set
    @Volatile var dropped = 0L
        private set

    @Synchronized fun push(seq: Long, data: ByteArray, nowMs: Long) {
        if (next >= 0 && (seq < next - RESTART_GAP || seq > next + RESTART_GAP)) reset()  // el host empezó de cero
        // Parado y vacío: el trozo que llega marca el nuevo comienzo (salvo una copia de uno recién sonado).
        if (next < 0 || (!playing && frames.isEmpty() && (seq > next || seq < next - 2))) {
            next = seq
            newest = seq - 1
        }
        if (seq < next || frames.containsKey(seq)) return  // tarde (ya se rellenó) o repetido
        frames[seq] = data
        if (seq > newest) newest = seq
        if (!playing && bufferingSinceMs < 0) bufferingSinceMs = nowMs
        // Nunca más de ~0,4 s de retraso: tras un corte largo, lo viejo se tira.
        while (depth() > MAX_FRAMES) {
            if (frames.remove(next) != null) dropped++
            next++
        }
    }

    @Synchronized fun pop(nowMs: Long): Out {
        if (next < 0) return IDLE
        val d = depth()
        if (!playing) {
            // Empieza con el colchón lleno, o con lo que haya si el sonido era muy corto (p. ej. un clic).
            val waited = bufferingSinceMs >= 0 && nowMs - bufferingSinceMs >= target * FRAME_MS
            if (d == 0 || (d < target && !waited)) return IDLE
            playing = true
            misses = 0
            avgDepth = d.toDouble()
            bufferingSinceMs = -1
        }
        if (d == 0) {
            // Vacío: el siguiente trozo llega tarde, o el PC dejó de enviar porque no suena nada.
            misses++
            if (misses > MAX_MISSES) {
                playing = false
                misses = 0
                return IDLE
            }
            if (misses == 1) underruns++
            return LOST_OUT
        }
        if (misses > 0) {
            // Llegó tras un hueco: la red tiene más altibajos que el colchón; se agranda.
            misses = 0
            if (target < maxTarget) {
                target++
                lastTargetChangeMs = nowMs
            }
        } else if (target > minTarget && nowMs - lastTargetChangeMs >= SHRINK_AFTER_MS) {
            target--
            lastTargetChangeMs = nowMs
        }
        // Demasiado retraso de golpe (p. ej. tras una ráfaga): se salta hasta el objetivo.
        if (d > target + CATCH_UP) {
            while (depth() > target + 1) {
                if (frames.remove(next) != null) dropped++
                next++
            }
            avgDepth = depth().toDouble()
        }
        avgDepth += (depth() - avgDepth) * 0.02  // media de ~0,5 s
        val data = frames.remove(next)
        next++
        if (data == null) {
            lost++
            return LOST_OUT
        }
        val adjust = when {
            avgDepth > target + 1.0 -> -1
            avgDepth < target - 0.5 -> 1
            else -> 0
        }
        return Out(Kind.FRAME, data, adjust)
    }

    /** Trozos en el colchón, contando los huecos: del siguiente que toca al más nuevo. */
    @Synchronized fun depth(): Int = if (frames.isEmpty()) 0 else (newest - next + 1).toInt()

    @Synchronized fun reset() {
        frames.clear()
        next = -1
        newest = -1
        playing = false
        bufferingSinceMs = -1
        misses = 0
    }

    companion object {
        const val FRAME_MS = 10
        private const val MAX_FRAMES = 40
        private const val MAX_MISSES = 5
        private const val CATCH_UP = 8
        private const val RESTART_GAP = 500L
        private const val SHRINK_AFTER_MS = 20_000L
        private val IDLE = Out(Kind.IDLE)
        private val LOST_OUT = Out(Kind.LOST)
    }
}

/** Utilidades de PCM de 16 bits intercalado. */
object Pcm {
    /**
     * Estira o encoge [frames] muestras de [channels] canales a [outFrames] por interpolación lineal (para
     * corregir la deriva de reloj un 1 %, inaudible). Escribe en [out] y devuelve las muestras escritas.
     */
    fun stretch(input: ShortArray, frames: Int, channels: Int, outFrames: Int, out: ShortArray): Int {
        if (frames <= 0 || outFrames <= 0) return 0
        if (frames == 1 || outFrames == 1) {
            for (i in 0 until outFrames) for (c in 0 until channels) out[i * channels + c] = input[c]
            return outFrames * channels
        }
        // Los extremos se conservan: la primera y la última muestra de salida son las de entrada.
        val step = (frames - 1).toDouble() / (outFrames - 1)
        for (i in 0 until outFrames) {
            val pos = i * step
            val k = minOf(pos.toInt(), frames - 2)
            val t = pos - k
            for (c in 0 until channels) {
                val a = input[k * channels + c]
                val b = input[(k + 1) * channels + c]
                out[i * channels + c] = (a + (b - a) * t).roundToInt().toShort()
            }
        }
        return outFrames * channels
    }
}
