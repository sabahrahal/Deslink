package app.extendesk

import org.junit.Assert.assertEquals
import org.junit.Test

class ByeTest {
    @Test
    fun byeCarriesItsReason() {
        for (reason in listOf(Dlp.BYE_LEAVE, Dlp.BYE_PAUSE)) {
            val packet = Dlp.bye(reason, 7)
            val h = Dlp.parseHeader(packet, packet.size)!!
            // Mismo número que dlp::PacketType::kBye en protocol/include/dlp/packet.h.
            assertEquals(20, h.type)
            assertEquals(1, h.payloadSize)
            assertEquals(reason, Dlp.parseByeReason(packet, h))
        }
    }

    @Test
    fun byeWithoutPayloadMeansLeave() {
        val packet = Dlp.controlPacket(Dlp.TYPE_BYE, 7)
        assertEquals(Dlp.BYE_LEAVE, Dlp.parseByeReason(packet, Dlp.parseHeader(packet, packet.size)!!))
    }

    @Test
    fun theTabletGivesUpWhenThePcStopsKeepingTheScreen() {
        // La tablet deja de esperar a la vez que el PC quita la pantalla extra por un corte sin aviso (5 s + 15 s).
        assertEquals(20_000_000L, StreamSession.LOST_US)
    }
}
