package com.desklink

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionConfigTest {
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    private fun parse(payload: ByteArray): Dlp.SessionConfig? {
        val p = Dlp.controlPacket(Dlp.TYPE_SESSION_CONFIG, 9, payload)
        return Dlp.parseSessionConfig(p, Dlp.parseHeader(p, p.size)!!)
    }

    @Test
    fun sameBytesAsThePc() {
        // Mismo caso que protocol/tests/test_session.cpp (TestSessionConfigRoundtrip).
        val golden = bytes(1, 3, 2, 1, 120, 0, 0x9C, 0x01, 0x08, 0x02)
        val c = parse(golden)!!
        assertEquals(Dlp.SessionConfig(1, 3, Dlp.INPUT_VIEW_ONLY, Dlp.REDUCED_WIFI, 120, 412, 520), c)
        assertTrue(c.audioOnTablet && c.batteryAdapt && c.viewOnly)
        assertArrayEquals(golden, c.encode())
        assertEquals(PowerProfile.EQUILIBRADO, PowerProfile.fromWire(c.profile))
    }

    @Test
    fun unknownValuesAreClampedAndShortIsIgnored() {
        val c = parse(bytes(9, 0, 7, 9, 60, 0, 0, 0, 0, 0))!!
        assertEquals(1, c.profile)
        assertEquals(Dlp.INPUT_TOUCH, c.input)
        assertEquals(Dlp.REDUCED_NONE, c.reduced)
        assertEquals(Dlp.REDUCED_TABLET, parse(bytes(0, 0, 0, 4, 90, 0, 0, 0, 0, 0))!!.reduced)
        assertFalse(c.audioOnTablet)
        assertNull(parse(bytes(1, 2, 3)))
    }
}
