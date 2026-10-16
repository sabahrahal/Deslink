// Generado por tools/design/gen_tokens.py desde design/tokens.json. No editar a mano.
package com.desklink.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Colores de un modo (claro u oscuro). */
data class DlColors(
    /** Fondo de pantalla. */
    val background: Color,
    /** Tarjetas. */
    val surface: Color,
    /** Campos y opciones sin elegir. */
    val surfaceSecondary: Color,
    /** Bordes y líneas. */
    val separator: Color,
    /** Texto y títulos. */
    val text: Color,
    /** Explicaciones. */
    val textSecondary: Color,
    /** Botón principal, selección, enlaces y nombre del logo. */
    val accent: Color,
    /** Texto sobre el acento. */
    val onAccent: Color,
    /** Conectado. */
    val success: Color,
    /** Avisos. */
    val warning: Color,
    /** Errores. */
    val error: Color,
)

val LightColors = DlColors(
    background = Color(0xFFF5F5F7),
    surface = Color(0xFFFFFFFF),
    surfaceSecondary = Color(0xFFECECF0),
    separator = Color(0xFFD2D2D7),
    text = Color(0xFF1D1D1F),
    textSecondary = Color(0xFF5E5E63),
    accent = Color(0xFF3F0D91),
    onAccent = Color(0xFFFFFFFF),
    success = Color(0xFF1E7D34),
    warning = Color(0xFFA84B00),
    error = Color(0xFFD70015),
)

val DarkColors = DlColors(
    background = Color(0xFF1C1C1E),
    surface = Color(0xFF2C2C2E),
    surfaceSecondary = Color(0xFF3A3A3C),
    separator = Color(0xFF48484A),
    text = Color(0xFFF5F5F7),
    textSecondary = Color(0xFFAEAEB2),
    accent = Color(0xFFA68CFF),
    onAccent = Color(0xFF000000),
    success = Color(0xFF30D158),
    warning = Color(0xFFFF9F0A),
    error = Color(0xFFFF6961),
)

/** Tamaños, pesos y fuente del texto (Brand = Montserrat, la del logo; si no, la del sistema). */
object DlType {
    /** Nombre de la pantalla. */
    val LargeTitleSize = 28.sp
    val LargeTitleWeight = FontWeight(900)
    const val LargeTitleBrand = true
    /** Título de tarjeta. */
    val TitleSize = 20.sp
    val TitleWeight = FontWeight(700)
    const val TitleBrand = true
    /** Nombre de un PC o de una opción. */
    val HeadlineSize = 17.sp
    val HeadlineWeight = FontWeight(600)
    const val HeadlineBrand = false
    /** Encabezado de sección, en color secundario. */
    val SectionSize = 13.sp
    val SectionWeight = FontWeight(700)
    const val SectionBrand = true
    /** Texto normal. */
    val BodySize = 15.sp
    val BodyWeight = FontWeight(400)
    const val BodyBrand = false
    /** Explicaciones bajo una opción. */
    val NoteSize = 13.sp
    val NoteWeight = FontWeight(400)
    const val NoteBrand = false
}

/** Colores fijos del icono (los mismos en claro y oscuro). */
object DlLogo {
    /** Eslabón de atrás (morado oscuro). */
    val Back = Color(0xFF4D13C7)
    /** Eslabón de delante. */
    val Front = Color(0xFF7033FD)
    /** Sombra donde el eslabón de delante pasa por debajo. */
    val FrontShadow = Color(0xFF3F11A5)
}

object DlShape {
    val CardRadius = 12.dp
    val ControlRadius = 8.dp
}
