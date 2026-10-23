package app.extendesk

import androidx.test.ext.junit.runners.AndroidJUnit4
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

/** Inglés por defecto, español con el sistema en español, y los dos archivos con los mismos textos. */
@RunWith(AndroidJUnit4::class)
@Config(sdk = [34])
class LocalesTest {
    @Test fun englishIsTheDefault() {
        assertEquals("Over USB cable · the most stable connection", Strings.get(R.string.route_cable))
        assertEquals("Connecting to ESCRITORIO", Strings.get(R.string.phase_connecting, "ESCRITORIO"))
    }

    @Config(qualifiers = "es")
    @Test fun spanishWithASpanishSystem() {
        assertEquals("Por cable USB · la conexión más estable", Strings.get(R.string.route_cable))
        assertEquals("Conectando con ESCRITORIO", Strings.get(R.string.phase_connecting, "ESCRITORIO"))
    }

    @Test fun everyLanguageHasEveryText() {
        val base = names(File("src/main/res/values/strings.xml")) - "app_name"
        val dirs = File("src/main/res").listFiles { f -> f.isDirectory && f.name.matches(Regex("values-[a-z]{2}(-r[A-Z]{2})?")) }!!
        for (dir in dirs) {
            val file = File(dir, "strings.xml")
            if (file.exists()) assertEquals("Textos de ${dir.name}", base, names(file))
        }
    }

    private fun names(file: File): Set<String> =
        Regex("<string name=\"([^\"]+)\"").findAll(file.readText()).map { it.groupValues[1] }.toSet()
}
