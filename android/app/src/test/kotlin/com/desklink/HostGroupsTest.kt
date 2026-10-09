package com.desklink

import java.net.InetAddress
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class HostGroupsTest {
    private val wifi = Route("wlan0", InetAddress.getByName("192.168.1.40"), Transport.WIFI)
    private val usbCable = Found(Link.CABLE_HOST, null, "ESCRITORIO", acceptsConfig = true)
    private val pcWifi = Found("192.168.1.20", wifi, "ESCRITORIO", acceptsConfig = true)
    private val laptop = Found("192.168.1.31", wifi, "PORTATIL", acceptsConfig = true)

    @Test
    fun usbAndWifiOfTheSamePcAreOneCard() {
        val pcs = HostGroups.group(listOf(pcWifi, usbCable))
        assertEquals(1, pcs.size)
        val pc = pcs[0]
        assertEquals("ESCRITORIO", pc.name)
        assertTrue(pc.hasBoth)
        // De serie, el cable; si se elige Wi-Fi, la IP.
        assertEquals(Link.CABLE_HOST, pc.route().ip)
        assertEquals("192.168.1.20", pc.route(preferUsb = false).ip)
        assertTrue(pc.matches(Link.CABLE_HOST) && pc.matches("192.168.1.20"))
    }

    @Test
    fun differentPcsAreDifferentCardsWithCableFirst() {
        val pcs = HostGroups.group(listOf(laptop, pcWifi, usbCable))
        assertEquals(listOf("ESCRITORIO", "PORTATIL"), pcs.map { it.name })
        assertFalse(pcs[1].hasBoth)
        assertNull(pcs[1].usb)
        assertEquals("192.168.1.31", pcs[1].route().ip)
    }

    @Test
    fun onlyWifiUsesWifiEvenIfUsbIsPreferred() {
        val pc = HostGroups.group(listOf(pcWifi)).single()
        assertEquals("192.168.1.20", pc.route(preferUsb = true).ip)
        assertEquals("Por Wi-Fi · 192.168.1.20", HostGroups.describe(pc.route()))
        assertEquals("Por cable USB · la conexión más rápida", HostGroups.describe(usbCable))
    }

    @Test
    fun pcsWithoutNameAreKeptApart() {
        val pcs = HostGroups.group(listOf(Found("10.0.0.2", wifi), Found("10.0.0.3", wifi)))
        assertEquals(2, pcs.size)
    }
}
