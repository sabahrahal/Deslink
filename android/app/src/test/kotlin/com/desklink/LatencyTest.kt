package com.desklink

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LatencyTest {
    @Test fun clockUsesTheFastestPong() {
        val c = ClockSync()
        assertNull(c.toLocal(1_000))
        // El reloj del PC va 1.000.000 µs por delante. Primero un PONG lento (cola de WiFi a la vuelta): se equivoca.
        c.onPong(sentUs = 0, hostUs = 1_002_000, receivedUs = 40_000)
        assertEquals(982_000L, c.offsetUs)
        // Luego uno rápido y simétrico: manda ese.
        c.onPong(sentUs = 100_000, hostUs = 1_101_000, receivedUs = 102_000)
        assertEquals(1_000_000L, c.offsetUs)
        assertEquals(1_000L, c.errorUs)
        // Otro lento después no lo empeora.
        c.onPong(sentUs = 200_000, hostUs = 1_230_000, receivedUs = 260_000)
        assertEquals(1_000_000L, c.offsetUs)
        assertEquals(500_000L, c.toLocal(1_500_000))
    }

    @Test fun clockForgetsOldSamples() {
        val c = ClockSync()
        c.onPong(0, 1_001_000, 2_000)
        repeat(ClockSync.WINDOW) { i ->
            val t = 1_000_000L * (i + 1)
            c.onPong(t, t + 2_000_000 + 5_000, t + 10_000)
        }
        assertEquals(2_000_000L, c.offsetUs)
        c.reset()
        assertNull(c.offsetUs)
    }

    @Test fun ignoresBadPongs() {
        val c = ClockSync()
        c.onPong(sentUs = 10, hostUs = 5_000, receivedUs = 5) // respuesta de antes de mandar
        c.onPong(sentUs = 10, hostUs = 0, receivedUs = 20)
        assertNull(c.offsetUs)
    }

    @Test fun stagesAddUpToTotal() {
        val c = ClockSync()
        c.onPong(sentUs = 0, hostUs = 500_000, receivedUs = 0) // PC = tablet + 500.000
        val s = LatencyStats(c)
        // Entra al codificador en 500.000 (PC) = 0 (tablet); entero a 12 ms, al decodificador a 15, sale a 20, se ve a 28.
        s.record(hostUs = 500_000, completeUs = 12_000, queuedUs = 15_000, decodedUs = 20_000, shownUs = 28_000)
        s.record(hostUs = 516_000, completeUs = 26_000, queuedUs = 26_000, decodedUs = 30_000, shownUs = 36_000)
        val snap = s.take()
        assertEquals(2, snap.frames)
        assertEquals(11_000L, snap.arriveUs)
        assertEquals(1_500L, snap.holdUs)
        assertEquals(4_500L, snap.decodeUs)
        assertEquals(7_000L, snap.screenUs)
        assertEquals(24_000L, snap.totalUs)
        assertEquals(28_000L, snap.maxTotalUs)
        assertTrue(snap.text().startsWith("retardo 24.0 ms (peor 28.0)") || snap.text().startsWith("retardo 24,0 ms (peor 28,0)"))
        // Empieza de cero.
        assertEquals(0, s.take().frames)
    }

    @Test fun withoutHostClockOnlyLocalStages() {
        val s = LatencyStats(ClockSync())
        s.record(hostUs = 7, completeUs = 1_000, queuedUs = 1_000, decodedUs = 6_000, shownUs = null)
        val snap = s.take()
        assertEquals(1, snap.frames)
        assertFalse(snap.hasTotal)
        assertFalse(snap.hasScreen)
        assertEquals(-1L, snap.arriveUs)
        assertEquals(5_000L, snap.decodeUs)
    }

    @Test fun inputWaitIsNotDecoding() {
        val s = LatencyStats(ClockSync())
        // Se le pasa a 1 ms, no hay buffer de entrada libre hasta los 9 y sale a 12: 8 de espera y 3 de decodificar.
        s.record(hostUs = 7, completeUs = 1_000, queuedUs = 1_000, decodedUs = 12_000, shownUs = null, inputUs = 9_000)
        val snap = s.take()
        assertEquals(8_000L, snap.inputWaitUs)
        assertEquals(3_000L, snap.decodeUs)
        assertTrue(snap.text().contains("esperar entrada"))
    }

    @Test fun rateReportCarriesLatency() {
        val c = ClockSync()
        c.onPong(0, 500_000, 0)
        val s = LatencyStats(c)
        s.record(500_000, 12_000, 15_000, 20_000, 28_000)
        val p = Dlp.rateReport(60, 0, 60, 50, 1L, s.take())
        val h = Dlp.parseHeader(p, p.size)!!
        assertEquals(Dlp.RATE_REPORT_LATENCY_SIZE, h.payloadSize)
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(50, b.getShort(Dlp.HEADER_SIZE + 6).toInt())
        assertEquals(280, b.getShort(Dlp.HEADER_SIZE + 8).toInt()) // total 28,0 ms
        assertEquals(120, b.getShort(Dlp.HEADER_SIZE + 10).toInt()) // llegar 12,0 ms
        assertEquals(30, b.getShort(Dlp.HEADER_SIZE + 12).toInt()) // reenvíos 3,0 ms
        assertEquals(80, b.getShort(Dlp.HEADER_SIZE + 14).toInt()) // pantalla 8,0 ms
        assertEquals(280, b.getShort(Dlp.HEADER_SIZE + 16).toInt()) // peor
    }

    @Test fun unknownStagesGoAsZero() {
        val s = LatencyStats(ClockSync())
        s.record(7, 1_000, 1_000, 6_000, null)
        val p = Dlp.rateReport(60, 0, 60, 50, 1L, s.take())
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0, b.getShort(Dlp.HEADER_SIZE + 8).toInt())
        assertEquals(0, b.getShort(Dlp.HEADER_SIZE + 10).toInt())
        assertEquals(0, b.getShort(Dlp.HEADER_SIZE + 14).toInt())
        assertEquals(0, b.getShort(Dlp.HEADER_SIZE + 16).toInt())
    }

    @Test fun pongWithHostTime() {
        val payload = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(123_456_789L).array()
        val p = Dlp.controlPacket(Dlp.TYPE_PONG, 42L, payload)
        assertEquals(123_456_789L, Dlp.parsePongHostUs(p, Dlp.parseHeader(p, p.size)!!))
        val old = Dlp.controlPacket(Dlp.TYPE_PONG, 42L)
        assertNull(Dlp.parsePongHostUs(old, Dlp.parseHeader(old, old.size)!!))
    }
}
