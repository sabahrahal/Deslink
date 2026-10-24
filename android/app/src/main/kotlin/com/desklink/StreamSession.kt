package com.desklink

import android.os.Process
import android.view.Surface
import java.io.IOException
import java.util.concurrent.Executors

/**
 * Sesión de recepción: envía HELLO al host, recibe vídeo (UDP o cable USB, ver [Link]), lo reensambla y lo
 * entrega al decodificador. Sin colas: un frame que no cabe en el decodificador se descarta
 * y se pide un frame clave.
 */
class StreamSession(
    private val host: String,
    private val surface: Surface,
    private val listener: Listener,
    /** Lo que la tablet dice de sí misma en cada HELLO (pantalla, frecuencia, HEVC); null = HELLO vacío. */
    private val helloConfig: Dlp.HelloConfig? = null,
    /** Volumen multimedia de la tablet («5/15»), para el informe de sonido del log del PC. */
    private val mediaVolume: () -> String = { "?" },
    /** Cada segundo con vídeo: codec, tamaño y lo que tardó de media en decodificarse cada frame (para [CodecMeter]). */
    private val onDecodeSample: (codec: Int, width: Int, height: Int, decodeMs: Double) -> Unit = { _, _, _, _ -> },
) {
    /** En qué punto está la conexión, para la pantalla de estado. */
    enum class Phase { CONNECTING, NO_ANSWER, STREAMING, RECONNECTING, FAILED }

    interface Listener {
        fun onStreamInfo(info: Dlp.StreamInfo)
        fun onStats(text: String)
        fun onError(message: String)
        /** Cambio de estado; [detail] explica qué pasa y qué puede hacer el usuario. */
        fun onPhase(phase: Phase, detail: String) = Unit
        /** Interfaz por la que se llega al host (null si no se supo). */
        fun onRoute(route: Route?) = Unit
        /** Modo del táctil en el PC (ver [Dlp.InputModeState]); llega al conectar y cada vez que cambia. */
        fun onInputMode(state: Dlp.InputModeState) = Unit
        /** Lo que decidió el PC (calidad, sonido, táctil…); llega al conectar y cuando cambia. */
        fun onSessionConfig(config: Dlp.SessionConfig) = Unit
    }

    @Volatile private var phase: Phase? = null
    private fun setPhase(p: Phase, detail: String = "") {
        if (phase == p && p != Phase.FAILED) return
        phase = p
        listener.onPhase(p, detail)
    }
    private val isCable = host.equals(Link.CABLE_HOST, ignoreCase = true)
    private fun hello() = Dlp.hello(helloConfig, nowUs())

    @Volatile private var running = false
    @Volatile private var decoder: VideoDecoder? = null
    @Volatile private var streamInfo: Dlp.StreamInfo? = null
    @Volatile private var rttUs = -1L
    @Volatile private var routeLabel = ""
    @Volatile private var lastRxUs = 0L
    @Volatile var inputMode: Dlp.InputModeState? = null
        private set
    /** Perfil que pide la tablet para no gastar batería; null = no pide nada (lo que decida el PC). */
    @Volatile private var batteryProfile: PowerProfile? = null
    /** Decodificador a velocidad máxima (lo decide el perfil de batería de la tablet, ver [setFastDecode]). */
    @Volatile private var fastDecode = true
    @Volatile var sessionConfig: Dlp.SessionConfig? = null
        private set

    @Volatile private var link: Link? = null
    @Volatile private var audio: AudioPlayer? = null
    private var receiveThread: Thread? = null
    private var controlThread: Thread? = null
    private val reassembler = FrameReassembler()
    /** Reloj del PC y retardo de punta a punta por etapas (ver [LatencyStats]). */
    private val clock = ClockSync()
    private val latency = LatencyStats(clock)

    private val touchExecutor = Executors.newSingleThreadExecutor { Thread(it, "desklink-touch") }
    private var bytesReceived = 0L
    private var framesReceived = 0L
    private var decoderDrops = 0L
    private var lastKeyframeRequestUs = 0L

    fun start() {
        running = true
        setPhase(Phase.CONNECTING)
        receiveThread = Thread({ receiveLoop() }, "desklink-receive").also { it.start() }
        controlThread = Thread({ controlLoop() }, "desklink-control").also { it.start() }
    }

    fun stop() {
        running = false
        touchExecutor.shutdownNow()
        link?.close()
        receiveThread?.interrupt()
        receiveThread?.join(500)
        controlThread?.interrupt()
        controlThread?.join(500)
        decoder?.release()
        decoder = null
        audio?.stop()
        audio = null
    }

    private fun nowUs() = System.nanoTime() / 1000

    private fun send(packet: ByteArray) {
        link?.send(packet)
    }

    private fun receiveLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        val cable = host.equals(Link.CABLE_HOST, ignoreCase = true)
        val buf = ByteArray(2048)
        var reported = false
        while (running) {
            val l: Link
            try {
                l = Link.open(host)
                routeLabel = l.route?.let { "[${it.transport.label}] " } ?: ""
                listener.onRoute(l.route)
                clock.reset()
                link = l
            } catch (e: IOException) {
                if (!cable) {
                    setPhase(Phase.FAILED, "No se pudo conectar con $host (${e.message}). Revisa la dirección.")
                    return
                }
                // Por cable, el host o adb pueden tardar en volver (p. ej. al reiniciar desklink-host.exe).
                if (!reported) {
                    setPhase(
                        if (streamInfo == null) Phase.NO_ANSWER else Phase.RECONNECTING,
                        "Esperando al PC por el cable USB. Comprueba que Extendesk está abierto en el PC.",
                    )
                }
                reported = true
                if (!sleepWhileRunning(1000)) return
                continue
            }
            if (!running) {
                l.close()
                return
            }
            reported = false
            l.send(hello())
            batteryProfile?.let { l.send(Dlp.qualityRequest(it, nowUs())) }
            while (running) {
                // Con frames a medias se despierta a menudo para reclamar paquetes perdidos y entregar frames;
                // con el escritorio quieto no llega nada y despertar 250 veces por segundo solo gasta batería.
                // Por cable no se pierde nada: basta con despertar de vez en cuando.
                val waitMs = when {
                    l is CableLink -> CABLE_TIMEOUT_MS
                    reassembler.hasPending -> BUSY_TIMEOUT_MS
                    else -> IDLE_TIMEOUT_MS
                }
                val length = try {
                    l.receive(buf, waitMs)
                } catch (e: IOException) {
                    if (running && !cable) setPhase(Phase.FAILED, "Se perdió la conexión: ${e.message}")
                    break
                }
                if (length < 0) {
                    pump()
                    continue
                }
                val header = Dlp.parseHeader(buf, length) ?: continue
                lastRxUs = nowUs()
                if (streamInfo != null && phase != Phase.STREAMING) setPhase(Phase.STREAMING)
                when (header.type) {
                    Dlp.TYPE_VIDEO, Dlp.TYPE_VIDEO_FEC -> onVideo(buf, length)
                    Dlp.TYPE_STREAM_INFO -> onStreamInfo(buf, header)
                    Dlp.TYPE_PONG -> {
                        val now = nowUs()
                        rttUs = now - header.timestampUs
                        // Un host nuevo devuelve también su hora: así se sabe cuánto tarda cada frame de punta a punta.
                        Dlp.parsePongHostUs(buf, header)?.let { clock.onPong(header.timestampUs, it, now) }
                    }
                    // El host también mide el retardo (bandeja de Windows): se le devuelve su marca de tiempo.
                    Dlp.TYPE_PING -> send(Dlp.controlPacket(Dlp.TYPE_PONG, header.timestampUs))
                    Dlp.TYPE_INPUT_MODE -> Dlp.parseInputMode(buf, header)?.let {
                        inputMode = it
                        listener.onInputMode(it)
                    }
                    Dlp.TYPE_AUDIO -> onAudio(buf, header, cable)
                    Dlp.TYPE_SESSION_CONFIG -> Dlp.parseSessionConfig(buf, header)?.let { onSessionConfig(it) }
                }
            }
            l.close()
            if (!cable || !running) return
            // El cable se cortó (host cerrado, cable desenchufado): se reintenta solo.
            setPhase(Phase.RECONNECTING, "Se cortó el cable USB o se cerró Extendesk en el PC. Reconectando…")
            if (!sleepWhileRunning(1000)) return
        }
    }

    private fun sleepWhileRunning(ms: Long): Boolean {
        try {
            Thread.sleep(ms)
        } catch (_: InterruptedException) {
            return false
        }
        return running
    }

    /**
     * Pide al PC bajar a [p] porque la batería no aguanta (null = lo que decida el PC). El PC solo lo atiende si allí
     * está activado; se manda en el acto y se repite cada pocos segundos.
     */
    fun setBatteryProfile(p: PowerProfile?) {
        if (p == batteryProfile) return
        batteryProfile = p
        val request = p ?: return
        // Android no deja enviar por red desde el hilo de la interfaz.
        try {
            touchExecutor.execute { send(Dlp.qualityRequest(request, nowUs())) }
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            // Sesión ya parada.
        }
    }

    /** Decodificador a velocidad máxima o a la justa para los fps (perfiles de ahorro). */
    fun setFastDecode(fast: Boolean) {
        if (fast == fastDecode) return
        fastDecode = fast
        decoder?.setPace(fast, streamInfo?.fps ?: 0)
    }

    private fun onSessionConfig(config: Dlp.SessionConfig) {
        val previous = sessionConfig
        sessionConfig = config
        if (!config.audioOnTablet) {
            audio?.stop()
            audio = null
        }
        if (config != previous) listener.onSessionConfig(config)
    }

    private fun onStreamInfo(buf: ByteArray, header: Dlp.Header) {
        val info = Dlp.parseStreamInfo(buf, header) ?: return
        val current = streamInfo
        if (current != null && current.width == info.width && current.height == info.height &&
            current.codec == info.codec
        ) {
            // Solo cambian los fps (perfil de batería): el decodificador sigue, sin esperar a otro frame clave.
            if (current.fps != info.fps) {
                decoder?.setPace(fastDecode, info.fps)
                streamInfo = info
                listener.onStreamInfo(info)
            }
            return
        }
        try {
            decoder?.release()
            decoder = VideoDecoder(surface, info, latency, fastDecode) { listener.onError(it) }
            streamInfo = info
            listener.onStreamInfo(info)
            setPhase(Phase.STREAMING)
        } catch (e: Exception) {
            setPhase(Phase.FAILED, "Este dispositivo no pudo abrir el vídeo (${e.message}). Prueba el códec H.264 en la Configuración avanzada del PC.")
        }
    }

    private fun onAudio(buf: ByteArray, header: Dlp.Header, cable: Boolean) {
        // El PC solo manda sonido si eligió que suene en la tablet.
        if (!running || sessionConfig?.audioOnTablet == false) return
        val player = audio ?: AudioPlayer(cable).also {
            it.start()
            audio = it
        }
        for (frame in Dlp.parseAudio(buf, header)) player.push(frame)
    }

    private fun onVideo(buf: ByteArray, length: Int) {
        val dec = decoder ?: return
        bytesReceived += length
        val now = nowUs()
        var frame = reassembler.push(buf, length, now)
        while (frame != null) {
            deliver(dec, frame)
            frame = reassembler.poll(now)
        }
        afterPoll(now)
    }

    /** Sin paquetes nuevos también hay que reclamar los perdidos y soltar frames que esperaron demasiado. */
    private fun pump() {
        val dec = decoder ?: return
        val now = nowUs()
        var frame = reassembler.poll(now)
        while (frame != null) {
            deliver(dec, frame)
            frame = reassembler.poll(now)
        }
        afterPoll(now)
    }

    private fun deliver(dec: VideoDecoder, frame: Frame) {
        framesReceived++
        // Mientras falte un frame clave, los frames intermedios no se pueden decodificar.
        if (!reassembler.needsKeyframe || frame.keyframe) {
            if (!dec.queue(frame)) {
                decoderDrops++
                requestKeyframe()
            }
        }
    }

    private fun afterPoll(now: Long) {
        // Reintento a ~2 RTT (mínimo 4 ms); la espera máxima de un frame crece con el RTT.
        val rtt = rttUs.takeIf { it > 0 } ?: 2_000L
        reassembler.graceUs = (4 * rtt + 8_000).coerceIn(15_000L, 60_000L)
        for (n in reassembler.collectNacks(now, retryUs = maxOf(2 * rtt, 4_000L))) {
            send(Dlp.nack(n.frameId, n.indices, n.count, now))
        }
        if (reassembler.needsKeyframe) requestKeyframe()
    }

    /** Manda el estado táctil completo (ver [Dlp.touch]); se llama desde el hilo de la interfaz. */
    fun sendTouch(state: FloatArray, n: Int) {
        val packet = Dlp.touch(state, n, nowUs())
        // Android no permite enviar por red desde el hilo de la interfaz.
        touchExecutor.execute { send(packet) }
    }

    /** Manda el estado del lápiz (ver [Dlp.pen]); se llama desde el hilo de la interfaz. */
    fun sendPen(flags: Int, x: Float, y: Float, pressure: Float, tiltX: Int, tiltY: Int) {
        val packet = Dlp.pen(flags, x, y, pressure, tiltX, tiltY, nowUs())
        touchExecutor.execute { send(packet) }
    }

    /** Cambia el táctil del PC entre toques reales ([Dlp.INPUT_TOUCH]) y ratón ([Dlp.INPUT_MOUSE]). */
    fun setInputMode(mode: Int) {
        val packet = Dlp.inputMode(mode, nowUs())
        touchExecutor.execute { send(packet) }
    }

    private fun requestKeyframe() {
        val now = nowUs()
        if (now - lastKeyframeRequestUs < 500_000) return
        lastKeyframeRequestUs = now
        send(Dlp.controlPacket(Dlp.TYPE_KEYFRAME_REQUEST, now))
    }

    private companion object {
        const val BUSY_TIMEOUT_MS = 4
        const val IDLE_TIMEOUT_MS = 50
        const val CABLE_TIMEOUT_MS = 100
    }

    /** Cada segundo: HELLO hasta que el host responda, PING para medir RTT y estadísticas. */
    private fun controlLoop() {
        var lastBytes = 0L
        var lastFrames = 0L
        var lastDecoded = 0L
        var lastDecodeNanos = 0L
        var lastDecodeCount = 0L
        var infoTicks = 0
        var lastLost = 0L
        var lastTime = nowUs()
        var helloAttempts = 0
        var ticks = 0
        while (running) {
            val info = streamInfo
            // Si el host dejó de oírnos (p. ej. un corte de red) nos habrá olvidado: un HELLO lo reengancha.
            val silent = info != null && nowUs() - lastRxUs > 3_000_000
            send(if (info == null || silent) hello() else Dlp.controlPacket(Dlp.TYPE_PING, nowUs()))
            // La petición de batería se repite cada 5 s por si se perdió o el PC se reinició.
            if (info == null || silent || ticks++ % 5 == 0) {
                batteryProfile?.let { send(Dlp.qualityRequest(it, nowUs())) }
            }
            if (silent && phase == Phase.STREAMING) {
                setPhase(Phase.RECONNECTING, "El PC no responde. Reintentando…")
            }
            try {
                Thread.sleep(1000)
            } catch (_: InterruptedException) {
                break
            }
            if (info == null) {
                helloAttempts++
                // El PC puede tardar unos segundos en preparar el monitor virtual a nuestra medida.
                if (helloAttempts >= 8 && phase == Phase.CONNECTING) {
                    setPhase(
                        Phase.NO_ANSWER,
                        if (isCable) {
                            "El PC no responde por el cable USB. Comprueba que Extendesk está abierto en el PC " +
                                "y que la depuración USB está aceptada."
                        } else {
                            "El PC ($host) no responde. Comprueba que Extendesk está abierto en el PC, que el " +
                                "cortafuegos de Windows lo permite y que los dos estáis en la misma red."
                        },
                    )
                }
                continue
            }
            val now = nowUs()
            val secs = (now - lastTime) / 1e6
            val decoded = decoder?.decodedFrames ?: 0L
            // Lo que tardó de media cada frame en la tablet (llegada → decodificado) en el último segundo.
            val decodeNs = decoder?.decodeNanos ?: 0L
            val decodeN = decoder?.decodeCount ?: 0L
            val decodeMs = if (decodeN > lastDecodeCount) (decodeNs - lastDecodeNanos) / 1e6 / (decodeN - lastDecodeCount) else -1.0
            if (info != null && decodeMs >= 0 && decodeN - lastDecodeCount >= 10) {
                onDecodeSample(info.codec, info.width, info.height, decodeMs)
            }
            lastDecodeNanos = decodeNs
            lastDecodeCount = decodeN
            val mbps = (bytesReceived - lastBytes) * 8 / secs / 1e6
            val rtt = rttUs
            val lat = latency.take()
            val target = sessionConfig?.let { " · objetivo ${it.fps} fps ${"%.0f".format(it.bitrateX100kbps / 10.0)} Mbps" } ?: ""
            listener.onStats(
                "$routeLabel${info.width}x${info.height}$target · recibidos ${"%.0f".format((framesReceived - lastFrames) / secs)}/s" +
                    " · mostrados ${"%.0f".format((decoded - lastDecoded) / secs)}/s" +
                    " · ${"%.1f".format(mbps)} Mbps" +
                    (if (rtt >= 0) " · RTT ${"%.1f".format(rtt / 1000.0)} ms" else "") +
                    (if (lat.frames > 0) " · ${lat.text()}" else if (decodeMs >= 0) " · decodificar ${"%.1f".format(decodeMs)} ms" else "") +
                    " · perdidos ${reassembler.framesDropped + decoderDrops}" +
                    " · reclamados ${reassembler.packetsRequested}" +
                    (if (reassembler.packetsRecovered > 0) " · rehechos ${reassembler.packetsRecovered}" else "") +
                    when (inputMode?.active) {
                        Dlp.INPUT_TOUCH -> " · táctil directo"
                        Dlp.INPUT_MOUSE -> " · táctil como ratón"
                        else -> ""
                    } +
                    (audio?.let { " · ${it.statsText()}" } ?: ""),
            )
            val lost = reassembler.framesDropped + decoderDrops
            send(
                Dlp.rateReport(
                    (framesReceived - lastFrames).toInt(), (lost - lastLost).toInt(), (decoded - lastDecoded).toInt(),
                    if (decodeMs >= 0) (decodeMs * 10).toInt() else 0, now, lat.takeIf { it.frames > 0 },
                ),
            )
            // Cada 5 s, qué tablet y qué decodificador, para que salga en el log del PC.
            if (++infoTicks % 5 == 1) {
                decoder?.let { send(Dlp.clientInfo(it.description, now)) }
                audio?.let { send(Dlp.clientInfo(it.report(mediaVolume()), now)) }
            }
            lastLost = lost
            lastBytes = bytesReceived
            lastFrames = framesReceived
            lastDecoded = decoded
            lastTime = now
        }
    }
}
