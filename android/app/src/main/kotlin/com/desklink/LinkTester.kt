package com.desklink

/**
 * Prueba de conexión: envía PINGs del protocolo DLP (uno cada 20 ms) y mide el tiempo hasta el PONG.
 * Usa el mismo camino que el vídeo ([Link]), de modo que sirve igual por WiFi, anclaje USB o cable USB.
 */
object LinkTester {
    fun run(host: String, count: Int = 100, onProgress: (Int) -> Unit = {}): LinkResult {
        val rtts = ArrayList<Double>()
        val link = Link.open(host)
        link.use {
            val buf = ByteArray(2048)
            for (i in 0 until count) {
                val sentAt = System.nanoTime()
                link.send(Dlp.controlPacket(Dlp.TYPE_PING, sentAt / 1000))
                val deadline = sentAt + 20_000_000L
                while (System.nanoTime() < deadline) {
                    val n = link.receive(buf, 20)
                    if (n < 0) break
                    val h = Dlp.parseHeader(buf, n) ?: continue
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
        return Advisor.summarize(count, rtts).copy(route = link.route)
    }

    /**
     * Prueba de ráfaga: el host envía [count] paquetes de 1200 bytes seguidos (como un frame clave).
     * Con [paceMbps] > 0 los reparte a esa tasa. Mide cuántos llegan y a qué velocidad.
     */
    fun burst(host: String, count: Int = 400, paceMbps: Int = 0): BurstResult {
        Link.open(host).use { link ->
            val buf = ByteArray(2048)
            val nonce = System.nanoTime() / 1000
            link.send(Dlp.burstRequest(count, Dlp.MAX_PACKET_SIZE, paceMbps, nonce))
            var received = 0
            var first = 0L
            var last = 0L
            val deadline = System.nanoTime() + 3_000_000_000L
            while (received < count && System.nanoTime() < deadline) {
                val n = link.receive(buf, 300)
                if (n < 0) break // 300 ms sin recibir nada: la ráfaga terminó
                val h = Dlp.parseHeader(buf, n) ?: continue
                if (h.type != Dlp.TYPE_BURST_DATA || h.timestampUs != nonce) continue
                val now = System.nanoTime()
                if (received == 0) first = now
                last = now
                received++
            }
            val spanMs = if (received > 1) (last - first) / 1e6 else 0.0
            val mbps = if (spanMs > 0) (received - 1) * Dlp.MAX_PACKET_SIZE * 8 / (spanMs * 1000) else 0.0
            return BurstResult(count, received, mbps, paceMbps)
        }
    }
}
