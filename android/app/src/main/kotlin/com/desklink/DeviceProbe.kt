package com.desklink

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
 * Lee el estado de batería, red y temperatura. El consumo neto se calcula con el contador de carga
 * (CHARGE_COUNTER), cuyo signo es fiable, porque CURRENT_NOW cambia de signo según el fabricante.
 */
class DeviceProbe(private val context: Context) {
    private val battery = context.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
    private var windowStartUah = -1L
    private var windowStartMs = 0L

    fun sampleBattery(): BatteryInfo {
        val intent: Intent? = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val plugged = (intent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        val tempC = (intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0) / 10.0
        val voltageMv = intent?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) ?: 0
        val percent = if (level >= 0 && scale > 0) level * 100 / scale else -1

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
                netMw = deltaUah / hours * (voltageMv / 1000.0) / 1000.0
                // Ventana deslizante para que la cifra siga al uso actual.
                if (now - windowStartMs >= 60_000) {
                    windowStartUah = counter
                    windowStartMs = now
                }
            }
        }
        return BatteryInfo(percent, plugged, netMw, tempC)
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
}
