package com.desklink.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

val CardShape = RoundedCornerShape(20.dp)

/** Tarjeta oscura con borde; [glow] le pone el borde en degradado neón y un halo. */
@Composable
fun NeonCard(
    modifier: Modifier = Modifier,
    glow: Boolean = false,
    onClick: (() -> Unit)? = null,
    content: @Composable () -> Unit,
) {
    val base = modifier
        .fillMaxWidth()
        .then(
            if (glow) {
                Modifier.shadow(18.dp, CardShape, ambientColor = Palette.Accent, spotColor = Palette.Accent)
            } else {
                Modifier
            },
        )
        .clip(CardShape)
        .background(Palette.Surface)
        .border(
            width = if (glow) 1.5.dp else 1.dp,
            brush = if (glow) Palette.AccentGradient else Brush.linearGradient(listOf(Palette.Outline, Palette.Outline)),
            shape = CardShape,
        )
        .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
        .padding(18.dp)
    Box(base) { content() }
}

/** Botón principal: degradado cian-magenta con halo. */
@Composable
fun GlowButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    val shape = RoundedCornerShape(14.dp)
    Box(
        modifier
            .height(46.dp)
            .then(if (enabled) Modifier.shadow(12.dp, shape, ambientColor = Palette.Accent, spotColor = Palette.Accent) else Modifier)
            .clip(shape)
            .background(if (enabled) Palette.AccentGradient else Brush.linearGradient(listOf(Palette.Outline, Palette.Outline)))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 22.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = Color(0xFF05070C), fontWeight = FontWeight.Black, letterSpacing = 0.5.sp)
    }
}

/** Icono dentro de un círculo con halo del color [tint]. */
@Composable
fun IconBadge(icon: ImageVector, tint: Color, size: Dp = 52.dp) {
    Box(
        Modifier
            .size(size)
            .clip(CircleShape)
            .background(tint.copy(alpha = 0.14f))
            .border(1.dp, tint.copy(alpha = 0.6f), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(size * 0.5f))
    }
}

/** Etiqueta pequeña tipo "píldora". */
@Composable
fun Pill(text: String, color: Color) {
    Text(
        text.uppercase(),
        color = color,
        fontSize = 10.sp,
        fontWeight = FontWeight.Black,
        letterSpacing = 1.sp,
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(color.copy(alpha = 0.14f))
            .border(1.dp, color.copy(alpha = 0.5f), RoundedCornerShape(50))
            .padding(horizontal = 8.dp, vertical = 3.dp),
    )
}

/** Ondas de radar mientras se busca el PC. */
@Composable
fun Radar(modifier: Modifier = Modifier, color: Color = Palette.Accent, active: Boolean = true) {
    val t = rememberInfiniteTransition(label = "radar")
    val phase by t.animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(2200, easing = LinearEasing), RepeatMode.Restart),
        label = "phase",
    )
    Canvas(modifier) {
        val center = Offset(size.width / 2, size.height / 2)
        val maxR = size.minDimension / 2
        if (active) {
            for (i in 0 until 3) {
                val p = (phase + i / 3f) % 1f
                drawCircle(color.copy(alpha = (1f - p) * 0.55f), radius = maxR * p, center = center, style = Stroke(2.dp.toPx()))
            }
        } else {
            drawCircle(color.copy(alpha = 0.35f), radius = maxR * 0.7f, center = center, style = Stroke(2.dp.toPx()))
        }
        drawCircle(color, radius = maxR * 0.16f, center = center)
    }
}

/** Fila de ajuste con interruptor. */
@Composable
fun SwitchRow(title: String, subtitle: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable { onChange(!checked) }
            .padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(subtitle, color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            onCheckedChange = onChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color(0xFF05070C),
                checkedTrackColor = Palette.Accent,
                uncheckedTrackColor = Palette.SurfaceRaised,
                uncheckedBorderColor = Palette.Outline,
            ),
        )
    }
}

/** Opción seleccionable de un grupo (como una ficha). */
@Composable
fun OptionChip(label: String, hint: String?, selected: Boolean, enabled: Boolean = true, onClick: () -> Unit) {
    val border = if (selected) Palette.AccentGradient else Brush.linearGradient(listOf(Palette.Outline, Palette.Outline))
    Column(
        Modifier
            .clip(RoundedCornerShape(14.dp))
            .background(if (selected) Palette.Accent.copy(alpha = 0.12f) else Palette.SurfaceRaised)
            .border(if (selected) 1.5.dp else 1.dp, border, RoundedCornerShape(14.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            label,
            fontWeight = FontWeight.Bold,
            color = when {
                !enabled -> Palette.TextMuted.copy(alpha = 0.5f)
                selected -> Palette.Accent
                else -> Palette.Text
            },
        )
        if (hint != null) {
            Text(hint, fontSize = 11.sp, color = Palette.TextMuted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
fun VSpace(h: Dp) = Spacer(Modifier.height(h))
