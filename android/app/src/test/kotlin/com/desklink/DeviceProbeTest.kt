package com.desklink

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DeviceProbeTest {
    @Test
    fun currentInMicroampsUsesChargingStateForSign() {
        assertEquals(-3_800.0, DeviceProbe.currentToMw(1_000_000, 3_800, charging = false)!!, 0.01)
        assertEquals(-3_800.0, DeviceProbe.currentToMw(-1_000_000, 3_800, charging = false)!!, 0.01)
        assertEquals(3_800.0, DeviceProbe.currentToMw(-1_000_000, 3_800, charging = true)!!, 0.01)
    }

    @Test
    fun currentInMilliampsIsScaled() {
        assertEquals(-1_900.0, DeviceProbe.currentToMw(500, 3_800, charging = false)!!, 0.01)
    }

    @Test
    fun unsupportedPropertyGivesNull() {
        assertNull(DeviceProbe.currentToMw(0, 3_800, charging = false))
        assertNull(DeviceProbe.currentToMw(Long.MIN_VALUE, 3_800, charging = false))
        assertNull(DeviceProbe.currentToMw(1_000_000, 0, charging = false))
    }
}
