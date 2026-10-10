package com.desklink.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.desklink.StreamSession.Phase

/** Pantalla de estado sobre el vídeo mientras no hay imagen: conectando, sin respuesta, reconectando o error. */
@Composable
fun StreamOverlay(phase: Phase, detail: String, hostName: String, onRetry: () -> Unit, onBack: () -> Unit) {
    Box(Modifier.fillMaxSize().background(palette.background), contentAlignment = Alignment.Center) {
        DlCard(Modifier.widthIn(max = 440.dp).padding(24.dp), padding = 24.dp) {
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                if (phase == Phase.FAILED) {
                    StatusDot(palette.error, "Error")
                } else {
                    CircularProgressIndicator(
                        Modifier.size(28.dp),
                        color = if (phase == Phase.CONNECTING) palette.accent else palette.warning,
                        strokeWidth = 2.5.dp,
                    )
                }
                VSpace(16.dp)
                Text(
                    when (phase) {
                        Phase.CONNECTING -> "Conectando con $hostName"
                        Phase.NO_ANSWER -> "El PC no responde"
                        Phase.RECONNECTING -> "Reconectando…"
                        Phase.FAILED -> "No se pudo conectar"
                        Phase.STREAMING -> ""
                    },
                    style = DlText.Title,
                    textAlign = TextAlign.Center,
                )
                VSpace(8.dp)
                Text(
                    detail.ifBlank { if (phase == Phase.CONNECTING) "Preparando la pantalla a la medida de este dispositivo…" else "" },
                    style = DlText.Body,
                    color = palette.textSecondary,
                    textAlign = TextAlign.Center,
                )
                VSpace(20.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    SecondaryButton(if (phase == Phase.FAILED) "Volver" else "Cancelar", onBack)
                    if (phase == Phase.FAILED || phase == Phase.NO_ANSWER) PrimaryButton("Reintentar", onRetry)
                }
            }
        }
    }
}

/** Qué decidió el PC, para el menú rápido. */
data class SessionSummary(
    val quality: String = "",
    val details: String = "",
    /** 0 táctil, 1 ratón, 2 solo ver. */
    val input: Int = 0,
    val reduced: String = "",
)

/** Menú rápido (botón sobre el vídeo): estadísticas y desconectar. Lo demás se cambia en el PC. */
@Composable
fun QuickMenu(
    summary: SessionSummary,
    statsVisible: Boolean,
    onStats: () -> Unit,
    onDisconnect: () -> Unit,
    onClose: () -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.45f))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onClose),
        contentAlignment = Alignment.Center,
    ) {
        DlCard(
            Modifier
                .widthIn(max = 420.dp)
                .padding(24.dp)
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
            padding = 20.dp,
        ) {
            Column {
                Text("DeskLink", style = DlText.Title)
                if (summary.quality.isNotBlank()) {
                    Text("Calidad ${summary.quality} · ${summary.details}", style = DlText.Note, color = palette.textSecondary)
                }
                if (summary.reduced.isNotBlank()) Text(summary.reduced, style = DlText.Note, color = palette.warning)
                VSpace(16.dp)
                if (summary.input == 2) {
                    Text("Solo ver: los toques no llegan al PC. Se cambia en el PC.", style = DlText.Note, color = palette.textSecondary)
                    VSpace(12.dp)
                }
                SwitchRow("Estadísticas", null, statsVisible) { onStats() }
                VSpace(8.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    SecondaryButton("Desconectar", onDisconnect, Modifier.weight(1f), destructive = true)
                    PrimaryButton("Seguir", onClose, Modifier.weight(1f))
                }
            }
        }
    }
}
