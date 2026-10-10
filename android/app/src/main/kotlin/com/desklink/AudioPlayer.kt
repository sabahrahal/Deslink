package com.desklink

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Process
import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

/**
 * Reproduce el sonido del PC: los trozos Opus que llegan (ver [push]) pasan por un [AudioJitterBuffer],
 * se decodifican con el decodificador Opus de Android (MediaCodec, sin librerías nativas) y suenan por un
 * AudioTrack de baja latencia. El volumen es el de multimedia de la tablet (botones de volumen).
 */
class AudioPlayer(cable: Boolean) {
    // Por cable no hay altibajos de red: basta con 20 ms de colchón. Por WiFi se empieza con 30 ms y se adapta.
    private val buffer = AudioJitterBuffer(minTarget = if (cable) 2 else 3)
    private val signal = Semaphore(0)  // despierta al hilo de sonido cuando llega un trozo
    @Volatile private var running = false
    private var thread: Thread? = null

    // Para el log del PC (ver [report]): cuántos trozos llegan, cuántos salen por el altavoz y qué falló.
    @Volatile private var received = 0L
    @Volatile private var played = 0L
    @Volatile private var decodeErrors = 0L
    @Volatile private var lastError: String? = null

    fun start() {
        running = true
        thread = Thread({ playLoop() }, "desklink-audio").also { it.start() }
    }

    fun stop() {
        running = false
        signal.release()
        thread?.join(500)
        thread = null
    }

    fun push(frame: Dlp.AudioFrame) {
        received++
        buffer.push(frame.seq, frame.data, nowMs())
        if (signal.availablePermits() == 0) signal.release()
    }

    /** Para las estadísticas: colchón actual y cortes desde el principio. */
    fun statsText(): String = "sonido ${buffer.target * AudioJitterBuffer.FRAME_MS} ms" +
        (if (buffer.underruns > 0) ", ${buffer.underruns} cortes" else "")

    /** Una línea para el log del PC: qué llega, qué suena y qué falló desde el principio. */
    fun report(volume: String): String =
        "Sonido en la tablet: llegaron $received trozos, sonaron $played, ${buffer.lost} perdidos, " +
            "${buffer.underruns} cortes, volumen multimedia $volume" +
            (if (decodeErrors > 0) ", $decodeErrors errores al decodificar" else "") +
            (lastError?.let { ", último error: $it" } ?: "")

    private fun nowMs() = System.nanoTime() / 1_000_000

    private fun playLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val channels = Dlp.AUDIO_CHANNELS
        val frameSamples = Dlp.AUDIO_FRAME_SAMPLES * channels
        val track = try {
            createTrack()
        } catch (e: Exception) {
            lastError = "no se pudo abrir la salida de sonido (${e.message})"
            return
        }
        var decoder: OpusDecoder? = null
        val pcm = ShortArray(8 * frameSamples)
        val stretched = ShortArray(9 * frameSamples)
        val silence = ShortArray(frameSamples)
        var idleSinceMs = -1L
        try {
            while (running) {
                val out = buffer.pop(nowMs())
                when (out.kind) {
                    AudioJitterBuffer.Kind.IDLE -> {
                        // Sin sonido un rato: se pausa la salida para que la tablet no mantenga el audio despierto.
                        val now = nowMs()
                        if (idleSinceMs < 0) idleSinceMs = now
                        if (now - idleSinceMs > 1000 && track.playState == AudioTrack.PLAYSTATE_PLAYING) {
                            track.pause()
                            track.flush()
                        }
                        signal.tryAcquire(5, TimeUnit.MILLISECONDS)
                        signal.drainPermits()
                        continue
                    }
                    AudioJitterBuffer.Kind.LOST -> write(track, silence, silence.size)
                    AudioJitterBuffer.Kind.FRAME -> {
                        val n = try {
                            val dec = decoder ?: OpusDecoder().also { decoder = it }
                            dec.decode(out.data!!, pcm)
                        } catch (e: Exception) {
                            // Un decodificador en mal estado se cambia por uno nuevo.
                            decodeErrors++
                            lastError = "${e.javaClass.simpleName}: ${e.message}"
                            decoder?.close()
                            decoder = null
                            0
                        }
                        if (n > 0) played++
                        val frames = n / channels
                        if (out.adjust != 0 && frames > 1) {
                            val outFrames = frames + out.adjust * maxOf(frames / 100, 1)
                            write(track, stretched, Pcm.stretch(pcm, frames, channels, outFrames, stretched))
                        } else if (n > 0) {
                            write(track, pcm, n)
                        }
                    }
                }
                idleSinceMs = -1
            }
        } catch (_: InterruptedException) {
        } finally {
            decoder?.close()
            track.release()
        }
    }

    private fun write(track: AudioTrack, data: ShortArray, size: Int) {
        if (size <= 0) return
        if (track.playState != AudioTrack.PLAYSTATE_PLAYING) track.play()
        // Bloqueante: marca el ritmo del bucle al de la tarjeta de sonido de la tablet.
        track.write(data, 0, size, AudioTrack.WRITE_BLOCKING)
    }

    private fun createTrack(): AudioTrack {
        val rate = Dlp.AUDIO_SAMPLE_RATE
        val minBytes = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        val frameBytes = Dlp.AUDIO_FRAME_SAMPLES * 4
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(rate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build(),
            )
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .setBufferSizeInBytes(maxOf(minBytes, 4 * frameBytes))
            .build()
        // Capacidad de 40 ms, pero solo se deja llenar ~20 ms (o lo mínimo que pida la tablet): menos retraso.
        track.bufferSizeInFrames = maxOf(minBytes / 4, 2 * Dlp.AUDIO_FRAME_SAMPLES)
        return track
    }
}

