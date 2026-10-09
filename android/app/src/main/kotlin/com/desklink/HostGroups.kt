package com.desklink

/**
 * Un PC en la pantalla de inicio, con todas las formas de llegar a él: por cable USB y por Wi-Fi (o Ethernet).
 * La búsqueda devuelve una entrada por camino; aquí se juntan por el nombre del PC para enseñar una sola
 * tarjeta con las dos opciones. Lógica pura para poder probarla.
 */
data class PcEntry(
    /** Clave estable de la tarjeta (el nombre en minúsculas, o la dirección si el PC no da nombre). */
    val key: String,
    val name: String,
    val usb: Found?,
    /** Caminos por red, el primero el preferido. */
    val network: List<Found>,
) {
    val hasBoth: Boolean get() = usb != null && network.isNotEmpty()

    /** Camino que se usa al pulsar Conectar: el cable si está y se eligió (o no se dijo nada), si no la red. */
    fun route(preferUsb: Boolean = true): Found = if (usb != null && (preferUsb || network.isEmpty())) usb else network.first()

    /** ¿Alguno de sus caminos es esta dirección? */
    fun matches(address: String?): Boolean = address != null && (usb?.ip == address || network.any { it.ip == address })
}

object HostGroups {
    /** Junta los resultados de una búsqueda por PC; primero los que tienen cable, luego por nombre. */
    fun group(found: List<Found>): List<PcEntry> {
        val byKey = LinkedHashMap<String, MutableList<Found>>()
        for (f in found) {
            val key = f.name.trim().lowercase().ifEmpty { f.ip }
            byKey.getOrPut(key) { ArrayList() }.add(f)
        }
        return byKey.map { (key, list) ->
            val usb = list.firstOrNull { it.isCable }
            val network = list.filter { !it.isCable }.sortedBy { if (it.transport == Transport.WIFI) 1 else 0 }
            PcEntry(key, (list.firstOrNull { it.name.isNotBlank() } ?: list.first()).title, usb, network)
        }.sortedWith(compareBy({ it.usb == null }, { it.name.lowercase() }))
    }

    /** Texto bajo el nombre del PC para el camino elegido. */
    fun describe(f: Found): String = when {
        f.isCable -> "Por cable USB · la conexión más rápida"
        f.transport == Transport.ETHERNET -> "Por cable de red · ${f.ip}"
        else -> "Por Wi-Fi · ${f.ip}"
    }
}
