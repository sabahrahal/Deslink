package app.extendesk

import kotlin.math.max

/** Resultado de la prueba de conexión: latencia de ida y vuelta (ms) y pérdida de paquetes. */
data class LinkResult(val sent: Int, val received: Int, val rttP50Ms: Double, val rttP95Ms: Double, val jitterMs: Double, val route: Route? = null) {
    val lossPercent: Double get() = if (sent == 0) 0.0 else (sent - received) * 100.0 / sent
}

/** Resultado de una ráfaga de prueba (como un frame clave). [paceMbps] 0 = de golpe. */
data class BurstResult(val sent: Int, val received: Int, val mbps: Double, val paceMbps: Int) {
    val lossPercent: Double get() = if (sent == 0) 0.0 else (sent - received) * 100.0 / sent
}

/** Estado de la red de la tablet. */
data class NetworkInfo(val isWifi: Boolean, val wifiLinkMbps: Int, val wifiRssiDbm: Int, val wifiFrequencyMhz: Int, val isEthernetLike: Boolean)

/** De dónde sale la cifra de potencia (ver [DeviceProbe]): medida o estimada. */
enum class PowerSource(val label: String) {
    CURRENT_LEARNED("medido: corriente de la batería"),
    COUNTER("medido: contador de carga"),
    CURRENT_GUESS("estimado: signo según el estado de carga"),
}

/** Estado de la batería. `netMilliwatts`: positivo = se carga, negativo = se descarga; null si aún no se sabe. */
data class BatteryInfo(
    val percent: Int,
    val pluggedUsbOrAc: Boolean,
    val netMilliwatts: Double?,
    val temperatureC: Double,
    val source: PowerSource? = null,
    /** «USB», «cargador», «inalámbrica» o null sin cable. */
    val plugLabel: String? = null,
    val chargingStatus: Boolean = false,
)

enum class Severity { OK, INFO, WARNING }

data class Finding(val severity: Severity, val title: String, val detail: String)

/**
 * Convierte medidas en recomendaciones en lenguaje sencillo. Lógica pura (sin Android) para poder probarla.
 * Los umbrales son conservadores y se ajustarán con las mediciones reales (ver docs/diagnostics-panel.md).
 */
object Advisor {
    fun advise(
        link: LinkResult?,
        net: NetworkInfo?,
        battery: BatteryInfo?,
        thermalStatus: Int?,
        raw: BurstResult? = null,
        paced: BurstResult? = null,
    ): List<Finding> {
        val out = ArrayList<Finding>()
        if (link != null) out += adviseLink(link, net)
        if (raw != null) out += adviseBurst(raw, paced)
        val wifiRoute = link?.route?.transport?.let { it == Transport.WIFI } ?: (net?.isWifi == true)
        if (net != null && wifiRoute) out += adviseWifi(net)
        if (battery != null) out += adviseBattery(battery)
        if (thermalStatus != null && thermalStatus >= THERMAL_MODERATE) {
            out += Finding(
                Severity.WARNING, "El dispositivo se está calentando",
                "El sistema puede bajar el rendimiento para enfriarse y eso causa tirones. Quita la funda, evita el sol " +
                    "o activa el modo ahorro (60 Hz y menos calidad).",
            )
        }
        if (out.none { it.severity == Severity.WARNING }) {
            out += Finding(Severity.OK, "Todo en orden", "No hemos encontrado nada que limite la experiencia ahora mismo.")
        }
        return out
    }

    private fun adviseLink(link: LinkResult, net: NetworkInfo?): List<Finding> {
        if (link.received == 0) {
            return listOf(
                Finding(
                    Severity.WARNING, "No hay respuesta del PC",
                    "Comprueba que extendesk-host está abierto, que el cortafuegos permite UDP 47800 y que ambos " +
                        "equipos están en la misma red.",
                ),
            )
        }
        val out = ArrayList<Finding>()
        val transport = link.route?.transport
        val isWifi = transport?.let { it == Transport.WIFI } ?: (net?.isWifi == true)
        val kind = transport?.label ?: if (isWifi) "WiFi" else "cable/red"
        when {
            link.lossPercent >= 2.0 -> out += Finding(
                Severity.WARNING, "Se pierden paquetes ($kind)",
                "Se perdió el ${"%.1f".format(link.lossPercent)} % de las pruebas; verás tirones o imagen rota. " +
                    if (isWifi) "Acércate al router o usa la banda de 5 GHz/6 GHz; si puedes, el cable USB va más estable."
                    else "Prueba con otro cable o puerto.",
            )
            link.lossPercent > 0.0 -> out += Finding(Severity.INFO, "Pérdida baja", "Se perdió el ${"%.1f".format(link.lossPercent)} %. Es normal y no se nota.")
        }
        val tail = link.rttP95Ms
        when {
            tail > 25 || link.jitterMs > 8 -> out += Finding(
                Severity.WARNING, "La conexión es irregular",
                "La latencia típica es ${"%.1f".format(link.rttP50Ms)} ms pero a veces sube a ${"%.1f".format(tail)} ms " +
                    "(variación ${"%.1f".format(link.jitterMs)} ms). Eso se nota como saltos al mover el ratón.",
            )
            link.rttP50Ms <= 4 && tail <= 12 -> out += Finding(
                Severity.OK, "Conexión excelente",
                "Latencia de ${"%.1f".format(link.rttP50Ms)} ms (máx. habitual ${"%.1f".format(tail)} ms).",
            )
            else -> out += Finding(
                Severity.INFO, "Conexión correcta",
                "Latencia de ${"%.1f".format(link.rttP50Ms)} ms (máx. habitual ${"%.1f".format(tail)} ms).",
            )
        }
        return out
    }

