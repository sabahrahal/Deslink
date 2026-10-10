package com.desklink

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.provider.Settings

/** Qué falta en la tablet para que el cable USB funcione solo al enchufarla. */
enum class UsbDebugHint {
    /** Nada: la depuración USB está activa o la tablet no está enchufada a un PC. */
    NONE,

    /** Hay que activar antes las Opciones de desarrollador (tocar 7 veces «Número de compilación»). */
    ENABLE_DEVELOPER,

    /** Las Opciones de desarrollador están; falta activar «Depuración USB». */
    ENABLE_DEBUGGING,
}

/**
 * El cable de DeskLink va por adb, que funciona con cualquier modo USB de Android (también «Solo cargar») siempre
 * que la depuración USB esté activa. Sin ella la única salida es elegir «Anclaje USB» en cada enchufe; esto lo
 * detecta para decir cómo dejarlo resuelto una vez.
 */
object UsbDebug {
    fun hint(adbEnabled: Boolean, developerEnabled: Boolean, usbConnected: Boolean): UsbDebugHint = when {
        adbEnabled || !usbConnected -> UsbDebugHint.NONE
        developerEnabled -> UsbDebugHint.ENABLE_DEBUGGING
        else -> UsbDebugHint.ENABLE_DEVELOPER
    }

    fun read(context: Context): UsbDebugHint = read(context, usbConnected(context))

    /** ¿Enchufada a un PC con la depuración USB activa? Entonces el cable va a aparecer en cuanto el PC lo prepare. */
    fun cableExpected(context: Context): Boolean {
        val adb = Settings.Global.getInt(context.contentResolver, Settings.Global.ADB_ENABLED, 0) == 1
        return adb && usbConnected(context)
    }

    /** Lo que falta aunque no esté enchufada (para la guía de inicio). */
    fun readUnplugged(context: Context): UsbDebugHint = read(context, true)

    private fun read(context: Context, usb: Boolean): UsbDebugHint {
        val resolver = context.contentResolver
        val adb = Settings.Global.getInt(resolver, Settings.Global.ADB_ENABLED, 0) == 1
        val dev = Settings.Global.getInt(resolver, Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) == 1
        return hint(adb, dev, usb)
    }

    /** Enchufada a un PC: el estado USB de Android o, si el fabricante no lo da, la carga por USB. */
    private fun usbConnected(context: Context): Boolean = try {
        val state = context.registerReceiver(null, IntentFilter("android.hardware.usb.action.USB_STATE"))
        if (state != null && state.hasExtra("connected")) {
            state.getBooleanExtra("connected", false)
        } else {
            val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) == BatteryManager.BATTERY_PLUGGED_USB
        }
    } catch (_: Exception) {
        false
    }

    /** La pantalla de Ajustes donde se hace el paso que falta. */
    fun settingsIntent(hint: UsbDebugHint): Intent = Intent(
        if (hint == UsbDebugHint.ENABLE_DEVELOPER) Settings.ACTION_DEVICE_INFO_SETTINGS
        else Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS,
    ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
}
