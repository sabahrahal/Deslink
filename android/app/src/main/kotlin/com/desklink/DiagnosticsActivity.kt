package com.desklink

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.concurrent.thread

/** Panel de diagnóstico: mide conexión, WiFi, batería y temperatura y lo explica en lenguaje sencillo. */
class DiagnosticsActivity : Activity() {
    private lateinit var host: String
    private lateinit var probe: DeviceProbe
    private lateinit var results: LinearLayout
    private lateinit var status: TextView
    private lateinit var runButton: Button
    private var link: LinkResult? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        host = intent.getStringExtra(StreamActivity.EXTRA_HOST) ?: return finish()
        probe = DeviceProbe(this)

        status = TextView(this).apply { textSize = 14f; gravity = Gravity.CENTER }
        results = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        runButton = Button(this).apply {
            text = "Probar ahora (unos 3 segundos)"
            setOnClickListener { startTest() }
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            addView(TextView(this@DiagnosticsActivity).apply { text = "Diagnóstico"; textSize = 26f })
            addView(TextView(this@DiagnosticsActivity).apply {
                text = "Mide la conexión con $host, el WiFi y la batería y te dice qué mejorar. " +
                    "Para medir el consumo, déjala unos 20 segundos abierta."
            })
            addView(runButton)
            addView(status)
            addView(ScrollView(this@DiagnosticsActivity).apply { addView(results) }, LinearLayout.LayoutParams(-1, 0, 1f))
        }
        setContentView(root)
        refresh()
    }

    private fun startTest() {
        runButton.isEnabled = false
        status.text = "Midiendo…"
        thread(name = "desklink-linktest") {
            val result = try {
                LinkTester.run(host) { done -> runOnUiThread { status.text = "Midiendo… $done/100" } }
            } catch (e: Exception) {
                null
            }
            runOnUiThread {
                link = result
                runButton.isEnabled = true
                status.text = if (result == null) "No se pudo realizar la prueba." else "Prueba terminada."
                refresh()
            }
        }
    }

    private fun refresh() {
        val net = probe.networkInfo()
        val findings = Advisor.advise(link, net, probe.sampleBattery(), probe.thermalStatus())
        results.removeAllViews()
        for (f in findings) {
            val color = when (f.severity) {
                Severity.OK -> 0xFF2E7D32.toInt()
                Severity.INFO -> 0xFF1565C0.toInt()
                Severity.WARNING -> 0xFFC62828.toInt()
            }
            results.addView(TextView(this).apply { text = f.title; textSize = 18f; setTextColor(color); setPadding(0, 32, 0, 4) })
            results.addView(TextView(this).apply { text = f.detail; setTextColor(Color.DKGRAY) })
        }
        link?.let {
            results.addView(
                TextView(this).apply {
                    setPadding(0, 32, 0, 0)
                    textSize = 16f
                    text = "Conexión usada: " + (it.route?.description ?: "no identificada (IP fuera de las redes de la tablet)")
                },
            )
            results.addView(
                TextView(this).apply {
                    setPadding(0, 32, 0, 0)
                    textSize = 12f
                    text = "Detalle: ${it.received}/${it.sent} respuestas · mediana ${"%.1f".format(it.rttP50Ms)} ms · " +
                        "p95 ${"%.1f".format(it.rttP95Ms)} ms · variación ${"%.1f".format(it.jitterMs)} ms"
                },
            )
        }
    }
}
