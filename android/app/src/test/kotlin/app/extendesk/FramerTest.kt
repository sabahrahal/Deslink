package app.extendesk

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream
import java.net.SocketTimeoutException

class FramerTest {
    private fun packet(type: Int, size: Int): ByteArray {
        val p = Dlp.controlPacket(type, 42L).copyOf(size)
        for (i in Dlp.HEADER_SIZE until size) p[i] = i.toByte()
        return p
    }

    /** Entrega los bytes en trozos de [chunk] y, entre trozo y trozo, un tiempo de espera agotado. */
    private class Trickle(private val data: ByteArray, private val chunk: Int) : InputStream() {
        private var pos = 0
        private var timeoutNext = false
        override fun read(): Int = throw UnsupportedOperationException()
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (pos == data.size) return -1
            if (timeoutNext) {
                timeoutNext = false
                throw SocketTimeoutException()
            }
            timeoutNext = true
            val n = minOf(chunk, len, data.size - pos)
            System.arraycopy(data, pos, b, off, n)
            pos += n
            return n
        }
    }

    @Test fun splitsPacketsFromStream() {
        val a = packet(Dlp.TYPE_VIDEO, Dlp.MAX_PACKET_SIZE)
        val b = packet(Dlp.TYPE_PONG, Dlp.HEADER_SIZE)
        val framer = Framer()
        val input = ByteArrayInputStream(Framer.frame(a) + Framer.frame(b))
        val out = ByteArray(2048)
        assertEquals(a.size, framer.next(input, out))
        assertArrayEquals(a, out.copyOf(a.size))
        assertEquals(b.size, framer.next(input, out))
        assertArrayEquals(b, out.copyOf(b.size))
    }

    @Test fun keepsPartialPacketAcrossTimeouts() {
        val packets = List(50) { packet(Dlp.TYPE_VIDEO, 100 + it * 20) }
        val stream = packets.fold(ByteArray(0)) { acc, p -> acc + Framer.frame(p) }
        val input = Trickle(stream, 7)
        val framer = Framer()
        val out = ByteArray(2048)
        val got = ArrayList<ByteArray>()
        while (got.size < packets.size) {
            try {
                val n = framer.next(input, out)
                got += out.copyOf(n)
            } catch (_: SocketTimeoutException) {
            }
        }
        for (i in packets.indices) assertArrayEquals(packets[i], got[i])
    }

    @Test(expected = IOException::class)
    fun rejectsOversizedLength() {
        val bad = byteArrayOf(0xFF.toByte(), 0xFF.toByte(), 1, 2, 3)
        Framer().next(ByteArrayInputStream(bad), ByteArray(2048))
    }

    @Test(expected = IOException::class)
    fun reportsClosedStream() {
        Framer().next(ByteArrayInputStream(ByteArray(0)), ByteArray(2048))
    }
}
