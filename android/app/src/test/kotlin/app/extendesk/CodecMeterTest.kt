package app.extendesk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CodecMeterTest {
    @Test
    fun normalizesPerMegapixel() {
        // Idea Tab Pro: 2944x1840 = 5,42 MP; 9 ms por frame → 1,66 ms/MP.
        assertEquals(1.66, CodecMeter.normalize(2944, 1840, 9.0)!!, 0.01)
        assertNull(CodecMeter.normalize(0, 1840, 9.0))
        assertNull(CodecMeter.normalize(2944, 1840, 0.0))
        assertNull(CodecMeter.normalize(2944, 1840, 900.0))
    }

    @Test
    fun blendIsSlow() {
        assertEquals(2.0, CodecMeter.blend(null, 2.0), 1e-9)
        assertEquals(1.05, CodecMeter.blend(1.0, 2.0), 1e-9)
    }

    @Test
    fun picksFasterCodecWithMargin() {
        // MediaTek medido: H.264 9 ms, HEVC 14 ms → H.264.
        assertTrue(CodecMeter.decide(1.66, 2.58, default = false))
        // Casi iguales: se queda HEVC, que se ve algo mejor a igual tasa.
        assertFalse(CodecMeter.decide(1.0, 1.1, default = true))
        // HEVC más rápido.
        assertFalse(CodecMeter.decide(2.0, 1.0, default = true))
    }

    @Test
    fun withoutDataUsesRuleAndThenTriesTheOther() {
        assertTrue(CodecMeter.decide(null, null, default = true))
        assertFalse(CodecMeter.decide(null, null, default = false))
        assertFalse(CodecMeter.decide(1.5, null, default = true))
        assertTrue(CodecMeter.decide(null, 1.5, default = false))
    }
}
