package app.extendesk

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
    private var raw: BurstResult? = null
    private var paced: BurstResult? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        host = intent.getStringExtra(StreamActivity.EXTRA_HOST) ?: return finish()
        probe = DeviceProbe(this)

        status = TextView(this).apply { textSize = 14f; gravity = Gravity.CENTER }
        results = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        runButton = Button(this).apply {
            text = getString(R.string.diag_run)
            setOnClickListener { startTest() }
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            addView(TextView(this@DiagnosticsActivity).apply { text = getString(R.string.diagnostics); textSize = 26f })
            addView(TextView(this@DiagnosticsActivity).apply {
                text = getString(R.string.diag_intro, host)
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
        status.text = getString(R.string.diag_measuring)
        thread(name = "extendesk-linktest") {
            val result = try {
                LinkTester.run(host) { done -> runOnUiThread { status.text = getString(R.string.diag_measuring_progress, done) } }
            } catch (e: Exception) {
                null
            }
            runOnUiThread { status.text = getString(R.string.diag_bursts) }
            val rawBurst = try { LinkTester.burst(host, paceMbps = 0) } catch (e: Exception) { null }
            Thread.sleep(300)
            val pacedBurst = try { LinkTester.burst(host, paceMbps = 100) } catch (e: Exception) { null }
            runOnUiThread {
                link = result
                raw = rawBurst
                paced = pacedBurst
                runButton.isEnabled = true
                status.text = getString(if (result == null) R.string.diag_failed else R.string.diag_done)
                refresh()
            }
        }
    }

    private fun refresh() {
        val net = probe.networkInfo()
        val findings = Advisor.advise(link, net, probe.sampleBattery(), probe.thermalStatus(), raw, paced)
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
                    text = getString(R.string.diag_route, it.route?.description ?: getString(R.string.diag_route_unknown))
                },
            )
            results.addView(
                TextView(this).apply {
                    setPadding(0, 32, 0, 0)
                    textSize = 12f
                    text = getString(
                        R.string.diag_detail,
                        it.received,
                        it.sent,
                        "%.1f".format(it.rttP50Ms),
                        "%.1f".format(it.rttP95Ms),
                        "%.1f".format(it.jitterMs),
                    ) +
                        (raw?.let { r -> getString(R.string.diag_burst_raw, r.received, r.sent, "%.0f".format(r.mbps)) } ?: "") +
                        (paced?.let { r -> getString(R.string.diag_burst_paced, r.paceMbps, r.received, r.sent) } ?: "")
                },
            )
        }
    }
}
