package app.extendesk.ui

import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.extendesk.Found
import app.extendesk.Link
import app.extendesk.Route
import app.extendesk.Transport
import app.extendesk.UsbDebugHint
import java.net.InetAddress
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

/** La pantalla de inicio enseña cada cosa donde debe (Robolectric, sin tablet). */
@RunWith(AndroidJUnit4::class)
@Config(sdk = [34], qualifiers = "es")
class HomeScreenTest {
    @get:Rule val compose = createComposeRule()

    private val wifi = Route("wlan0", InetAddress.getByName("192.168.1.40"), Transport.WIFI)
    private val usb = Found(Link.CABLE_HOST, null, "ESCRITORIO", acceptsConfig = true)
    private val net = Found("192.168.1.20", wifi, "ESCRITORIO", acceptsConfig = true)

    private fun show(
        state: HomeState,
        onConnect: (Found) -> Unit = {},
        onAdvanced: () -> Unit = {},
        onUsbDebug: () -> Unit = {},
    ) {
        compose.setContent {
            ExtendeskTheme(dark = true) {
                HomeScreen(
                    state,
                    onConnect = onConnect,
                    onConnectAddress = {},
                    onCancelAuto = {},
                    onOpenAdvanced = onAdvanced,
                    onOpenUsbDebug = onUsbDebug,
                )
            }
        }
    }

    @Test
    fun usbAndWifiAreOneCardThatUsesTheCable() {
        var connected: Found? = null
        show(HomeState(hosts = listOf(net, usb), scans = 1), onConnect = { connected = it })
        // Una sola tarjeta para el PC, con un solo botón Conectar y nada que elegir: va por el cable.
        compose.onAllNodesWithText("ESCRITORIO").assertCountEquals(1)
        compose.onAllNodesWithText("Conectar").assertCountEquals(1)
        compose.onNode(hasTestTag("pc:escritorio")).assertIsDisplayed()
        compose.onAllNodesWithText("Wi-Fi").assertCountEquals(0)
        compose.onNodeWithText("Por cable USB · la conexión más estable").assertIsDisplayed()

        compose.onNodeWithText("Conectar").performClick()
        assertEquals(Link.CABLE_HOST, connected?.ip)
    }

    @Test
    fun usbTetheringShowsAsCable() {
        val rndis = Route("rndis0", InetAddress.getByName("192.168.42.129"), Transport.USB)
        val tether = Found("192.168.42.10", rndis, "ESCRITORIO", acceptsConfig = true)
        show(HomeState(hosts = listOf(net, tether), scans = 1))
        // El mismo «cable USB» que por depuración: sin hablar de anclaje.
        compose.onNodeWithText("Por cable USB · la conexión más estable").assertIsDisplayed()
    }

    @Test
    fun onlyWifiConnectsByWifi() {
        show(HomeState(hosts = listOf(net), scans = 1))
        compose.onNodeWithText("ESCRITORIO").assertIsDisplayed()
        compose.onNodeWithText("Por Wi-Fi · 192.168.1.20").assertIsDisplayed()
    }

    @Test
    fun searchingShowsHelpOnlyWhenAsked() {
        show(HomeState(scans = 3, networks = "WiFi (192.168.1.40)"))
        compose.onNodeWithText("Buscando tu PC…").assertIsDisplayed()
        compose.onAllNodes(hasText("Redes de este dispositivo", substring = true)).assertCountEquals(0)
        compose.onNodeWithText("¿No aparece?").performClick()
        compose.onNode(hasText("Redes de este dispositivo", substring = true)).assertIsDisplayed()
    }

    @Test
    fun advancedSettingsIsALinkOnHome() {
        var opened = false
        show(HomeState(), onAdvanced = { opened = true })
        compose.onNodeWithText("Extendesk").assertIsDisplayed()
        compose.onNodeWithText("Configuración avanzada").performClick()
        assertEquals(true, opened)
        // Calidad y sonido no están en la tablet: se eligen en el PC.
        compose.onAllNodesWithText("Calidad").assertCountEquals(0)
        compose.onAllNodesWithText("Sonido").assertCountEquals(0)
    }

    @Test
    fun pluggedWithoutCableShowsOneStepToTurnItOn() {
        var tether = 0
        var debug = 0
        compose.setContent {
            ExtendeskTheme(dark = true) {
                HomeScreen(
                    state = HomeState(scans = 1, usbDebug = UsbDebugHint.ENABLE_DEVELOPER),
                    onConnect = {}, onConnectAddress = {}, onCancelAuto = {}, onOpenAdvanced = {},
                    onOpenUsbDebug = { debug++ }, onOpenTether = { tether++ },
                )
            }
        }
        compose.onNode(hasTestTag("cable-step")).assertIsDisplayed()
        compose.onNodeWithText("Activa la conexión por cable").assertIsDisplayed()
        compose.onNodeWithText("Abrir «Anclaje USB»").performClick()
        assertEquals(1, tether)
        // La depuración USB es un consejo opcional, plegado.
        compose.onAllNodes(hasText("toca 7 veces", substring = true)).assertCountEquals(0)
        compose.onNodeWithText("Que conecte solo al enchufar (opcional)").performClick()
        compose.onNode(hasText("toca 7 veces", substring = true)).assertIsDisplayed()
        compose.onNodeWithText("Abrir ajustes de depuración USB").performScrollTo().performClick()
        assertEquals(1, debug)
    }

    @Test
    fun noCableStepOnceTheCableWorks() {
        // Enchufada y el PC ya llega por el anclaje: no hay nada que activar.
        val rndis = Route("rndis0", InetAddress.getByName("192.168.42.129"), Transport.USB)
        val tether = Found("192.168.42.10", rndis, "ESCRITORIO", acceptsConfig = true)
        show(HomeState(hosts = listOf(tether), scans = 1, usbDebug = UsbDebugHint.ENABLE_DEBUGGING))
        compose.onAllNodes(hasTestTag("cable-step")).assertCountEquals(0)
    }

    @Test
    fun noCableStepWhenNothingIsMissing() {
        show(HomeState(hosts = listOf(usb), scans = 1))
        compose.onAllNodes(hasTestTag("cable-step")).assertCountEquals(0)
    }
}
