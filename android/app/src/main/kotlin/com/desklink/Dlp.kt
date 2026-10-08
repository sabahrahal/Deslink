package com.desklink

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** DeskLink Protocol v1. Equivale a protocol/include/dlp/packet.h; ver docs/protocol-dlp.md. */
object Dlp {
    const val MAGIC = 0x4C44
    const val VERSION = 1
    const val HEADER_SIZE = 24
    const val MAX_PACKET_SIZE = 1200
    const val MAX_PAYLOAD = MAX_PACKET_SIZE - HEADER_SIZE
    const val DEFAULT_PORT = 47800

    const val TYPE_VIDEO = 1
    const val TYPE_HELLO = 2
    const val TYPE_KEYFRAME_REQUEST = 3
    const val TYPE_PING = 4
    const val TYPE_PONG = 5
    const val TYPE_STREAM_INFO = 6
    const val TYPE_BURST_REQUEST = 7
    const val TYPE_BURST_DATA = 8
    const val TYPE_RATE_REPORT = 9
    const val TYPE_NACK = 10
    const val TYPE_TOUCH = 11
    const val TYPE_AUDIO = 12
    const val TYPE_AUDIO_CONTROL = 13

    /** Sonido: Opus a 48 kHz estéreo, un trozo cada 10 ms. */
    const val AUDIO_SAMPLE_RATE = 48000
    const val AUDIO_CHANNELS = 2
    const val AUDIO_FRAME_SAMPLES = 480

    const val FLAG_KEYFRAME = 1
    const val FLAG_LAST_PACKET = 2

    const val CODEC_H264 = 1
    const val CODEC_HEVC = 2

    class Header(
        val type: Int,
        val flags: Int,
        val frameId: Long,
        val packetIndex: Int,
        val packetCount: Int,
        val timestampUs: Long,
        val payloadSize: Int,
    )

    class StreamInfo(val width: Int, val height: Int, val fps: Int, val codec: Int)

    /** Un trozo Opus de 10 ms; [seq] cuenta trozos desde que el host empezó a enviar sonido. */
    class AudioFrame(val seq: Long, val data: ByteArray)

    /** Devuelve null si el paquete no es válido (corto, magic/versión distintos o payload que no cabe). */
    fun parseHeader(data: ByteArray, length: Int): Header? {
        if (length < HEADER_SIZE) return null
        val b = ByteBuffer.wrap(data, 0, length).order(ByteOrder.LITTLE_ENDIAN)
        if ((b.getShort(0).toInt() and 0xFFFF) != MAGIC || (data[2].toInt() and 0xFF) != VERSION) return null
        val payload = b.getShort(22).toInt() and 0xFFFF
        if (payload > MAX_PAYLOAD || length < HEADER_SIZE + payload) return null
        return Header(
            type = data[3].toInt() and 0xFF,
            flags = data[4].toInt() and 0xFF,
            frameId = b.getInt(6).toLong() and 0xFFFFFFFFL,
            packetIndex = b.getShort(10).toInt() and 0xFFFF,
            packetCount = b.getShort(12).toInt() and 0xFFFF,
            timestampUs = b.getLong(14),
            payloadSize = payload,
        )
    }

