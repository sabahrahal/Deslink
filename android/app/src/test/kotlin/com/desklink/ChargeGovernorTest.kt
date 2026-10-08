package com.desklink

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ChargeGovernorTest {
    private val s = 1_000L

    /** Llama a update cada segundo durante [seconds] con la misma medida; devuelve el instante final. */
    private fun ChargeGovernor.run(from: Long, seconds: Int, plugged: Boolean, netMw: Double?, percent: Int = 60): Long {
        var t = from
        repeat(seconds) {
            update(t, plugged, netMw, percent)
            t += s
        }
        return t
    }

    @Test
    fun drainingOnCableStepsDownUntilBalanced() {
        val g = ChargeGovernor(PowerProfile.MAXIMA, adapt = true)
        var t = g.run(0, 2, plugged = true, netMw = -1_200.0)
        assertEquals(PowerProfile.MAXIMA, g.current)
        assertEquals(ChargeState.ADAPTING, g.state)
        // Espera a que se asiente la medida antes de bajar.
        t = g.run(t, 44, plugged = true, netMw = -1_200.0)
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
        t = g.run(t, 45, plugged = true, netMw = -400.0)
        assertEquals(PowerProfile.AHORRO, g.current)
        // Ya carga: se queda en Ahorro y lo dice.
        g.run(t, 60, plugged = true, netMw = 300.0)
        assertEquals(PowerProfile.AHORRO, g.current)
        assertEquals(ChargeState.CHARGING, g.state)
    }

    @Test
    fun warnsWhenNothingLeftToLower() {
        val g = ChargeGovernor(PowerProfile.AHORRO, adapt = true)
        var t = g.run(0, 45, plugged = true, netMw = -2_000.0)
        assertEquals(PowerProfile.MINIMO, g.current)
        t = g.run(t, 20, plugged = true, netMw = -900.0)
        assertEquals(ChargeState.ADAPTING, g.state)
        g.run(t, 20, plugged = true, netMw = -900.0)
        assertEquals(ChargeState.DRAINING, g.state)
        assertTrue(g.indicator().startsWith("Se descarga pese al cable"))
    }

    @Test
    fun withoutAdaptOnlyWarns() {
        val g = ChargeGovernor(PowerProfile.MAXIMA, adapt = false)
        g.run(0, 60, plugged = true, netMw = -1_500.0)
        assertEquals(PowerProfile.MAXIMA, g.current)
        assertEquals(ChargeState.DRAINING, g.state)
    }

    @Test
    fun surplusRaisesBackButNotAboveUserChoiceNorIntoBlockedProfile() {
        val g = ChargeGovernor(PowerProfile.EQUILIBRADO, adapt = true)
        var t = g.run(0, 45, plugged = true, netMw = -800.0)
        assertEquals(PowerProfile.AHORRO, g.current)
        // Mucho margen, pero Equilibrado acaba de descargar: no se vuelve antes de 15 min.
        t = g.run(t, 300, plugged = true, netMw = 3_000.0)
        assertEquals(PowerProfile.AHORRO, g.current)
        t = g.run(t, 15 * 60, plugged = true, netMw = 3_000.0)
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
        // Nunca por encima de lo que eligió el usuario.
        g.run(t, 600, plugged = true, netMw = 5_000.0)
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
    }

    @Test
    fun smallSurplusDoesNotRaise() {
        val g = ChargeGovernor(PowerProfile.MAXIMA, adapt = true)
        var t = g.run(0, 45, plugged = true, netMw = -500.0)
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
        g.run(t, 30 * 60, plugged = true, netMw = 800.0)
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
    }

    @Test
    fun percentDropCountsAsDrainWithoutMeasurement() {
        val g = ChargeGovernor(PowerProfile.EQUILIBRADO, adapt = true)
        var t = 0L
        repeat(10) { g.update(t, true, null, 50); t += s }
        assertEquals(ChargeState.MEASURING, g.state)
        repeat(45) { g.update(t, true, null, 49); t += s }
        assertEquals(PowerProfile.AHORRO, g.current)
    }

    @Test
    fun unpluggingRestoresUserProfileAndLowBatteryStepsDown() {
        val g = ChargeGovernor(PowerProfile.EQUILIBRADO, adapt = true)
        var t = g.run(0, 45, plugged = true, netMw = -800.0)
        assertEquals(PowerProfile.AHORRO, g.current)
        assertTrue(g.update(t, false, -5_000.0, 60))
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
        t += s
        g.update(t, false, -5_000.0, 60)
        assertEquals(ChargeState.ON_BATTERY, g.state)
        assertTrue(g.update(t + s, false, -5_000.0, 19))
        assertEquals(PowerProfile.AHORRO, g.current)
        // Histéresis: a 22 % sigue en Ahorro.
        assertFalse(g.update(t + 2 * s, false, -5_000.0, 22))
        assertEquals(PowerProfile.AHORRO, g.current)
    }

    @Test
    fun shortDipIsIgnored() {
        val g = ChargeGovernor(PowerProfile.EQUILIBRADO, adapt = true)
        var t = g.run(0, 50, plugged = true, netMw = 1_000.0)
        t = g.run(t, 3, plugged = true, netMw = -900.0)
        g.run(t, 10, plugged = true, netMw = 1_000.0)
        assertEquals(PowerProfile.EQUILIBRADO, g.current)
    }
}
