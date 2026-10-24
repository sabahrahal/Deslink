package app.extendesk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class InputModeTest {
    @Test fun requestCarriesMode() {
        val p = Dlp.inputMode(Dlp.INPUT_MOUSE, 7L)
        val h = Dlp.parseHeader(p, p.size)!!
        assertEquals(Dlp.TYPE_INPUT_MODE, h.type)
        assertEquals(1, h.payloadSize)
        assertEquals(Dlp.INPUT_MOUSE, p[Dlp.HEADER_SIZE].toInt())
    }

    @Test fun parsesHostState() {
        // Lo que manda el host: pedido táctil, en uso ratón (Windows no aceptó los toques).
        val p = Dlp.controlPacket(Dlp.TYPE_INPUT_MODE, 1L).copyOf(Dlp.HEADER_SIZE + 2)
        p[22] = 2
        p[Dlp.HEADER_SIZE] = Dlp.INPUT_TOUCH.toByte()
        p[Dlp.HEADER_SIZE + 1] = Dlp.INPUT_MOUSE.toByte()
        val h = Dlp.parseHeader(p, p.size)!!
        val s = Dlp.parseInputMode(p, h)!!
        assertEquals(Dlp.INPUT_TOUCH, s.requested)
        assertEquals(Dlp.INPUT_MOUSE, s.active)
    }

    @Test fun rejectsShortState() {
        val p = Dlp.controlPacket(Dlp.TYPE_INPUT_MODE, 1L)
        assertNull(Dlp.parseInputMode(p, Dlp.parseHeader(p, p.size)!!))
    }
}
