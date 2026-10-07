package com.desklink

import java.util.TreeMap

class Frame(
    val frameId: Long,
    val keyframe: Boolean,
    val timestampUs: Long,
    val data: ByteArray,
    val size: Int,
)

/**
 * Reensambla frames a partir de paquetes de vídeo. Sin colas: al completarse un frame se
 * descartan los anteriores que sigan incompletos. Cada pérdida activa [needsKeyframe] hasta
 * que llegue un frame clave completo. Equivale a dlp::FrameReassembler de protocol/.
 */
class FrameReassembler {
    private class Pending(
        val packetCount: Int,
        val keyframe: Boolean,
        val timestampUs: Long,
    ) {
        val have = BooleanArray(packetCount)
        val data = ByteArray(packetCount * Dlp.MAX_PAYLOAD)
        var received = 0
        var lastPayload = 0
    }

    private val pending = TreeMap<Long, Pending>()
    private var lastDelivered: Long? = null

    var needsKeyframe = true
        private set
    var framesCompleted = 0L
        private set
    var framesDropped = 0L
        private set

    /** Procesa un paquete completo; devuelve el frame si este paquete lo completa. */
    fun push(packet: ByteArray, length: Int): Frame? {
        val h = Dlp.parseHeader(packet, length) ?: return null
        if (h.type != Dlp.TYPE_VIDEO) return null
        if (h.packetCount == 0 || h.packetIndex >= h.packetCount) return null
        val isLast = h.packetIndex + 1 == h.packetCount
        // Todos los paquetes salvo el último van llenos: la posición es index * MAX_PAYLOAD.
        if (!isLast && h.payloadSize != Dlp.MAX_PAYLOAD) return null
        if (h.payloadSize == 0) return null

        val last = lastDelivered
        if (last != null && ((h.frameId - last).toInt()) <= 0) return null

        var p = pending[h.frameId]
        if (p == null) {
            while (pending.size >= MAX_PENDING) {
                pending.pollFirstEntry()
                framesDropped++
                needsKeyframe = true
            }
            p = Pending(h.packetCount, (h.flags and Dlp.FLAG_KEYFRAME) != 0, h.timestampUs)
            pending[h.frameId] = p
        }
        if (p.packetCount != h.packetCount || p.have[h.packetIndex]) return null

        System.arraycopy(packet, Dlp.HEADER_SIZE, p.data, h.packetIndex * Dlp.MAX_PAYLOAD, h.payloadSize)
        p.have[h.packetIndex] = true
        p.received++
        if (isLast) p.lastPayload = h.payloadSize
        if (p.received != p.packetCount) return null

        val frame = Frame(
            frameId = h.frameId,
            keyframe = p.keyframe,
            timestampUs = p.timestampUs,
            data = p.data,
            size = (p.packetCount - 1) * Dlp.MAX_PAYLOAD + p.lastPayload,
        )

        // Frames anteriores incompletos y huecos de numeración cuentan como pérdidas.
        var lost = 0L
        val it = pending.entries.iterator()
        while (it.hasNext()) {
            val e = it.next()
            if (e.key == h.frameId) {
                it.remove()
            } else if ((e.key - h.frameId).toInt() < 0) {
                lost++
                it.remove()
            }
        }
        if (last != null) {
            val gap = ((h.frameId - last - 1).toInt()).toLong() and 0xFFFFFFFFL
            if (gap > 0) lost = maxOf(lost, gap)
        }
        if (lost > 0) {
            framesDropped += lost
            needsKeyframe = true
        }
        if (frame.keyframe) needsKeyframe = false

        lastDelivered = h.frameId
        framesCompleted++
        return frame
    }

    companion object {
        private const val MAX_PENDING = 4
    }
}
