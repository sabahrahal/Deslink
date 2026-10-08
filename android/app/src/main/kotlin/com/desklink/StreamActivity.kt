package com.desklink

import android.content.Context
import android.graphics.Color
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
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
    private var legacyWarned = false
    private var lastBackMs = 0L
    private val probe by lazy { DeviceProbe(this) }

    // Estado que pinta la capa de Compose.
    private var phase by mutableStateOf(StreamSession.Phase.CONNECTING)
    private var phaseDetail by mutableStateOf("")
    private var hostName by mutableStateOf("")

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
        helloConfig = settings.toHelloConfig(Device.display(this), Device.hevcSupported)

        if (settings.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Evita que la CPU/GPU bajen de frecuencia a mitad de sesión.
        window.setSustainedPerformanceMode(true)
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
                    StreamOverlay(phase, phaseDetail, hostName, onRetry = { restartSession() }, onBack = { finish() })
                }
            }
        }

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
        root.addView(statsView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        root.addView(overlay, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)
        hideSystemBars()

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

    override fun onDestroy() {
        super.onDestroy()
        ui.removeCallbacks(releaseScreen)
        CableLink.sessionActive = false
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
    private fun acquireWifiLock() {
        if (wifiLock != null) return
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
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
            overlay.visibility = if (streaming) View.GONE else View.VISIBLE
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
            val want = helloConfig
            if (!legacyWarned && !info.acceptsHelloConfig && want != null && (info.width != want.width || info.height != want.height)) {
                legacyWarned = true
                Toast.makeText(
                    this,
                    "El DeskLink del PC es antiguo y no ajusta el monitor a esta pantalla. Actualízalo para verlo a ${want.width}×${want.height}.",
                    Toast.LENGTH_LONG,
                ).show()
            }
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

    override fun onStats(text: String) {
        if (!statsVisible) return  // ocultas: ni se mide la batería ni se repinta el texto
        val b = probe.sampleBattery()
        val level = if (b.percent >= 0) " ${b.percent} %" else ""
        val watts = b.netMilliwatts?.let { " ${"%+.1f".format(it / 1000)} W" } ?: " (midiendo…)"
        val power = " · batería$level$watts"
        runOnUiThread { statsView.text = text + power }
    }

    override fun onError(message: String) {
        runOnUiThread { Toast.makeText(this, message, Toast.LENGTH_LONG).show() }
    }

    companion object {
        const val EXTRA_HOST = "host"
        const val EXTRA_HOST_NAME = "host_name"
        private const val SCREEN_GRACE_MS = 2 * 60 * 1000L
    }
}
