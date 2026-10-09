package com.desklink.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

// Piezas comunes, planas y sin brillos (docs/design.md): tarjetas blancas o grises, un solo color de acento.

val CardShape = RoundedCornerShape(DlShape.CardRadius)
val ControlShape = RoundedCornerShape(DlShape.ControlRadius)

/** Tarjeta: superficie con borde fino. */
@Composable
fun DlCard(modifier: Modifier = Modifier, onClick: (() -> Unit)? = null, padding: Dp = 16.dp, content: @Composable () -> Unit) {
    Box(
        modifier
            .fillMaxWidth()
            .clip(CardShape)
            .background(palette.surface)
            .border(0.5.dp, palette.separator, CardShape)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(padding),
    ) { content() }
}

/** Botón principal: fondo de acento y texto en negrita. */
@Composable
fun PrimaryButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    Box(
        modifier
            .heightIn(min = 40.dp)
            .clip(ControlShape)
            .background(if (enabled) palette.accent else palette.surfaceSecondary)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = 18.dp, vertical = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = if (enabled) palette.onAccent else palette.textSecondary, fontWeight = FontWeight.SemiBold)
    }
}

/** Botón secundario: gris con el texto en el color de acento (o en rojo si [destructive]). */
@Composable
fun SecondaryButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, destructive: Boolean = false) {
    Box(
        modifier
            .heightIn(min = 40.dp)
            .clip(ControlShape)
            .background(palette.surfaceSecondary)
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 18.dp, vertical = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = if (destructive) palette.error else palette.accent, fontWeight = FontWeight.SemiBold)
    }
}

/** Enlace de texto en el color de acento. */
@Composable
fun LinkText(text: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Text(
        text,
        color = palette.accent,
        style = DlText.Body,
        modifier = modifier
            .clip(ControlShape)
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 8.dp),
    )
}

/**
 * Control segmentado como el de macOS e iOS: un carril gris y la opción elegida en una pastilla clara. Cada
 * opción lleva su texto y se marca como «seleccionada» para las pruebas y la accesibilidad.
 */
@Composable
fun Segmented(options: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val dark = palette == DarkColors
    Row(
        modifier
            .clip(ControlShape)
            .background(palette.surfaceSecondary)
            .padding(2.dp),
    ) {
        options.forEachIndexed { i, label ->
            val chosen = i == selected
            Box(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(DlShape.ControlRadius - 2.dp))
                    .background(if (chosen) (if (dark) palette.separator else palette.surface) else Color.Transparent)
                    .semantics {
                        this.selected = chosen
                        role = Role.Tab
                    }
                    .clickable { onSelect(i) }
                    .padding(horizontal = 10.dp, vertical = 7.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label,
                    style = DlText.Note,
                    fontWeight = if (chosen) FontWeight.SemiBold else FontWeight.Normal,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

/** Encabezado de sección, pequeño y en gris, encima de una tarjeta. */
@Composable
fun SectionHeader(text: String) {
    Text(text, style = DlText.Section, color = palette.textSecondary, modifier = Modifier.padding(start = 4.dp, bottom = 6.dp))
}

/** Fila con interruptor; toda la fila responde al toque. */
@Composable
fun SwitchRow(title: String, subtitle: String?, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable { onChange(!checked) }
            .padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = DlText.Body)
            if (subtitle != null) Text(subtitle, style = DlText.Note, color = palette.textSecondary)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            onCheckedChange = onChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = palette.accent,
                checkedBorderColor = palette.accent,
                uncheckedThumbColor = Color.White,
                uncheckedTrackColor = palette.separator,
                uncheckedBorderColor = palette.separator,
            ),
        )
    }
}

@Composable
fun RowDivider() = HorizontalDivider(color = palette.separator, thickness = 0.5.dp)

/** Punto de estado (verde conectado, ámbar esperando, rojo error). */
@Composable
fun StatusDot(color: Color, label: String) {
    Box(
        Modifier
            .size(10.dp)
            .clip(CircleShape)
            .background(color)
            .semantics { contentDescription = label },
    )
}

@Composable
fun VSpace(h: Dp) = Spacer(Modifier.height(h))

