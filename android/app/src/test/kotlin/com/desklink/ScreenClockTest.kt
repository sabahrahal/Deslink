package com.desklink

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ScreenClockTest {
    private val period = 6_944_444L // 144 Hz

    @Test
    fun noDataYet() {
        assertNull(ScreenClock.estimate(1_000, 0, period, LongArray(0)))
        assertNull(ScreenClock.estimate(1_000, 500, 0, LongArray(0)))
    }

    @Test
    fun withoutTimelinesNextVsyncPlusOne() {
        val vsync = 100_000_000L
        // Soltado 1 ms después del refresco: lo recoge el siguiente y se enciende en el de después.
        assertEquals(vsync + 2 * period, ScreenClock.estimate(vsync + 1_000_000, vsync, period, LongArray(0)))
        // Soltado justo antes del siguiente refresco: sigue siendo ese.
        assertEquals(vsync + 2 * period, ScreenClock.estimate(vsync + period - 1, vsync, period, LongArray(0)))
    }

    @Test
    fun usesFirstTimelineWhoseDeadlineIsAhead() {
        val vsync = 100_000_000L
        val lines = longArrayOf(
            vsync + 4_000_000, vsync + 13_000_000,
            vsync + 4_000_000 + period, vsync + 13_000_000 + period,
        )
        assertEquals(vsync + 13_000_000, ScreenClock.estimate(vsync + 3_000_000, vsync, period, lines))
        assertEquals(vsync + 13_000_000 + period, ScreenClock.estimate(vsync + 5_000_000, vsync, period, lines))
        // Más allá de la última línea: se extrapola al ritmo de la pantalla.
        assertEquals(
            vsync + 13_000_000 + 2 * period,
            ScreenClock.estimate(vsync + 4_000_000 + period + 1, vsync, period, lines),
        )
    }
}
