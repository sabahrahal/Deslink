package com.desklink

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.media.AudioManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
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

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Los botones de volumen controlan el sonido del PC (multimedia), no el del timbre.
        volumeControlStream = AudioManager.STREAM_MUSIC
        // Evita que la CPU/GPU bajen de frecuencia a mitad de sesión.
        window.setSustainedPerformanceMode(true)
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

        val root = FrameLayout(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1))
        root.addView(statsView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        setContentView(root)
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

    @Suppress("DEPRECATION")
    private fun hideSystemBars() {
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        session = StreamSession(host, holder.surface, this, Settings.audioEnabled(this)).also { it.start() }
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
    }
}
