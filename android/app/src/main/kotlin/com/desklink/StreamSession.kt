package com.desklink

import android.os.Process
import android.view.Surface
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketTimeoutException

/**
 * Sesión de recepción: envía HELLO al host, recibe vídeo por UDP, lo reensambla y lo
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

    private var socket: DatagramSocket? = null
    private var receiveThread: Thread? = null
    private var controlThread: Thread? = null
    private val reassembler = FrameReassembler()

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
        socket?.close()
        receiveThread?.join(500)
        controlThread?.interrupt()
        controlThread?.join(500)
        decoder?.release()
        decoder = null
    }

    private fun nowUs() = System.nanoTime() / 1000

    private fun send(packet: ByteArray) {
        try {
            socket?.send(DatagramPacket(packet, packet.size))
        } catch (_: IOException) {
        }
    }

    private fun receiveLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        val sock: DatagramSocket
        try {
            sock = DatagramSocket()
            sock.receiveBufferSize = 1 shl 20
            sock.soTimeout = 200
            sock.connect(InetAddress.getByName(host), Dlp.DEFAULT_PORT)
            socket = sock
        } catch (e: IOException) {
            listener.onError("No se pudo conectar con $host: ${e.message}")
            return
        }

        val buf = ByteArray(2048)
        val packet = DatagramPacket(buf, buf.size)
        while (running) {
            packet.length = buf.size
            try {
                sock.receive(packet)
            } catch (_: SocketTimeoutException) {
                continue
            } catch (e: IOException) {
                if (running) listener.onError("Conexión perdida: ${e.message}")
                break
            }
            val length = packet.length
            val header = Dlp.parseHeader(buf, length) ?: continue
            when (header.type) {
                Dlp.TYPE_VIDEO -> onVideo(buf, length)
                Dlp.TYPE_STREAM_INFO -> onStreamInfo(buf, header)
                Dlp.TYPE_PONG -> rttUs = nowUs() - header.timestampUs
            }
        }
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
        val frame = reassembler.push(buf, length)
        if (frame != null) {
            framesReceived++
            // Mientras falte un frame clave, los frames intermedios no se pueden decodificar.
            if (!reassembler.needsKeyframe || frame.keyframe) {
                if (!dec.queue(frame)) {
                    decoderDrops++
                    requestKeyframe()
                }
            }
        }
        if (reassembler.needsKeyframe) requestKeyframe()
    }

    private fun requestKeyframe() {
        val now = nowUs()
        if (now - lastKeyframeRequestUs < 200_000) return
        lastKeyframeRequestUs = now
        send(Dlp.controlPacket(Dlp.TYPE_KEYFRAME_REQUEST, now))
    }

    /** Cada segundo: HELLO hasta que el host responda, PING para medir RTT y estadísticas. */
    private fun controlLoop() {
        var lastBytes = 0L
        var lastFrames = 0L
        var lastDecoded = 0L
        var lastTime = nowUs()
        while (running) {
            val info = streamInfo
            send(Dlp.controlPacket(if (info == null) Dlp.TYPE_HELLO else Dlp.TYPE_PING, nowUs()))
            try {
                Thread.sleep(1000)
            } catch (_: InterruptedException) {
                break
            }
            if (info == null) {
                listener.onStats("Conectando con $host…")
                continue
            }
            val now = nowUs()
            val secs = (now - lastTime) / 1e6
            val decoded = decoder?.decodedFrames ?: 0L
            val mbps = (bytesReceived - lastBytes) * 8 / secs / 1e6
            val rtt = rttUs
            listener.onStats(
                "${info.width}x${info.height} · recibidos ${"%.0f".format((framesReceived - lastFrames) / secs)}/s" +
                    " · decodificados ${"%.0f".format((decoded - lastDecoded) / secs)}/s" +
                    " · ${"%.1f".format(mbps)} Mbps" +
                    (if (rtt >= 0) " · RTT ${"%.1f".format(rtt / 1000.0)} ms" else "") +
                    " · perdidos ${reassembler.framesDropped + decoderDrops}",
            )
            lastBytes = bytesReceived
            lastFrames = framesReceived
            lastDecoded = decoded
            lastTime = now
        }
    }
}