    /** Paquete de control sin payload (HELLO, KEYFRAME_REQUEST, PING). */
    fun controlPacket(type: Int, timestampUs: Long): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, type.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        return b.array()
    }

    /** Pide al host una ráfaga de [count] paquetes de [size] bytes; [paceMbps] > 0 limita su tasa. */
    fun burstRequest(count: Int, size: Int, paceMbps: Int, timestampUs: Long): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE + 6).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_BURST_REQUEST.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, 6)
        b.putShort(HEADER_SIZE, count.toShort())
        b.putShort(HEADER_SIZE + 2, size.toShort())
        b.putShort(HEADER_SIZE + 4, paceMbps.toShort())
        return b.array()
    }

    /** Informe de calidad: frames recibidos y perdidos en el último segundo. */
    fun rateReport(received: Int, lost: Int, timestampUs: Long): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE + 4).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_RATE_REPORT.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, 4)
        b.putShort(HEADER_SIZE, received.coerceIn(0, 0xFFFF).toShort())
        b.putShort(HEADER_SIZE + 2, lost.coerceIn(0, 0xFFFF).toShort())
        return b.array()
    }

    /** Reclama al host los paquetes [indices] del frame [frameId] (como mucho 128). */
    fun nack(frameId: Long, indices: IntArray, count: Int, timestampUs: Long): ByteArray {
        val n = minOf(count, 128)
        val b = ByteBuffer.allocate(HEADER_SIZE + 2 * n).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_NACK.toByte())
        b.putInt(6, frameId.toInt())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, (2 * n).toShort())
        for (i in 0 until n) b.putShort(HEADER_SIZE + 2 * i, indices[i].toShort())
        return b.array()
    }

    /**
     * Estado táctil completo: [n] dedos con (id, x, y, presión) consecutivos en [state]; x e y en 0..1.
     * Se manda entero cada vez, así un paquete perdido no deja un dedo "pegado" en el PC.
     */
    fun touch(state: FloatArray, n: Int, timestampUs: Long): ByteArray {
        val count = minOf(n, 10)
        val size = 1 + 7 * count
        val b = ByteBuffer.allocate(HEADER_SIZE + size).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_TOUCH.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, size.toShort())
        b.put(HEADER_SIZE, count.toByte())
        for (i in 0 until count) {
            val o = HEADER_SIZE + 1 + 7 * i
            b.put(o, state[4 * i].toInt().toByte())
            b.putShort(o + 1, (state[4 * i + 1].coerceIn(0f, 1f) * 65535f).toInt().toShort())
            b.putShort(o + 3, (state[4 * i + 2].coerceIn(0f, 1f) * 65535f).toInt().toShort())
            b.putShort(o + 5, (state[4 * i + 3].coerceIn(0f, 1f) * 1024f).toInt().toShort())
        }
        return b.array()
    }

    /** Dice al host si queremos el sonido del PC. Se repite cada segundo (UDP puede perderlo). */
    fun audioControl(enabled: Boolean, timestampUs: Long): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE + 1).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_AUDIO_CONTROL.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, 1)
        b.put(HEADER_SIZE, if (enabled) 1 else 0)
        return b.array()
    }

    /**
     * Trozos de un paquete AUDIO: u8 n y n x {u16 tamaño, bytes}; la entrada i es el trozo frameId - i (el host
     * repite el anterior por si se perdió). Vacío si el payload no es válido.
     */
    fun parseAudio(data: ByteArray, header: Header): List<AudioFrame> {
        if (header.payloadSize < 1) return emptyList()
        val end = HEADER_SIZE + header.payloadSize
        val n = data[HEADER_SIZE].toInt() and 0xFF
        var o = HEADER_SIZE + 1
        val frames = ArrayList<AudioFrame>(n)
        for (i in 0 until n) {
            if (o + 2 > end) return emptyList()
            val size = (data[o].toInt() and 0xFF) or ((data[o + 1].toInt() and 0xFF) shl 8)
            o += 2
            if (size == 0 || o + size > end) return emptyList()
            frames.add(AudioFrame(header.frameId - i, data.copyOfRange(o, o + size)))
            o += size
        }
        return frames
    }

    fun parseStreamInfo(data: ByteArray, header: Header): StreamInfo? {
        if (header.payloadSize < 7) return null
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        return StreamInfo(
            width = b.getShort(HEADER_SIZE).toInt() and 0xFFFF,
            height = b.getShort(HEADER_SIZE + 2).toInt() and 0xFFFF,
            fps = b.getShort(HEADER_SIZE + 4).toInt() and 0xFFFF,
            codec = data[HEADER_SIZE + 6].toInt() and 0xFF,
        )
    }
}
