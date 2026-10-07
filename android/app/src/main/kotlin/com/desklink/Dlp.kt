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
