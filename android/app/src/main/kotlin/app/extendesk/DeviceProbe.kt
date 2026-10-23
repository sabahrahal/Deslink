package app.extendesk

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager

/**
 * Lee el estado de batería, red y temperatura. La potencia neta de la batería sale, por orden de preferencia, de:
 * 1. CURRENT_NOW con el signo aprendido de este aparato. Cada fabricante usa un signo, así que se aprende cuando
 *    no hay cable (entonces la batería solo puede bajar) o cuando el contador de carga se mueve claramente, y se
 *    guarda. Es la única que responde en segundos y la que distingue «cargando» de «enchufada pero bajando».
 * 2. El contador de carga (CHARGE_COUNTER) en una ventana de 20–60 s, si se mueve (algunas tablets solo lo
 *    actualizan a saltos de 1 %).
 * 3. CURRENT_NOW con el signo del estado de carga: con cable Android suele decir «cargando» aunque la batería
 *    baje, así que esta es solo una estimación.
 */
class DeviceProbe(private val context: Context) {
    private val battery = context.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
    private val prefs = Prefs.open(context)
    private var windowStartUah = -1L
    private var windowStartMs = 0L
    private var currentEmaMw: Double? = null
    private var learnedEmaMw: Double? = null
    private var rawEma: Double? = null  // CURRENT_NOW tal cual (con su signo), para aprender el convenio
    /** Signo que tiene CURRENT_NOW en este aparato cuando la batería baja (+1/−1); 0 = aún no se sabe. */
    private var dischargeSign = prefs.getInt(PREF_DISCHARGE_SIGN, 0)

    /** Olvida la ventana del contador (tras cambiar de perfil, para no mezclar el consumo de antes). */
    fun restartWindow() {
        windowStartUah = -1
    }

    fun sampleBattery(): BatteryInfo {
        val intent: Intent? = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val plugged = (intent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        val tempC = (intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0) / 10.0
        val voltageMv = intent?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) ?: 0
        val pluggedType = intent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        val charging = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) == BatteryManager.BATTERY_STATUS_CHARGING
        val percent = if (level >= 0 && scale > 0) level * 100 / scale else -1

        val raw = battery.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
        val validRaw = raw != 0L && raw != Long.MIN_VALUE
        if (validRaw) rawEma = rawEma?.let { it + 0.2 * (raw - it) } ?: raw.toDouble()
        // Sin cable la batería solo puede bajar: el signo que tenga ahora CURRENT_NOW es el de descarga.
        if (!plugged && validRaw) learnSign(if (raw > 0) 1 else -1)
        val instantMw = currentToMw(raw, voltageMv, charging)
        if (instantMw != null) currentEmaMw = currentEmaMw?.let { it + 0.2 * (instantMw - it) } ?: instantMw
        val learnedMw = if (dischargeSign != 0) currentToMw(raw, voltageMv, charging, dischargeSign) else null
        if (learnedMw != null) learnedEmaMw = learnedEmaMw?.let { it + 0.2 * (learnedMw - it) } ?: learnedMw

        var netMw: Double? = null
        val counter = battery.getLongProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER) // µAh
        val now = System.currentTimeMillis()
        if (counter > 0 && voltageMv > 0) {
            if (windowStartUah < 0) {
                windowStartUah = counter
                windowStartMs = now
            } else if (now - windowStartMs >= 20_000) {
                val deltaUah = counter - windowStartUah
                val hours = (now - windowStartMs) / 3_600_000.0
                // µAh / h = µA; × V = µW; /1000 = mW. Positivo: la batería sube.
                if (deltaUah != 0L) netMw = deltaUah / hours * (voltageMv / 1000.0) / 1000.0
                // Un movimiento claro del contador también enseña el signo de CURRENT_NOW.
                val r = rawEma
                if (kotlin.math.abs(deltaUah) >= 3_000 && r != null && kotlin.math.abs(r) > 50_000) {
                    learnSign(if ((deltaUah < 0) == (r > 0)) 1 else -1)
                }
                // Ventana deslizante para que la cifra siga al uso actual.
                if (now - windowStartMs >= 60_000) {
                    windowStartUah = counter
                    windowStartMs = now
                }
            }
        }
        val source = when {
            learnedEmaMw != null -> PowerSource.CURRENT_LEARNED
            netMw != null -> PowerSource.COUNTER
            currentEmaMw != null -> PowerSource.CURRENT_GUESS
            else -> null
        }
        val plugLabel = when (pluggedType) {
            BatteryManager.BATTERY_PLUGGED_USB -> Strings.get(R.string.plug_usb)
            BatteryManager.BATTERY_PLUGGED_AC -> Strings.get(R.string.plug_charger)
            BatteryManager.BATTERY_PLUGGED_WIRELESS -> Strings.get(R.string.plug_wireless)
            else -> null
        }
        return BatteryInfo(percent, plugged, learnedEmaMw ?: netMw ?: currentEmaMw, tempC, source, plugLabel, charging)
    }

    private fun learnSign(sign: Int) {
        if (sign == dischargeSign) return
        dischargeSign = sign
        learnedEmaMw = null
        prefs.edit().putInt(PREF_DISCHARGE_SIGN, sign).apply()
    }

    fun networkInfo(): NetworkInfo {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val caps = cm.getNetworkCapabilities(cm.activeNetwork)
        val isWifi = caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        val wired = caps?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true
        var link = 0
        var rssi = 0
        var freq = 0
        if (isWifi) {
            @Suppress("DEPRECATION")
            val info = (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager).connectionInfo
            if (info != null) {
                link = info.linkSpeed
                rssi = info.rssi
                freq = info.frequency
            }
        }
        return NetworkInfo(isWifi, link, rssi, freq, wired)
    }

    fun thermalStatus(): Int? {
        if (Build.VERSION.SDK_INT < 29) return null
        return (context.getSystemService(Context.POWER_SERVICE) as PowerManager).currentThermalStatus
    }

    companion object {
        /**
         * CURRENT_NOW → mW netos (positivo = se carga). Se toma el valor absoluto y el signo sale de
         * [charging], porque cada fabricante usa uno. Debería venir en µA, pero algunos dan mA: con la
         * pantalla encendida nunca se gastan menos de 10 mA, así que un valor menor se toma como mA.
         * Con [dischargeSign] (el signo que usa este aparato al descargar, ver [DeviceProbe]) el signo sale del
         * propio valor y [charging] no se usa.
         * Null si la propiedad no existe (0 o Long.MIN_VALUE) o falta el voltaje.
         */
        fun currentToMw(current: Long, voltageMv: Int, charging: Boolean, dischargeSign: Int = 0): Double? {
            if (current == 0L || current == Long.MIN_VALUE || voltageMv <= 0) return null
            var microamps = kotlin.math.abs(current.toDouble())
            if (microamps < 10_000) microamps *= 1000
            val mw = microamps * voltageMv / 1_000_000.0
            val up = if (dischargeSign != 0) (current > 0) != (dischargeSign > 0) else charging
            return if (up) mw else -mw
        }

        const val PREF_DISCHARGE_SIGN = "current_discharge_sign"
    }
}
