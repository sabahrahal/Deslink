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
    private val freeInputs = ArrayDeque<Int>()

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

        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(c: MediaCodec, index: Int) {
                synchronized(freeInputs) { freeInputs.addLast(index) }
            }

            override fun onOutputBufferAvailable(c: MediaCodec, index: Int, bufferInfo: MediaCodec.BufferInfo) {
                try {
                    c.releaseOutputBuffer(index, true) // se pinta ya, sin esperar al vsync de un temporizador
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

    /** Devuelve false si no había buffer de entrada libre (el decodificador va retrasado). */
    fun queue(frame: Frame): Boolean {
        val index = synchronized(freeInputs) { if (freeInputs.isEmpty()) null else freeInputs.removeFirst() } ?: return false
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
        try {
            codec.stop()
        } catch (_: IllegalStateException) {
        }
        codec.release()
    }
}
