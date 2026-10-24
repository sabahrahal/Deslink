package app.extendesk

import android.view.MotionEvent
import kotlin.math.atan
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.tan

/** Lápiz de la tablet: de los ejes de Android a lo que espera un lápiz de Windows (ver [Dlp.pen]). */
object Stylus {
    fun isPen(toolType: Int) = toolType == MotionEvent.TOOL_TYPE_STYLUS || toolType == MotionEvent.TOOL_TYPE_ERASER

    /**
     * Android da la inclinación respecto a la vertical ([tilt]) y hacia dónde apunta el lápiz ([orientation]: 0 hacia
     * arriba, +π/2 hacia la derecha), en radianes. Windows la quiere por ejes, en grados: x positiva con el lápiz
     * inclinado hacia la derecha, y positiva hacia el usuario (abajo en la pantalla). Devuelve x en los 16 bits altos
     * e y en los bajos, ver [tiltX] y [tiltY] (sin crear objetos en cada muestra).
     */
    fun tilt(tilt: Float, orientation: Float): Int {
        val t = tan(tilt.coerceIn(0f, 1.5f).toDouble())
        val x = Math.toDegrees(atan(t * sin(orientation.toDouble()))).roundToInt().coerceIn(-90, 90)
        val y = Math.toDegrees(atan(-t * cos(orientation.toDouble()))).roundToInt().coerceIn(-90, 90)
        return (x shl 16) or (y and 0xFFFF)
    }

    fun tiltX(packed: Int) = packed shr 16
    fun tiltY(packed: Int) = packed.toShort().toInt()
}
