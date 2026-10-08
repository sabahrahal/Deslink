package com.desklink.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.desklink.StreamSession.Phase

/** Pantalla de estado sobre el vídeo mientras no hay imagen: conectando, sin respuesta, reconectando o error. */
@Composable
fun StreamOverlay(phase: Phase, detail: String, hostName: String, onRetry: () -> Unit, onBack: () -> Unit) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0xE60A0E17)),
        contentAlignment = Alignment.Center,
    ) {
        NeonCard(Modifier.widthIn(max = 460.dp).padding(24.dp), glow = phase == Phase.CONNECTING) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                val color = when (phase) {
                    Phase.FAILED -> Palette.Error
                    Phase.NO_ANSWER, Phase.RECONNECTING -> Palette.Warning
                    else -> Palette.Accent
                }
                Radar(Modifier.size(72.dp), color = color, active = phase != Phase.FAILED)
                VSpace(16.dp)
                Text(
                    when (phase) {
                        Phase.CONNECTING -> "Conectando con $hostName"
                        Phase.NO_ANSWER -> "El PC no responde"
                        Phase.RECONNECTING -> "Reconectando…"
                        Phase.FAILED -> "No se pudo conectar"
                        Phase.STREAMING -> ""
                    },
                    style = MaterialTheme.typography.titleLarge,
                    textAlign = TextAlign.Center,
                )
                VSpace(8.dp)
                Text(
                    detail.ifBlank { if (phase == Phase.CONNECTING) "Preparando el monitor a la medida de esta pantalla…" else "" },
                    color = Palette.TextMuted,
                    textAlign = TextAlign.Center,
                    style = MaterialTheme.typography.bodyMedium,
                )
                VSpace(20.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = onBack) { Text(if (phase == Phase.FAILED) "Volver" else "Cancelar", color = Palette.TextMuted) }
                    if (phase == Phase.FAILED || phase == Phase.NO_ANSWER) GlowButton("Reintentar", onRetry)
                }
            }
        }
    }
}
