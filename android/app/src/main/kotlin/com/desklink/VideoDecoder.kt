package com.desklink

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.view.Surface

/**
 * Decodificador por hardware en modo asíncrono y baja latencia. Cada frame se envía al
 * decodificador en cuanto llega y cada salida se pinta en la superficie sin esperar.
 */
class VideoDecoder(
    surface: Surface,
    info: Dlp.StreamInfo,
    /** Dónde se apunta el retardo de cada frame mostrado (null = no se mide). */
    private val latency: LatencyStats? = null,
    private val onError: (String) -> Unit,
) {
    private val codec: MediaCodec
    /**
     * Hilo propio para los avisos del decodificador. Sin él, Android los entrega en el hilo principal, que también
     * pinta la interfaz y atiende el táctil: cada frame ya decodificado esperaba ahí su turno antes de mostrarse.
     */
    private val callbackThread = HandlerThread("desklink-decoder", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    /** Buffers de entrada libres; también hace de monitor para esperar uno (wait/notify). */
    private val freeInputs = ArrayDeque<Int>()
    private var released = false // protegido por freeInputs

    @Volatile
    var decodedFrames = 0L
        private set

    /** Tiempos de un frame (reloj de la tablet, µs) mientras pasa por el decodificador y hasta verse. */
    private class Timing(val completeUs: Long, val queuedNanos: Long) {
        /** Cuándo entró de verdad al decodificador, tras esperar un buffer de entrada libre. */
        var inputNanos = queuedNanos
        var decodedNanos = 0L
    }

    /** Cada frame que está dentro del decodificador, por su marca de tiempo, para medir cuánto tarda. */
    private val arrivals = LinkedHashMap<Long, Timing>()

    /** Frames ya decodificados que esperan el aviso de que se encendieron en pantalla. */
    private val awaitingShow = LinkedHashMap<Long, Timing>()

    /** Android avisó al menos una vez de un frame en pantalla (algunos dispositivos no lo hacen nunca). */
    @Volatile private var showCallbacks = false

    /** Suma de lo que tardaron los frames ya mostrados (llegada → salida del decodificador) y cuántos son. */
    @Volatile
    var decodeNanos = 0L
        private set
    @Volatile
    var decodeCount = 0L
        private set

    /** Modelo, Android y decodificador, para el log del PC (CLIENT_INFO). */
    val description: String

    /** Ajustes propios del fabricante que admite el decodificador y cuáles de baja latencia se activaron, para el log. */
    private var vendorInfo = ""

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
            name.startsWith("c2.qti") || name.startsWith("omx.qcom") -> {
                format.setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
                // Saca cada frame en cuanto se decodifica, sin esperar a ordenarlo (el host no manda frames B).
                format.setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
            }
            name.startsWith("c2.exynos") || name.startsWith("omx.exynos") ->
                format.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
            name.startsWith("c2.amlogic") || name.startsWith("omx.amlogic") ->
                format.setInteger("vendor.low-latency.enable", 1)
            // MediaTek no usa una clave «vendor.»: su decodificador lee esta (la misma que usa Moonlight). En una
            // Lenovo con MediaTek, sin ella cada frame tardaba ~17 ms fijos en salir, fuera cual fuera su tamaño.
            name.startsWith("c2.mtk") || name.startsWith("omx.mtk") ->
                format.setInteger("vdec-lowlatency", 1)
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
                    val now = System.nanoTime()
                    c.releaseOutputBuffer(index, now)
                    decodedFrames++
                    synchronized(arrivals) { arrivals.remove(bufferInfo.presentationTimeUs) }?.let {
                        decodeNanos += now - it.queuedNanos
                        decodeCount++
                        it.decodedNanos = now
                        onDecoded(bufferInfo.presentationTimeUs, it)
                    }
                } catch (_: IllegalStateException) {
                    // El códec se cerró mientras se entregaba la salida.
                }
            }

            override fun onOutputFormatChanged(c: MediaCodec, format: MediaFormat) = Unit

            override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
                onError("Error del decodificador: ${e.diagnosticInfo}")
            }
        }, Handler(callbackThread.looper))
        // Cuándo se encendió de verdad cada frame en la pantalla (el refresco en que se mostró). Los frames que Android
        // descarta porque en ese refresco ya había otro más nuevo no avisan.
        codec.setOnFrameRenderedListener({ _, presentationTimeUs, nanoTime ->
            val first = !showCallbacks
            showCallbacks = true
            val t = synchronized(awaitingShow) { awaitingShow.remove(presentationTimeUs) }
            // El primer aviso: ese frame ya se apuntó sin pantalla (aún no se sabía si este dispositivo avisa).
            // Algunos (p. ej. MediaTek con Android 16) no dicen cuándo se encendió, sino la hora pedida al soltarlo, que
            // es la misma con la que se apuntó la salida: eso daba «pantalla 0,0 ms». Un refresco real nunca es tan rápido.
            val shown = nanoTime.takeIf { t != null && it - t.decodedNanos >= MIN_REAL_SHOW_NANOS }
            if (t != null && !first) record(presentationTimeUs, t, shown)
        }, Handler(callbackThread.looper))
        codec.configure(format, surface, null, 0)
        codec.start()
        enableVendorLowLatency()
        description = describe(codec, mime, format) + vendorInfo
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
        val arrived = System.nanoTime()
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
                synchronized(arrivals) {
                    // Si el decodificador se traga algún frame sin sacarlo, no se acumula para siempre.
                    if (arrivals.size >= 32) arrivals.remove(arrivals.keys.first())
                    arrivals[frame.timestampUs] = Timing(frame.completeUs, arrived).also { it.inputNanos = System.nanoTime() }
                }
                codec.queueInputBuffer(index, 0, frame.size, frame.timestampUs, 0)
                true
            }
        } catch (_: IllegalStateException) {
            false
        }
    }

    private fun onDecoded(ptsUs: Long, t: Timing) {
        if (latency == null) return
        // Hasta que Android avise de un frame en pantalla se apunta al salir del decodificador, sin la etapa de pantalla.
        if (!showCallbacks) record(ptsUs, t, null)
        synchronized(awaitingShow) {
            if (awaitingShow.size >= 32) awaitingShow.remove(awaitingShow.keys.first())
            awaitingShow[ptsUs] = t
        }
    }

    private fun record(ptsUs: Long, t: Timing, shownNanos: Long?) {
        latency?.record(ptsUs, t.completeUs, t.queuedNanos / 1000, t.decodedNanos / 1000, shownNanos?.div(1000), t.inputNanos / 1000)
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
        callbackThread.quitSafely()
    }

    /**
     * Desde Android 12 el decodificador dice qué ajustes propios del fabricante admite. Si alguno es de baja latencia
     * (en una Lenovo con MediaTek y Android 16 `vdec-lowlatency` no hacía nada: ~17 ms fijos por frame), se activa aquí.
     * Todo va al log del PC para saber qué probar en cada tablet.
     */
    private fun enableVendorLowLatency() {
        if (Build.VERSION.SDK_INT < 31) return
        val names = try {
            codec.supportedVendorParameters
        } catch (_: IllegalStateException) {
            return
        }
        val enabled = mutableListOf<String>()
        for (name in names.filter { LOW_LATENCY_NAME.containsMatchIn(it) }) {
            try {
                if (codec.getParameterDescriptor(name)?.type != MediaFormat.TYPE_INTEGER) continue
                codec.subscribeToVendorParameters(listOf(name))
                codec.setParameters(android.os.Bundle().apply { putInt(name, 1) })
                enabled += name
            } catch (_: RuntimeException) {
                // Ajuste de solo lectura o que el códec rechaza: se sigue con los demás.
            }
        }
        vendorInfo = ", baja latencia activada: ${enabled.joinToString().ifEmpty { "ninguna" }}" +
            ", ajustes del fabricante (${names.size}): ${names.sorted().joinToString()}"
    }

    private fun describe(codec: MediaCodec, mime: String, format: MediaFormat): String {
        val info = codec.codecInfo
        val lowLatency = if (Build.VERSION.SDK_INT >= 30) {
            try {
                if (info.getCapabilitiesForType(mime).isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) "sí" else "no"
            } catch (_: IllegalArgumentException) {
                "no se sabe"
            }
        } else {
            "no se sabe (Android anterior al 11)"
        }
        val hardware = if (Build.VERSION.SDK_INT >= 29) (if (info.isHardwareAccelerated) "por hardware" else "por software") else ""
        return "${Build.MANUFACTURER} ${Build.MODEL} (Android ${Build.VERSION.RELEASE}, ${Build.HARDWARE}), " +
            "decodificador ${codec.name} $hardware, modo de baja latencia: $lowLatency" +
            VENDOR_LOW_LATENCY_KEYS.filter { format.containsKey(it) }.let { if (it.isEmpty()) "" else ", ajuste propio: ${it.joinToString()}" }
    }

    private companion object {
        /** Claves de baja latencia de cada fabricante que se ponen arriba (para decir en el log cuál se usó). */
        val VENDOR_LOW_LATENCY_KEYS = listOf(
            "vendor.qti-ext-dec-low-latency.enable", "vendor.rtc-ext-dec-low-latency.enable",
            "vendor.low-latency.enable", "vdec-lowlatency",
        )

        /** Lo más que espera [queue] a que el decodificador libere un buffer de entrada. */
        const val INPUT_WAIT_MS = 20L

        /** Menos que esto entre salir del decodificador y «encenderse» no es un refresco real (ver arriba). */
        const val MIN_REAL_SHOW_NANOS = 100_000L

        /** Nombre de un ajuste del fabricante que parece de baja latencia (low-latency, lowlatency, low_latency...). */
        val LOW_LATENCY_NAME = Regex("low[-_.]?latency", RegexOption.IGNORE_CASE)
    }
}
