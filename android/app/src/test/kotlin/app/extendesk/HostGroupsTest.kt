package app.extendesk

import androidx.test.ext.junit.runners.AndroidJUnit4
import java.net.InetAddress
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

// Los textos salen de strings.xml (Robolectric); se comprueban en español.
@RunWith(AndroidJUnit4::class)
@Config(sdk = [34], qualifiers = "es")
class HostGroupsTest {
    private val wifi = Route("wlan0", InetAddress.getByName("192.168.1.40"), Transport.WIFI)
    private val usbCable = Found(Link.CABLE_HOST, null, "ESCRITORIO", acceptsConfig = true)
    private val pcWifi = Found("192.168.1.20", wifi, "ESCRITORIO", acceptsConfig = true)
    private val laptop = Found("192.168.1.31", wifi, "PORTATIL", acceptsConfig = true)
    private val rndis = Route("rndis0", InetAddress.getByName("192.168.42.129"), Transport.USB)
    private val pcTether = Found("192.168.42.10", rndis, "ESCRITORIO", acceptsConfig = true)

    @Test
    fun usbAndWifiOfTheSamePcAreOneCard() {
        val pcs = HostGroups.group(listOf(pcWifi, usbCable))
        assertEquals(1, pcs.size)
        val pc = pcs[0]
        assertEquals("ESCRITORIO", pc.name)
        // Se conecta sola por el cable: no hay nada que elegir.
        assertEquals(Link.CABLE_HOST, pc.route().ip)
        assertTrue(pc.matches(Link.CABLE_HOST) && pc.matches("192.168.1.20"))
    }

    @Test
    fun differentPcsAreDifferentCardsWithCableFirst() {
        val pcs = HostGroups.group(listOf(laptop, pcWifi, usbCable))
        assertEquals(listOf("ESCRITORIO", "PORTATIL"), pcs.map { it.name })
        assertNull(pcs[1].usb)
        assertEquals("192.168.1.31", pcs[1].route().ip)
    }

    @Test
    fun onlyWifiUsesWifi() {
        val pc = HostGroups.group(listOf(pcWifi)).single()
        assertEquals("192.168.1.20", pc.route().ip)
        assertEquals("Por Wi-Fi · 192.168.1.20", HostGroups.describe(pc.route()))
        assertEquals("Por cable USB · la conexión más estable", HostGroups.describe(usbCable))
    }

    @Test
    fun pcsWithoutNameAreKeptApart() {
        val pcs = HostGroups.group(listOf(Found("10.0.0.2", wifi), Found("10.0.0.3", wifi)))
        assertEquals(2, pcs.size)
    }

    @Test
    fun usbTetheringIsCableNotWifi() {
        val pc = HostGroups.group(listOf(pcWifi, pcTether)).single()
        // El anclaje de red por USB va como opción de cable; la lista de red queda solo con el Wi-Fi.
        assertEquals("192.168.42.10", pc.usb?.ip)
        assertEquals(listOf("192.168.1.20"), pc.network.map { it.ip })
        assertEquals("192.168.42.10", pc.route().ip)
        // Para el usuario es el mismo cable USB, venga por el anclaje o por la depuración.
        assertEquals("Por cable USB · la conexión más estable", HostGroups.describe(pcTether))
    }

    @Test
    fun tetheringBeatsAdbCable() {
        // Por adb los picos llegaban a 75-95 ms; por el anclaje, ~55.
        val pc = HostGroups.group(listOf(usbCable, pcWifi, pcTether)).single()
        assertEquals("192.168.42.10", pc.usb?.ip)
        assertEquals("192.168.42.10", HostGroups.autoTarget(listOf(usbCable, pcTether), Link.CABLE_HOST, "", scans = 1)?.ip)
        assertEquals(listOf("192.168.1.20"), pc.network.map { it.ip })
    }

    @Test
    fun autoConnectPrefersCableEvenIfLastWasWifi() {
        val found = listOf(pcWifi, usbCable)
        // Por la dirección del Wi-Fi o por el nombre: el mismo PC, y por cable desde la primera búsqueda.
        assertEquals(Link.CABLE_HOST, HostGroups.autoTarget(found, "192.168.1.20", "ESCRITORIO", scans = 1)?.ip)
        assertEquals(Link.CABLE_HOST, HostGroups.autoTarget(found, "192.168.1.99", "escritorio", scans = 1)?.ip)
        assertEquals(Link.CABLE_HOST, HostGroups.autoTarget(found, Link.CABLE_HOST, "", scans = 1)?.ip)
    }

    @Test
    fun autoConnectWaitsASecondScanBeforeUsingTheNetwork() {
        val found = listOf(pcWifi, laptop)
        // En la primera búsqueda el cable aún puede estar respondiendo: no se elige la red todavía.
        assertNull(HostGroups.autoTarget(found, "192.168.1.20", "ESCRITORIO", scans = 1))
        assertEquals("192.168.1.20", HostGroups.autoTarget(found, "192.168.1.20", "ESCRITORIO", scans = 2)?.ip)
        // El anclaje USB va por el cable: gana al Wi-Fi sin esperar.
        assertEquals("192.168.42.10", HostGroups.autoTarget(found + pcTether, "192.168.1.20", null, scans = 1)?.ip)
    }

    @Test
    fun autoConnectWaitsForTheCableWhenItIsOnItsWay() {
        val found = listOf(pcWifi)
        // Enchufada con depuración USB: la red espera a que el PC prepare el cable.
        assertNull(HostGroups.autoTarget(found, "192.168.1.20", "ESCRITORIO", scans = 2, cableExpected = true))
        assertNull(HostGroups.autoTarget(found, "192.168.1.20", "ESCRITORIO", scans = HostGroups.CABLE_WAIT_SCANS - 1, cableExpected = true))
        // Si el cable no llega, al final se usa la red.
        assertEquals("192.168.1.20", HostGroups.autoTarget(found, "192.168.1.20", "ESCRITORIO", scans = HostGroups.CABLE_WAIT_SCANS, cableExpected = true)?.ip)
        // Y en cuanto aparece, gana sin esperar.
        assertEquals(Link.CABLE_HOST, HostGroups.autoTarget(listOf(pcWifi, usbCable), "192.168.1.20", "ESCRITORIO", scans = 1, cableExpected = true)?.ip)
    }

    @Test
    fun autoConnectNeedsALastHost() {
        val found = listOf(pcWifi, usbCable)
        assertNull(HostGroups.autoTarget(found, null, "ESCRITORIO", scans = 5))
        assertNull(HostGroups.autoTarget(found, "10.0.0.9", "OTRO", scans = 5))
        assertEquals("192.168.1.31", HostGroups.autoTarget(found + laptop, "192.168.1.31", "PORTATIL", scans = 2)?.ip)
    }
}
