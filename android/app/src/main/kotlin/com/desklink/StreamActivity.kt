package com.desklink

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.view.Gravity
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
    private lateinit var host: String
    private val probe by lazy { DeviceProbe(this) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        host = intent.getStringExtra(EXTRA_HOST) ?: run {
            finish()
            return
        }

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
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
        surfaceView.setOnClickListener {
            statsView.visibility = if (statsView.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }

        val root = FrameLayout(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1))
        root.addView(statsView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        setContentView(root)
    }

    override fun onStart() {
        super.onStart()
        // Sin ahorro de energía del WiFi: es la mayor fuente de picos de latencia en la red.
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wifi.createWifiLock(mode, "desklink").also { it.acquire() }
    }

    override fun onStop() {
        super.onStop()
        wifiLock?.takeIf { it.isHeld }?.release()
        wifiLock = null
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
        session = StreamSession(host, holder.surface, this).also { it.start() }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
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
        val b = probe.sampleBattery()
        val power = b.netMilliwatts?.let { " · batería ${"%+.1f".format(it / 1000)} W" } ?: ""
        runOnUiThread { statsView.text = text + power }
    }

    override fun onError(message: String) {
        runOnUiThread { Toast.makeText(this, message, Toast.LENGTH_LONG).show() }
    }

    companion object {
        const val EXTRA_HOST = "host"
    }
}
