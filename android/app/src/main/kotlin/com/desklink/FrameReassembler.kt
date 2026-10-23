package com.desklink

import java.util.TreeMap

class Frame(
    val frameId: Long,
    val keyframe: Boolean,
    val timestampUs: Long,
    val data: ByteArray,
    val size: Int,
    /** Cuándo llegó su último paquete (reloj de la tablet, µs), para medir el retardo ([LatencyStats]). */
    val completeUs: Long = 0L,
)

/** Paquetes que faltan de un frame, para reclamarlos al host. */
class Nack(val frameId: Long, val indices: IntArray, val count: Int)

/**
 * Reensambla frames a partir de paquetes de vídeo. Sin colas: al completarse un frame se
 * descartan los anteriores que sigan incompletos. Con [graceUs] > 0 un frame completo espera
 * (como mucho ese tiempo) a que se recuperen, vía [collectNacks], los paquetes que faltan en un
 * frame anterior. Cada pérdida no recuperada activa [needsKeyframe] hasta que llegue un frame
 * clave completo. Equivale a dlp::FrameReassembler de protocol/ más reenvíos selectivos.
 */
class FrameReassembler {
    private class Pending(
        val packetCount: Int,
        val keyframe: Boolean,
        val timestampUs: Long,
        val firstSeenUs: Long,
    ) {
        val have = BooleanArray(packetCount)
        val nackedAt = LongArray(packetCount)
        val tries = ByteArray(packetCount)
        val data = ByteArray(packetCount * Dlp.MAX_PAYLOAD)
        var received = 0
        var lastPayload = 0
        var highest = -1
        var lastRxUs = firstSeenUs
        val complete get() = received == packetCount
    }

    private val pending = TreeMap<Long, Pending>()
    private var lastDelivered: Long? = null
    private var droppedSinceDelivery = 0L

    /** Tiempo máximo que un frame completo espera a uno anterior incompleto (0 = no espera). */
    var graceUs = 0L

    var needsKeyframe = true
        private set
    var framesCompleted = 0L
        private set
    var framesDropped = 0L
        private set
    var packetsRequested = 0L
        private set

    /** Hay frames a medias (esperando paquetes o reenvíos): conviene despertar a menudo para reclamarlos. */
    val hasPending: Boolean get() = pending.isNotEmpty()

    /** Procesa un paquete; devuelve un frame listo (llamar a [poll] hasta null para el resto). */
    fun push(packet: ByteArray, length: Int, nowUs: Long = 0L): Frame? {
        store(packet, length, nowUs)
        return poll(nowUs)
    }

    private fun store(packet: ByteArray, length: Int, nowUs: Long) {
        val h = Dlp.parseHeader(packet, length) ?: return
        if (h.type != Dlp.TYPE_VIDEO) return
        if (h.packetCount == 0 || h.packetIndex >= h.packetCount) return
        val isLast = h.packetIndex + 1 == h.packetCount
        // Todos los paquetes salvo el último van llenos: la posición es index * MAX_PAYLOAD.
        if (!isLast && h.payloadSize != Dlp.MAX_PAYLOAD) return
        if (h.payloadSize == 0) return

        val last = lastDelivered
        if (last != null && ((h.frameId - last).toInt()) <= 0) return

        var p = pending[h.frameId]
        if (p == null) {
            while (pending.size >= MAX_PENDING) {
                pending.pollFirstEntry()
                framesDropped++
                needsKeyframe = true
            }
            p = Pending(h.packetCount, (h.flags and Dlp.FLAG_KEYFRAME) != 0, h.timestampUs, nowUs)
            pending[h.frameId] = p
        }
        if (p.packetCount != h.packetCount || p.have[h.packetIndex]) return

        System.arraycopy(packet, Dlp.HEADER_SIZE, p.data, h.packetIndex * Dlp.MAX_PAYLOAD, h.payloadSize)
        p.have[h.packetIndex] = true
        p.received++
        p.lastRxUs = nowUs
        if (h.packetIndex > p.highest) p.highest = h.packetIndex
        if (isLast) p.lastPayload = h.payloadSize
    }

    /** Entrega, en orden, el siguiente frame listo; descarta los incompletos que ya no se esperan. */
    fun poll(nowUs: Long = 0L): Frame? {
        while (true) {
            val first = pending.firstEntry() ?: return null
            val p = first.value
            if (!p.complete) {
                val laterComplete = pending.tailMap(first.key, false).values.any { it.complete }
                if (!laterComplete) return null
                if (graceUs > 0 && nowUs - p.firstSeenUs < graceUs) return null
                pending.pollFirstEntry()
                droppedSinceDelivery++
                framesDropped++
                needsKeyframe = true
                continue
            }
            pending.pollFirstEntry()
            val frame = Frame(
                frameId = first.key,
                keyframe = p.keyframe,
                timestampUs = p.timestampUs,
                data = p.data,
                size = (p.packetCount - 1) * Dlp.MAX_PAYLOAD + p.lastPayload,
                completeUs = p.lastRxUs,
            )
            // Huecos de numeración (frames de los que nunca llegó nada) también cuentan como pérdidas.
            val last = lastDelivered
            if (last != null) {
                val gap = ((first.key - last - 1).toInt()).toLong() and 0xFFFFFFFFL
                if (gap > droppedSinceDelivery) {
                    framesDropped += gap - droppedSinceDelivery
                    needsKeyframe = true
                }
            }
            droppedSinceDelivery = 0
            if (frame.keyframe) needsKeyframe = false
            lastDelivered = first.key
            framesCompleted++
            return frame
        }
    }

    /**
     * Paquetes que faltan y conviene reclamar ya: huecos por debajo del último recibido, o todo lo
     * que falta si ya llegó un frame posterior o el frame lleva [quietUs] sin recibir nada.
     * Cada paquete se reclama como mucho 4 veces, separadas [retryUs].
     */
    fun collectNacks(nowUs: Long, retryUs: Long, quietUs: Long = 3_000): List<Nack> {
        val out = ArrayList<Nack>()
        var budget = 256
        val lastKey = if (pending.isEmpty()) 0L else pending.lastKey()
        for ((frameId, p) in pending) {
            if (p.complete || budget <= 0) continue
            val everything = frameId != lastKey || nowUs - p.lastRxUs >= quietUs
            val limit = if (everything) p.packetCount - 1 else p.highest - 1
            var idx = IntArray(32)
            var n = 0
            for (i in 0..limit) {
                if (p.have[i] || p.tries[i] >= MAX_TRIES || nowUs - p.nackedAt[i] < retryUs) continue
                p.nackedAt[i] = nowUs
                p.tries[i]++
                if (n == idx.size) idx = idx.copyOf(n * 2)
                idx[n++] = i
                if (--budget <= 0) break
            }
            if (n > 0) {
                packetsRequested += n
                var off = 0
                while (off < n) {
                    val c = minOf(128, n - off)
                    out.add(Nack(frameId, idx.copyOfRange(off, off + c), c))
                    off += c
                }
            }
        }
        return out
    }

    companion object {
        private const val MAX_PENDING = 4
        private const val MAX_TRIES = 4
    }
}