/** Decodificador Opus de Android (MediaCodec), 48 kHz estéreo, salida PCM de 16 bits. */
class OpusDecoder : Closeable {
    private val codec: MediaCodec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
    private val info = MediaCodec.BufferInfo()
    private var ptsUs = 0L

    init {
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, Dlp.AUDIO_SAMPLE_RATE, Dlp.AUDIO_CHANNELS)
        format.setByteBuffer("csd-0", ByteBuffer.wrap(opusHead()))
        format.setByteBuffer("csd-1", nanos(0))  // pre-skip: ninguno (no se recorta el principio)
        format.setByteBuffer("csd-2", nanos(80_000_000))  // seek pre-roll estándar de Opus
        try {
            codec.configure(format, null, null, 0)
            codec.start()
        } catch (e: Exception) {
            codec.release()
            throw e
        }
    }

    /** Decodifica un trozo y copia en [out] el PCM que salga (puede ir un trozo por detrás). Devuelve muestras. */
    fun decode(packet: ByteArray, out: ShortArray): Int {
        val inIndex = codec.dequeueInputBuffer(10_000)
        if (inIndex >= 0) {
            val input = codec.getInputBuffer(inIndex)!!
            input.clear()
            input.put(packet)
            codec.queueInputBuffer(inIndex, 0, packet.size, ptsUs, 0)
            ptsUs += 10_000
        }
        var written = 0
        var timeoutUs = 5_000L
        while (true) {
            val outIndex = codec.dequeueOutputBuffer(info, timeoutUs)
            if (outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) continue
            if (outIndex < 0) break
            val buf = codec.getOutputBuffer(outIndex)
            if (buf != null && info.size > 0) {
                buf.position(info.offset)
                buf.limit(info.offset + info.size)
                val shorts = buf.order(ByteOrder.nativeOrder()).asShortBuffer()
                val n = minOf(shorts.remaining(), out.size - written)
                shorts.get(out, written, n)
                written += n
            }
            codec.releaseOutputBuffer(outIndex, false)
            timeoutUs = 0  // lo que ya esté listo, sin esperar más
        }
        return written
    }

    override fun close() {
        try {
            codec.stop()
        } catch (_: Exception) {
        }
        codec.release()
    }

    private companion object {
        /** Cabecera de identificación Opus (RFC 7845): 2 canales, 48 kHz, sin pre-skip ni ganancia. */
        fun opusHead(): ByteArray {
            val b = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN)
            b.put("OpusHead".toByteArray(Charsets.US_ASCII))
            b.put(1)  // versión
            b.put(Dlp.AUDIO_CHANNELS.toByte())
            b.putShort(0)  // pre-skip
            b.putInt(Dlp.AUDIO_SAMPLE_RATE)
            b.putShort(0)  // ganancia
            b.put(0)  // mapa de canales: mono/estéreo
            return b.array()
        }

        fun nanos(value: Long): ByteBuffer =
            ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(value).also { it.flip() }
    }
}
