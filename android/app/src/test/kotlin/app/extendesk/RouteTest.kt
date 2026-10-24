package app.extendesk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RouteTest {
    @Test fun classifiesInterfaces() {
        assertEquals(Transport.WIFI, Transport.classify("wlan0"))
        assertEquals(Transport.USB, Transport.classify("rndis0"))
        assertEquals(Transport.USB, Transport.classify("ncm0"))
        assertEquals(Transport.ETHERNET, Transport.classify("eth0"))
        assertEquals(Transport.OTHER, Transport.classify("dummy0"))
    }

    @Test fun subnetMatching() {
        val local = byteArrayOf(192.toByte(), 168.toByte(), 42, 129.toByte())
        assertTrue(Route.sameSubnet(local, 24, byteArrayOf(192.toByte(), 168.toByte(), 42, 2)))
        assertFalse(Route.sameSubnet(local, 24, byteArrayOf(192.toByte(), 168.toByte(), 0, 206.toByte())))
        assertTrue(Route.sameSubnet(local, 16, byteArrayOf(192.toByte(), 168.toByte(), 0, 206.toByte())))
    }
}
