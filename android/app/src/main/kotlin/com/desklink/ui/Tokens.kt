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
    /** Botón principal, selección, enlaces. */
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
    accent = Color(0xFF0066CC),
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
    accent = Color(0xFF409CFF),
    onAccent = Color(0xFF000000),
    success = Color(0xFF30D158),
    warning = Color(0xFFFF9F0A),
    error = Color(0xFFFF6961),
)

/** Tamaños y pesos de texto. */
object DlType {
    /** Nombre de la pantalla. */
    val LargeTitleSize = 28.sp
    val LargeTitleWeight = FontWeight(600)
    /** Título de tarjeta. */
    val TitleSize = 20.sp
    val TitleWeight = FontWeight(600)
    /** Nombre de un PC o de una opción. */
    val HeadlineSize = 17.sp
    val HeadlineWeight = FontWeight(600)
    /** Encabezado de sección, en color secundario. */
    val SectionSize = 13.sp
    val SectionWeight = FontWeight(600)
    /** Texto normal. */
    val BodySize = 15.sp
    val BodyWeight = FontWeight(400)
    /** Explicaciones bajo una opción. */
    val NoteSize = 13.sp
    val NoteWeight = FontWeight(400)
}

object DlShape {
    val CardRadius = 12.dp
    val ControlRadius = 8.dp
}
