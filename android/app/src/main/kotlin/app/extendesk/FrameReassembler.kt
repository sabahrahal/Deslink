package app.extendesk

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
 *
 * Por WiFi el host manda además una paridad por cada grupo de paquetes (VIDEO_FEC, dlp::AddFec): si de un grupo falta
 * uno solo, se rehace con ella al momento, sin reclamarlo ni esperar una ida y vuelta.
 */
class FrameReassembler {
    private class Pending(
        val packetCount: Int,
        val keyframe: Boolean,
        val timestampUs: Long,
        val firstSeenUs: Long,
    ) {
        val have = BooleanArray(packetCount)
        /** Paquetes por paridad (0 = el frame va sin FEC) y la paridad de cada grupo que llegó. */
        var fecGroup = 0
        var parity: Array<ByteArray?> = emptyArray()
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
    /** Paquetes rehechos con la paridad, sin reclamarlos. */
    var packetsRecovered = 0L
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
        if (h.type == Dlp.TYPE_VIDEO_FEC) {
            storeParity(h, packet, nowUs)
            return
        }
        if (h.type != Dlp.TYPE_VIDEO) return
        if (h.packetCount == 0 || h.packetIndex >= h.packetCount) return
        val isLast = h.packetIndex + 1 == h.packetCount
        // Todos los paquetes salvo el último van llenos: la posición es index * MAX_PAYLOAD.
        if (!isLast && h.payloadSize != Dlp.MAX_PAYLOAD) return
        if (h.payloadSize == 0) return

        val last = lastDelivered
        if (last != null && ((h.frameId - last).toInt()) <= 0) return

        val p = pendingFor(h, nowUs)
        if (p.packetCount != h.packetCount || p.have[h.packetIndex]) return
        if (h.fecGroup >= 2) p.setFec(h.fecGroup)

        System.arraycopy(packet, Dlp.HEADER_SIZE, p.data, h.packetIndex * Dlp.MAX_PAYLOAD, h.payloadSize)
        p.have[h.packetIndex] = true
        p.received++
        p.lastRxUs = nowUs
        if (h.packetIndex > p.highest) p.highest = h.packetIndex
        if (isLast) p.lastPayload = h.payloadSize
        if (p.fecGroup >= 2) recover(p, h.packetIndex / p.fecGroup, nowUs)
    }

    private fun pendingFor(h: Dlp.Header, nowUs: Long): Pending {
        pending[h.frameId]?.let { return it }
        while (pending.size >= MAX_PENDING) {
            pending.pollFirstEntry()
            framesDropped++
            needsKeyframe = true
        }
        val p = Pending(h.packetCount, (h.flags and Dlp.FLAG_KEYFRAME) != 0, h.timestampUs, nowUs)
        pending[h.frameId] = p
        return p
    }

    private fun Pending.setFec(group: Int) {
        if (fecGroup == group) return
        fecGroup = group
        parity = arrayOfNulls((packetCount + group - 1) / group)
    }

    private fun storeParity(h: Dlp.Header, packet: ByteArray, nowUs: Long) {
        if (h.fecGroup < 2 || h.packetCount == 0 || h.payloadSize < 3) return
        if (h.packetIndex * h.fecGroup >= h.packetCount) return
        val last = lastDelivered
        if (last != null && ((h.frameId - last).toInt()) <= 0) return
        val p = pendingFor(h, nowUs)
        if (p.packetCount != h.packetCount || p.complete) return
        p.setFec(h.fecGroup)
        if (p.parity[h.packetIndex] != null) return
        p.parity[h.packetIndex] = packet.copyOfRange(Dlp.HEADER_SIZE, Dlp.HEADER_SIZE + h.payloadSize)
        recover(p, h.packetIndex, nowUs)
    }

    /** Si al grupo [g] le falta un solo paquete y su paridad llegó, lo rehace. */
    private fun recover(p: Pending, g: Int, nowUs: Long) {
        val par = p.parity.getOrNull(g) ?: return
        val start = g * p.fecGroup
        val end = minOf(start + p.fecGroup, p.packetCount)
        var missing = -1
        for (i in start until end) {
            if (p.have[i]) continue
            if (missing >= 0) return // faltan dos o más: toca reclamarlos
            missing = i
        }
        if (missing < 0) return
        var lenXor = (par[0].toInt() and 0xFF) or ((par[1].toInt() and 0xFF) shl 8)
        val out = ByteArray(Dlp.MAX_PAYLOAD)
        System.arraycopy(par, 2, out, 0, minOf(par.size - 2, Dlp.MAX_PAYLOAD))
        for (i in start until end) {
            if (i == missing) continue
            val len = if (i + 1 == p.packetCount) p.lastPayload else Dlp.MAX_PAYLOAD
            lenXor = lenXor xor len
            val off = i * Dlp.MAX_PAYLOAD
            for (b in 0 until len) out[b] = (out[b].toInt() xor p.data[off + b].toInt()).toByte()
        }
        val isLast = missing + 1 == p.packetCount
        val len = if (isLast) lenXor else Dlp.MAX_PAYLOAD
        if (len <= 0 || len > Dlp.MAX_PAYLOAD) return
        System.arraycopy(out, 0, p.data, missing * Dlp.MAX_PAYLOAD, len)
        p.have[missing] = true
        p.received++
        p.lastRxUs = nowUs
        if (missing > p.highest) p.highest = missing
        if (isLast) p.lastPayload = len
        packetsRecovered++
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
                // Con FEC un hueco se espera a la paridad de su grupo: se reclama si ya llegó (y no bastó) o si ya
                // llegan paquetes del grupo siguiente (la paridad, que va antes, se perdió).
                if (!everything && p.fecGroup >= 2) {
                    val g = i / p.fecGroup
                    if (p.parity[g] == null && p.highest < (g + 1) * p.fecGroup) continue
                }
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
