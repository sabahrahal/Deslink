package com.desklink

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface

/**
 * Busca el PC sin que el usuario escriba la IP: envía HELLO por broadcast a cada red activa
 * (WiFi, anclaje USB, Ethernet) y espera la respuesta STREAM_INFO del host.
 */
object Discovery {
    /** Devuelve la IP del primer host que responde, o null si nadie contesta en [timeoutMs]. */
    fun find(timeoutMs: Int = 3000): String? {
        val targets = broadcastAddresses()
        DatagramSocket().use { sock ->
            sock.broadcast = true
            sock.soTimeout = 250
            val deadline = System.currentTimeMillis() + timeoutMs
            val buf = ByteArray(2048)
            val reply = DatagramPacket(buf, buf.size)
            while (System.currentTimeMillis() < deadline) {
                val hello = Dlp.controlPacket(Dlp.TYPE_HELLO, System.nanoTime() / 1000)
                for (t in targets) {
                    try {
                        sock.send(DatagramPacket(hello, hello.size, t, Dlp.DEFAULT_PORT))
                    } catch (_: Exception) {
                    }
                }
                try {
                    reply.length = buf.size
                    sock.receive(reply)
                    val h = Dlp.parseHeader(buf, reply.length)
                    if (h != null && h.type == Dlp.TYPE_STREAM_INFO) return reply.address.hostAddress
                } catch (_: java.net.SocketTimeoutException) {
                }
            }
        }
        return null
    }

    private fun broadcastAddresses(): List<InetAddress> {
        val out = LinkedHashSet<InetAddress>()
        try {
            for (nif in NetworkInterface.getNetworkInterfaces()) {
                if (!nif.isUp || nif.isLoopback) continue
                for (ia in nif.interfaceAddresses) {
                    val b = ia.broadcast
                    if (b is Inet4Address) out += b
                }
            }
        } catch (_: Exception) {
        }
        out += InetAddress.getByName("255.255.255.255")
        return out.toList()
    }
}
