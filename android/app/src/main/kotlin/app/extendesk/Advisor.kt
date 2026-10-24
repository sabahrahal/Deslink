package app.extendesk

import androidx.annotation.StringRes
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
enum class PowerSource(@StringRes private val labelRes: Int) {
    CURRENT_LEARNED(R.string.power_source_current),
    COUNTER(R.string.power_source_counter),
    CURRENT_GUESS(R.string.power_source_guess),
    ;

    val label: String get() = Strings.get(labelRes)
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
                Severity.WARNING, Strings.get(R.string.adv_hot_title),
                Strings.get(R.string.adv_hot_detail),
            )
        }
        if (out.none { it.severity == Severity.WARNING }) {
            out += Finding(Severity.OK, Strings.get(R.string.adv_ok_title), Strings.get(R.string.adv_ok_detail))
        }
        return out
    }

    private fun adviseLink(link: LinkResult, net: NetworkInfo?): List<Finding> {
        if (link.received == 0) {
            return listOf(
                Finding(
                    Severity.WARNING, Strings.get(R.string.adv_no_answer_title),
                    Strings.get(R.string.adv_no_answer_detail),
                ),
            )
        }
        val out = ArrayList<Finding>()
        val transport = link.route?.transport
        val isWifi = transport?.let { it == Transport.WIFI } ?: (net?.isWifi == true)
        val kind = transport?.label ?: Strings.get(if (isWifi) R.string.adv_kind_wifi else R.string.adv_kind_cable)
        when {
            link.lossPercent >= 2.0 -> out += Finding(
                Severity.WARNING, Strings.get(R.string.adv_loss_title, kind),
                Strings.get(
                    R.string.adv_loss_detail,
                    "%.1f".format(link.lossPercent),
                    Strings.get(if (isWifi) R.string.adv_loss_wifi_tip else R.string.adv_loss_cable_tip),
                ),
            )
            link.lossPercent > 0.0 -> out += Finding(
                Severity.INFO, Strings.get(R.string.adv_low_loss_title),
                Strings.get(R.string.adv_low_loss_detail, "%.1f".format(link.lossPercent)),
            )
        }
        val tail = link.rttP95Ms
        when {
            tail > 25 || link.jitterMs > 8 -> out += Finding(
                Severity.WARNING, Strings.get(R.string.adv_jitter_title),
                Strings.get(R.string.adv_jitter_detail, "%.1f".format(link.rttP50Ms), "%.1f".format(tail), "%.1f".format(link.jitterMs)),
            )
            link.rttP50Ms <= 4 && tail <= 12 -> out += Finding(
                Severity.OK, Strings.get(R.string.adv_excellent_title),
                Strings.get(R.string.adv_latency_detail, "%.1f".format(link.rttP50Ms), "%.1f".format(tail)),
            )
            else -> out += Finding(
                Severity.INFO, Strings.get(R.string.adv_correct_title),
                Strings.get(R.string.adv_latency_detail, "%.1f".format(link.rttP50Ms), "%.1f".format(tail)),
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
                Finding(Severity.OK, Strings.get(R.string.adv_burst_ok_title), Strings.get(R.string.adv_burst_ok_detail, "%.0f".format(raw.mbps))),
            )
            paced != null && pacedLoss != null && pacedLoss < 3.0 -> listOf(
                Finding(
                    Severity.INFO, Strings.get(R.string.adv_burst_paced_title),
                    Strings.get(R.string.adv_burst_paced_detail, "%.0f".format(rawLoss), paced.paceMbps),
                ),
            )
            else -> {
                val base = paced?.takeIf { it.received > 0 }?.mbps ?: raw.mbps
                val suggested = max(5, (base * 0.4).toInt())
                listOf(
                    Finding(
                        Severity.WARNING, Strings.get(R.string.adv_burst_bad_title),
                        Strings.get(R.string.adv_burst_bad_detail, "%.0f".format(rawLoss), suggested, max(10, (base * 0.8).toInt())),
                    ),
                )
            }
        }
    }

    private fun adviseWifi(net: NetworkInfo): List<Finding> {
        val out = ArrayList<Finding>()
        if (net.wifiFrequencyMhz in 2400..2500) {
            out += Finding(
                Severity.WARNING, Strings.get(R.string.adv_24ghz_title),
                Strings.get(R.string.adv_24ghz_detail),
            )
        }
        if (net.wifiRssiDbm != 0 && net.wifiRssiDbm < -70) {
            out += Finding(Severity.WARNING, Strings.get(R.string.adv_weak_title), Strings.get(R.string.adv_weak_detail, net.wifiRssiDbm))
        }
        if (net.wifiLinkMbps in 1..299) {
            out += Finding(Severity.INFO, Strings.get(R.string.adv_link_speed_title), Strings.get(R.string.adv_link_speed_detail, net.wifiLinkMbps))
        }
        return out
    }

    private fun adviseBattery(b: BatteryInfo): List<Finding> {
        val out = ArrayList<Finding>()
        val net = b.netMilliwatts
        if (b.pluggedUsbOrAc && net != null && net < -500) {
            out += Finding(
                Severity.WARNING, Strings.get(R.string.adv_drain_title),
                Strings.get(R.string.adv_drain_detail, "%.1f".format(-net / 1000)),
            )
        } else if (!b.pluggedUsbOrAc && net != null && net < -1000) {
            val hours = if (b.percent > 0) estimateHours(b.percent, -net) else null
            out += Finding(
                Severity.INFO, Strings.get(R.string.adv_battery_title),
                if (hours != null) {
                    Strings.get(R.string.adv_battery_detail_hours, "%.1f".format(-net / 1000), "%.1f".format(hours))
                } else {
                    Strings.get(R.string.adv_battery_detail, "%.1f".format(-net / 1000))
                },
            )
        }
        if (b.temperatureC >= 42) {
            out += Finding(Severity.WARNING, Strings.get(R.string.adv_hot_battery_title), Strings.get(R.string.adv_hot_battery_detail, "%.0f".format(b.temperatureC)))
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
