package app.extendesk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class PowerProfileTest {
    @Test
    fun unknownOrInternalIdFallsBackToRecommended() {
        assertEquals(PowerProfile.AHORRO, PowerProfile.fromId("ahorro"))
        assertEquals(PowerProfile.MAXIMA, PowerProfile.fromId("maxima"))
        assertEquals(PowerProfile.EQUILIBRADO, PowerProfile.fromId(null))
        assertEquals(PowerProfile.EQUILIBRADO, PowerProfile.fromId("minimo"))
        assertEquals(PowerProfile.EQUILIBRADO, PowerProfile.DEFAULT)
    }

    @Test
    fun ladderOrder() {
        assertEquals(PowerProfile.EQUILIBRADO, PowerProfile.MAXIMA.lower())
        assertEquals(PowerProfile.MINIMO, PowerProfile.MINIMO.lower())
        assertEquals(PowerProfile.MAXIMA, PowerProfile.MAXIMA.higher())
    }

    @Test
    fun refreshRatePrefersExactMultiple() {
        val rates = listOf(144f, 120f, 90f, 60f)
        assertEquals(60f, PowerProfile.pickRefreshRate(rates, 30)!!, 0.01f)
        assertEquals(60f, PowerProfile.pickRefreshRate(rates, 60)!!, 0.01f)
        assertEquals(120f, PowerProfile.pickRefreshRate(rates, 24)!!, 0.01f)
        assertEquals(144f, PowerProfile.pickRefreshRate(rates, 144)!!, 0.01f)
        assertNull(PowerProfile.pickRefreshRate(listOf(60f), 120))
    }

    @Test
    fun fastRefreshRateAvoidsJudder() {
        val rates = listOf(144f, 120f, 90f, 60f)
        assertEquals(120f, PowerProfile.pickFastRefreshRate(rates, 60)!!, 0.01f)
        assertEquals(120f, PowerProfile.pickFastRefreshRate(rates, 30)!!, 0.01f)
        assertEquals(90f, PowerProfile.pickFastRefreshRate(rates, 90)!!, 0.01f)
        assertEquals(144f, PowerProfile.pickFastRefreshRate(listOf(144f, 60f), 50)!!, 0.01f)
        assertNull(PowerProfile.pickFastRefreshRate(rates, 0))
    }

    @Test
    fun qualityRequestMatchesProtocol() {
        val p = Dlp.qualityRequest(PowerProfile.AHORRO, 9)
        val h = Dlp.parseHeader(p, p.size)!!
        assertEquals(Dlp.TYPE_QUALITY_REQUEST, h.type)
        assertEquals(6, h.payloadSize)
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(30, b.getShort(Dlp.HEADER_SIZE).toInt())
        assertEquals(120, b.getShort(Dlp.HEADER_SIZE + 2).toInt()) // 12 Mbps en unidades de 100 kbps
        assertEquals(75, p[Dlp.HEADER_SIZE + 4].toInt())
        assertEquals(2, p[Dlp.HEADER_SIZE + 5].toInt())
    }
}
