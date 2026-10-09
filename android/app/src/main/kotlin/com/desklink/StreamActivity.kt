package com.desklink

import android.content.Context
import android.graphics.Color
import android.media.AudioManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.view.Gravity
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.ComposeView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.desklink.ui.DeskLinkTheme
import com.desklink.ui.QuickMenu
import com.desklink.ui.SessionSummary
import com.desklink.ui.StreamOverlay

class StreamActivity : ComponentActivity(), SurfaceHolder.Callback, StreamSession.Listener {
    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var statsView: TextView
    private lateinit var overlay: ComposeView
    private var session: StreamSession? = null
    private var surface: Surface? = null
    private var wifiLock: WifiManager.WifiLock? = null
    @Volatile private var wifiNeeded = true  // false si el host se alcanza por USB o Ethernet
    @Volatile private var statsVisible = false
    private var started = false
    private lateinit var host: String
    private lateinit var settings: AppSettings
    private var helloConfig: Dlp.HelloConfig? = null
    private var lastBackMs = 0L
    private val probe by lazy { DeviceProbe(this) }
    private var touchFallbackShown = false

    // Lo decide el PC (SESSION_CONFIG): la calidad es el perfil base y, si allí está activado «bajar si la batería
    // no aguanta», el gobernador de carga puede pedir uno menor (ver PowerProfile y docs/battery.md).
    private var config: Dlp.SessionConfig? = null
    private var governor = ChargeGovernor(PowerProfile.MAXIMA, false)
    private var profile = PowerProfile.MAXIMA
    @Volatile private var viewOnly = false
    private lateinit var badgeView: TextView
    @Volatile private var streamFps = 0
    private var badgeUntilMs = 0L
    private var warnedDrain = false

    // Estado que pinta la capa de Compose.
    private var phase by mutableStateOf(StreamSession.Phase.CONNECTING)
    private var phaseDetail by mutableStateOf("")
    private var hostName by mutableStateOf("")
    private var menuOpen by mutableStateOf(false)
    private var summary by mutableStateOf(SessionSummary())
    private var statsShown by mutableStateOf(false)

