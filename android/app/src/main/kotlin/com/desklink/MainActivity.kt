package com.desklink

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.widget.Button
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
            text = "Deja la IP vacía para buscar el PC automáticamente (WiFi o cable USB con anclaje), o escríbela."
            gravity = Gravity.CENTER
        }
        val ip = EditText(this).apply {
            setHint("192.168.1.20")
            inputType = InputType.TYPE_CLASS_PHONE
            setText(prefs.getString("host", ""))
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
            statusText.text = "Buscando el PC en la red y por USB…"
            Thread {
                val found = try { Discovery.find() } catch (_: Exception) { null }
                runOnUiThread {
                    if (found == null) {
                        statusText.text = "No encontré el PC. Abre desklink-host.exe, comprueba el cortafuegos (UDP 47800) " +
                            "o escribe la IP a mano."
                    } else {
                        statusText.text = "PC encontrado: ${found.ip} por ${found.route?.transport?.label ?: "red"}"
                        ip.setText(found.ip)
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
            addView(diagnostics)
            addView(statusText)
        }
        setContentView(root)
    }
}
