package app.extendesk.ui

import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.Text
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [34])
class ThemeTest {
    @get:Rule val compose = createComposeRule()

    private fun contrast(a: Color, b: Color): Double {
        val la = a.luminance() + 0.05
        val lb = b.luminance() + 0.05
        return maxOf(la, lb).toDouble() / minOf(la, lb)
    }

    private fun contentColorIn(dark: Boolean): Color {
        var color = Color.Unspecified
        compose.setContent {
            ExtendeskTheme(dark = dark) {
                color = LocalContentColor.current
                Text("Título")
            }
        }
        compose.waitForIdle()
        return color
    }

    // El fallo de los títulos negros sobre fondo oscuro: un Text sin color debe salir con el color del tema.
    @Test
    fun textWithoutColorIsLightInDarkMode() = assertEquals(DarkColors.text, contentColorIn(dark = true))

    @Test
    fun textWithoutColorIsDarkInLightMode() = assertEquals(LightColors.text, contentColorIn(dark = false))

    @Test
    fun textIsReadableOnEveryBackground() {
        for (c in listOf(LightColors, DarkColors)) {
            for (bg in listOf(c.background, c.surface)) {
                assertTrue(contrast(c.text, bg) >= 4.5)
                assertTrue(contrast(c.textSecondary, bg) >= 4.5)
            }
            assertTrue(contrast(c.onAccent, c.accent) >= 4.5)
            assertTrue(contrast(c.accent, c.surface) >= 4.5)
        }
    }
}
