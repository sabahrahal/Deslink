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
    const val TYPE_DISCOVER = 12

    const val FLAG_KEYFRAME = 1
    const val FLAG_LAST_PACKET = 2

    const val CODEC_H264 = 1
    const val CODEC_HEVC = 2

    /** Bit de [StreamInfo.hostFlags]: el host aplica la [HelloConfig] que la tablet manda en HELLO. */
    const val HOST_ACCEPTS_HELLO_CONFIG = 1
    const val HELLO_CONFIG_SIZE = 14
    const val MAX_HOST_NAME = 64

    class Header(
        val type: Int,
        val flags: Int,
        val frameId: Long,
        val packetIndex: Int,
        val packetCount: Int,
        val timestampUs: Long,
        val payloadSize: Int,
    )

    /** [hostFlags] y [hostName] solo los mandan hosts nuevos; uno antiguo deja 0 y "". */
    class StreamInfo(
        val width: Int,
        val height: Int,
        val fps: Int,
        val codec: Int,
        val hostFlags: Int = 0,
        val hostName: String = "",
    ) {
        val acceptsHelloConfig: Boolean get() = hostFlags and HOST_ACCEPTS_HELLO_CONFIG != 0
    }

    /**
     * Lo que la tablet pide al host en el payload de HELLO (ver docs/protocol-dlp.md). 0 = lo que diga el host.
     * Un host antiguo ignora el payload.
     */
    data class HelloConfig(
        val width: Int = 0,
        val height: Int = 0,
        val fps: Int = 0,
        val bitrateMbps: Int = 0,
        val codec: Int = 0,
        val densityDpi: Int = 0,
        val refreshHz: Int = 0,
    ) {
        fun encode(): ByteArray {
            val b = ByteBuffer.allocate(HELLO_CONFIG_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            b.putShort(0, width.toShort())
            b.putShort(2, height.toShort())
            b.putShort(4, fps.toShort())
            b.putShort(6, bitrateMbps.toShort())
            b.put(8, codec.toByte())
            b.put(9, 0)
            b.putShort(10, densityDpi.toShort())
            b.putShort(12, refreshHz.toShort())
            return b.array()
        }
    }

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

    /** Paquete de control (HELLO, KEYFRAME_REQUEST, PING, DISCOVER), sin payload salvo que se pase uno. */
    fun controlPacket(type: Int, timestampUs: Long, payload: ByteArray? = null): ByteArray {
        val size = payload?.size ?: 0
        val b = ByteBuffer.allocate(HEADER_SIZE + size).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, type.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, size.toShort())
        if (payload != null) System.arraycopy(payload, 0, b.array(), HEADER_SIZE, size)
        return b.array()
    }

    /** HELLO con la configuración que pide la tablet (o vacío si [config] es null). */
    fun hello(config: HelloConfig?, timestampUs: Long): ByteArray =
        controlPacket(TYPE_HELLO, timestampUs, config?.encode())

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

    fun parseStreamInfo(data: ByteArray, header: Header): StreamInfo? {
        if (header.payloadSize < 7) return null
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        var flags = 0
        var name = ""
        if (header.payloadSize >= 9) {
            flags = data[HEADER_SIZE + 7].toInt() and 0xFF
            val len = minOf(data[HEADER_SIZE + 8].toInt() and 0xFF, MAX_HOST_NAME, header.payloadSize - 9)
            name = String(data, HEADER_SIZE + 9, len, Charsets.UTF_8)
        }
        return StreamInfo(
            width = b.getShort(HEADER_SIZE).toInt() and 0xFFFF,
            height = b.getShort(HEADER_SIZE + 2).toInt() and 0xFFFF,
            fps = b.getShort(HEADER_SIZE + 4).toInt() and 0xFFFF,
            codec = data[HEADER_SIZE + 6].toInt() and 0xFF,
            hostFlags = flags,
            hostName = name,
        )
    }
}
