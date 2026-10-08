package com.desklink

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.desklink.ui.DeskLinkTheme
import com.desklink.ui.HomeScreen
import com.desklink.ui.HomeState
import com.desklink.ui.Palette
import com.desklink.ui.SettingsScreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * Pantalla de inicio: busca PCs solo (cable USB y red) mientras está visible, los lista y conecta con un toque.
 * Los ajustes van en la misma actividad.
 */
class MainActivity : ComponentActivity() {
    private lateinit var store: SettingsStore
    private var resumed by mutableStateOf(false)
    private var settings by mutableStateOf(AppSettings())
    private var home by mutableStateOf(HomeState())

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        store = SettingsStore(this)
        settings = store.load()
        home = home.copy(lastHost = store.lastHost, lastHostName = store.lastHostName)
        val display = Device.display(this)
        val version = try {
            packageManager.getPackageInfo(packageName, 0).versionName ?: ""
        } catch (_: Exception) {
            ""
        }
        // Al abrir la app (no al volver de una sesión ni al girar la pantalla) puede conectarse sola al último PC.
        val coldStart = savedInstanceState == null

        setContent {
            DeskLinkTheme {
                var showSettings by rememberSaveable { mutableStateOf(false) }
                var autoDone by rememberSaveable { mutableStateOf(!coldStart) }
                BackHandler(enabled = showSettings) { showSettings = false }

                // Búsqueda continua mientras la pantalla está a la vista. No corre durante una sesión: la prueba
                // del cable abre una conexión USB nueva y el PC cortaría la de la sesión.
                LaunchedEffect(resumed) {
                    if (!resumed) return@LaunchedEffect
                    val seen = LinkedHashMap<String, Pair<Found, Int>>()
                    var scan = home.scans
                    while (true) {
                        home = home.copy(scanning = true)
                        val results = scanOnce()
                        scan++
                        for (f in results) seen[f.ip] = f to scan
                        // Un PC que falla una búsqueda no desaparece de golpe de la lista.
                        seen.entries.removeAll { scan - it.value.second > 1 }
                        val hosts = seen.values.map { it.first }.sortedByDescending { it.isCable }
                        home = home.copy(
                            hosts = hosts,
                            scanning = false,
                            scans = scan,
                            networks = withContext(Dispatchers.IO) { Discovery.describeNetworks() },
                        )
                        if (!autoDone && settings.autoConnect && !showSettings && home.autoConnecting == null) {
                            hosts.firstOrNull { it.ip == home.lastHost }?.let { home = home.copy(autoConnecting = it) }
                        }
                        delay(if (hosts.isEmpty()) 1000 else 3000)
                    }
                }
                LaunchedEffect(home.autoConnecting) {
                    val target = home.autoConnecting ?: return@LaunchedEffect
                    delay(1500)
                    autoDone = true
                    home = home.copy(autoConnecting = null)
                    connect(target)
                }

                Box(Modifier.fillMaxSize().background(Palette.BackgroundGradient)) {
                    AnimatedContent(showSettings, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "screen") { inSettings ->
                        if (inSettings) {
                            SettingsScreen(
                                settings = settings,
                                display = display,
                                hevcSupported = Device.hevcSupported,
                                versionName = version,
                                onChange = {
                                    settings = it
                                    store.save(it)
                                },
                                onOpenDiagnostics = { openDiagnostics() },
                                onBack = { showSettings = false },
                            )
                        } else {
                            HomeScreen(
                                state = home,
                                onConnect = {
                                    autoDone = true
                                    connect(it)
                                },
                                onConnectAddress = {
                                    autoDone = true
                                    connect(Found(it, null))
                                },
                                onCancelAuto = {
                                    autoDone = true
                                    home = home.copy(autoConnecting = null)
                                },
                                onOpenSettings = {
                                    autoDone = true
                                    home = home.copy(autoConnecting = null)
                                    showSettings = true
                                },
                            )
                        }
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
    }

    override fun onPause() {
        super.onPause()
        resumed = false
    }

    /** Una búsqueda: el cable USB y la red a la vez. */
    private suspend fun scanOnce(): List<Found> = coroutineScope {
        val cable = async(Dispatchers.IO) { CableLink.probe() }
        val lan = async(Dispatchers.IO) { Discovery.findAll() }
        listOfNotNull(cable.await()) + lan.await()
    }

    private fun connect(found: Found) {
        // Sin nombre (IP escrita a mano o host antiguo) se conserva el que hubiera para esa misma dirección.
        if (found.name.isNotBlank() || store.lastHost != found.ip) store.lastHostName = found.name
        store.lastHost = found.ip
        home = home.copy(lastHost = found.ip, lastHostName = store.lastHostName)
        CableLink.sessionActive = true // que ninguna búsqueda en curso abra otra conexión por cable
        startActivity(
            Intent(this, StreamActivity::class.java)
                .putExtra(StreamActivity.EXTRA_HOST, found.ip)
                .putExtra(StreamActivity.EXTRA_HOST_NAME, found.title),
        )
    }

    private fun openDiagnostics() {
        val host = store.lastHost
        if (host == null) {
            Toast.makeText(this, "Conéctate primero a un PC", Toast.LENGTH_SHORT).show()
            return
        }
        startActivity(Intent(this, DiagnosticsActivity::class.java).putExtra(StreamActivity.EXTRA_HOST, host))
    }
}