    // Pantalla encendida mientras hay conexión; sin PC durante 2 minutos se deja apagar.
    private val ui = Handler(Looper.getMainLooper())
    private val releaseScreen = Runnable { window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) }

    // Estado táctil: [id, x, y, presión] por dedo apoyado. Se reenvía cada 100 ms mientras haya dedos
    // para que el PC no suelte un toque si se pierde algún paquete, y se repite el "sin dedos" final.
    private val touchState = FloatArray(40)
    private var touchCount = 0
    private var touchIdleRepeats = 0
    private val touchHandler = Handler(Looper.getMainLooper())
    private val touchKeepAlive = object : Runnable {
        override fun run() {
            if (touchCount > 0 || touchIdleRepeats > 0) {
                session?.sendTouch(touchState, touchCount)
                if (touchCount == 0) touchIdleRepeats--
                touchHandler.postDelayed(this, 100)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        host = intent.getStringExtra(EXTRA_HOST) ?: run {
            finish()
            return
        }
        CableLink.sessionActive = true
        hostName = intent.getStringExtra(EXTRA_HOST_NAME) ?: host
        settings = SettingsStore(this).load()
        statsVisible = settings.showStats
        statsShown = statsVisible
        helloConfig = AppSettings.hello(Device.display(this), Device.hevcSupported)

        if (settings.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Los botones de volumen controlan el sonido del PC (multimedia), no el del timbre.
        volumeControlStream = AudioManager.STREAM_MUSIC
        // Toda la pantalla, también la zona de la cámara: el monitor del PC se crea con la resolución completa.
        WindowCompat.setDecorFitsSystemWindows(window, false)
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        statsView = TextView(this).apply {
            setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt())
            textSize = 12f
            setPadding(16, 8, 16, 8)
            visibility = if (statsVisible) View.VISIBLE else View.GONE
        }
        surfaceView.setOnTouchListener { _, event -> onSurfaceTouch(event) }
        overlay = ComposeView(this).apply {
            setContent {
                DeskLinkTheme {
                    if (phase != StreamSession.Phase.STREAMING) {
                        StreamOverlay(phase, phaseDetail, hostName, onRetry = { restartSession() }, onBack = { finish() })
                    } else if (menuOpen) {
                        QuickMenu(
                            summary = summary,
                            statsVisible = statsShown,
                            onInput = { mode -> setTouchMode(mode) },
                            onStats = { toggleStats() },
                            onDisconnect = { finish() },
                            onClose = { setMenu(false) },
                        )
                    }
                }
            }
        }

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
        root.addView(statsView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        badgeView = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 12f
            setPadding(16, 8, 16, 8)
            visibility = View.GONE
        }
        root.addView(badgeView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.END))
        root.addView(overlay, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)
        hideSystemBars()
        applyProfile(profile)

        // Atrás dos veces para desconectar: con la pantalla completa es fácil tocarlo sin querer.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                val now = SystemClock.uptimeMillis()
                if (phase != StreamSession.Phase.STREAMING || now - lastBackMs < 2000) {
                    finish()
                } else {
                    lastBackMs = now
                    Toast.makeText(this@StreamActivity, "Pulsa atrás otra vez para desconectar", Toast.LENGTH_SHORT).show()
                }
            }
        })
    }

    /** Lo que el perfil cambia en la tablet; lo que cambia en el PC se pide con [StreamSession.setBatteryProfile]. */
    private fun applyProfile(p: PowerProfile) {
        val wifiModeChanged = p.lowLatencyWifi != profile.lowLatencyWifi
        profile = p
        val adapt = config?.batteryAdapt == true
        session?.setBatteryProfile(if (adapt && p != governorBase()) p else null)
        // Rendimiento sostenido: frecuencias estables (sin tirones por calor) a costa de no dejar bajar del todo.
        window.setSustainedPerformanceMode(p.sustainedPerformance)
        val lp = window.attributes
        lp.screenBrightness = brightnessFor(p)
        lp.preferredDisplayModeId = if (p.lowRefreshMode && streamFps > 0) lowRefreshModeId(streamFps) else 0
        window.attributes = lp
        if (wifiModeChanged && wifiLock != null) {
            releaseWifiLock()
            if (wifiNeeded && started) acquireWifiLock()
        }
    }

    /** Brillo de la ventana: el tope del perfil si el usuario lo tiene más alto en manual; si no, el suyo. */
    private fun brightnessFor(p: PowerProfile): Float {
        val cap = p.brightnessCap ?: return WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        return try {
            // En automático no se toca: al sol bajaría tanto que no se vería.
            if (Settings.System.getInt(contentResolver, Settings.System.SCREEN_BRIGHTNESS_MODE) ==
                Settings.System.SCREEN_BRIGHTNESS_MODE_AUTOMATIC
            ) {
                return WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
            }
            val level = Settings.System.getInt(contentResolver, Settings.System.SCREEN_BRIGHTNESS) / 255f
            if (level > cap) cap else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        } catch (_: Settings.SettingNotFoundException) {
            WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }

    /** Modo de pantalla de la misma resolución con la menor frecuencia que acomoda [fps] (0 = sin preferencia). */
    @Suppress("DEPRECATION")
    private fun lowRefreshModeId(fps: Int): Int {
        val display = (if (Build.VERSION.SDK_INT >= 30) display else windowManager.defaultDisplay) ?: return 0
        val current = display.mode
        val modes = display.supportedModes.filter {
            it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight
        }
        val rate = PowerProfile.pickRefreshRate(modes.map { it.refreshRate }, fps) ?: return 0
        return modes.first { it.refreshRate == rate }.modeId
    }

    private fun governorBase() = PowerProfile.fromWire(config?.profile ?: 0)

    private fun showBadge(ms: Long) {
        badgeUntilMs = SystemClock.elapsedRealtime() + ms
    }

    private fun setMenu(open: Boolean) {
        menuOpen = open
        overlay.visibility = if (open || phase != StreamSession.Phase.STREAMING) View.VISIBLE else View.GONE
        if (!open) hideSystemBars()
    }

    private fun toggleStats() {
        statsVisible = !statsVisible
        statsShown = statsVisible
        statsView.visibility = if (statsVisible) View.VISIBLE else View.GONE
    }

    /** Táctil (0) o ratón (1) desde el menú rápido: se pide al PC, que lo recuerda en sus ajustes. */
    private fun setTouchMode(mode: Int) {
        session?.setInputMode(if (mode == 1) Dlp.INPUT_MOUSE else Dlp.INPUT_TOUCH)
        summary = summary.copy(input = mode)
    }

    /** Lo que decidió el PC: calidad, sonido, táctil y si va reducida. */
    override fun onSessionConfig(config: Dlp.SessionConfig) {
        runOnUiThread {
            val previous = this.config
            this.config = config
            viewOnly = config.viewOnly
            if (previous == null || previous.profile != config.profile || previous.batteryAdapt != config.batteryAdapt) {
                val base = PowerProfile.fromWire(config.profile)
                governor = ChargeGovernor(base, config.batteryAdapt)
                applyProfile(base)
            }
            val quality = when (config.profile) {
                0 -> "alta"
                1 -> "equilibrada"
                else -> "ahorro"
            }
            val reduced = when (config.reduced) {
                Dlp.REDUCED_WIFI -> "Calidad reducida un momento: el Wi-Fi va justo"
                Dlp.REDUCED_BATTERY -> "Fluidez reducida para no gastar la batería"
                Dlp.REDUCED_LOAD -> "Fluidez reducida: la gráfica del PC va cargada"
                else -> ""
            }
            summary = SessionSummary(
                quality = quality,
                details = "${config.fps} fps · ${"%.0f".format(config.maxBitrateX100kbps / 10.0)} Mbps",
                input = config.input,
                reduced = reduced,
            )
            if (previous == null) {
                badgeView.text = "Calidad $quality · ${config.fps} fps · toca con 4 dedos para el menú"
                showBadge(BADGE_MS)
            }
            updateBadge()
        }
    }

    /** Aviso arriba a la derecha: por qué va reducida, la batería si se descarga, o el resumen de los primeros segundos. */
    private fun updateBadge() {
        val state = governor.state
        val adapt = config?.batteryAdapt == true
        val batteryAlert = adapt && (state == ChargeState.DRAINING || state == ChargeState.ADAPTING)
        val reduced = summary.reduced
        when {
            batteryAlert -> {
                badgeView.text = governor.indicator()
                badgeView.setBackgroundColor(if (state == ChargeState.DRAINING) 0xCCB00020.toInt() else 0xCCA84B00.toInt())
            }
            reduced.isNotEmpty() -> {
                badgeView.text = reduced
                badgeView.setBackgroundColor(0xCCA84B00.toInt())
            }
            else -> badgeView.setBackgroundColor(0x99000000.toInt())
        }
        badgeView.visibility =
            if (batteryAlert || reduced.isNotEmpty() || SystemClock.elapsedRealtime() < badgeUntilMs) View.VISIBLE else View.GONE
    }

    override fun onDestroy() {
        super.onDestroy()
        ui.removeCallbacks(releaseScreen)
        CableLink.sessionActive = false
    }

    private fun onSurfaceTouch(event: MotionEvent): Boolean {
        // Cuatro dedos a la vez: menú rápido (el toque normal va al PC). Se sueltan los dedos que ya iban al PC.
        if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount == 4) {
            if (touchCount > 0) {
                touchCount = 0
                session?.sendTouch(touchState, 0)
            }
            setMenu(true)
            return true
        }
        // «Solo ver» (lo elige el PC): los toques se quedan en la tablet.
        if (viewOnly || menuOpen) return true
        // Cada muestra del táctil sale en cuanto llega, sin esperar a agruparlas con el refresco de pantalla
        // (Android las junta por vsync): unos ms menos de retraso al arrastrar.
        if (event.actionMasked == MotionEvent.ACTION_DOWN) surfaceView.requestUnbufferedDispatch(event)
        val w = surfaceView.width.coerceAtLeast(1).toFloat()
        val h = surfaceView.height.coerceAtLeast(1).toFloat()
        val up = event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL
        val liftedIndex = if (event.actionMasked == MotionEvent.ACTION_POINTER_UP) event.actionIndex else -1
        var n = 0
        if (!up) {
            for (i in 0 until minOf(event.pointerCount, 10)) {
                if (i == liftedIndex) continue
                touchState[4 * n] = event.getPointerId(i).toFloat()
                touchState[4 * n + 1] = event.getX(i) / w
                touchState[4 * n + 2] = event.getY(i) / h
                touchState[4 * n + 3] = event.getPressure(i)
                n++
            }
        }
        touchCount = n
        session?.sendTouch(touchState, n)
        touchHandler.removeCallbacks(touchKeepAlive)
        if (n == 0) touchIdleRepeats = 3
        touchHandler.postDelayed(touchKeepAlive, 100)
        return true
    }

    override fun onStart() {
        super.onStart()
        started = true
        if (wifiNeeded) acquireWifiLock()
    }

    override fun onStop() {
        super.onStop()
        started = false
        releaseWifiLock()
    }

    // Sin ahorro de energía del WiFi: es la mayor fuente de picos de latencia en la red. Por cable no hace
    // falta y mantener la radio WiFi despierta sin usarla solo gasta batería.
    // En los perfiles de ahorro se usa el modo de alto rendimiento, que en muchos aparatos deja dormir la radio
    // entre ráfagas (algo más de latencia, menos consumo).
    private fun acquireWifiLock() {
        if (wifiLock != null) return
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val mode = if (Build.VERSION.SDK_INT >= 29 && profile.lowLatencyWifi) {
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        } else {
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        }
        wifiLock = wifi.createWifiLock(mode, "desklink").also { it.acquire() }
    }

    private fun releaseWifiLock() {
        wifiLock?.takeIf { it.isHeld }?.release()
        wifiLock = null
    }

    override fun onRoute(route: Route?) {
        val needed = route == null || route.transport == Transport.WIFI || route.transport == Transport.OTHER
        runOnUiThread {
            wifiNeeded = needed
            if (!needed) releaseWifiLock() else if (started) acquireWifiLock()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        val c = WindowCompat.getInsetsController(window, window.decorView)
        c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        c.hide(WindowInsetsCompat.Type.systemBars())
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surface = holder.surface
        startSession()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopSession()
        surface = null
    }

    private fun startSession() {
        val s = surface ?: return
        phase = StreamSession.Phase.CONNECTING
        phaseDetail = ""
        overlay.visibility = View.VISIBLE
        session = StreamSession(host, s, this, helloConfig).also { it.start() }
    }

    private fun stopSession() {
        touchHandler.removeCallbacks(touchKeepAlive)
        touchCount = 0
        session?.stop()
        session = null
    }

    private fun restartSession() {
        stopSession()
        startSession()
    }

    override fun onPhase(phase: StreamSession.Phase, detail: String) {
        runOnUiThread {
            this.phase = phase
            phaseDetail = detail
            val streaming = phase == StreamSession.Phase.STREAMING
            if (!streaming) menuOpen = false
            overlay.visibility = if (streaming && !menuOpen) View.GONE else View.VISIBLE
            if (settings.keepScreenOn) {
                ui.removeCallbacks(releaseScreen)
                if (streaming || phase == StreamSession.Phase.CONNECTING) {
                    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                } else {
                    ui.postDelayed(releaseScreen, SCREEN_GRACE_MS)
                }
            }
            if (!streaming) hideSystemBars()
        }
    }

    override fun onStreamInfo(info: Dlp.StreamInfo) {
        runOnUiThread {
            if (info.hostName.isNotBlank()) hostName = info.hostName
            fitSurface(info.width, info.height)
            // La tablet adapta su frecuencia de pantalla a la del stream (menos espera de vsync).
            if (Build.VERSION.SDK_INT >= 30 && info.fps > 0) {
                surfaceView.holder.surface.setFrameRate(info.fps.toFloat(), android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
            }
            // Con los perfiles de ahorro además se fija el modo de pantalla (p. ej. 60 Hz en vez de 144 Hz).
            streamFps = info.fps
            if (profile.lowRefreshMode) applyProfile(profile)
        }
    }

    /** Encaja el vídeo sin deformarlo (bandas negras si el PC manda otra proporción). */
    private fun fitSurface(videoW: Int, videoH: Int) {
        root.post {
            val rw = root.width
            val rh = root.height
            if (rw <= 0 || rh <= 0 || videoW <= 0 || videoH <= 0) return@post
            val scale = minOf(rw.toFloat() / videoW, rh.toFloat() / videoH)
            val w = Math.round(videoW * scale)
            val h = Math.round(videoH * scale)
            // Diferencias de un par de píxeles: mejor llenar la pantalla.
            val lp = if (rw - w <= 4 && rh - h <= 4) {
                FrameLayout.LayoutParams(-1, -1, Gravity.CENTER)
            } else {
                FrameLayout.LayoutParams(w, h, Gravity.CENTER)
            }
            surfaceView.layoutParams = lp
        }
    }

    // Llega cada segundo desde la sesión. La batería se mide siempre (es leer dos propiedades del sistema):
    // el modo «adaptar a la carga» la necesita aunque las estadísticas estén ocultas.
    override fun onStats(text: String) {
        val b = probe.sampleBattery()
        runOnUiThread {
            // Solo si en el PC está activado «bajar la calidad si la batería de la tablet no aguanta».
            val adapt = config?.batteryAdapt == true
            val changed = adapt && governor.update(SystemClock.elapsedRealtime(), b.pluggedUsbOrAc, b.netMilliwatts, b.percent)
            if (changed) {
                probe.restartWindow()
                applyProfile(governor.current)
            }
            val state = governor.state
            if (adapt && state == ChargeState.DRAINING && !warnedDrain) {
                warnedDrain = true
                Toast.makeText(
                    this,
                    "La tablet se descarga aunque esté enchufada: el puerto USB da menos de lo que gasta. Usa un " +
                        "puerto USB-C o USB 3 del PC, un cargador de pared con WiFi, o baja el brillo.",
                    Toast.LENGTH_LONG,
                ).show()
            }
            updateBadge()
            if (statsVisible) {
                val level = if (b.percent >= 0) " ${b.percent} %" else ""
                val watts = b.netMilliwatts?.let { " ${"%+.1f".format(it / 1000)} W" } ?: " (midiendo…)"
                val plug = b.plugLabel?.let { ", por $it" } ?: ""
                val source = b.source?.let { " (${it.label})" } ?: ""
                statsView.text = "$text · batería$level$watts$plug$source · perfil ${profile.summary()}"
            }
        }
    }

    override fun onInputMode(state: Dlp.InputModeState) {
        runOnUiThread { if (summary.input != Dlp.INPUT_VIEW_ONLY) summary = summary.copy(input = state.active) }
        if (state.requested == Dlp.INPUT_TOUCH && state.active == Dlp.INPUT_MOUSE && !touchFallbackShown) {
            touchFallbackShown = true
            onError("El PC no aceptó el táctil directo: los toques mueven el ratón")
        }
    }

    override fun onError(message: String) {
        runOnUiThread { Toast.makeText(this, message, Toast.LENGTH_LONG).show() }
    }

    companion object {
        const val EXTRA_HOST = "host"
        const val EXTRA_HOST_NAME = "host_name"
        private const val SCREEN_GRACE_MS = 2 * 60 * 1000L
        private const val BADGE_MS = 8_000L
    }
}
