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
            text = "Indica la IP del PC donde se ejecuta desklink-host."
            gravity = Gravity.CENTER
        }
        val ip = EditText(this).apply {
            setHint("192.168.1.20")
            inputType = InputType.TYPE_CLASS_PHONE
            setText(prefs.getString("host", ""))
            setSingleLine()
        }
        val connect = Button(this).apply {
            text = "Conectar"
            setOnClickListener {
                val host = ip.text.toString().trim()
                if (host.isNotEmpty()) {
                    prefs.edit().putString("host", host).apply()
                    startActivity(Intent(this@MainActivity, StreamActivity::class.java).putExtra(StreamActivity.EXTRA_HOST, host))
                }
            }
        }

        val diagnostics = Button(this).apply {
            text = "Diagnóstico y recomendaciones"
            setOnClickListener {
                val host = ip.text.toString().trim()
                if (host.isNotEmpty()) {
                    prefs.edit().putString("host", host).apply()
                    startActivity(Intent(this@MainActivity, DiagnosticsActivity::class.java).putExtra(StreamActivity.EXTRA_HOST, host))
                }
            }
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
        }
        setContentView(root)
    }
}
