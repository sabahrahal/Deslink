package com.desklink

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SettingsTest {
    private val tablet = DisplaySpec.landscape(1840, 2944, densityDpi = 320, refreshHz = 90)

    @Test
    fun landscapeUsesLongSideAsWidth() {
        assertEquals(2944, tablet.width)
        assertEquals(1840, tablet.height)
    }

    @Test
    fun scaledSizesAreEvenAndProportional() {
        assertEquals(2944 to 1840, tablet.scaled(100))
        assertEquals(2208 to 1380, tablet.scaled(75))
        assertEquals(1472 to 920, tablet.scaled(50))
        val odd = DisplaySpec(2001, 1201)
        val (w, h) = odd.scaled(75)
        assertEquals(0, w % 2)
        assertEquals(0, h % 2)
    }

    @Test
    fun smallScreensStayAboveTheHostMinimum() {
        val phone = DisplaySpec.landscape(1080, 2400, 420, 120)
        val (w, h) = phone.scaled(50)
        assertTrue(h >= DisplaySpec.MIN_H && w >= DisplaySpec.MIN_W)
        // Sin deformar: la proporción se mantiene (±1 px por redondeo a par).
        assertEquals(2400.0 / 1080, w.toDouble() / h, 0.01)
    }

    @Test
    fun hugeScreensStayBelowTheHostMaximum() {
        val (w, h) = DisplaySpec(10000, 5000).scaled(100)
        assertTrue(w <= DisplaySpec.MAX_W && h <= DisplaySpec.MAX_H)
    }

    @Test
    fun helloSaysWhatTheTabletCanDoAndLeavesTheRestToThePc() {
        // Pantalla completa, su frecuencia y si decodifica HEVC; fps y tasa a 0: los decide el PC.
        assertEquals(Dlp.HelloConfig(2944, 1840, 0, 0, Dlp.CAN_DECODE_HEVC, 320, 90), AppSettings.hello(tablet, hevcSupported = true))
        assertEquals(Dlp.CAN_DECODE_H264_ONLY, AppSettings.hello(tablet, hevcSupported = false).codec)
        assertEquals(Dlp.CAN_DECODE_HEVC_PREFERS_H264, AppSettings.hello(tablet, hevcSupported = true, prefersH264 = true).codec)
        assertEquals(Dlp.CAN_DECODE_H264_ONLY, AppSettings.hello(tablet, hevcSupported = false, prefersH264 = true).codec)
        assertTrue(Device.isMediaTek("c2.mtk.hevc.decoder"))
        assertFalse(Device.isMediaTek("c2.qti.hevc.decoder"))
    }

    @Test
    fun batteryRequestCarriesTheProfileCaps() {
        val p = Dlp.qualityRequest(PowerProfile.AHORRO, 1)
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(30, b.getShort(Dlp.HEADER_SIZE).toInt())
        assertEquals(120, b.getShort(Dlp.HEADER_SIZE + 2).toInt())
        assertEquals(PowerProfile.AHORRO.wire, p[Dlp.HEADER_SIZE + 5].toInt())
    }
}
