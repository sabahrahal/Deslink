package com.desklink

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface

/** PC encontrado: su IP y la interfaz (WiFi, USB…) por la que respondió. */
class Found(val ip: String, val route: Route?)

/**
 * Busca el PC sin que el usuario escriba la IP: envía HELLO por broadcast desde cada interfaz activa
 * (WiFi, anclaje USB, Ethernet) con el socket atado a esa interfaz, de modo que funciona aunque el
 * WiFi esté apagado, y espera la respuesta STREAM_INFO del host.
 */
object Discovery {
    private class Leg(val route: Route, val broadcast: InetAddress, val socket: DatagramSocket)

    fun find(timeoutMs: Int = 3000): Found? {
        val legs = openLegs()
        try {
            val deadline = System.currentTimeMillis() + timeoutMs
            val buf = ByteArray(2048)
            val reply = DatagramPacket(buf, buf.size)
            while (System.currentTimeMillis() < deadline) {
                val hello = Dlp.controlPacket(Dlp.TYPE_HELLO, System.nanoTime() / 1000)
                for (leg in legs) {
                    try {
                        leg.socket.send(DatagramPacket(hello, hello.size, leg.broadcast, Dlp.DEFAULT_PORT))
                    } catch (_: Exception) {
                    }
                }
                for (leg in legs) {
                    try {
                        reply.length = buf.size
                        leg.socket.receive(reply)
                        val h = Dlp.parseHeader(buf, reply.length)
                        if (h != null && h.type == Dlp.TYPE_STREAM_INFO) {
                            return Found(reply.address.hostAddress ?: continue, leg.route)
                        }
                    } catch (_: java.net.SocketTimeoutException) {
                    } catch (_: Exception) {
                    }
                }
            }
        } finally {
            legs.forEach { it.socket.close() }
        }
        return null
    }

    private fun openLegs(): List<Leg> {
        val out = ArrayList<Leg>()
        try {
            for (nif in NetworkInterface.getNetworkInterfaces()) {
                if (!nif.isUp || nif.isLoopback) continue
                for (ia in nif.interfaceAddresses) {
                    val local = ia.address as? Inet4Address ?: continue
                    val bc = ia.broadcast ?: continue
                    try {
                        val s = DatagramSocket(InetSocketAddress(local, 0))
                        s.broadcast = true
                        s.soTimeout = 60
                        out += Leg(Route(nif.name, local, Transport.classify(nif.name)), bc, s)
                    } catch (_: Exception) {
                    }
                }
            }
        } catch (_: Exception) {
        }
        return out
    }

    /** Redes activas de la tablet en texto, para mostrar al usuario cuando la búsqueda falla. */
    fun describeNetworks(): String {
        val parts = ArrayList<String>()
        try {
            for (nif in NetworkInterface.getNetworkInterfaces()) {
                if (!nif.isUp || nif.isLoopback) continue
                for (ia in nif.interfaceAddresses) {
                    val a = ia.address as? Inet4Address ?: continue
                    parts += "${Transport.classify(nif.name).label} (${nif.name} ${a.hostAddress})"
                }
            }
        } catch (_: Exception) {
        }
        return if (parts.isEmpty()) "ninguna" else parts.joinToString(", ")
    }
}
