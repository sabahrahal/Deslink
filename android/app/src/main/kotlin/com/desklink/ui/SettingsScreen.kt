package com.desklink.ui

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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.desklink.AppSettings
import com.desklink.DisplaySpec

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
            LinkText("‹ Volver", onBack, Modifier.padding(bottom = 4.dp))
            Text("Configuración avanzada", style = DlText.LargeTitle)
            VSpace(20.dp)

            DlCard {
                Column {
                    Text("La calidad, el sonido y el táctil se eligen en el PC", style = DlText.Headline)
                    Text(
                        "En la ventana de DeskLink del PC. La tablet usa lo que el PC decida y lo enseña al conectar.",
                        style = DlText.Note,
                        color = palette.textSecondary,
                    )
                }
            }
            VSpace(24.dp)

            SectionHeader("Conexión")
            DlCard(padding = 12.dp) {
                Column {
                    SwitchRow("Conectar solo al abrir", "Si encuentra el último PC que usaste.", settings.autoConnect) {
                        onChange(settings.copy(autoConnect = it))
                    }
                    RowDivider()
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Conectar escribiendo la IP", style = DlText.Body)
                            Text("La ves en la ventana de DeskLink del PC.", style = DlText.Note, color = palette.textSecondary)
                        }
                        SecondaryButton("Escribir IP", { manual = true })
                    }
                }
            }
            VSpace(24.dp)

            SectionHeader("Pantalla")
            DlCard(padding = 12.dp) {
                Column {
                    SwitchRow("Mostrar estadísticas", "fps, Mbps y retardo. También desde el botón del menú durante la sesión.", settings.showStats) {
                        onChange(settings.copy(showStats = it))
                    }
                }
            }
            VSpace(24.dp)

            SectionHeader("Ayuda")
            DlCard(padding = 12.dp) {
                Column {
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Diagnóstico", style = DlText.Body)
                            Text("Mide la conexión y la batería con el último PC.", style = DlText.Note, color = palette.textSecondary)
                        }
                        Spacer(Modifier.width(12.dp))
                        SecondaryButton("Abrir", onOpenDiagnostics)
                    }
                    RowDivider()
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Guía de inicio", style = DlText.Body)
                            Text("Cable, resolución y escala de Windows, y el menú.", style = DlText.Note, color = palette.textSecondary)
                        }
                        Spacer(Modifier.width(12.dp))
                        SecondaryButton("Ver", onOpenGuide)
                    }
                    RowDivider()
                    Text(
                        "DeskLink $versionName · pantalla ${display.width}×${display.height} a ${display.refreshHz} Hz",
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
        title = { Text("IP del PC", style = DlText.Title) },
        text = {
            Column {
                Text("Por ejemplo 192.168.1.20.", style = DlText.Note, color = palette.textSecondary)
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
        confirmButton = { PrimaryButton("Conectar", { onConnect(text) }, enabled = text.isNotBlank()) },
        dismissButton = { SecondaryButton("Cancelar", onDismiss) },
    )
}
