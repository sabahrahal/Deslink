package com.desklink

import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface

enum class Transport(val label: String) {
    WIFI("WiFi"), USB("USB (anclaje)"), CABLE("USB (cable)"), ETHERNET("Ethernet"), OTHER("Otra red");

    companion object {
        /** Clasifica una interfaz de Android por su nombre (wlan0, rndis0, usb0, ncm0, eth0…). */
        fun classify(interfaceName: String): Transport {
            val n = interfaceName.lowercase()
            return when {
                n.startsWith("wlan") || n.startsWith("wifi") -> WIFI
                n.startsWith("rndis") || n.startsWith("usb") || n.startsWith("ncm") -> USB
                n.startsWith("eth") -> ETHERNET
                else -> OTHER
            }
        }
    }
}

/** Interfaz local por la que se llega al host: sirve para atarle el socket y para saber si es WiFi o USB. */
class Route(val interfaceName: String, val local: InetAddress, val transport: Transport) {
    val description: String get() = "${transport.label} · ${interfaceName} ${local.hostAddress}"

    companion object {
        /** ¿Está [host] dentro de la subred [local]/[prefix]? */
        fun sameSubnet(local: ByteArray, prefix: Int, host: ByteArray): Boolean {
            if (local.size != host.size || prefix !in 0..local.size * 8) return false
            for (bit in 0 until prefix) {
                val mask = 0x80 shr (bit % 8)
                if ((local[bit / 8].toInt() and mask) != (host[bit / 8].toInt() and mask)) return false
            }
            return true
        }

        /** Busca entre las interfaces activas la que está en la misma subred que [host]. */
        fun forHost(host: String): Route? {
            val target = try {
                InetAddress.getByName(host) as? Inet4Address ?: return null
            } catch (_: Exception) {
                return null
            }
            try {
                for (nif in NetworkInterface.getNetworkInterfaces()) {
                    if (!nif.isUp || nif.isLoopback) continue
                    for (ia in nif.interfaceAddresses) {
                        val a = ia.address as? Inet4Address ?: continue
                        if (sameSubnet(a.address, ia.networkPrefixLength.toInt(), target.address)) {
                            return Route(nif.name, a, Transport.classify(nif.name))
                        }
                    }
                }
            } catch (_: Exception) {
            }
            return null
        }
    }
}
