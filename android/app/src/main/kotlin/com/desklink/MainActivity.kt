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
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.desklink.ui.DeskLinkTheme
import com.desklink.ui.HomeScreen
import com.desklink.ui.HomeState
import com.desklink.ui.OnboardingScreen
import com.desklink.ui.palette
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
    /** Se abrió porque el PC vio enchufar la tablet: conectar por el cable en cuanto responda. */
    private var plugPending by mutableStateOf(false)

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
        if (coldStart && intent?.action == ACTION_USB_ATTACHED) {
            // Con una sesión en marcha la orden del PC llega encima de ella: nada que hacer, se vuelve a la sesión.
            if (CableLink.sessionActive) {
                finish()
                return
            }
            plugPending = true
        }

        setContent {
            DeskLinkTheme {
                var showSettings by rememberSaveable { mutableStateOf(false) }
                // Guía de inicio la primera vez (no si se abrió porque el PC vio enchufar la tablet).
                var showGuide by rememberSaveable { mutableStateOf(coldStart && !plugPending && !store.onboardingDone) }
                var guideUsb by remember { mutableStateOf(UsbDebugHint.NONE) }
                var autoDone by rememberSaveable { mutableStateOf(!coldStart || showGuide) }
                BackHandler(enabled = showSettings && !showGuide) { showSettings = false }
                BackHandler(enabled = showGuide) {
                    store.onboardingDone = true
                    showGuide = false
                }
                LaunchedEffect(showGuide, resumed) {
                    // Al volver de Ajustes se ve al momento si ya quedó activada la depuración USB.
                    if (showGuide && resumed) guideUsb = UsbDebug.readUnplugged(this@MainActivity)
                }

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
                            usbDebug = UsbDebug.read(this@MainActivity),
                            cableExpected = UsbDebug.cableExpected(this@MainActivity),
                        )
                        val cable = hosts.firstOrNull { it.isCable }
                        if (plugPending && cable != null) {
                            // Enchufada la tablet: se conecta por el cable sin mirar el ajuste de conexión automática.
                            plugPending = false
                            autoDone = true
                            showSettings = false
                            home = home.copy(autoConnecting = null)
                            connect(cable)
                        } else if (!plugPending && !autoDone && settings.autoConnect && !showSettings) {
                            // El cable siempre primero: si el último PC llega también por cable, se usa el cable,
                            // aunque ya se hubiera elegido su Wi-Fi en esta misma cuenta atrás.
                            val current = home.autoConnecting
                            val target = HostGroups.autoTarget(hosts, home.lastHost, home.lastHostName, scan, home.cableExpected)
                            if (target != null && (current == null || (target.isCable && !current.isCable))) {
                                home = home.copy(autoConnecting = target)
                            }
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

                Box(Modifier.fillMaxSize().background(palette.background)) {
                    if (showGuide) {
                        OnboardingScreen(
                            usbDebug = guideUsb,
                            onOpenUsbDebug = { openUsbDebugSettings(guideUsb) },
                            onOpenTether = { openTetherSettings() },
                            onDone = {
                                store.onboardingDone = true
                                showGuide = false
                            },
                        )
                    } else {
                        AnimatedContent(showSettings, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "screen") { inSettings ->
                            if (inSettings) {
                                SettingsScreen(
                                    settings = settings,
                                    display = display,
                                    versionName = version,
                                    onChange = {
                                        settings = it
                                        store.save(it)
                                    },
                                    onConnectAddress = {
                                        autoDone = true
                                        showSettings = false
                                        connect(Found(it, null))
                                    },
                                    onOpenDiagnostics = { openDiagnostics() },
                                    onBack = { showSettings = false },
                                    onOpenGuide = {
                                        showSettings = false
                                        showGuide = true
                                    },
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
                                    onOpenAdvanced = {
                                        autoDone = true
                                        home = home.copy(autoConnecting = null)
                                        showSettings = true
                                    },
                                    onOpenUsbDebug = { openUsbDebugSettings(home.usbDebug) },
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.action == ACTION_USB_ATTACHED && !CableLink.sessionActive) {
            // Manda el cable: se olvida una cuenta atrás que fuera a conectar por la red.
            plugPending = true
            home = home.copy(autoConnecting = null)
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

    companion object {
        /** Lo manda el PC (adb shell am start -a …) al ver la tablet enchufada, si el usuario lo activó. */
        const val ACTION_USB_ATTACHED = "com.desklink.action.USB_ATTACHED"
    }

    private fun openUsbDebugSettings(hint: UsbDebugHint) {
        try {
            startActivity(UsbDebug.settingsIntent(hint))
        } catch (_: Exception) {
            // Algún fabricante no tiene la pantalla directa: los Ajustes generales.
            startActivity(Intent(android.provider.Settings.ACTION_SETTINGS))
        }
    }

    private fun openTetherSettings() {
        try {
            startActivity(UsbDebug.tetherIntent())
        } catch (_: Exception) {
            // Sin la pantalla directa: los ajustes de red, donde está «Anclaje a red».
            try {
                startActivity(Intent(android.provider.Settings.ACTION_WIRELESS_SETTINGS))
            } catch (_: Exception) {
                startActivity(Intent(android.provider.Settings.ACTION_SETTINGS))
            }
        }
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
