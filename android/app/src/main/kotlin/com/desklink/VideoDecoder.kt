package com.desklink

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.view.Surface

/**
 * Decodificador por hardware en modo asíncrono y baja latencia. Cada frame se envía al
 * decodificador en cuanto llega y cada salida se pinta en la superficie sin esperar.
 */
class VideoDecoder(
    surface: Surface,
    info: Dlp.StreamInfo,
    private val onError: (String) -> Unit,
) {
    private val codec: MediaCodec
    /** Buffers de entrada libres; también hace de monitor para esperar uno (wait/notify). */
    private val freeInputs = ArrayDeque<Int>()
    private var released = false // protegido por freeInputs

    @Volatile
    var decodedFrames = 0L
        private set

    init {
        val mime = if (info.codec == Dlp.CODEC_HEVC) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        codec = MediaCodec.createDecoderByType(mime)

        val format = MediaFormat.createVideoFormat(mime, info.width, info.height)
        // Un frame clave a 2944x1840 supera el tamaño de entrada por defecto de algunos decodificadores.
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, (info.width * info.height * 3 / 2).coerceIn(1 shl 20, 16 shl 20))
        format.setInteger(MediaFormat.KEY_PRIORITY, 0) // tiempo real
        format.setInteger(MediaFormat.KEY_OPERATING_RATE, if (info.fps > 0) info.fps else 60)
        if (Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        // Antes de Android 11 (y en algunos que ignoran KEY_LOW_LATENCY) el modo de baja latencia va con
        // claves propias del fabricante; un decodificador de otra marca no las ve.
        val name = codec.name.lowercase()
        when {
            name.startsWith("c2.qti") || name.startsWith("omx.qcom") ->
                format.setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            name.startsWith("c2.exynos") || name.startsWith("omx.exynos") ->
                format.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
            name.startsWith("c2.amlogic") || name.startsWith("omx.amlogic") ->
                format.setInteger("vendor.low-latency.enable", 1)
        }
        // El host convierte a BT.709 de rango limitado y lo declara en el flujo; se repite aquí por si el
        // decodificador no lee la VUI (si no, algunos suponen BT.601 y el color sale desviado).
        format.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
        format.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
        format.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)

        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(c: MediaCodec, index: Int) {
                synchronized(freeInputs) {
                    freeInputs.addLast(index)
                    (freeInputs as Object).notifyAll()
                }
            }

            override fun onOutputBufferAvailable(c: MediaCodec, index: Int, bufferInfo: MediaCodec.BufferInfo) {
                try {
                    // Con la hora de ahora: se pinta en el próximo refresco de pantalla y, si en ese refresco ya hay
                    // otro frame más nuevo, Android descarta el viejo en vez de encolarlo. Así no se acumula
                    // retraso cuando llegan dos frames juntos (Wi-Fi a ráfagas) y el movimiento va más fluido.
                    c.releaseOutputBuffer(index, System.nanoTime())
                    decodedFrames++
                } catch (_: IllegalStateException) {
                    // El códec se cerró mientras se entregaba la salida.
                }
            }

            override fun onOutputFormatChanged(c: MediaCodec, format: MediaFormat) = Unit

            override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
                onError("Error del decodificador: ${e.diagnosticInfo}")
            }
        })
        codec.configure(format, surface, null, 0)
        codec.start()
    }

    /** Ajusta el ritmo previsto (perfil de batería) sin rehacer el decodificador; si no lo admite, se ignora. */
    fun setOperatingRate(fps: Int) {
        if (fps <= 0) return
        try {
            codec.setParameters(android.os.Bundle().apply { putInt(MediaFormat.KEY_OPERATING_RATE, fps) })
        } catch (_: IllegalStateException) {
        }
    }

    /**
     * Devuelve false si en [INPUT_WAIT_MS] no quedó libre ningún buffer de entrada (el decodificador va retrasado).
     * Se espera un poco en vez de rendirse al momento: tirar el frame obliga a pedir otro frame clave, que es más
     * grande y atasca todavía más al decodificador.
     */
    fun queue(frame: Frame): Boolean {
        val index = synchronized(freeInputs) {
            val deadline = System.nanoTime() + INPUT_WAIT_MS * 1_000_000L
            while (freeInputs.isEmpty() && !released) {
                val leftMs = (deadline - System.nanoTime()) / 1_000_000L
                if (leftMs <= 0) break
                try {
                    (freeInputs as Object).wait(leftMs)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    break
                }
            }
            if (released || freeInputs.isEmpty()) null else freeInputs.removeFirst()
        } ?: return false
        return try {
            val buffer = codec.getInputBuffer(index)
            if (buffer == null || frame.size > buffer.capacity()) {
                synchronized(freeInputs) { freeInputs.addFirst(index) }
                false
            } else {
                buffer.clear()
                buffer.put(frame.data, 0, frame.size)
                codec.queueInputBuffer(index, 0, frame.size, frame.timestampUs, 0)
                true
            }
        } catch (_: IllegalStateException) {
            false
        }
    }

    fun release() {
        // Despierta a quien espere un buffer de entrada: el códec ya no dará ninguno.
        synchronized(freeInputs) {
            released = true
            freeInputs.clear()
            (freeInputs as Object).notifyAll()
        }
        try {
            codec.stop()
        } catch (_: IllegalStateException) {
        }
        codec.release()
    }

    private companion object {
        /** Lo más que espera [queue] a que el decodificador libere un buffer de entrada. */
        const val INPUT_WAIT_MS = 20L
    }
}
