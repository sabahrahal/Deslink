package com.desklink

import android.content.Context
import android.content.SharedPreferences

/** Resolución del monitor virtual respecto a la pantalla de la tablet. */
enum class ResolutionScale(val percent: Int, val label: String, val hint: String) {
    NATIVE(100, "Nativa", "Máxima nitidez"),
    BALANCED(75, "Equilibrada", "Menos trabajo para el PC y la tablet"),
    LIGHT(50, "Ligera", "La que menos batería gasta"),
}

/** Calidad de imagen (Avanzado): tasa máxima de vídeo. 0 = la del perfil. */
enum class Quality(val mbps: Int, val label: String, val hint: String) {
    AUTO(0, "Automática", "La del perfil"),
    ULTRA(80, "Ultra", "80 Mbps · mejor por cable"),
    HIGH(50, "Alta", "50 Mbps"),
    BALANCED(30, "Equilibrada", "30 Mbps · buena por WiFi"),
    SAVER(15, "Ahorro", "15 Mbps · redes lentas"),
}

/** Códec pedido al PC. */
enum class CodecChoice(val wire: Int, val label: String, val hint: String) {
    AUTO(0, "Automático", "HEVC si la tablet lo decodifica"),
    HEVC(Dlp.CODEC_HEVC, "HEVC", "Mejor calidad con la misma tasa"),
    H264(Dlp.CODEC_H264, "H.264", "El más compatible"),
}

/**
 * Perfil: el ajuste principal. Decide fps, Mbps y lo que gasta la tablet (PowerProfile.kt); aquí solo se elige.
 * [id] es el valor guardado en la clave "power_profile".
 */
enum class PowerChoice(val id: String, val label: String, val hint: String) {
    MAX("maxima", "Calidad", "La mejor imagen · gasta más"),
    BALANCED("equilibrado", "Equilibrado", "Recomendado · no se nota el ahorro"),
    SAVER("ahorro", "Ahorro", "Dura más la batería · 30 fps"),
}

/** Qué hace el toque en la tablet. [wire] es el modo que se pide al PC (INPUT_MODE); null = no se manda nada. */
enum class TouchChoice(val wire: Int?, val label: String, val hint: String) {
    DIRECT(Dlp.INPUT_TOUCH, "Táctil", "Tocar y arrastrar como en el móvil"),
    MOUSE(Dlp.INPUT_MOUSE, "Ratón", "El dedo mueve el puntero"),
    OFF(null, "Solo ver", "Los toques no llegan al PC"),
}

/**
 * Ajustes de DeskLink. La tablet es el único sitio donde se eligen: al conectar se mandan al PC (imagen en el
 * HELLO con [toHelloConfig], perfil en QUALITY_REQUEST con [qualityRequest], sonido en AUDIO_CONTROL y táctil
 * en INPUT_MODE); la app del PC solo pone lo que la tablet no puede saber (qué monitor, cable USB).
 * Para añadir un ajuste: un campo aquí con su valor por defecto, su clave en [SettingsStore] y una fila en
 * SettingsScreen: arriba solo si casi todo el mundo lo va a tocar; si no, en «Avanzado».
 */
data class AppSettings(
    val resolution: ResolutionScale = ResolutionScale.NATIVE,
    val quality: Quality = Quality.AUTO,
    /** 0 = la del PC. */
    val fps: Int = 0,
    val codec: CodecChoice = CodecChoice.AUTO,
    val showStats: Boolean = false,
    val keepScreenOn: Boolean = true,
    val autoConnect: Boolean = true,
    val power: PowerChoice = PowerChoice.BALANCED,
    /** Por cable USB, no gastar más de lo que entra por el cable. */
    val powerAdapt: Boolean = true,
    /** Sonido del PC en la tablet (por defecto apagado: el PC lo sigue oyendo por sus altavoces). */
    val audio: Boolean = false,
    val touch: TouchChoice = TouchChoice.DIRECT,
) {
    /** Hay algo fijado en «Avanzado» que manda sobre el perfil. */
    val hasAdvancedOverrides: Boolean
        get() = resolution != ResolutionScale.NATIVE || quality != Quality.AUTO || fps != 0 || codec != CodecChoice.AUTO

    /**
     * QUALITY_REQUEST para el perfil en uso [current]. Un fps o una calidad fijados en Avanzado mandan sobre
     * el perfil elegido (se quita ese tope y vale lo del HELLO); pero si «adaptar a la carga» bajó a un perfil
     * menor que el elegido, sus topes vuelven: no descargar la batería por cable va primero.
     */
    fun qualityRequest(current: PowerProfile, timestampUs: Long): ByteArray {
        val adapted = current.ordinal > PowerProfile.fromId(power.id).ordinal
        return Dlp.qualityRequest(current, timestampUs, liftFps = fps != 0 && !adapted, liftBitrate = quality != Quality.AUTO && !adapted)
    }

    /**
     * Lo que se pide al PC para [display]. [hevcSupported]: si la tablet no decodifica HEVC, "Automático"
     * pide H.264 para no recibir un vídeo que no puede mostrar.
     */
    fun toHelloConfig(display: DisplaySpec, hevcSupported: Boolean): Dlp.HelloConfig {
        val (w, h) = display.scaled(resolution.percent)
        val codecWire = when {
            codec == CodecChoice.AUTO && !hevcSupported -> Dlp.CODEC_H264
            codec == CodecChoice.HEVC && !hevcSupported -> Dlp.CODEC_H264
            else -> codec.wire
        }
        return Dlp.HelloConfig(
            width = w,
            height = h,
            fps = fps,
            bitrateMbps = quality.mbps,
            codec = codecWire,
            densityDpi = display.densityDpi,
            refreshHz = display.refreshHz,
        )
    }

    companion object {
        /** Opciones de fluidez que tienen sentido para una pantalla de [refreshHz] (0 = automática). */
        fun fpsOptions(refreshHz: Int): List<Int> =
            listOf(0) + listOf(30, 60, 90, 120, 144).filter { it <= 60 || it <= refreshHz + 1 }
    }
}

