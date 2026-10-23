package app.extendesk

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

    /** Mismo formato que dlp::AddFec en C++: una paridad tras cada grupo de [group] paquetes. */
    private fun addFec(packets: List<ByteArray>, group: Int): List<ByteArray> {
        val out = ArrayList<ByteArray>()
        for (start in packets.indices step group) {
            val end = minOf(packets.size, start + group)
            val parity = ByteArray(Dlp.MAX_PAYLOAD)
            var lenXor = 0
            var longest = 0
            for (i in start until end) {
                val pkt = packets[i]
                pkt[5] = group.toByte()
                val h = Dlp.parseHeader(pkt, pkt.size)!!
                lenXor = lenXor xor h.payloadSize
                longest = maxOf(longest, h.payloadSize)
                for (b in 0 until h.payloadSize) parity[b] = (parity[b].toInt() xor pkt[Dlp.HEADER_SIZE + b].toInt()).toByte()
                out.add(pkt)
            }
            val first = Dlp.parseHeader(packets[start], packets[start].size)!!
            val b = ByteBuffer.allocate(Dlp.HEADER_SIZE + 2 + longest).order(ByteOrder.LITTLE_ENDIAN)
            b.putShort(0, Dlp.MAGIC.toShort())
            b.put(2, Dlp.VERSION.toByte())
            b.put(3, Dlp.TYPE_VIDEO_FEC.toByte())
            b.put(4, (first.flags and Dlp.FLAG_KEYFRAME).toByte())
            b.put(5, group.toByte())
            b.putInt(6, first.frameId.toInt())
            b.putShort(10, (start / group).toShort())
            b.putShort(12, packets.size.toShort())
            b.putLong(14, first.timestampUs)
            b.putShort(22, (2 + longest).toShort())
            b.putShort(Dlp.HEADER_SIZE, lenXor.toShort())
            System.arraycopy(parity, 0, b.array(), Dlp.HEADER_SIZE + 2, longest)
            out.add(b.array())
        }
        return out
    }

    private fun feed(r: FrameReassembler, packets: List<ByteArray>): Frame? {
        var out: Frame? = null
        for (p in packets) r.push(p, p.size)?.let { out = it }
        return out
    }

    @Test
    fun fecRebuildsOneLostPacketPerGroupWithoutNack() {
        for (lost in listOf(0, 2, 4)) { // uno lleno del primer grupo, el último del primero y el corto del final
            val r = FrameReassembler()
            val d = data(Dlp.MAX_PAYLOAD * 4 + 300, 5)
            val packets = addFec(packetize(d, 1, true, 7), 3)
            val dataIndex = listOf(0, 1, 2, 4, 5) // posiciones de los VIDEO entre las paridades (3 y 6)
            val f = feed(r, packets.filterIndexed { i, _ -> i != dataIndex[lost] })
            assertNotNull("perdido $lost", f)
            assertArrayEquals(d, f!!.data.copyOf(f.size))
            assertEquals(1L, r.packetsRecovered)
            assertEquals(0L, r.packetsRequested)
        }
    }

    @Test
    fun fecWaitsForParityBeforeNacking() {
        val r = FrameReassembler()
        val packets = addFec(packetize(data(Dlp.MAX_PAYLOAD * 6, 9), 1, true, 7), 3)
        // Llegan 0 y 2 (falta el 1): sin la paridad del grupo aún no se reclama.
        r.push(packets[0], packets[0].size, 0)
        r.push(packets[2], packets[2].size, 0)
        assertTrue(r.collectNacks(10_000, retryUs = 4_000, quietUs = 100_000).isEmpty())
        // Se pierde también la paridad: en cuanto llega el grupo siguiente, se reclama.
        r.push(packets[4], packets[4].size, 10_000)
        val nacks = r.collectNacks(11_000, retryUs = 4_000, quietUs = 100_000)
        assertEquals(listOf(1), nacks.flatMap { it.indices.take(it.count) })
    }

    @Test
    fun fecTwoLostInAGroupStillNacks() {
        val r = FrameReassembler()
        val packets = addFec(packetize(data(Dlp.MAX_PAYLOAD * 3, 2), 1, true, 7), 3)
        r.push(packets[0], packets[0].size, 0)
        r.push(packets[3], packets[3].size, 0) // la paridad, sin 1 ni 2
        assertEquals(0L, r.packetsRecovered)
        val nacks = r.collectNacks(10_000, retryUs = 4_000, quietUs = 3_000)
        assertEquals(listOf(1, 2), nacks.flatMap { it.indices.take(it.count) })
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
    fun pendingOnlyWhileAFrameIsIncomplete() {
        val r = FrameReassembler()
        assertFalse(r.hasPending)
        val packets = packetize(data(5_000, 4), 1, true, 0)
        r.push(packets[0], packets[0].size)
        assertTrue(r.hasPending)
        assertNotNull(feed(r, packets.drop(1)))
        assertFalse(r.hasPending)
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
