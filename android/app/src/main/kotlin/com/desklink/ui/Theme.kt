package com.desklink.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

/** Colores de DeskLink: los mismos que la app del PC (ver docs/design.md). */
object Palette {
    val Background = Color(0xFF0A0E17)
    val Surface = Color(0xFF121826)
    val SurfaceRaised = Color(0xFF1A2233)
    val Outline = Color(0xFF263049)
    val Accent = Color(0xFF00E5FF)      // cian neón: acción principal, foco
    val AccentAlt = Color(0xFFFF2BD6)   // magenta: degradados y detalles
    val Success = Color(0xFF3DFF9A)
    val Warning = Color(0xFFFFB547)
    val Error = Color(0xFFFF4D6D)
    val Text = Color(0xFFE8EEFF)
    val TextMuted = Color(0xFF8A96B8)

    val AccentGradient = Brush.linearGradient(listOf(Accent, AccentAlt))
    val BackgroundGradient = Brush.verticalGradient(listOf(Color(0xFF0E1424), Background, Color(0xFF07090F)))
}

private val Colors = darkColorScheme(
    primary = Palette.Accent,
    onPrimary = Color(0xFF00151A),
    secondary = Palette.AccentAlt,
    onSecondary = Color.White,
    background = Palette.Background,
    onBackground = Palette.Text,
    surface = Palette.Surface,
    onSurface = Palette.Text,
    surfaceVariant = Palette.SurfaceRaised,
    onSurfaceVariant = Palette.TextMuted,
    outline = Palette.Outline,
    error = Palette.Error,
)

private val Type = Typography().let { t ->
    t.copy(
        displaySmall = t.displaySmall.copy(fontWeight = FontWeight.Black, letterSpacing = 1.sp),
        headlineSmall = t.headlineSmall.copy(fontWeight = FontWeight.Bold),
        titleLarge = t.titleLarge.copy(fontWeight = FontWeight.Bold),
        titleMedium = t.titleMedium.copy(fontWeight = FontWeight.SemiBold),
        labelLarge = t.labelLarge.copy(fontWeight = FontWeight.Bold, letterSpacing = 0.5.sp),
    )
}

/** Texto pequeño en mayúsculas para títulos de sección. */
val SectionLabel = TextStyle(
    fontFamily = FontFamily.Default,
    fontWeight = FontWeight.Bold,
    fontSize = 12.sp,
    letterSpacing = 2.sp,
    color = Palette.Accent,
)

@Composable
fun DeskLinkTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors, typography = Type, content = content)
}
