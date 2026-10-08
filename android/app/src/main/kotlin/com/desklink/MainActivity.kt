package com.desklink

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prefs = getSharedPreferences("desklink", Context.MODE_PRIVATE)

        val title = TextView(this).apply {
            text = "DeskLink"
            textSize = 28f
            gravity = Gravity.CENTER
        }
        val hint = TextView(this).apply {
            text = "Deja la IP vacía para buscar el PC automáticamente (primero por el cable USB, luego por WiFi), " +
                "o escribe la IP del PC."
            gravity = Gravity.CENTER
        }
        val ip = EditText(this).apply {
            setHint(prefs.getString("host", null)?.let { "Última: $it (vacío = buscar solo)" } ?: "Vacío = buscar el PC solo")
            inputType = InputType.TYPE_CLASS_PHONE
            setSingleLine()
        }
        val statusText = TextView(this).apply { gravity = Gravity.CENTER }
        // Si el campo está vacío, busca el PC automáticamente (WiFi o USB) y luego abre la pantalla.
        fun launch(target: Class<*>) {
            val typed = ip.text.toString().trim()
            if (typed.isNotEmpty()) {
                prefs.edit().putString("host", typed).apply()
                startActivity(Intent(this, target).putExtra(StreamActivity.EXTRA_HOST, typed))
                return
            }
            statusText.text = "Buscando el PC por el cable USB y en la red…"
            Thread {
                // El cable primero: no depende del WiFi y tiene menos latencia.
                val found = if (CableLink.probe()) {
                    Found(Link.CABLE_HOST, Route("adb", java.net.InetAddress.getLoopbackAddress(), Transport.CABLE))
                } else {
                    try { Discovery.find() } catch (_: Exception) { null }
                }
                runOnUiThread {
                    if (found == null) {
                        statusText.text = "No encontré el PC. Redes de la tablet: ${Discovery.describeNetworks()}. " +
                            "Abre desklink-host.exe. Por cable USB: activa la depuración USB (Opciones de desarrollador) " +
                            "y acepta el aviso del PC. Por WiFi: permite UDP 47800 en el cortafuegos de Windows."
                    } else {
                        statusText.text = if (found.ip == Link.CABLE_HOST) {
                            "PC encontrado por el cable USB"
                        } else {
                            "PC encontrado: ${found.ip} por ${found.route?.transport?.label ?: "red"}"
                        }
                        prefs.edit().putString("host", found.ip).apply()
                        startActivity(Intent(this, target).putExtra(StreamActivity.EXTRA_HOST, found.ip))
                    }
                }
            }.start()
        }
        val connect = Button(this).apply {
            text = "Conectar (busca el PC solo)"
            setOnClickListener { launch(StreamActivity::class.java) }
        }

        val audio = CheckBox(this).apply {
            text = "Sonido del PC en la tablet"
            isChecked = Settings.audioEnabled(this@MainActivity)
            setOnCheckedChangeListener { _, checked -> Settings.setAudioEnabled(this@MainActivity, checked) }
        }

        val diagnostics = Button(this).apply {
            text = "Diagnóstico y recomendaciones"
            setOnClickListener { launch(DiagnosticsActivity::class.java) }
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(64, 64, 64, 64)
            addView(title)
            addView(hint)
            addView(ip, LinearLayout.LayoutParams(-1, -2))
            addView(connect)
            addView(audio)
            addView(diagnostics)
            addView(statusText)
        }
        setContentView(root)
    }
}
