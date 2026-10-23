package app.extendesk

import android.os.Build
import android.os.Handler
import android.view.Choreographer

/**
 * Cuándo se encenderá en pantalla un frame soltado ahora, para las tablets que no lo dicen (MediaTek con Android 16
 * avisa con la hora pedida, no con la real). Sigue los refrescos con Choreographer: desde Android 13 cada refresco trae
 * sus «líneas de tiempo» (hasta cuándo se puede entregar un frame y cuándo se mostrará); antes, se supone que el frame se
 * recoge en el siguiente refresco y se enciende en el que viene después, que es lo habitual en Android.
 * Es una estimación: queda bien para comparar, pero no sustituye al aviso real.
 */
class ScreenClock(private val handler: Handler) {
    @Volatile private var lastVsyncNanos = 0L
    @Volatile private var periodNanos = 0L
    /** Líneas de tiempo del último refresco, en pares (plazo, se muestra); vacío antes de Android 13. */
    @Volatile private var timelines = LongArray(0)
    @Volatile private var started = false
    @Volatile private var stopped = false

    private val frameCallback = Choreographer.FrameCallback { onVsync(it, null) }

    private fun onVsync(frameTimeNanos: Long, lines: LongArray?) {
        if (stopped) return
        val last = lastVsyncNanos
        if (last > 0) {
            val delta = frameTimeNanos - last
            // Media lenta y solo con saltos de un refresco: un refresco perdido no cuenta como periodo.
            if (delta in 2_000_000L..50_000_000L && (periodNanos == 0L || delta < periodNanos * 3 / 2)) {
                periodNanos = if (periodNanos == 0L) delta else (periodNanos * 15 + delta) / 16
            }
        }
        lastVsyncNanos = frameTimeNanos
        if (lines != null) timelines = lines
        post()
    }

    private fun post() {
        val choreographer = Choreographer.getInstance()
        if (Build.VERSION.SDK_INT >= 33) {
            choreographer.postVsyncCallback { data ->
                val all = data.frameTimelines
                val lines = LongArray(all.size * 2)
                all.forEachIndexed { i, t ->
                    lines[i * 2] = t.deadlineNanos
                    lines[i * 2 + 1] = t.expectedPresentationTimeNanos
                }
                onVsync(data.frameTimeNanos, lines)
            }
        } else {
            choreographer.postFrameCallback(frameCallback)
        }
    }

    /** Hora estimada (System.nanoTime) en que se verá un frame soltado en [releaseNanos]; null mientras no hay datos. */
    fun estimateShown(releaseNanos: Long): Long? {
        if (!started) {
            started = true
            handler.post { post() }
            return null
        }
        return estimate(releaseNanos, lastVsyncNanos, periodNanos, timelines)
    }

    fun stop() {
        stopped = true
    }

    companion object {
        fun estimate(releaseNanos: Long, lastVsyncNanos: Long, periodNanos: Long, timelines: LongArray): Long? {
            if (lastVsyncNanos <= 0 || periodNanos <= 0) return null
            var i = 0
            while (i + 1 < timelines.size) {
                if (timelines[i] >= releaseNanos && timelines[i + 1] > releaseNanos) return timelines[i + 1]
                i += 2
            }
            if (timelines.size >= 2) {
                // Más allá de la última línea de tiempo: se sigue su ritmo.
                val deadline = timelines[timelines.size - 2]
                val present = timelines[timelines.size - 1]
                val steps = (releaseNanos - deadline + periodNanos - 1) / periodNanos
                return present + steps * periodNanos
            }
            // Sin líneas de tiempo: el siguiente refresco lo recoge y el de después lo enciende.
            val sinceVsync = releaseNanos - lastVsyncNanos
            val next = lastVsyncNanos + ((sinceVsync / periodNanos) + 1) * periodNanos
            return next + periodNanos
        }
    }
}
