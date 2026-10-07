package com.desklink

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/**
 * Prueba de conexión: envía PINGs del protocolo DLP (uno cada 20 ms) y mide el tiempo hasta el PONG.
 * Usa el mismo camino que el vídeo (UDP), de modo que sirve igual por WiFi o por USB (anclaje).
 */
object LinkTester {
    fun run(host: String, count: Int = 100, onProgress: (Int) -> Unit = {}): LinkResult {
        val rtts = ArrayList<Double>()
        val route = Route.forHost(host)
        val bound = if (route != null) DatagramSocket(InetSocketAddress(route.local, 0)) else DatagramSocket()
        bound.use { sock ->
            sock.soTimeout = 20
            sock.connect(InetAddress.getByName(host), Dlp.DEFAULT_PORT)
            val buf = ByteArray(2048)
            val reply = DatagramPacket(buf, buf.size)
            for (i in 0 until count) {
                val sentAt = System.nanoTime()
                sock.send(DatagramPacket(Dlp.controlPacket(Dlp.TYPE_PING, sentAt / 1000), Dlp.HEADER_SIZE))
                val deadline = sentAt + 20_000_000L
                while (System.nanoTime() < deadline) {
                    try {
                        reply.length = buf.size
                        sock.receive(reply)
                    } catch (_: SocketTimeoutException) {
                        break
                    }
                    val h = Dlp.parseHeader(buf, reply.length) ?: continue
                    if (h.type == Dlp.TYPE_PONG && h.timestampUs == sentAt / 1000) {
                        rtts += (System.nanoTime() - sentAt) / 1e6
                        break
                    }
                }
                val left = deadline - System.nanoTime()
                if (left > 0) Thread.sleep(left / 1_000_000, (left % 1_000_000).toInt())
                if (i % 10 == 9) onProgress(i + 1)
            }
        }
        return Advisor.summarize(count, rtts).copy(route = route)
    }
}