    /** Qué hacer según cómo aguanta el enlace una ráfaga del tamaño de un frame clave. */
    fun adviseBurst(raw: BurstResult, paced: BurstResult?): List<Finding> {
        val rawLoss = raw.lossPercent
        val pacedLoss = paced?.lossPercent
        return when {
            raw.received == 0 -> emptyList()
            rawLoss < 3.0 -> listOf(
                Finding(Severity.OK, "Aguanta bien las ráfagas", "Un frame grande llega completo (${"%.0f".format(raw.mbps)} Mbps de pico)."),
            )
            paced != null && pacedLoss != null && pacedLoss < 3.0 -> listOf(
                Finding(
                    Severity.INFO, "El enlace pierde datos en ráfagas",
                    "Enviado de golpe se pierde el ${"%.0f".format(rawLoss)} % de un frame grande, y repartido a ${paced.paceMbps} Mbps " +
                        "llega completo. El programa del PC ya reparte el envío a ${paced.paceMbps} Mbps de serie; si usas una " +
                        "versión antigua, actualízala o arráncala con `extendesk-host.exe --pace ${paced.paceMbps}`.",
                ),
            )
            else -> {
                val base = paced?.takeIf { it.received > 0 }?.mbps ?: raw.mbps
                val suggested = max(5, (base * 0.4).toInt())
                listOf(
                    Finding(
                        Severity.WARNING, "El enlace no da para tanta calidad",
                        "Se pierde el ${"%.0f".format(rawLoss)} % de un frame grande aunque se reparta. Baja la calidad: " +
                            "`extendesk-host.exe --bitrate $suggested --pace ${max(10, (base * 0.8).toInt())}`. " +
                            "Si es USB, prueba otro cable o puerto, o el WiFi.",
                    ),
                )
            }
        }
    }

    private fun adviseWifi(net: NetworkInfo): List<Finding> {
        val out = ArrayList<Finding>()
        if (net.wifiFrequencyMhz in 2400..2500) {
            out += Finding(
                Severity.WARNING, "Estás en WiFi de 2,4 GHz",
                "Esa banda va saturada y tiene poca velocidad. Conecta el dispositivo y el PC a la red de 5 GHz (o 6 GHz).",
            )
        }
        if (net.wifiRssiDbm != 0 && net.wifiRssiDbm < -70) {
            out += Finding(Severity.WARNING, "Señal WiFi débil", "Señal de ${net.wifiRssiDbm} dBm. Acércate al router.")
        }
        if (net.wifiLinkMbps in 1..299) {
            out += Finding(Severity.INFO, "Velocidad de enlace baja", "El WiFi negocia ${net.wifiLinkMbps} Mbps; el vídeo a 50 Mbps puede quedarse justo.")
        }
        return out
    }

    private fun adviseBattery(b: BatteryInfo): List<Finding> {
        val out = ArrayList<Finding>()
        val net = b.netMilliwatts
        if (b.pluggedUsbOrAc && net != null && net < -500) {
            out += Finding(
                Severity.WARNING, "Está conectado pero se descarga",
                "El cable da menos energía de la que gasta la pantalla (${"%.1f".format(-net / 1000)} W de saldo negativo). " +
                    "Los puertos USB del PC suelen dar 2,5–4,5 W. Prueba con un puerto USB trasero o un cargador de pared, " +
                    "o activa el modo ahorro.",
            )
        } else if (!b.pluggedUsbOrAc && net != null && net < -1000) {
            val hours = if (b.percent > 0) estimateHours(b.percent, -net) else null
            out += Finding(
                Severity.INFO, "Funcionando con batería",
                "Consumo medio ${"%.1f".format(-net / 1000)} W" + if (hours != null) "; te quedan unas ${"%.1f".format(hours)} h." else ".",
            )
        }
        if (b.temperatureC >= 42) {
            out += Finding(Severity.WARNING, "Batería caliente", "${"%.0f".format(b.temperatureC)} °C. Cargar y transmitir a la vez la calienta; el sistema limitará la carga.")
        }
        return out
    }

    /** Horas restantes suponiendo una batería de ~38 Wh (Idea Tab Pro: 10200 mAh); es una aproximación. */
    private fun estimateHours(percent: Int, drainMilliwatts: Double): Double =
        max(0.0, percent / 100.0 * ASSUMED_BATTERY_WH * 1000 / drainMilliwatts)

    const val THERMAL_MODERATE = 2
    const val ASSUMED_BATTERY_WH = 38.0

    /** Percentil sobre una lista ya ordenada. */
    fun percentile(sorted: List<Double>, p: Double): Double {
        if (sorted.isEmpty()) return 0.0
        val idx = ((sorted.size - 1) * p).toInt().coerceIn(0, sorted.size - 1)
        return sorted[idx]
    }

    fun summarize(sent: Int, rttsMs: List<Double>): LinkResult {
        val s = rttsMs.sorted()
        val jitter = if (rttsMs.size < 2) 0.0 else rttsMs.zipWithNext { a, b -> kotlin.math.abs(b - a) }.average()
        return LinkResult(sent, rttsMs.size, percentile(s, 0.5), percentile(s, 0.95), jitter)
    }
}
