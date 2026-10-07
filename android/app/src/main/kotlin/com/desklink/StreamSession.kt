package com.desklink

import android.os.Process
import android.view.Surface
import java.io.IOException
import java.util.concurrent.Executors

/**
 * Sesión de recepción: envía HELLO al host, recibe vídeo (UDP o cable USB, ver [Link]), lo reensambla y lo
 * entrega al decodificador. Sin colas: un frame que no cabe en el decodificador se descarta
 * y se pide un frame clave.
 */
class StreamSession(
    private val host: String,
    private val surface: Surface,
    private val listener: Listener,
) {
    interface Listener {
        fun onStreamInfo(info: Dlp.StreamInfo)
        fun onStats(text: String)
        fun onError(message: String)
    }

    @Volatile private var running = false
    @Volatile private var decoder: VideoDecoder? = null
    @Volatile private var streamInfo: Dlp.StreamInfo? = null
    @Volatile private var rttUs = -1L
    @Volatile private var routeLabel = ""
    @Volatile private var lastRxUs = 0L

    @Volatile private var link: Link? = null
    private var receiveThread: Thread? = null
    private var controlThread: Thread? = null
    private val reassembler = FrameReassembler()

    private val touchExecutor = Executors.newSingleThreadExecutor { Thread(it, "desklink-touch") }
    private var bytesReceived = 0L
    private var framesReceived = 0L
    private var decoderDrops = 0L
    private var lastKeyframeRequestUs = 0L

    fun start() {
        running = true
        receiveThread = Thread({ receiveLoop() }, "desklink-receive").also { it.start() }
        controlThread = Thread({ controlLoop() }, "desklink-control").also { it.start() }
    }

    fun stop() {
        running = false
        touchExecutor.shutdownNow()
        link?.close()
        receiveThread?.interrupt()
        receiveThread?.join(500)
        controlThread?.interrupt()
        controlThread?.join(500)
        decoder?.release()
        decoder = null
    }

    private fun nowUs() = System.nanoTime() / 1000

    private fun send(packet: ByteArray) {
        link?.send(packet)
    }

    private fun receiveLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        val cable = host.equals(Link.CABLE_HOST, ignoreCase = true)
        val buf = ByteArray(2048)
        var reported = false
        while (running) {
            val l: Link
            try {
                l = Link.open(host)
                routeLabel = l.route?.let { "[${it.transport.label}] " } ?: ""
                link = l
            } catch (e: IOException) {
                if (!cable) {
                    listener.onError("No se pudo conectar con $host: ${e.message}")
                    return
                }
                // Por cable, el host o adb pueden tardar en volver (p. ej. al reiniciar desklink-host.exe).
                if (!reported) listener.onError("Esperando al PC por el cable USB…")
                reported = true
                if (!sleepWhileRunning(1000)) return
                continue
            }
            if (!running) {
                l.close()
                return
            }
            l.send(Dlp.controlPacket(Dlp.TYPE_HELLO, nowUs()))
            // Por UDP se despierta a menudo para reclamar paquetes perdidos y entregar frames retenidos;
            // por cable no se pierde nada y basta con despertar de vez en cuando (menos CPU y batería).
            val waitMs = if (l is CableLink) 100 else 4
            while (running) {
                val length = try {
                    l.receive(buf, waitMs)
                } catch (e: IOException) {
                    if (running && !cable) listener.onError("Conexión perdida: ${e.message}")
                    break
                }
                if (length < 0) {
                    pump()
                    continue
                }
                val header = Dlp.parseHeader(buf, length) ?: continue
                lastRxUs = nowUs()
                when (header.type) {
                    Dlp.TYPE_VIDEO -> onVideo(buf, length)
                    Dlp.TYPE_STREAM_INFO -> onStreamInfo(buf, header)
                    Dlp.TYPE_PONG -> rttUs = nowUs() - header.timestampUs
                }
            }
            l.close()
            if (!cable || !running) return
            // El cable se cortó (host cerrado, cable desenchufado): se reintenta solo.
            listener.onStats("Conexión USB perdida. Reconectando…")
            if (!sleepWhileRunning(1000)) return
        }
    }

    private fun sleepWhileRunning(ms: Long): Boolean {
        try {
            Thread.sleep(ms)
        } catch (_: InterruptedException) {
            return false
        }
        return running
    }

    private fun onStreamInfo(buf: ByteArray, header: Dlp.Header) {
        val info = Dlp.parseStreamInfo(buf, header) ?: return
        val current = streamInfo
        if (current != null && current.width == info.width && current.height == info.height &&
            current.fps == info.fps && current.codec == info.codec
        ) {
            return
        }
        try {
            decoder?.release()
            decoder = VideoDecoder(surface, info) { listener.onError(it) }
            streamInfo = info
            listener.onStreamInfo(info)
        } catch (e: Exception) {
            listener.onError("No se pudo iniciar el decodificador: ${e.message}")
        }
    }

    private fun onVideo(buf: ByteArray, length: Int) {
        val dec = decoder ?: return
        bytesReceived += length
        val now = nowUs()
        var frame = reassembler.push(buf, length, now)
        while (frame != null) {
            deliver(dec, frame)
            frame = reassembler.poll(now)
        }
        afterPoll(now)
    }

    /** Sin paquetes nuevos también hay que reclamar los perdidos y soltar frames que esperaron demasiado. */
    private fun pump() {
        val dec = decoder ?: return
        val now = nowUs()
        var frame = reassembler.poll(now)
        while (frame != null) {
            deliver(dec, frame)
            frame = reassembler.poll(now)
        }
        afterPoll(now)
    }

    private fun deliver(dec: VideoDecoder, frame: Frame) {
        framesReceived++
        // Mientras falte un frame clave, los frames intermedios no se pueden decodificar.
        if (!reassembler.needsKeyframe || frame.keyframe) {
            if (!dec.queue(frame)) {
                decoderDrops++
                requestKeyframe()
            }
        }
    }

    private fun afterPoll(now: Long) {
        // Reintento a ~2 RTT (mínimo 4 ms); la espera máxima de un frame crece con el RTT.
        val rtt = rttUs.takeIf { it > 0 } ?: 2_000L
        reassembler.graceUs = (4 * rtt + 8_000).coerceIn(15_000L, 60_000L)
        for (n in reassembler.collectNacks(now, retryUs = maxOf(2 * rtt, 4_000L))) {
            send(Dlp.nack(n.frameId, n.indices, n.count, now))
        }
        if (reassembler.needsKeyframe) requestKeyframe()
    }

    /** Manda el estado táctil completo (ver [Dlp.touch]); se llama desde el hilo de la interfaz. */
    fun sendTouch(state: FloatArray, n: Int) {
        val packet = Dlp.touch(state, n, nowUs())
        // Android no permite enviar por red desde el hilo de la interfaz.
        touchExecutor.execute { send(packet) }
    }

    private fun requestKeyframe() {
        val now = nowUs()
        if (now - lastKeyframeRequestUs < 500_000) return
        lastKeyframeRequestUs = now
        send(Dlp.controlPacket(Dlp.TYPE_KEYFRAME_REQUEST, now))
    }

    /** Cada segundo: HELLO hasta que el host responda, PING para medir RTT y estadísticas. */
    private fun controlLoop() {
        var lastBytes = 0L
        var lastFrames = 0L
        var lastDecoded = 0L
        var lastLost = 0L
        var lastTime = nowUs()
        var helloAttempts = 0
        while (running) {
            val info = streamInfo
            // Si el host dejó de oírnos (p. ej. un corte de red) nos habrá olvidado: un HELLO lo reengancha.
            val silent = info != null && nowUs() - lastRxUs > 3_000_000
            send(Dlp.controlPacket(if (info == null || silent) Dlp.TYPE_HELLO else Dlp.TYPE_PING, nowUs()))
            try {
                Thread.sleep(1000)
            } catch (_: InterruptedException) {
                break
            }
            if (info == null) {
                helloAttempts++
                listener.onStats(
                    if (helloAttempts < 5) {
                        "${routeLabel}Conectando con $host…"
                    } else {
                        if (host.equals(Link.CABLE_HOST, ignoreCase = true)) {
                            "Sin respuesta del PC por el cable USB. Comprueba que desklink-host.exe está abierto " +
                                "y que dice «conectada por USB (ADB)»."
                        } else {
                            "Sin respuesta del PC en $host:${Dlp.DEFAULT_PORT}. Comprueba que desklink-host.exe " +
                                "está abierto, que el cortafuegos permite UDP ${Dlp.DEFAULT_PORT} y que el PC y " +
                                "la tablet están en la misma red."
                        }
                    },
                )
                continue
            }
            val now = nowUs()
            val secs = (now - lastTime) / 1e6
            val decoded = decoder?.decodedFrames ?: 0L
            val mbps = (bytesReceived - lastBytes) * 8 / secs / 1e6
            val rtt = rttUs
            listener.onStats(
                "$routeLabel${info.width}x${info.height} · recibidos ${"%.0f".format((framesReceived - lastFrames) / secs)}/s" +
                    " · decodificados ${"%.0f".format((decoded - lastDecoded) / secs)}/s" +
                    " · ${"%.1f".format(mbps)} Mbps" +
                    (if (rtt >= 0) " · RTT ${"%.1f".format(rtt / 1000.0)} ms" else "") +
                    " · perdidos ${reassembler.framesDropped + decoderDrops}" +
                    " · reclamados ${reassembler.packetsRequested}",
            )
            val lost = reassembler.framesDropped + decoderDrops
            send(Dlp.rateReport((framesReceived - lastFrames).toInt(), (lost - lastLost).toInt(), now))
            lastLost = lost
            lastBytes = bytesReceived
            lastFrames = framesReceived
            lastDecoded = decoded
            lastTime = now
        }
    }
}
