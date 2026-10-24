package app.extendesk.ui

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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.extendesk.R
import app.extendesk.StreamSession.Phase

/**
 * Pantalla de estado sobre el vídeo mientras no hay imagen: conectando, sin respuesta, reconectando, el PC se detuvo o
 * error.
 */
@Composable
fun StreamOverlay(phase: Phase, detail: String, hostName: String, onRetry: () -> Unit, onBack: () -> Unit) {
    Box(Modifier.fillMaxSize().background(palette.background), contentAlignment = Alignment.Center) {
        DlCard(Modifier.widthIn(max = 440.dp).padding(24.dp), padding = 24.dp) {
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                if (phase == Phase.FAILED) {
                    StatusDot(palette.error, stringResource(R.string.error))
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
                        Phase.CONNECTING -> stringResource(R.string.phase_connecting, hostName)
                        Phase.NO_ANSWER -> stringResource(R.string.phase_no_answer)
                        Phase.RECONNECTING -> stringResource(R.string.phase_reconnecting)
                        Phase.HOST_STOPPED -> stringResource(R.string.phase_host_stopped, hostName)
                        Phase.FAILED -> stringResource(R.string.phase_failed)
                        Phase.STREAMING -> ""
                    },
                    style = DlText.Title,
                    textAlign = TextAlign.Center,
                )
                VSpace(8.dp)
                Text(
                    detail.ifBlank { if (phase == Phase.CONNECTING) stringResource(R.string.phase_connecting_detail) else "" },
                    style = DlText.Body,
                    color = palette.textSecondary,
                    textAlign = TextAlign.Center,
                )
                VSpace(20.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    SecondaryButton(stringResource(if (phase == Phase.FAILED || phase == Phase.HOST_STOPPED) R.string.action_back else R.string.action_cancel), onBack)
                    if (phase == Phase.FAILED || phase == Phase.NO_ANSWER) PrimaryButton(stringResource(R.string.action_retry), onRetry)
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
                Text("Extendesk", style = DlText.Title)
                if (summary.quality.isNotBlank()) {
                    Text(stringResource(R.string.menu_quality, summary.quality, summary.details), style = DlText.Note, color = palette.textSecondary)
                }
                if (summary.reduced.isNotBlank()) Text(summary.reduced, style = DlText.Note, color = palette.warning)
                VSpace(16.dp)
                if (summary.input == 2) {
                    Text(stringResource(R.string.menu_view_only), style = DlText.Note, color = palette.textSecondary)
                    VSpace(12.dp)
                }
                SwitchRow(stringResource(R.string.menu_stats), null, statsVisible) { onStats() }
                VSpace(8.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    SecondaryButton(stringResource(R.string.action_disconnect), onDisconnect, Modifier.weight(1f), destructive = true)
                    PrimaryButton(stringResource(R.string.action_continue), onClose, Modifier.weight(1f))
                }
            }
        }
    }
}
