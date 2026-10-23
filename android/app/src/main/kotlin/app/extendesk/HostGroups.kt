package app.extendesk

/**
 * Un PC en la pantalla de inicio, con todas las formas de llegar a él: por cable USB y por Wi-Fi (o Ethernet).
 * La búsqueda devuelve una entrada por camino; aquí se juntan por el nombre del PC para enseñar una sola
 * tarjeta, que conecta sola por el mejor camino: el cable si lo hay. El anclaje de red por USB también cuenta como
 * cable, no como Wi-Fi. Lógica pura para poder probarla.
 */
data class PcEntry(
    /** Clave estable de la tarjeta (el nombre en minúsculas, o la dirección si el PC no da nombre). */
    val key: String,
    val name: String,
    /**
     * Camino por cable: el anclaje de red por USB si está activo y, si no, el cable USB de Extendesk (adb). Por el
     * anclaje los paquetes van directos; por adb pasan por su túnel y en una Lenovo con MediaTek el peor frame subía
     * de ~55 a 75-95 ms.
     */
    val usb: Found?,
    /** Caminos por red (Wi-Fi, Ethernet), el primero el preferido. */
    val network: List<Found>,
    /** Todos los caminos encontrados, también el cable de adb cuando gana el anclaje. */
    val all: List<Found> = listOfNotNull(usb) + network,
) {
    /** Camino que se usa al pulsar Conectar: el cable si está (no hay nada que elegir), si no la red. */
    fun route(): Found = usb ?: network.first()

    /** ¿Alguno de sus caminos es esta dirección? */
    fun matches(address: String?): Boolean = address != null && all.any { it.ip == address }
}

object HostGroups {
    /** Búsquedas (unos 3 s cada una) que la red espera al cable cuando se sabe que viene: el PC tarda en preparar adb. */
    const val CABLE_WAIT_SCANS = 7

    /** Junta los resultados de una búsqueda por PC; primero los que tienen cable, luego por nombre. */
    fun group(found: List<Found>): List<PcEntry> {
        val byKey = LinkedHashMap<String, MutableList<Found>>()
        for (f in found) {
            val key = f.name.trim().lowercase().ifEmpty { f.ip }
            byKey.getOrPut(key) { ArrayList() }.add(f)
        }
        return byKey.map { (key, list) ->
            val usb = list.firstOrNull { isTether(it) } ?: list.firstOrNull { it.isCable }
            val network = list.filter { !it.isCable && !isTether(it) }.sortedBy { if (it.transport == Transport.WIFI) 1 else 0 }
            PcEntry(key, (list.firstOrNull { it.name.isNotBlank() } ?: list.first()).title, usb, network, list)
        }.sortedWith(compareBy({ it.usb == null }, { it.name.lowercase() }))
    }

    /** ¿Llega por el anclaje de red por USB (rndis, usb, ncm)? Va por el cable aunque sea una IP. */
    fun isTether(f: Found): Boolean = !f.isCable && f.transport == Transport.USB

    /**
     * PC al que conectarse solo al abrir la app: el último usado (por su dirección o por su nombre), y siempre
     * por el cable si llega por él (el anclaje antes que adb). El cable vale en cuanto aparece; cualquier otro camino espera a
     * la segunda búsqueda ([scans] >= 2), por si la prueba del cable solo tardaba. Si la tablet está enchufada con la
     * depuración USB activa ([cableExpected]) el cable va a aparecer en unos segundos, así que la red espera hasta
     * [CABLE_WAIT_SCANS] búsquedas. Null si aún no toca.
     */
    fun autoTarget(
        found: List<Found>,
        lastHost: String?,
        lastHostName: String?,
        scans: Int,
        cableExpected: Boolean = false,
    ): Found? {
        if (lastHost == null) return null
        val name = lastHostName?.trim().orEmpty()
        val pc = group(found).firstOrNull {
            it.matches(lastHost) || (name.isNotEmpty() && it.name.equals(name, ignoreCase = true))
        } ?: return null
        pc.usb?.takeIf { it.isCable || isTether(it) }?.let { return it }
        return if (scans >= (if (cableExpected) CABLE_WAIT_SCANS else 2)) pc.route() else null
    }

    /** Texto bajo el nombre del PC para el camino elegido. Por anclaje o por depuración USB es el mismo cable. */
    fun describe(f: Found): String = when {
        f.isCable || isTether(f) -> "Por cable USB · la conexión más estable"
        f.transport == Transport.ETHERNET -> "Por cable de red · ${f.ip}"
        else -> "Por Wi-Fi · ${f.ip}"
    }
}
