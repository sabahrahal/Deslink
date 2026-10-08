package com.desklink

import org.junit.Assert.assertEquals
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
    fun helloConfigFromSettings() {
        val s = AppSettings(resolution = ResolutionScale.BALANCED, quality = Quality.BALANCED, fps = 60, codec = CodecChoice.AUTO)
        val c = s.toHelloConfig(tablet, hevcSupported = true)
        assertEquals(Dlp.HelloConfig(2208, 1380, 60, 30, 0, 320, 90), c)
    }

    @Test
    fun withoutHevcTheTabletAsksForH264() {
        assertEquals(Dlp.CODEC_H264, AppSettings().toHelloConfig(tablet, hevcSupported = false).codec)
        assertEquals(Dlp.CODEC_H264, AppSettings(codec = CodecChoice.HEVC).toHelloConfig(tablet, false).codec)
        assertEquals(0, AppSettings().toHelloConfig(tablet, hevcSupported = true).codec)
    }

    @Test
    fun fpsOptionsFollowTheScreen() {
        assertEquals(listOf(0, 30, 60, 90), AppSettings.fpsOptions(90))
        assertEquals(listOf(0, 30, 60), AppSettings.fpsOptions(60))
        assertEquals(listOf(0, 30, 60, 90, 120, 144), AppSettings.fpsOptions(144))
    }
}
