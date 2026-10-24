package app.extendesk.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import app.extendesk.AppSettings
import app.extendesk.DisplaySpec
import app.extendesk.R

/**
 * Configuración avanzada de la tablet: solo lo que es de la tablet. La calidad, el sonido y el táctil se eligen en
 * el PC (docs/ajustes.md), así que aquí no aparecen.
 */
@Composable
fun SettingsScreen(
    settings: AppSettings,
    display: DisplaySpec,
    versionName: String,
    onChange: (AppSettings) -> Unit,
    onConnectAddress: (String) -> Unit,
    onOpenDiagnostics: () -> Unit,
    onBack: () -> Unit,
    onOpenGuide: () -> Unit = {},
    onOpenHelp: () -> Unit = {},
) {
    var manual by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        Column(
            Modifier
                .widthIn(max = 640.dp)
                .fillMaxWidth()
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 16.dp),
        ) {
            LinkText(stringResource(R.string.settings_back), onBack, Modifier.padding(bottom = 4.dp))
            Text(stringResource(R.string.advanced_settings), style = DlText.LargeTitle)
            VSpace(20.dp)

            DlCard {
                Column {
                    Text(stringResource(R.string.settings_quality_on_pc), style = DlText.Headline)
                    Text(
                        stringResource(R.string.settings_quality_on_pc_note),
                        style = DlText.Note,
                        color = palette.textSecondary,
                    )
                }
            }
            VSpace(24.dp)

            SectionHeader(stringResource(R.string.settings_connection))
            DlCard(padding = 12.dp) {
                Column {
                    SwitchRow(stringResource(R.string.settings_auto_connect), stringResource(R.string.settings_auto_connect_note), settings.autoConnect) {
                        onChange(settings.copy(autoConnect = it))
                    }
                    RowDivider()
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.settings_connect_ip), style = DlText.Body)
                            Text(stringResource(R.string.settings_connect_ip_note), style = DlText.Note, color = palette.textSecondary)
                        }
                        SecondaryButton(stringResource(R.string.settings_type_ip), { manual = true })
                    }
                }
            }
            VSpace(24.dp)

            SectionHeader(stringResource(R.string.settings_help))
            DlCard(padding = 12.dp) {
                Column {
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.settings_help_options), style = DlText.Body)
                            Text(stringResource(R.string.settings_help_options_note), style = DlText.Note, color = palette.textSecondary)
                        }
                        Spacer(Modifier.width(12.dp))
                        SecondaryButton(stringResource(R.string.action_open), onOpenHelp)
                    }
                    RowDivider()
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.diagnostics), style = DlText.Body)
                            Text(stringResource(R.string.settings_diagnostics_note), style = DlText.Note, color = palette.textSecondary)
                        }
                        Spacer(Modifier.width(12.dp))
                        SecondaryButton(stringResource(R.string.action_open), onOpenDiagnostics)
                    }
                    RowDivider()
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.settings_guide), style = DlText.Body)
                            Text(stringResource(R.string.settings_guide_note), style = DlText.Note, color = palette.textSecondary)
                        }
                        Spacer(Modifier.width(12.dp))
                        SecondaryButton(stringResource(R.string.action_view), onOpenGuide)
                    }
                    RowDivider()
                    Text(
                        stringResource(R.string.settings_version, versionName, display.width, display.height, display.refreshHz),
                        style = DlText.Note,
                        color = palette.textSecondary,
                        modifier = Modifier.padding(vertical = 10.dp),
                    )
                }
            }
        }
    }
    if (manual) {
        ManualDialog(onDismiss = { manual = false }, onConnect = {
            manual = false
            onConnectAddress(it)
        })
    }
}

@Composable
private fun ManualDialog(onDismiss: () -> Unit, onConnect: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = palette.surface,
        titleContentColor = palette.text,
        textContentColor = palette.text,
        title = { Text(stringResource(R.string.manual_title), style = DlText.Title) },
        text = {
            Column {
                Text(stringResource(R.string.manual_example), style = DlText.Note, color = palette.textSecondary)
                VSpace(12.dp)
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it.trim() },
                    singleLine = true,
                    placeholder = { Text("192.168.1.20") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = palette.accent,
                        cursorColor = palette.accent,
                        focusedTextColor = palette.text,
                        unfocusedTextColor = palette.text,
                    ),
                )
            }
        },
        confirmButton = { PrimaryButton(stringResource(R.string.action_connect), { onConnect(text) }, enabled = text.isNotBlank()) },
        dismissButton = { SecondaryButton(stringResource(R.string.action_cancel), onDismiss) },
    )
}
