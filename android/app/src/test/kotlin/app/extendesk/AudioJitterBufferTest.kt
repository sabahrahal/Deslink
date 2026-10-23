package app.extendesk

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class AudioJitterBufferTest {
    private fun frame(seq: Long) = byteArrayOf(seq.toByte())

    private fun AudioJitterBuffer.popSeq(now: Long): Long? {
        val out = pop(now)
        return if (out.kind == AudioJitterBuffer.Kind.FRAME) out.data!![0].toLong() else null
    }

    @Test fun waitsForTargetThenPlaysInOrder() {
        val b = AudioJitterBuffer(minTarget = 3)
        b.push(10, frame(10), 0)
        b.push(11, frame(11), 0)
        assertEquals(AudioJitterBuffer.Kind.IDLE, b.pop(5).kind)
        b.push(12, frame(12), 10)
        assertEquals(10L, b.popSeq(10))
        assertEquals(11L, b.popSeq(10))
        assertEquals(12L, b.popSeq(10))
    }

    @Test fun shortSoundPlaysAfterWaitingTarget() {
        val b = AudioJitterBuffer(minTarget = 3)
        b.push(0, frame(0), 0)
        assertEquals(AudioJitterBuffer.Kind.IDLE, b.pop(10).kind)
        assertEquals(0L, b.popSeq(30))
    }

    @Test fun missingFrameWithLaterOnesIsLost() {
        val b = AudioJitterBuffer(minTarget = 2)
        b.push(0, frame(0), 0)
        b.push(2, frame(2), 0)
        b.push(3, frame(3), 0)
        assertEquals(0L, b.popSeq(0))
        assertEquals(AudioJitterBuffer.Kind.LOST, b.pop(0).kind)
        assertEquals(2L, b.popSeq(0))
        assertEquals(1L, b.lost)
        // Llega tarde: ya se rellenó, se ignora.
        b.push(1, frame(1), 0)
        assertEquals(3L, b.popSeq(0))
    }

    @Test fun underrunWaitsForLateFrameAndGrowsTarget() {
        val b = AudioJitterBuffer(minTarget = 2)
        b.push(0, frame(0), 0)
        b.push(1, frame(1), 0)
        assertEquals(0L, b.popSeq(0))
        assertEquals(1L, b.popSeq(10))
        // El 2 llega tarde: silencio sin avanzar.
        assertEquals(AudioJitterBuffer.Kind.LOST, b.pop(20).kind)
        assertEquals(AudioJitterBuffer.Kind.LOST, b.pop(30).kind)
        b.push(2, frame(2), 35)
        assertEquals(2L, b.popSeq(40))
        assertEquals(1L, b.underruns)
        assertEquals(3, b.target)
    }

    @Test fun silenceOnThePcStopsWithoutGrowingTarget() {
        val b = AudioJitterBuffer(minTarget = 2)
        b.push(0, frame(0), 0)
        b.push(1, frame(1), 0)
        b.popSeq(0)
        b.popSeq(0)
        repeat(5) { assertEquals(AudioJitterBuffer.Kind.LOST, b.pop(0).kind) }
        assertEquals(AudioJitterBuffer.Kind.IDLE, b.pop(0).kind)
        assertEquals(AudioJitterBuffer.Kind.IDLE, b.pop(0).kind)
        // Vuelve a sonar: espera otra vez a llenar el colchón.
        b.push(2, frame(2), 1000)
        assertEquals(AudioJitterBuffer.Kind.IDLE, b.pop(1000).kind)
        b.push(3, frame(3), 1010)
        assertEquals(2L, b.popSeq(1010))
        assertEquals(2, b.target)
    }

    @Test fun excessDelayPlaysFasterAndHugeDelaySkips() {
        val b = AudioJitterBuffer(minTarget = 2)
        for (s in 0L until 5) b.push(s, frame(s), 0)
        var out = b.pop(0)
        assertEquals(-1, out.adjust)  // 5 trozos con objetivo 2: sobra
        for (s in 5L until 30) b.push(s, frame(s), 0)
        out = b.pop(0)
        assertEquals(AudioJitterBuffer.Kind.FRAME, out.kind)
        assertTrue("salta hasta el objetivo", b.depth() <= b.target + 1)
        assertTrue(b.dropped > 0)
    }

    @Test fun shortBufferPlaysSlower() {
        val b = AudioJitterBuffer(minTarget = 4)
        for (s in 0L until 4) b.push(s, frame(s), 0)
        // Suenan dos sin que llegue nada y luego llega uno por cada uno que suena: el colchón se queda corto.
        b.pop(0)
        b.pop(0)
        var adjust = 0
        var seq = 4L
        repeat(200) {
            b.push(seq, frame(seq), 0)
            seq++
            adjust = b.pop(0).adjust
        }
        assertEquals(1, adjust)
    }

    @Test fun hostRestartResetsSequence() {
        val b = AudioJitterBuffer(minTarget = 2)
        b.push(5000, frame(1), 0)
        b.push(5001, frame(2), 0)
        b.popSeq(0)
        b.push(0, frame(9), 0)
        b.push(1, frame(10), 0)
        assertEquals(9L, b.popSeq(0))
    }

    @Test fun restartAfterIdleWithLowerSequence() {
        val b = AudioJitterBuffer(minTarget = 2)
        b.push(100, frame(1), 0)
        b.push(101, frame(2), 0)
        b.popSeq(0)
        b.popSeq(0)
        repeat(6) { b.pop(0) }
        assertEquals(AudioJitterBuffer.Kind.IDLE, b.pop(0).kind)
        b.push(3, frame(3), 100)
        b.push(4, frame(4), 100)
        assertEquals(3L, b.popSeq(100))
    }

    @Test fun stretchKeepsEndsAndLength() {
        val input = ShortArray(480 * 2) { (it / 2 * 10).toShort() }
        val out = ShortArray(500 * 2)
        assertEquals(485 * 2, Pcm.stretch(input, 480, 2, 485, out))
        assertEquals(input[0], out[0])
        assertEquals(input[479 * 2], out[484 * 2])
        assertEquals(475 * 2, Pcm.stretch(input, 480, 2, 475, out))
        assertEquals(input[479 * 2 + 1], out[474 * 2 + 1])
        // Rampa: sigue siendo creciente.
        for (i in 1 until 475) assertTrue(out[i * 2] >= out[(i - 1) * 2])
    }

    @Test fun parsesAudioPacketWithRedundantFrame() {
        val cur = byteArrayOf(1, 2, 3)
        val prev = byteArrayOf(4, 5)
        val payload = 1 + 2 + cur.size + 2 + prev.size
        val b = ByteBuffer.allocate(Dlp.HEADER_SIZE + payload).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, Dlp.MAGIC.toShort())
        b.put(2, Dlp.VERSION.toByte())
        b.put(3, Dlp.TYPE_AUDIO.toByte())
        b.putInt(6, 77)
        b.putShort(12, 1)
        b.putShort(22, payload.toShort())
        b.position(Dlp.HEADER_SIZE)
        b.put(2).putShort(cur.size.toShort()).put(cur).putShort(prev.size.toShort()).put(prev)
        val data = b.array()
        val header = Dlp.parseHeader(data, data.size)!!
        val frames = Dlp.parseAudio(data, header)
        assertEquals(2, frames.size)
        assertEquals(77L, frames[0].seq)
        assertArrayEquals(cur, frames[0].data)
        assertEquals(76L, frames[1].seq)
        assertArrayEquals(prev, frames[1].data)
        // Tamaño que se sale: paquete descartado.
        data[Dlp.HEADER_SIZE + 1] = 50
        assertEquals(0, Dlp.parseAudio(data, header).size)
    }

    @Test fun audioControlPacket() {
        val p = Dlp.audioControl(true, 5)
        val h = Dlp.parseHeader(p, p.size)!!
        assertEquals(Dlp.TYPE_AUDIO_CONTROL, h.type)
        assertEquals(1, h.payloadSize)
        assertEquals(1.toByte(), p[Dlp.HEADER_SIZE])
        assertEquals(0.toByte(), Dlp.audioControl(false, 5)[Dlp.HEADER_SIZE])
    }
}
