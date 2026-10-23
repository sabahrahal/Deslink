package app.extendesk

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
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
import android.view.ViewConfiguration
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
import app.extendesk.ui.ExtendeskTheme
import app.extendesk.ui.QuickMenu
import app.extendesk.ui.SessionSummary
import app.extendesk.ui.StreamOverlay

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
    private lateinit var codecMeter: CodecMeter
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
    private lateinit var menuButton: TextView
    private var menuX = 0f  // posición del botón ☰ (margen izquierdo y superior, en píxeles)
    private var menuY = 0f
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
        codecMeter = CodecMeter(this)
        helloConfig = AppSettings.hello(Device.display(this), Device.hevcSupported, codecMeter.prefersH264(Device.prefersH264))

        // La pantalla no se apaga mientras se usa como monitor (ya no es una opción: nadie la quiere apagada).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
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
                ExtendeskTheme {
                    if (phase != StreamSession.Phase.STREAMING) {
                        StreamOverlay(phase, phaseDetail, hostName, onRetry = { restartSession() }, onBack = { finish() })
                    } else if (menuOpen) {
                        QuickMenu(
                            summary = summary,
                            statsVisible = statsShown,
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
        menuButton = makeMenuButton()
        val size = dp(MENU_BUTTON_DP)
        root.addView(menuButton, FrameLayout.LayoutParams(size, size, Gravity.TOP or Gravity.START))
        root.addView(overlay, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)
        // Al girar la tablet (o al medir por primera vez) se recoloca en la misma proporción.
        root.addOnLayoutChangeListener { _, l, t, r, b, ol, ot, or_, ob ->
            // Fuera de la pasada de medida: cambia los márgenes del botón, y eso pide otra.
            if (r - l != or_ - ol || b - t != ob - ot) root.post { placeMenuButton() }
        }
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
                    Toast.makeText(this@StreamActivity, getString(R.string.back_again_to_disconnect), Toast.LENGTH_SHORT).show()
                }
            }
        })
    }

    /** Lo que el perfil cambia en la tablet; lo que cambia en el PC se pide con [StreamSession.setBatteryProfile]. */
    private fun applyProfile(p: PowerProfile) {
        val wifiModeChanged = p.lowLatencyWifi != profile.lowLatencyWifi
        profile = p
        val adapt = config?.batteryAdapt == true
        // También el perfil del PC al volver a él: si no, el PC se quedaba con la última petición de bajar y seguía
        // avisando de la batería aunque ya se hubiera recuperado.
        session?.setBatteryProfile(if (adapt) p else null)
        session?.setFastDecode(p.fastDecode)
        // Rendimiento sostenido: frecuencias estables (sin tirones por calor) a costa de no dejar bajar del todo.
        window.setSustainedPerformanceMode(p.sustainedPerformance)
        val lp = window.attributes
        lp.screenBrightness = brightnessFor(p)
        lp.preferredDisplayModeId = if (streamFps > 0) refreshModeId(streamFps, p.lowRefreshMode) else 0
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

    /**
     * Modo de pantalla de la misma resolución con la menor ([low]) o la mayor frecuencia que acomoda [fps]
     * (0 = sin preferencia).
     */
    @Suppress("DEPRECATION")
    private fun refreshModeId(fps: Int, low: Boolean): Int {
        val display = (if (Build.VERSION.SDK_INT >= 30) display else windowManager.defaultDisplay) ?: return 0
        val current = display.mode
        val modes = display.supportedModes.filter {
            it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight
        }
        val rates = modes.map { it.refreshRate }
        val rate = (if (low) PowerProfile.pickRefreshRate(rates, fps) else PowerProfile.pickFastRefreshRate(rates, fps)) ?: return 0
        return modes.first { it.refreshRate == rate }.modeId
    }

    private fun governorBase() = PowerProfile.fromWire(config?.profile ?: 0)

    private fun showBadge(ms: Long) {
        badgeUntilMs = SystemClock.elapsedRealtime() + ms
    }

    private fun setMenu(open: Boolean) {
        menuOpen = open
        overlay.visibility = if (open || phase != StreamSession.Phase.STREAMING) View.VISIBLE else View.GONE
        updateMenuButton()
        if (!open) hideSystemBars()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    /**
     * Botón que abre el menú rápido: redondo, del azul de Extendesk y con borde blanco para que se vea sobre cualquier
     * fondo. Se puede arrastrar a otro sitio si tapa algo del PC, pero nunca fuera de la pantalla ni pegado al borde
     * (donde los gestos de Android lo taparían); se recuerda dónde se dejó (en proporción, para que valga al girarla).
     *
     * Se coloca con márgenes, no desplazándolo (x/y), y sin sombra ni transparencia de la vista: encima del vídeo
     * (SurfaceView) Android solo respeta la zona donde la vista está colocada de verdad, y lo demás salía cortado.
     */
    private fun makeMenuButton(): TextView = TextView(this).apply {
        text = "☰"
        setTextColor(Color.WHITE)
        textSize = 15f
        gravity = Gravity.CENTER
        includeFontPadding = false
        background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(MENU_COLOR)
            setStroke(dp(1).coerceAtLeast(2), Color.WHITE)
        }
        contentDescription = getString(R.string.menu_description)
        visibility = View.GONE
        val slop = ViewConfiguration.get(this@StreamActivity).scaledTouchSlop
        var downX = 0f
        var downY = 0f
        var startX = 0f
        var startY = 0f
        var dragging = false
        setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = e.rawX
                    downY = e.rawY
                    startX = menuX
                    startY = menuY
                    dragging = false
                    (v.background as GradientDrawable).setColor(MENU_COLOR_PRESSED)
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = e.rawX - downX
                    val dy = e.rawY - downY
                    if (!dragging && dx * dx + dy * dy > slop * slop) dragging = true
                    if (dragging) moveMenuButton(startX + dx, startY + dy)
                }
                MotionEvent.ACTION_UP -> {
                    (v.background as GradientDrawable).setColor(MENU_COLOR)
                    if (dragging) {
                        saveMenuButton()
                    } else {
                        // Se sueltan los dedos que pudieran estar yendo al PC antes de abrir el menú.
                        if (touchCount > 0) {
                            touchCount = 0
                            session?.sendTouch(touchState, 0)
                        }
                        setMenu(true)
                    }
                }
                MotionEvent.ACTION_CANCEL -> (v.background as GradientDrawable).setColor(MENU_COLOR)
            }
            true
        }
    }

    private fun moveMenuButton(x: Float, y: Float) {
        val btn = dp(MENU_BUTTON_DP)
        val m = dp(MENU_EDGE_DP).toFloat()
        menuX = x.coerceIn(m, (root.width - btn - m).coerceAtLeast(m))
        menuY = y.coerceIn(m, (root.height - btn - m).coerceAtLeast(m))
        val lp = menuButton.layoutParams as FrameLayout.LayoutParams
        if (lp.leftMargin != menuX.toInt() || lp.topMargin != menuY.toInt()) {
            lp.leftMargin = menuX.toInt()
            lp.topMargin = menuY.toInt()
            menuButton.layoutParams = lp
        }
    }

    private fun saveMenuButton() {
        val btn = dp(MENU_BUTTON_DP)
        val w = (root.width - btn).coerceAtLeast(1).toFloat()
        val h = (root.height - btn).coerceAtLeast(1).toFloat()
        Prefs.open(this).edit()
            .putFloat(PREF_MENU_X, menuX / w)
            .putFloat(PREF_MENU_Y, menuY / h)
            .apply()
    }

    /** De serie, abajo a la izquierda pero un poco por encima de la barra de tareas de Windows. */
    private fun placeMenuButton() {
        val prefs = Prefs.open(this)
        val fx = prefs.getFloat(PREF_MENU_X, 0f).coerceIn(0f, 1f)
        val fy = prefs.getFloat(PREF_MENU_Y, 0.82f).coerceIn(0f, 1f)
        val margin = if (fx == 0f) dp(8).toFloat() else 0f
        val btn = dp(MENU_BUTTON_DP)
        moveMenuButton(margin + fx * (root.width - btn), fy * (root.height - btn))
    }

    private fun updateMenuButton() {
        menuButton.visibility = if (phase == StreamSession.Phase.STREAMING && !menuOpen) View.VISIBLE else View.GONE
    }

    /** Solo desde el menú ☰: se recuerda para la próxima sesión (ya no está en Configuración avanzada). */
    private fun toggleStats() {
        statsVisible = !statsVisible
        statsShown = statsVisible
        statsView.visibility = if (statsVisible) View.VISIBLE else View.GONE
        settings = settings.copy(showStats = statsVisible)
        SettingsStore(this).save(settings)
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
                0 -> getString(R.string.quality_high)
                1 -> getString(R.string.quality_balanced)
                else -> getString(R.string.quality_saver)
            }
            val reduced = when (config.reduced) {
                Dlp.REDUCED_WIFI -> getString(R.string.reduced_wifi)
                Dlp.REDUCED_BATTERY -> getString(R.string.reduced_battery)
                Dlp.REDUCED_LOAD -> getString(R.string.reduced_load)
                Dlp.REDUCED_TABLET -> getString(R.string.reduced_device)
                else -> ""
            }
            summary = SessionSummary(
                quality = quality,
                details = "${config.fps} fps · ${"%.0f".format(config.maxBitrateX100kbps / 10.0)} Mbps",
                input = config.input,
                reduced = reduced,
            )
            if (previous == null) {
                badgeView.text = getString(R.string.badge_quality, quality, config.fps)
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
        wifiLock = wifi.createWifiLock(mode, "extendesk").also { it.acquire() }
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
        // Saliendo (Desconectar, Atrás): el PC quita la pantalla extra ya. Si no, se bloqueó o pasó a segundo plano y
        // vuelve al desbloquearla: el PC la guarda un rato para que las ventanas no se muevan.
        stopSession(if (isFinishing) Dlp.BYE_LEAVE else Dlp.BYE_PAUSE)
        surface = null
    }

    private fun startSession() {
        val s = surface ?: return
        phase = StreamSession.Phase.CONNECTING
        phaseDetail = ""
        overlay.visibility = View.VISIBLE
        val audioManager = getSystemService(AudioManager::class.java)
        val meter = codecMeter
        session = StreamSession(
            host, s, this, helloConfig,
            mediaVolume = {
                "${audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)}/${audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)}"
            },
            onDecodeSample = meter::record,
        ).also {
            it.setFastDecode(profile.fastDecode)
            it.start()
        }
    }

    private fun stopSession(byeReason: Int = Dlp.BYE_LEAVE) {
        touchHandler.removeCallbacks(touchKeepAlive)
        touchCount = 0
        session?.stop(byeReason)
        session = null
    }

    private fun restartSession() {
        stopSession(Dlp.BYE_PAUSE)  // vuelve enseguida: que el PC no quite la pantalla extra entre medias
        startSession()
    }

    override fun onPhase(phase: StreamSession.Phase, detail: String) {
        runOnUiThread {
            this.phase = phase
            phaseDetail = detail
            val streaming = phase == StreamSession.Phase.STREAMING
            if (!streaming) menuOpen = false
            overlay.visibility = if (streaming && !menuOpen) View.GONE else View.VISIBLE
            updateMenuButton()
            ui.removeCallbacks(releaseScreen)
            if (streaming || phase == StreamSession.Phase.CONNECTING) {
                window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            } else {
                ui.postDelayed(releaseScreen, SCREEN_GRACE_MS)
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
            // Y se fija el modo de pantalla: con los perfiles de ahorro el más lento que acomoda los fps (60 Hz en vez de
            // 144 Hz); con los demás el más rápido (120 Hz para 60 fps), para que cada frame espere menos al refresco.
            streamFps = info.fps
            applyProfile(profile)
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
                    getString(R.string.drain_warning),
                    Toast.LENGTH_LONG,
                ).show()
            }
            updateBadge()
            if (statsVisible) {
                val level = if (b.percent >= 0) " ${b.percent} %" else ""
                val watts = b.netMilliwatts?.let { " ${"%+.1f".format(it / 1000)} W" } ?: getString(R.string.stats_measuring)
                val plug = b.plugLabel?.let { getString(R.string.stats_plug, it) } ?: ""
                val source = b.source?.let { " (${it.label})" } ?: ""
                statsView.text = getString(R.string.stats_battery, text, level, watts, plug, source, profile.summary())
            }
        }
    }

    override fun onInputMode(state: Dlp.InputModeState) {
        runOnUiThread { if (summary.input != Dlp.INPUT_VIEW_ONLY) summary = summary.copy(input = state.active) }
        if (state.requested == Dlp.INPUT_TOUCH && state.active == Dlp.INPUT_MOUSE && !touchFallbackShown) {
            touchFallbackShown = true
            onError(getString(R.string.touch_fallback))
        }
    }

    /** El PC se detuvo o se perdió: al inicio, con un aviso, en vez de quedarse esperando para siempre. */
    override fun onEnded(hostStopped: Boolean) {
        runOnUiThread {
            if (isFinishing) return@runOnUiThread
            val text = getString(if (hostStopped) R.string.ended_host_stopped else R.string.ended_lost, hostName)
            Toast.makeText(this, text, Toast.LENGTH_LONG).show()
            finish()
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
        private const val MENU_BUTTON_DP = 36
        /** Distancia mínima al borde: ahí los gestos de Android (volver, barra de estado) se tragan el toque. */
        private const val MENU_EDGE_DP = 16
        // Lila de la marca (el eslabón de delante del logo, design/tokens.json), un poco transparente; pulsado, el de atrás.
        private const val MENU_COLOR = 0xD97033FD.toInt()
        private const val MENU_COLOR_PRESSED = 0xFF4D13C7.toInt()
        private const val PREF_MENU_X = "menu_button_x"
        private const val PREF_MENU_Y = "menu_button_y"
    }
}
