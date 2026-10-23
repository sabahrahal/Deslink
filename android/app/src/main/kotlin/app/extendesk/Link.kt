package app.extendesk

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * Canal con el host. Los paquetes DLP son los mismos por cualquier camino:
 * - [UdpLink]: WiFi o anclaje USB.
 * - [CableLink]: cable USB sin anclaje, por TCP a 127.0.0.1, que `adb reverse` lleva hasta el PC.
 */
interface Link : Closeable {
    /** Interfaz por la que sale (para mostrarla y para el asesor); null si no se sabe. */
    val route: Route?

    /** Envía un paquete DLP entero. Se puede llamar desde varios hilos. */
    fun send(packet: ByteArray)

    /** Copia el siguiente paquete en [buf] y devuelve su tamaño, o -1 si no llega nada en [timeoutMs]. */
    @Throws(IOException::class)
    fun receive(buf: ByteArray, timeoutMs: Int): Int

    companion object {
        /** Lo que se guarda como "host" cuando la tablet va por el cable USB (no es una IP). */
        const val CABLE_HOST = "usb"

        fun open(host: String): Link = if (host.equals(CABLE_HOST, ignoreCase = true)) CableLink.connect() else UdpLink(host)
    }
}

class UdpLink(host: String) : Link {
    override val route: Route? = Route.forHost(host)
    private val socket: DatagramSocket =
        if (route != null) DatagramSocket(InetSocketAddress(route.local, 0)) else DatagramSocket()
    private val packet = DatagramPacket(ByteArray(0), 0)
    private var timeoutMs = -1

    init {
        try {
            socket.receiveBufferSize = 1 shl 21
            // Toques y teclas a la cola WMM de voz (DSCP 46): son pocos bytes y cada milisegundo se nota.
            try {
                socket.trafficClass = 0xB8
            } catch (_: Exception) {
            }
            socket.connect(InetAddress.getByName(host), Dlp.DEFAULT_PORT)
        } catch (e: IOException) {
            socket.close()
            throw e
        }
    }

    override fun send(packet: ByteArray) {
        try {
            socket.send(DatagramPacket(packet, packet.size))
        } catch (_: IOException) {
        }
    }

    override fun receive(buf: ByteArray, timeoutMs: Int): Int {
        if (timeoutMs != this.timeoutMs) {
            socket.soTimeout = timeoutMs
            this.timeoutMs = timeoutMs
        }
        packet.setData(buf, 0, buf.size)
        return try {
            socket.receive(packet)
            packet.length
        } catch (_: SocketTimeoutException) {
            -1
        }
    }

    override fun close() = socket.close()
}

/**
 * TCP por el cable USB. Cada paquete va precedido de su tamaño (u16, little-endian). Sin pérdidas ni
 * reordenación: no hacen falta reclamaciones ni repartir el envío.
 */
class CableLink private constructor(private val socket: Socket) : Link {
    override val route: Route = Route("adb", InetAddress.getLoopbackAddress(), Transport.CABLE)
    private val input: InputStream = socket.getInputStream()
    private val output: OutputStream = socket.getOutputStream()
    private val framer = Framer()
    private var timeoutMs = -1

    override fun send(packet: ByteArray) {
        try {
            val framed = Framer.frame(packet)
            synchronized(output) { output.write(framed) }
        } catch (_: IOException) {
        }
    }

    override fun receive(buf: ByteArray, timeoutMs: Int): Int {
        if (timeoutMs != this.timeoutMs) {
            socket.soTimeout = timeoutMs
            this.timeoutMs = timeoutMs
        }
        return try {
            framer.next(input, buf)
        } catch (_: SocketTimeoutException) {
            -1
        }
    }

    override fun close() = socket.close()

    companion object {
        /**
         * Hay una sesión en marcha: el host solo admite una conexión por cable y una nueva cortaría la de la
         * sesión, así que [probe] no se conecta.
         */
        @Volatile var sessionActive = false

        fun connect(connectTimeoutMs: Int = 500): CableLink {
            val s = Socket()
            try {
                s.tcpNoDelay = true
                s.receiveBufferSize = 1 shl 21
                s.connect(InetSocketAddress(InetAddress.getLoopbackAddress(), Dlp.DEFAULT_PORT), connectTimeoutMs)
            } catch (e: IOException) {
                s.close()
                throw e
            }
            return CableLink(s)
        }

        /**
         * ¿Está el PC al otro lado del cable? Sin `adb reverse` la conexión se rechaza al momento; con él,
         * adbd la acepta siempre, así que hace falta que el host conteste. Primero DISCOVER (no le hace
         * empezar a enviar vídeo); un host antiguo solo contesta a HELLO.
         */
        fun probe(timeoutMs: Int = 1500): Found? = if (sessionActive) null else try {
            connect().use { link ->
                val buf = ByteArray(2048)
                val start = System.currentTimeMillis()
                link.send(Dlp.controlPacket(Dlp.TYPE_DISCOVER, System.nanoTime() / 1000))
                var helloSent = false
                var found: Found? = null
                while (found == null && System.currentTimeMillis() - start < timeoutMs) {
                    if (!helloSent && System.currentTimeMillis() - start >= 600) {
                        helloSent = true
                        link.send(Dlp.controlPacket(Dlp.TYPE_HELLO, System.nanoTime() / 1000))
                    }
                    val n = link.receive(buf, 100)
                    val h = if (n > 0) Dlp.parseHeader(buf, n) else null
                    if (h != null && h.type == Dlp.TYPE_STREAM_INFO) {
                        found = Found.from(Link.CABLE_HOST, link.route, Dlp.parseStreamInfo(buf, h))
                    }
                }
                found
            }
        } catch (_: IOException) {
            null
        }
    }
}

/**
 * Separa los paquetes del flujo TCP. Un tiempo de espera puede cortar una lectura a medias: lo leído se guarda
 * y la siguiente llamada sigue donde se quedó.
 */
class Framer {
    private val rx = ByteArray(64 * 1024)
    private var start = 0
    private var end = 0

    /** Devuelve el tamaño del paquete copiado en [out]. Lanza IOException si la conexión se cierra o el flujo no es válido. */
    @Throws(IOException::class)
    fun next(input: InputStream, out: ByteArray): Int {
        while (true) {
            val have = end - start
            if (have >= 2) {
                val len = (rx[start].toInt() and 0xFF) or ((rx[start + 1].toInt() and 0xFF) shl 8)
                if (len == 0 || len > Dlp.MAX_PACKET_SIZE || len > out.size) throw IOException("Flujo USB no válido")
                if (have >= 2 + len) {
                    System.arraycopy(rx, start + 2, out, 0, len)
                    start += 2 + len
                    if (start == end) {
                        start = 0
                        end = 0
                    }
                    return len
                }
            }
            if (start > 0 && end == rx.size) {
                System.arraycopy(rx, start, rx, 0, end - start)
                end -= start
                start = 0
            }
            val n = input.read(rx, end, rx.size - end)
            if (n < 0) throw IOException("El PC cerró la conexión USB")
            end += n
        }
    }

    companion object {
        fun frame(packet: ByteArray): ByteArray {
            val out = ByteArray(packet.size + 2)
            out[0] = (packet.size and 0xFF).toByte()
            out[1] = (packet.size shr 8).toByte()
            System.arraycopy(packet, 0, out, 2, packet.size)
            return out
        }
    }
}
