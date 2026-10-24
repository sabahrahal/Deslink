package com.desklink

import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI

class StylusTest {
    private fun tilt(t: Double, o: Double): Pair<Int, Int> {
        val p = Stylus.tilt(t.toFloat(), o.toFloat())
        return Stylus.tiltX(p) to Stylus.tiltY(p)
    }

    @Test fun uprightPenHasNoTilt() {
        assertEquals(0 to 0, tilt(0.0, 1.0))
    }

    @Test fun tiltFollowsWherePenPoints() {
        assertEquals(45 to 0, tilt(PI / 4, PI / 2))   // hacia la derecha
        assertEquals(-45 to 0, tilt(PI / 4, -PI / 2)) // hacia la izquierda
        assertEquals(0 to -45, tilt(PI / 4, 0.0))     // hacia arriba (lejos del usuario)
        assertEquals(0 to 45, tilt(PI / 4, PI))       // hacia el usuario
    }

    @Test fun nearlyFlatPenStaysInRange() {
        val (x, y) = tilt(PI / 2, PI / 2)
        assertEquals(true, x in 80..90)
        assertEquals(0, y)
    }

    @Test fun penPacketLayout() {
        val p = Dlp.pen(Dlp.PEN_IN_RANGE or Dlp.PEN_CONTACT, 1f, 0.5f, 0.5f, -30, 120, 7L)
        assertEquals(Dlp.HEADER_SIZE + Dlp.PEN_SIZE, p.size)
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(Dlp.TYPE_PEN, b.get(3).toInt())
        assertEquals(Dlp.PEN_SIZE, b.getShort(22).toInt())
        assertEquals(3, b.get(Dlp.HEADER_SIZE).toInt())
        assertEquals(65535, b.getShort(Dlp.HEADER_SIZE + 1).toInt() and 0xFFFF)
        assertEquals(32767, b.getShort(Dlp.HEADER_SIZE + 3).toInt() and 0xFFFF)
        assertEquals(512, b.getShort(Dlp.HEADER_SIZE + 5).toInt())
        assertEquals(-30, b.get(Dlp.HEADER_SIZE + 7).toInt())
        assertEquals(90, b.get(Dlp.HEADER_SIZE + 8).toInt())
    }

    @Test fun hostFlagSaysIfPenIsAccepted() {
        assertEquals(true, Dlp.StreamInfo(1, 1, 60, 1, hostFlags = 3).acceptsPen)
        assertEquals(false, Dlp.StreamInfo(1, 1, 60, 1, hostFlags = 1).acceptsPen)
    }
}
