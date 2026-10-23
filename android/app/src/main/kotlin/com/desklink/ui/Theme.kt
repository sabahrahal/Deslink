package com.desklink.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import com.desklink.R

/** Colores del modo en uso (claro u oscuro, como el sistema). Salen de design/tokens.json, igual que en el PC. */
val LocalDlColors = staticCompositionLocalOf { LightColors }

/** Atajo para los colores: `palette.accent`, `palette.textSecondary`… */
val palette: DlColors
    @Composable @ReadOnlyComposable
    get() = LocalDlColors.current

/** Montserrat, la fuente del logo (design/fonts/): Black para el nombre y los títulos grandes, Bold para el resto. */
val Montserrat = FontFamily(
    Font(R.font.montserrat_black, FontWeight.Black),
    Font(R.font.montserrat_bold, FontWeight.Bold),
)

private fun style(size: TextUnit, weight: FontWeight, brand: Boolean) =
    TextStyle(fontSize = size, fontWeight = weight, fontFamily = if (brand) Montserrat else FontFamily.Default)

/** Estilos de texto de docs/design.md. */
object DlText {
    val LargeTitle = style(DlType.LargeTitleSize, DlType.LargeTitleWeight, DlType.LargeTitleBrand)
    val Title = style(DlType.TitleSize, DlType.TitleWeight, DlType.TitleBrand)
    val Headline = style(DlType.HeadlineSize, DlType.HeadlineWeight, DlType.HeadlineBrand)
    val Section = style(DlType.SectionSize, DlType.SectionWeight, DlType.SectionBrand)
    val Body = style(DlType.BodySize, DlType.BodyWeight, DlType.BodyBrand)
    val Note = style(DlType.NoteSize, DlType.NoteWeight, DlType.NoteBrand)
}

private fun schemeFor(c: DlColors, dark: Boolean) = if (dark) {
    darkColorScheme(
        primary = c.accent, onPrimary = c.onAccent, background = c.background, onBackground = c.text,
        surface = c.surface, onSurface = c.text, surfaceVariant = c.surfaceSecondary, onSurfaceVariant = c.textSecondary,
        outline = c.separator, error = c.error,
    )
} else {
    lightColorScheme(
        primary = c.accent, onPrimary = c.onAccent, background = c.background, onBackground = c.text,
        surface = c.surface, onSurface = c.text, surfaceVariant = c.surfaceSecondary, onSurfaceVariant = c.textSecondary,
        outline = c.separator, error = c.error,
    )
}

/**
 * Tema de Extendesk. Además de los colores de Material, fija el color y el estilo del texto por defecto: sin esto,
 * un Text sin color fuera de un Surface sale negro (el fallo de los títulos ilegibles sobre fondo oscuro).
 */
@Composable
fun DeskLinkTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    val colors = if (dark) DarkColors else LightColors
    MaterialTheme(colorScheme = schemeFor(colors, dark), typography = Typography()) {
        CompositionLocalProvider(
            LocalDlColors provides colors,
            LocalContentColor provides colors.text,
            LocalTextStyle provides DlText.Body,
            content = content,
        )
    }
}
