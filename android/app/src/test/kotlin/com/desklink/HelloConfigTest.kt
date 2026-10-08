package com.desklink

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HelloConfigTest {
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    @Test
    fun helloConfigMatchesTheHost() {
        val c = Dlp.HelloConfig(width = 2944, height = 1840, fps = 90, bitrateMbps = 30, codec = 2, densityDpi = 320, refreshHz = 90)
        // Mismos bytes que comprueba protocol/tests/test_protocol.cpp (TestHelloConfig).
        val golden = bytes(0x80, 0x0B, 0x30, 0x07, 0x5A, 0x00, 0x1E, 0x00, 0x02, 0x00, 0x40, 0x01, 0x5A, 0x00)
        assertArrayEquals(golden, c.encode())

        val packet = Dlp.hello(c, 7)
        val h = Dlp.parseHeader(packet, packet.size)!!
        assertEquals(Dlp.TYPE_HELLO, h.type)
        assertEquals(Dlp.HELLO_CONFIG_SIZE, h.payloadSize)
        assertArrayEquals(golden, packet.copyOfRange(Dlp.HEADER_SIZE, packet.size))
    }

    @Test
    fun emptyHelloForOldStyleAndDiscovery() {
        val packet = Dlp.hello(null, 7)
        assertEquals(Dlp.HEADER_SIZE, packet.size)
        assertEquals(0, Dlp.parseHeader(packet, packet.size)!!.payloadSize)
        val discover = Dlp.controlPacket(Dlp.TYPE_DISCOVER, 1)
        assertEquals(12, Dlp.parseHeader(discover, discover.size)!!.type)
    }

    private fun streamInfo(payload: ByteArray): Dlp.StreamInfo? {
        val p = Dlp.controlPacket(Dlp.TYPE_STREAM_INFO, 0, payload)
        return Dlp.parseStreamInfo(p, Dlp.parseHeader(p, p.size)!!)
    }

    @Test
    fun streamInfoWithNameAndFlags() {
        val name = "ESCRITORIO-SABAH".toByteArray()
        val payload = bytes(0x80, 0x07, 0x38, 0x04, 0x78, 0x00, 0x01, 0x01, name.size) + name
        val info = streamInfo(payload)!!
        assertEquals(1920, info.width)
        assertEquals(1080, info.height)
        assertEquals(120, info.fps)
        assertEquals(Dlp.CODEC_H264, info.codec)
        assertTrue(info.acceptsHelloConfig)
        assertEquals("ESCRITORIO-SABAH", info.hostName)
    }

    @Test
    fun streamInfoFromOldHost() {
        val info = streamInfo(bytes(0x80, 0x0B, 0x30, 0x07, 0x3C, 0x00, 0x02))!!
        assertEquals(2944, info.width)
        assertFalse(info.acceptsHelloConfig)
        assertEquals("", info.hostName)
    }

    @Test
    fun streamInfoNameLongerThanPayloadIsCut() {
        val info = streamInfo(bytes(0x80, 0x07, 0x38, 0x04, 0x3C, 0x00, 0x02, 0x01, 200) + "PC".toByteArray())!!
        assertEquals("PC", info.hostName)
    }
}
