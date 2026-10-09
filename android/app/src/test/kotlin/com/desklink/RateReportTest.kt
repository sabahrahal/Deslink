package com.desklink

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Test

class RateReportTest {
    @Test fun carriesReceivedLostAndDecoded() {
        val p = Dlp.rateReport(58, 2, 41, 7L)
        val h = Dlp.parseHeader(p, p.size)!!
        assertEquals(Dlp.TYPE_RATE_REPORT, h.type)
        // Tres u16: un host antiguo lee solo los dos primeros.
        assertEquals(6, h.payloadSize)
        assertEquals(Dlp.HEADER_SIZE + 6, p.size)
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(58, b.getShort(Dlp.HEADER_SIZE).toInt() and 0xFFFF)
        assertEquals(2, b.getShort(Dlp.HEADER_SIZE + 2).toInt() and 0xFFFF)
        assertEquals(41, b.getShort(Dlp.HEADER_SIZE + 4).toInt() and 0xFFFF)
    }

    @Test fun clampsToU16() {
        val p = Dlp.rateReport(70_000, -3, 0x1_0000, 1L)
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0xFFFF, b.getShort(Dlp.HEADER_SIZE).toInt() and 0xFFFF)
        assertEquals(0, b.getShort(Dlp.HEADER_SIZE + 2).toInt() and 0xFFFF)
        assertEquals(0xFFFF, b.getShort(Dlp.HEADER_SIZE + 4).toInt() and 0xFFFF)
    }
}
