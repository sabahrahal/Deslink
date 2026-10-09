package com.desklink.ui

import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotSelected
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.desklink.Found
import com.desklink.Link
import com.desklink.Route
import com.desklink.Transport
import java.net.InetAddress
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

/** La pantalla de inicio enseña cada cosa donde debe (Robolectric, sin tablet). */
@RunWith(AndroidJUnit4::class)
@Config(sdk = [34])
class HomeScreenTest {
    @get:Rule val compose = createComposeRule()

    private val wifi = Route("wlan0", InetAddress.getByName("192.168.1.40"), Transport.WIFI)
    private val usb = Found(Link.CABLE_HOST, null, "ESCRITORIO", acceptsConfig = true)
    private val net = Found("192.168.1.20", wifi, "ESCRITORIO", acceptsConfig = true)

    private fun show(state: HomeState, onConnect: (Found) -> Unit = {}, onAdvanced: () -> Unit = {}) {
        compose.setContent {
            DeskLinkTheme(dark = true) {
                HomeScreen(state, onConnect = onConnect, onConnectAddress = {}, onCancelAuto = {}, onOpenAdvanced = onAdvanced)
            }
        }
    }

    @Test
    fun usbAndWifiAreOneCardWithBothOptions() {
        var connected: Found? = null
        show(HomeState(hosts = listOf(net, usb), scans = 1), onConnect = { connected = it })
        // Una sola tarjeta para el PC, con un solo botón Conectar.
        compose.onAllNodesWithText("ESCRITORIO").assertCountEquals(1)
        compose.onAllNodesWithText("Conectar").assertCountEquals(1)
        compose.onNode(hasTestTag("pc:escritorio")).assertIsDisplayed()
        // Las dos formas de conectar dentro de la tarjeta, el cable elegido de serie.
        compose.onNodeWithText("Cable USB").assertIsSelected()
        compose.onNodeWithText("Wi-Fi").assertIsNotSelected()
        compose.onNodeWithText("Por cable USB · la conexión más rápida").assertIsDisplayed()

        compose.onNodeWithText("Conectar").performClick()
        assertEquals(Link.CABLE_HOST, connected?.ip)

        compose.onNodeWithText("Wi-Fi").performClick()
        compose.onNodeWithText("Wi-Fi").assertIsSelected()
        compose.onNodeWithText("Por Wi-Fi · 192.168.1.20").assertIsDisplayed()
        compose.onNodeWithText("Conectar").performClick()
        assertEquals("192.168.1.20", connected?.ip)
    }

    @Test
    fun onlyWifiHasNoUsbOption() {
        show(HomeState(hosts = listOf(net), scans = 1))
        compose.onNodeWithText("ESCRITORIO").assertIsDisplayed()
        compose.onAllNodesWithText("Cable USB").assertCountEquals(0)
    }

    @Test
    fun searchingShowsHelpOnlyWhenAsked() {
        show(HomeState(scans = 3, networks = "WiFi (192.168.1.40)"))
        compose.onNodeWithText("Buscando tu PC…").assertIsDisplayed()
        compose.onAllNodes(hasText("Redes de esta tablet", substring = true)).assertCountEquals(0)
        compose.onNodeWithText("¿No aparece?").performClick()
        compose.onNode(hasText("Redes de esta tablet", substring = true)).assertIsDisplayed()
    }

    @Test
    fun advancedSettingsIsALinkOnHome() {
        var opened = false
        show(HomeState(), onAdvanced = { opened = true })
        compose.onNodeWithText("DeskLink").assertIsDisplayed()
        compose.onNodeWithText("Configuración avanzada").performClick()
        assertEquals(true, opened)
        // Calidad, sonido y táctil no están en la tablet: se eligen en el PC.
        compose.onAllNodesWithText("Calidad").assertCountEquals(0)
        compose.onAllNodesWithText("Sonido").assertCountEquals(0)
    }
}
