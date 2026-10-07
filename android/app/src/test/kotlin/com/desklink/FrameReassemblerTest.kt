package com.desklink

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameReassemblerTest {
    /** Mismo formato que dlp::PacketizeFrame en C++. */
    private fun packetize(data: ByteArray, frameId: Long, keyframe: Boolean, ts: Long): List<ByteArray> {
        val count = (data.size + Dlp.MAX_PAYLOAD - 1) / Dlp.MAX_PAYLOAD
        return (0 until count).map { i ->
            val offset = i * Dlp.MAX_PAYLOAD
            val n = minOf(Dlp.MAX_PAYLOAD, data.size - offset)
            val b = ByteBuffer.allocate(Dlp.HEADER_SIZE + n).order(ByteOrder.LITTLE_ENDIAN)
            b.putShort(0, Dlp.MAGIC.toShort())
            b.put(2, Dlp.VERSION.toByte())
            b.put(3, Dlp.TYPE_VIDEO.toByte())
            b.put(4, ((if (keyframe) Dlp.FLAG_KEYFRAME else 0) or (if (i + 1 == count) Dlp.FLAG_LAST_PACKET else 0)).toByte())
            b.putInt(6, frameId.toInt())
            b.putShort(10, i.toShort())
            b.putShort(12, count.toShort())
            b.putLong(14, ts)
            b.putShort(22, n.toShort())
            System.arraycopy(data, offset, b.array(), Dlp.HEADER_SIZE, n)
            b.array()
        }
    }

    private fun data(n: Int, seed: Int) = ByteArray(n) { (seed + it * 31).toByte() }

    private fun feed(r: FrameReassembler, packets: List<ByteArray>): Frame? {
        var out: Frame? = null
        for (p in packets) r.push(p, p.size)?.let { out = it }
        return out
    }

    @Test
    fun roundtripVariousSizes() {
        for (size in listOf(1, Dlp.MAX_PAYLOAD - 1, Dlp.MAX_PAYLOAD, Dlp.MAX_PAYLOAD + 1, 100_000)) {
            val r = FrameReassembler()
            val d = data(size, 3)
            val f = feed(r, packetize(d, 1, true, 42))
            assertNotNull(f)
            assertArrayEquals(d, f!!.data.copyOf(f.size))
            assertTrue(f.keyframe)
            assertEquals(42L, f.timestampUs)
            assertFalse(r.needsKeyframe)
        }
    }

    @Test
    fun outOfOrderPackets() {
        val r = FrameReassembler()
        val d = data(20_000, 9)
        val f = feed(r, packetize(d, 5, false, 1).shuffled(java.util.Random(1234)))
        assertNotNull(f)
        assertArrayEquals(d, f!!.data.copyOf(f.size))
    }

    @Test
    fun lossRequestsKeyframeUntilNextKeyframe() {
        val r = FrameReassembler()
        feed(r, packetize(data(5000, 1), 1, true, 0))
        assertFalse(r.needsKeyframe)

        val p2 = packetize(data(5000, 2), 2, false, 0)
        for (p in p2.dropLast(1)) r.push(p, p.size)
        val f3 = feed(r, packetize(data(3000, 3), 3, false, 0))
        assertEquals(3L, f3!!.frameId)
        assertEquals(1L, r.framesDropped)
        assertTrue(r.needsKeyframe)

        // El paquete tardío del frame 2 se ignora.
        assertNull(r.push(p2.last(), p2.last().size))

        feed(r, packetize(data(2000, 4), 4, true, 0))
        assertFalse(r.needsKeyframe)
    }

    @Test
    fun gapInNumberingCountsAsLoss() {
        val r = FrameReassembler()
        feed(r, packetize(data(100, 1), 10, true, 0))
        feed(r, packetize(data(100, 2), 13, false, 0))
        assertEquals(2L, r.framesDropped)
        assertTrue(r.needsKeyframe)
    }

    @Test
    fun headerRejectsGarbage() {
        assertNull(Dlp.parseHeader(ByteArray(Dlp.HEADER_SIZE), Dlp.HEADER_SIZE))
        assertNull(Dlp.parseHeader(ByteArray(5), 5))
        val ping = Dlp.controlPacket(Dlp.TYPE_PING, 99)
        val h = Dlp.parseHeader(ping, ping.size)
        assertNotNull(h)
        assertEquals(Dlp.TYPE_PING, h!!.type)
        assertEquals(99L, h.timestampUs)
    }

    @Test
    fun missingPacketRecoveredByNack() {
        val r = FrameReassembler()
        r.graceUs = 50_000
        val d = data(10_000, 5)
        val packets = packetize(d, 1, true, 1)
        assertTrue(packets.size > 3)
        // Falta el paquete 1; llegan los demás.
        var out: Frame? = null
        for ((i, p) in packets.withIndex()) if (i != 1) r.push(p, p.size, 1_000)?.let { out = it }
        assertNull(out)
        val nacks = r.collectNacks(10_000, retryUs = 4_000)
        assertEquals(1, nacks.size)
        assertEquals(1L, nacks[0].frameId)
        assertEquals(1, nacks[0].count)
        assertEquals(1, nacks[0].indices[0])
        // No se vuelve a pedir antes del reintento.
        assertTrue(r.collectNacks(11_000, retryUs = 4_000).isEmpty())
        // Llega el reenvío.
        val f = r.push(packets[1], packets[1].size, 12_000)
        assertNotNull(f)
        assertArrayEquals(d, f!!.data.copyOf(f.size))
        assertFalse(r.needsKeyframe)
    }

    @Test
    fun completeFrameWaitsForOlderOneWithinGrace() {
        val r = FrameReassembler()
        r.graceUs = 50_000
        val a = packetize(data(5_000, 1), 1, true, 1)
        val b = packetize(data(5_000, 2), 2, false, 2)
        for (p in a.drop(1)) r.push(p, p.size, 1_000)
        var out: Frame? = null
        for (p in b) r.push(p, p.size, 2_000)?.let { out = it }
        assertNull(out)  // el frame 2 espera al 1
        // Llega el paquete que faltaba: salen ambos en orden.
        val first = r.push(a[0], a[0].size, 3_000)
        assertEquals(1L, first!!.frameId)
        assertEquals(2L, r.poll(3_000)!!.frameId)
        assertEquals(0L, r.framesDropped)
    }

    @Test
    fun olderFrameDroppedAfterGrace() {
        val r = FrameReassembler()
        r.graceUs = 20_000
        val a = packetize(data(5_000, 1), 1, true, 1)
        val b = packetize(data(5_000, 2), 2, false, 2)
        for (p in a.drop(1)) r.push(p, p.size, 1_000)
        for (p in b) r.push(p, p.size, 2_000)
        val f = r.poll(30_000)
        assertEquals(2L, f!!.frameId)
        assertEquals(1L, r.framesDropped)
        assertTrue(r.needsKeyframe)
    }
}
