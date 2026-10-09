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

/** Colores del modo en uso (claro u oscuro, como el sistema). Salen de design/tokens.json, igual que en el PC. */
val LocalDlColors = staticCompositionLocalOf { LightColors }

/** Atajo para los colores: `palette.accent`, `palette.textSecondary`… */
val palette: DlColors
    @Composable @ReadOnlyComposable
    get() = LocalDlColors.current

/** Estilos de texto de docs/design.md. */
object DlText {
    val LargeTitle = TextStyle(fontSize = DlType.LargeTitleSize, fontWeight = DlType.LargeTitleWeight)
    val Title = TextStyle(fontSize = DlType.TitleSize, fontWeight = DlType.TitleWeight)
    val Headline = TextStyle(fontSize = DlType.HeadlineSize, fontWeight = DlType.HeadlineWeight)
    val Section = TextStyle(fontSize = DlType.SectionSize, fontWeight = DlType.SectionWeight)
    val Body = TextStyle(fontSize = DlType.BodySize, fontWeight = DlType.BodyWeight)
    val Note = TextStyle(fontSize = DlType.NoteSize, fontWeight = DlType.NoteWeight)
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
 * Tema de DeskLink. Además de los colores de Material, fija el color y el estilo del texto por defecto: sin esto,
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
