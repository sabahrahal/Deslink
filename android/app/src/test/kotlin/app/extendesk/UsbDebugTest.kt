package app.extendesk

import org.junit.Assert.assertEquals
import org.junit.Test

class UsbDebugTest {
    @Test
    fun noHintWithDebuggingOnOrUnplugged() {
        assertEquals(UsbDebugHint.NONE, UsbDebug.hint(adbEnabled = true, developerEnabled = true, usbConnected = true))
        assertEquals(UsbDebugHint.NONE, UsbDebug.hint(adbEnabled = false, developerEnabled = false, usbConnected = false))
    }

    @Test
    fun pluggedWithoutDebuggingSaysWhatIsMissing() {
        assertEquals(
            UsbDebugHint.ENABLE_DEVELOPER,
            UsbDebug.hint(adbEnabled = false, developerEnabled = false, usbConnected = true),
        )
        assertEquals(
            UsbDebugHint.ENABLE_DEBUGGING,
            UsbDebug.hint(adbEnabled = false, developerEnabled = true, usbConnected = true),
        )
    }
}
