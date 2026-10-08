package com.desklink

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast

class StreamActivity : Activity(), SurfaceHolder.Callback, StreamSession.Listener {
    private lateinit var surfaceView: SurfaceView
    private lateinit var statsView: TextView
    private var session: StreamSession? = null
    private var wifiLock: WifiManager.WifiLock? = null
    @Volatile private var wifiNeeded = true  // false si el host se alcanza por USB o Ethernet
    @Volatile private var statsVisible = true
    private var started = false
    private lateinit var host: String
    private val probe by lazy { DeviceProbe(this) }

    // Perfil de batería (ver PowerProfile y docs/battery.md): el elegido en ajustes y, con cable, el que
    // decide el modo «adaptar a la carga».
    private lateinit var governor: ChargeGovernor
    private lateinit var profile: PowerProfile
    private lateinit var badgeView: TextView
    @Volatile private var streamFps = 0
    private var badgeUntilMs = 0L
    private var warnedDrain = false

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

        val prefs = getSharedPreferences(PowerProfile.PREFS, Context.MODE_PRIVATE)
        val base = PowerProfile.fromId(prefs.getString(PowerProfile.PREF_KEY, null))
        governor = ChargeGovernor(base, prefs.getBoolean(PowerProfile.PREF_ADAPT, PowerProfile.DEFAULT_ADAPT))
        profile = base

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        hideSystemBars()

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        statsView = TextView(this).apply {
            setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt())
            textSize = 12f
            setPadding(16, 8, 16, 8)
            text = "Conectando…"
        }
        surfaceView.setOnTouchListener { _, event -> onSurfaceTouch(event) }

        badgeView = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 12f
            setPadding(16, 8, 16, 8)
            visibility = View.GONE
        }

        val root = FrameLayout(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1))
        root.addView(statsView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        root.addView(badgeView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.END))
        setContentView(root)
        applyProfile(profile)
        showBadge(BADGE_MS)
    }

    /** Lo que el perfil cambia en la tablet; lo que cambia en el PC se manda con [StreamSession.setProfile]. */
    private fun applyProfile(p: PowerProfile) {
        val wifiModeChanged = p.lowLatencyWifi != profile.lowLatencyWifi
        profile = p
        session?.setProfile(p)
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

    private fun showBadge(ms: Long) {
        badgeUntilMs = SystemClock.elapsedRealtime() + ms
    }

    private fun onSurfaceTouch(event: MotionEvent): Boolean {
        // Cuatro dedos a la vez: mostrar u ocultar las estadísticas (el toque normal va al PC).
        if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount == 4) {
            statsVisible = statsView.visibility != View.VISIBLE
            statsView.visibility = if (statsVisible) View.VISIBLE else View.GONE
            return true
        }
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

    @Suppress("DEPRECATION")
    private fun hideSystemBars() {
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        session = StreamSession(host, holder.surface, this, profile).also { it.start() }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        touchHandler.removeCallbacks(touchKeepAlive)
        touchCount = 0
        session?.stop()
        session = null
    }

    override fun onStreamInfo(info: Dlp.StreamInfo) {
        runOnUiThread {
            // La tablet adapta su frecuencia de pantalla a la del stream (menos espera de vsync).
            if (Build.VERSION.SDK_INT >= 30 && info.fps > 0) {
                surfaceView.holder.surface.setFrameRate(info.fps.toFloat(), android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
            }
            // Con los perfiles de ahorro además se fija el modo de pantalla (p. ej. 60 Hz en vez de 144 Hz).
            streamFps = info.fps
            if (profile.lowRefreshMode) applyProfile(profile)
        }
    }

    // Llega cada segundo desde la sesión. La batería se mide siempre (es leer dos propiedades del sistema):
    // el modo «adaptar a la carga» la necesita aunque las estadísticas estén ocultas.
    override fun onStats(text: String) {
        val b = probe.sampleBattery()
        runOnUiThread {
            val changed = governor.update(SystemClock.elapsedRealtime(), b.pluggedUsbOrAc, b.netMilliwatts, b.percent)
            if (changed) {
                probe.restartWindow()
                applyProfile(governor.current)
                showBadge(BADGE_MS)
            }
            val state = governor.state
            if (state == ChargeState.DRAINING && !warnedDrain) {
                warnedDrain = true
                Toast.makeText(
                    this,
                    "La tablet se descarga aunque esté enchufada: el puerto USB da menos de lo que gasta. Usa un " +
                        "puerto USB-C o USB 3 del PC, un cargador de pared con WiFi, o baja el brillo.",
                    Toast.LENGTH_LONG,
                ).show()
            }
            val alert = state == ChargeState.DRAINING || state == ChargeState.ADAPTING
            badgeView.text = governor.indicator()
            badgeView.setBackgroundColor(
                when (state) {
                    ChargeState.DRAINING -> 0xCCB00020.toInt()
                    ChargeState.ADAPTING -> 0xCCB26A00.toInt()
                    ChargeState.CHARGING -> 0x9900701A.toInt()
                    else -> 0x99000000.toInt()
                },
            )
            badgeView.visibility = if (alert || SystemClock.elapsedRealtime() < badgeUntilMs) View.VISIBLE else View.GONE
            if (statsVisible) {
                val level = if (b.percent >= 0) " ${b.percent} %" else ""
                val watts = b.netMilliwatts?.let { " ${"%+.1f".format(it / 1000)} W" } ?: " (midiendo…)"
                val plug = b.plugLabel?.let { ", por $it" } ?: ""
                val source = b.source?.let { " (${it.label})" } ?: ""
                statsView.text = "$text · batería$level$watts$plug$source · perfil ${profile.summary()}"
            }
        }
    }

    override fun onError(message: String) {
        runOnUiThread { Toast.makeText(this, message, Toast.LENGTH_LONG).show() }
    }

    companion object {
        const val EXTRA_HOST = "host"
        private const val BADGE_MS = 8_000L
    }
}
