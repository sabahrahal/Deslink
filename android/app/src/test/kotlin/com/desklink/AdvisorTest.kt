package com.desklink

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AdvisorTest {
    private val wifi5 = NetworkInfo(true, 866, -50, 5180, false)

    @Test fun summarizeComputesPercentilesAndLoss() {
        val r = Advisor.summarize(10, listOf(2.0, 3.0, 4.0, 5.0, 100.0))
        assertEquals(5, r.received)
        assertEquals(50.0, r.lossPercent, 0.001)
        assertEquals(4.0, r.rttP50Ms, 0.001)
    }

    @Test fun noResponseWarns() {
        val f = Advisor.advise(Advisor.summarize(10, emptyList()), wifi5, null, null)
        assertTrue(f.any { it.severity == Severity.WARNING && it.title.contains("respuesta") })
    }

    @Test fun goodLinkIsOk() {
        val f = Advisor.advise(Advisor.summarize(50, List(50) { 3.0 }), wifi5, null, null)
        assertTrue(f.any { it.title == "Conexión excelente" })
        assertTrue(f.none { it.severity == Severity.WARNING })
    }

    @Test fun twoPointFourGhzWarns() {
        val f = Advisor.advise(null, wifi5.copy(wifiFrequencyMhz = 2437), null, null)
        assertTrue(f.any { it.title.contains("2,4") })
    }

    @Test fun chargingButDrainingWarns() {
        val f = Advisor.advise(null, null, BatteryInfo(60, true, -4000.0, 33.0), null)
        assertTrue(f.any { it.title.contains("se descarga") })
    }

    @Test fun lossyLinkWarns() {
        val f = Advisor.advise(Advisor.summarize(100, List(90) { 4.0 }), wifi5, null, null)
        assertTrue(f.any { it.title.contains("pierden") })
    }

    @Test fun thermalWarns() {
        val f = Advisor.advise(null, null, null, 3)
        assertTrue(f.any { it.title.contains("calentando") })
    }

    @Test fun burstLossWithPacedOkSuggestsPace() {
        val f = Advisor.adviseBurst(BurstResult(400, 250, 300.0, 0), BurstResult(400, 400, 98.0, 100))
        assertTrue(f.any { it.severity == Severity.WARNING && it.detail.contains("--pace 100") })
    }

    @Test fun burstLossEvenPacedSuggestsLowerBitrate() {
        val f = Advisor.adviseBurst(BurstResult(400, 200, 80.0, 0), BurstResult(400, 300, 60.0, 100))
        assertTrue(f.any { it.detail.contains("--bitrate") })
    }

    @Test fun cleanBurstIsOk() {
        val f = Advisor.adviseBurst(BurstResult(400, 400, 400.0, 0), null)
        assertTrue(f.single().severity == Severity.OK)
    }
}