/** Pantalla de la tablet en horizontal (como se usa): lado largo x lado corto, en píxeles reales. */
data class DisplaySpec(val width: Int, val height: Int, val densityDpi: Int = 0, val refreshHz: Int = 60) {
    /**
     * Tamaño pedido al PC a [percent] % de la pantalla, en números pares y dentro de lo que acepta el host
     * (640x480 a 7680x4320). Si la escala deja un lado por debajo del mínimo se sube la escala, sin deformar.
     */
    fun scaled(percent: Int): Pair<Int, Int> {
        if (width <= 0 || height <= 0) return 0 to 0
        var scale = percent.coerceIn(10, 100) / 100.0
        scale = maxOf(scale, MIN_W.toDouble() / width, MIN_H.toDouble() / height)
        scale = minOf(scale, MAX_W.toDouble() / width, MAX_H.toDouble() / height)
        val w = (width * scale).toInt() and 1.inv()
        val h = (height * scale).toInt() and 1.inv()
        return w.coerceIn(MIN_W, MAX_W) to h.coerceIn(MIN_H, MAX_H)
    }

    companion object {
        const val MIN_W = 640
        const val MIN_H = 480
        const val MAX_W = 7680
        const val MAX_H = 4320

        /** Ordena a horizontal: el lado largo es el ancho. */
        fun landscape(a: Int, b: Int, densityDpi: Int, refreshHz: Int) =
            DisplaySpec(maxOf(a, b), minOf(a, b), densityDpi, refreshHz)
    }
}

/** Guarda los ajustes y el último PC en SharedPreferences. */
class SettingsStore(context: Context) {
    private val prefs: SharedPreferences = context.getSharedPreferences("desklink", Context.MODE_PRIVATE)

    fun load(): AppSettings {
        val d = AppSettings()
        return AppSettings(
            resolution = enumOr(prefs.getString(K_RESOLUTION, null), d.resolution),
            quality = enumOr(prefs.getString(K_QUALITY, null), d.quality),
            fps = prefs.getInt(K_FPS, d.fps),
            codec = enumOr(prefs.getString(K_CODEC, null), d.codec),
            showStats = prefs.getBoolean(K_STATS, d.showStats),
            keepScreenOn = prefs.getBoolean(K_SCREEN_ON, d.keepScreenOn),
            autoConnect = prefs.getBoolean(K_AUTO, d.autoConnect),
            power = PowerChoice.entries.firstOrNull { it.id == prefs.getString(K_POWER, null) } ?: d.power,
            powerAdapt = prefs.getBoolean(K_POWER_ADAPT, d.powerAdapt),
            audio = prefs.getBoolean(K_AUDIO, d.audio),
            touch = enumOr(prefs.getString(K_TOUCH, null), d.touch),
        )
    }

    fun save(s: AppSettings) {
        prefs.edit()
            .putString(K_RESOLUTION, s.resolution.name)
            .putString(K_QUALITY, s.quality.name)
            .putInt(K_FPS, s.fps)
            .putString(K_CODEC, s.codec.name)
            .putBoolean(K_STATS, s.showStats)
            .putBoolean(K_SCREEN_ON, s.keepScreenOn)
            .putBoolean(K_AUTO, s.autoConnect)
            .putString(K_POWER, s.power.id)
            .putBoolean(K_POWER_ADAPT, s.powerAdapt)
            .putBoolean(K_AUDIO, s.audio)
            .putString(K_TOUCH, s.touch.name)
            .apply()
    }

    /** Último PC al que se conectó: IP o [Link.CABLE_HOST]. */
    var lastHost: String?
        get() = prefs.getString(K_HOST, null)
        set(v) = prefs.edit().putString(K_HOST, v).apply()

    var lastHostName: String?
        get() = prefs.getString(K_HOST_NAME, null)
        set(v) = prefs.edit().putString(K_HOST_NAME, v).apply()

    private inline fun <reified E : Enum<E>> enumOr(name: String?, default: E): E =
        enumValues<E>().firstOrNull { it.name == name } ?: default

    private companion object {
        const val K_HOST = "host" // la misma clave que usaban las versiones anteriores
        const val K_HOST_NAME = "host_name"
        const val K_RESOLUTION = "resolution"
        const val K_QUALITY = "quality"
        const val K_FPS = "fps"
        const val K_CODEC = "codec"
        const val K_STATS = "show_stats"
        const val K_SCREEN_ON = "keep_screen_on"
        const val K_AUTO = "auto_connect"
        const val K_POWER = "power_profile" // las mismas que PowerProfile.PREF_KEY / PREF_ADAPT
        const val K_POWER_ADAPT = "power_adapt"
        const val K_AUDIO = "audio_enabled"
        const val K_TOUCH = "touch_mode"
    }
}
