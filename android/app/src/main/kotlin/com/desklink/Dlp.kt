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
    const val TYPE_INPUT_MODE = 13

    /** Modos del táctil en el PC: toques reales de Windows (sin mover el cursor) o ratón. */
    const val INPUT_TOUCH = 0
    const val INPUT_MOUSE = 1
    const val TYPE_AUDIO = 14
    const val TYPE_AUDIO_CONTROL = 15

    /** Sonido: Opus a 48 kHz estéreo, un trozo cada 10 ms. */
    const val AUDIO_SAMPLE_RATE = 48000
    const val AUDIO_CHANNELS = 2
    const val AUDIO_FRAME_SAMPLES = 480

    const val TYPE_QUALITY_REQUEST = 16
    /** Host -> tablet: lo que el PC decidió para la sesión (ver [SessionConfig]). */
    const val TYPE_SESSION_CONFIG = 17
    const val SESSION_CONFIG_SIZE = 10
    /** Tablet -> host: texto para el log del PC (modelo y decodificador). */
    const val TYPE_CLIENT_INFO = 18

    /** Paridad de un grupo de paquetes de vídeo (dlp::AddFec): payload = u16 XOR de longitudes + XOR de datos. */
    const val TYPE_VIDEO_FEC = 19
    const val MAX_FEC_PAYLOAD = MAX_PAYLOAD + 2
    const val INPUT_VIEW_ONLY = 2

    /** [HelloConfig.codec] en el HELLO: qué puede decodificar la tablet. */
    const val CAN_DECODE_H264_ONLY = 1
    const val CAN_DECODE_HEVC = 2

    /** Por qué la imagen va por debajo de lo elegido en el PC ([SessionConfig.reduced]). */
    const val REDUCED_NONE = 0
    const val REDUCED_WIFI = 1
    const val REDUCED_BATTERY = 2
    const val REDUCED_LOAD = 3

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
        /** Byte 5: cuántos paquetes cubre cada paridad del frame (0 = sin FEC). */
        val fecGroup: Int = 0,
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

    /**
     * Lo que el PC decidió para la sesión (SESSION_CONFIG, ver dlp/session.h): calidad ([profile], el número de
     * [PowerProfile.wire]), sonido en la tablet, «bajar si la batería no aguanta», táctil, por qué va reducida y los
     * fps y la tasa de ahora. Llega al conectar, cada vez que cambia y cada 2 s.
     */
    data class SessionConfig(
        val profile: Int,
        val flags: Int,
        val input: Int,
        val reduced: Int,
        val fps: Int,
        val bitrateX100kbps: Int,
        val maxBitrateX100kbps: Int,
    ) {
        val audioOnTablet: Boolean get() = flags and 1 != 0
        val batteryAdapt: Boolean get() = flags and 2 != 0
        val viewOnly: Boolean get() = input == INPUT_VIEW_ONLY

        fun encode(): ByteArray {
            val b = ByteBuffer.allocate(SESSION_CONFIG_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            b.put(0, profile.toByte())
            b.put(1, flags.toByte())
            b.put(2, input.toByte())
            b.put(3, reduced.toByte())
            b.putShort(4, fps.toShort())
            b.putShort(6, bitrateX100kbps.toShort())
            b.putShort(8, maxBitrateX100kbps.toShort())
            return b.array()
        }
    }

    /** Null si el payload es corto. Valores desconocidos se acotan como en el PC. */
    fun parseSessionConfig(data: ByteArray, header: Header): SessionConfig? {
        if (header.type != TYPE_SESSION_CONFIG || header.payloadSize < SESSION_CONFIG_SIZE) return null
        val o = HEADER_SIZE
        fun u8(i: Int) = data[o + i].toInt() and 0xFF
        fun u16(i: Int) = u8(i) or (u8(i + 1) shl 8)
        return SessionConfig(
            profile = u8(0).takeIf { it <= 3 } ?: 1,
            flags = u8(1),
            input = u8(2).takeIf { it <= INPUT_VIEW_ONLY } ?: INPUT_TOUCH,
            reduced = u8(3).takeIf { it <= REDUCED_LOAD } ?: REDUCED_NONE,
            fps = u16(4),
            bitrateX100kbps = u16(6),
            maxBitrateX100kbps = u16(8),
        )
    }

    /** Un trozo Opus de 10 ms; [seq] cuenta trozos desde que el host empezó a enviar sonido. */
    class AudioFrame(val seq: Long, val data: ByteArray)

    /** Devuelve null si el paquete no es válido (corto, magic/versión distintos o payload que no cabe). */
    fun parseHeader(data: ByteArray, length: Int): Header? {
        if (length < HEADER_SIZE) return null
        val b = ByteBuffer.wrap(data, 0, length).order(ByteOrder.LITTLE_ENDIAN)
        if ((b.getShort(0).toInt() and 0xFFFF) != MAGIC || (data[2].toInt() and 0xFF) != VERSION) return null
        val payload = b.getShort(22).toInt() and 0xFFFF
        val type = data[3].toInt() and 0xFF
        if (payload > (if (type == TYPE_VIDEO_FEC) MAX_FEC_PAYLOAD else MAX_PAYLOAD) || length < HEADER_SIZE + payload) return null
        return Header(
            type = type,
            flags = data[4].toInt() and 0xFF,
            frameId = b.getInt(6).toLong() and 0xFFFFFFFFL,
            packetIndex = b.getShort(10).toInt() and 0xFFFF,
            packetCount = b.getShort(12).toInt() and 0xFFFF,
            timestampUs = b.getLong(14),
            payloadSize = payload,
            fecGroup = data[5].toInt() and 0xFF,
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

    /** CLIENT_INFO: [text] en UTF-8, recortado para que quepa en un paquete. */
    fun clientInfo(text: String, timestampUs: Long): ByteArray {
        val bytes = text.toByteArray(Charsets.UTF_8)
        return controlPacket(TYPE_CLIENT_INFO, timestampUs, if (bytes.size > 1176) bytes.copyOf(1176) else bytes)
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

    /**
     * Informe de calidad: frames recibidos, perdidos y mostrados (los que sacó el decodificador) en el último
     * segundo, y lo que tardó de media cada frame desde que llegó entero hasta salir del decodificador, en décimas de
     * ms. Con [latency] añade el retardo por etapas ([LatencyStats]). Un host antiguo lee solo los dos primeros campos.
     */
    fun rateReport(
        received: Int,
        lost: Int,
        decoded: Int,
        decodeMsX10: Int,
        timestampUs: Long,
        latency: LatencyStats.Snapshot? = null,
    ): ByteArray {
        val size = if (latency != null) RATE_REPORT_LATENCY_SIZE else 8
        val b = ByteBuffer.allocate(HEADER_SIZE + size).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_RATE_REPORT.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, size.toShort())
        b.putShort(HEADER_SIZE, received.coerceIn(0, 0xFFFF).toShort())
        b.putShort(HEADER_SIZE + 2, lost.coerceIn(0, 0xFFFF).toShort())
        b.putShort(HEADER_SIZE + 4, decoded.coerceIn(0, 0xFFFF).toShort())
        b.putShort(HEADER_SIZE + 6, decodeMsX10.coerceIn(0, 0xFFFF).toShort())
        if (latency != null) {
            // Retardo por etapas en décimas de ms; 0 = no se sabe (sin hora del PC o sin aviso de pantalla).
            fun x10(us: Long) = if (us < 0) 0 else (us / 100).coerceIn(1, 0xFFFF)
            b.putShort(HEADER_SIZE + 8, x10(latency.totalUs).toInt().toShort())
            b.putShort(HEADER_SIZE + 10, x10(latency.arriveUs).toInt().toShort())
            b.putShort(HEADER_SIZE + 12, x10(latency.holdUs).toInt().toShort())
            b.putShort(HEADER_SIZE + 14, x10(latency.screenUs).toInt().toShort())
            b.putShort(HEADER_SIZE + 16, x10(if (latency.hasTotal) latency.maxTotalUs else -1L).toInt().toShort())
        }
        return b.array()
    }

    /** RATE_REPORT con el retardo por etapas: los 8 bytes de siempre más 5 u16 (ver docs/protocol-dlp.md). */
    const val RATE_REPORT_LATENCY_SIZE = 18

    /**
     * Hora del PC (su reloj, µs) que trae el PONG de un host nuevo, para [ClockSync]; null si el host es antiguo y el
     * PONG viene vacío.
     */
    fun parsePongHostUs(data: ByteArray, header: Header): Long? {
        if (header.payloadSize < 8) return null
        return ByteBuffer.wrap(data, HEADER_SIZE, 8).order(ByteOrder.LITTLE_ENDIAN).getLong()
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

    /** Pide al host un modo de táctil ([INPUT_TOUCH] o [INPUT_MOUSE]); el host lo recuerda. */
    fun inputMode(mode: Int, timestampUs: Long): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE + 1).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_INPUT_MODE.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, 1)
        b.put(HEADER_SIZE, mode.toByte())
        return b.array()
    }

    /** Modo de táctil que respondió el host: el pedido y el que usa (ratón si Windows no aceptó los toques). */
    class InputModeState(val requested: Int, val active: Int)

    fun parseInputMode(data: ByteArray, header: Header): InputModeState? {
        if (header.type != TYPE_INPUT_MODE || header.payloadSize < 2) return null
        return InputModeState(data[HEADER_SIZE].toInt() and 0xFF, data[HEADER_SIZE + 1].toInt() and 0xFF)
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

    /**
     * Límites del perfil de batería: fps máx. u16, Mbps máx. en unidades de 100 kbps u16, escala % u8, perfil u8.
     * 0 = sin límite. El PC solo lo tiene en cuenta si allí está activado «bajar la calidad si la batería de la
     * tablet no aguanta», y nunca baja por él la resolución (ver docs/battery.md).
     */
    fun qualityRequest(profile: PowerProfile, timestampUs: Long): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE + 6).order(ByteOrder.LITTLE_ENDIAN)
        b.putShort(0, MAGIC.toShort())
        b.put(2, VERSION.toByte())
        b.put(3, TYPE_QUALITY_REQUEST.toByte())
        b.putShort(12, 1)
        b.putLong(14, timestampUs)
        b.putShort(22, 6)
        b.putShort(HEADER_SIZE, profile.maxFps.toShort())
        b.putShort(HEADER_SIZE + 2, (profile.maxBitrateMbps * 10).toShort())
        b.put(HEADER_SIZE + 4, profile.scalePercent.toByte())
        b.put(HEADER_SIZE + 5, profile.wire.toByte())
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
