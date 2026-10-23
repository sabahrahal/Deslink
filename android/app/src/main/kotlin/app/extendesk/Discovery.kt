package app.extendesk

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface

/**
 * PC encontrado: su dirección ([Link.CABLE_HOST] si es por el cable USB), la interfaz por la que respondió,
 * el nombre que da el host (vacío en hosts antiguos) y si entiende la configuración de HELLO.
 */
data class Found(val ip: String, val route: Route?, val name: String = "", val acceptsConfig: Boolean = false) {
    val transport: Transport get() = route?.transport ?: Transport.OTHER
    val isCable: Boolean get() = ip.equals(Link.CABLE_HOST, ignoreCase = true)

    /** Nombre para mostrar: el del PC si lo dio, si no la IP. */
    val title: String get() = name.ifBlank { if (isCable) Strings.get(R.string.pc_by_usb_cable) else ip }

    companion object {
        fun from(ip: String, route: Route?, info: Dlp.StreamInfo?) =
            Found(ip, route, info?.hostName.orEmpty(), info?.acceptsHelloConfig == true)
    }
}

/**
 * Busca PCs sin que el usuario escriba la IP: envía DISCOVER por broadcast desde cada interfaz activa
 * (WiFi, anclaje USB, Ethernet) con el socket atado a esa interfaz, de modo que funciona aunque el
 * WiFi esté apagado, y recoge las respuestas STREAM_INFO. Un host antiguo no conoce DISCOVER: si en
 * [LEGACY_AFTER_MS] nadie responde se manda también HELLO, al que sí contesta.
 */
object Discovery {
    private class Leg(val route: Route, val broadcast: InetAddress, val socket: DatagramSocket)

    private const val LEGACY_AFTER_MS = 1000L

    /** Todos los PCs que respondan en [timeoutMs], por orden de llegada. */
    fun findAll(timeoutMs: Int = 1800): List<Found> {
        val legs = openLegs()
        val found = LinkedHashMap<String, Found>()
        try {
            if (legs.isEmpty()) return emptyList()
            val start = System.currentTimeMillis()
            val deadline = start + timeoutMs
            val buf = ByteArray(2048)
            val reply = DatagramPacket(buf, buf.size)
            var lastSend = 0L
            while (System.currentTimeMillis() < deadline) {
                val now = System.currentTimeMillis()
                if (now - lastSend >= 300) {
                    lastSend = now
                    val legacy = found.isEmpty() && now - start >= LEGACY_AFTER_MS
                    val ts = System.nanoTime() / 1000
                    val packets = listOfNotNull(
                        Dlp.controlPacket(Dlp.TYPE_DISCOVER, ts),
                        if (legacy) Dlp.controlPacket(Dlp.TYPE_HELLO, ts) else null,
                    )
                    for (leg in legs) for (p in packets) {
                        try {
                            leg.socket.send(DatagramPacket(p, p.size, leg.broadcast, Dlp.DEFAULT_PORT))
                        } catch (_: Exception) {
                        }
                    }
                }
                for (leg in legs) {
                    try {
                        reply.length = buf.size
                        leg.socket.receive(reply)
                        val h = Dlp.parseHeader(buf, reply.length) ?: continue
                        if (h.type != Dlp.TYPE_STREAM_INFO) continue
                        val ip = reply.address.hostAddress ?: continue
                        if (ip !in found) found[ip] = Found.from(ip, leg.route, Dlp.parseStreamInfo(buf, h))
                    } catch (_: java.net.SocketTimeoutException) {
                    } catch (_: Exception) {
                    }
                }
            }
        } finally {
            legs.forEach { it.socket.close() }
        }
        return found.values.toList()
    }

    /** El primero que responda (para el diagnóstico y llamadas antiguas). */
    fun find(timeoutMs: Int = 3000): Found? = findAll(timeoutMs).firstOrNull()

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
                        s.soTimeout = 40
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
                    parts += "${Transport.classify(nif.name).label} (${a.hostAddress})"
                }
            }
        } catch (_: Exception) {
        }
        return if (parts.isEmpty()) Strings.get(R.string.networks_none) else parts.joinToString(", ")
    }
}
