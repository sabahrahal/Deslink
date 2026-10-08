package com.desklink.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.desklink.Found
import com.desklink.Transport

/** Lo que muestra la pantalla de inicio; lo calcula MainActivity con cada búsqueda. */
data class HomeState(
    val hosts: List<Found> = emptyList(),
    val scanning: Boolean = true,
    val scans: Int = 0,
    val lastHost: String? = null,
    val lastHostName: String? = null,
    val networks: String = "",
    /** PC al que se va a conectar solo (se puede cancelar). */
    val autoConnecting: Found? = null,
)

@Composable
fun HomeScreen(
    state: HomeState,
    onConnect: (Found) -> Unit,
    onConnectAddress: (String) -> Unit,
    onCancelAuto: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    var manual by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        Column(
            Modifier
                .widthIn(max = 720.dp)
                .fillMaxWidth()
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 16.dp)
                .animateContentSize(),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(
                        "DESKLINK",
                        style = TextStyle(brush = Palette.AccentGradient, fontSize = 30.sp, fontWeight = FontWeight.Black, letterSpacing = 4.sp),
                    )
                    Text("Tu tablet, segunda pantalla de tu PC", color = Palette.TextMuted)
                }
                IconButton(onClick = onOpenSettings) {
                    Icon(Icons.Filled.Settings, contentDescription = "Ajustes", tint = Palette.Text)
                }
            }
            VSpace(24.dp)

            StatusHeader(state)
            VSpace(16.dp)

            state.autoConnecting?.let { f ->
                NeonCard(glow = true) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Radar(Modifier.size(40.dp))
                        Spacer(Modifier.width(14.dp))
                        Column(Modifier.weight(1f)) {
                            Text("Conectando con ${f.title}…", style = MaterialTheme.typography.titleMedium)
                            Text("Es el último PC que usaste", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
                        }
                        TextButton(onClick = onCancelAuto) { Text("Cancelar", color = Palette.TextMuted) }
                    }
                }
                VSpace(12.dp)
            }

            for (host in state.hosts) {
                HostCard(host, isLast = host.ip == state.lastHost, onConnect = { onConnect(host) })
                VSpace(12.dp)
            }

            val last = state.lastHost
            if (last != null && state.hosts.none { it.ip == last } && state.scans > 0) {
                LastHostCard(last, state.lastHostName, onConnect = { onConnectAddress(last) })
                VSpace(12.dp)
            }

            AnimatedVisibility(state.hosts.isEmpty() && state.scans >= 2, enter = fadeIn(), exit = fadeOut()) {
                HelpCard(state.networks)
            }

            VSpace(8.dp)
            TextButton(onClick = { manual = true }, modifier = Modifier.align(Alignment.CenterHorizontally)) {
                Text("Conectar escribiendo la IP del PC", color = Palette.Accent)
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
private fun StatusHeader(state: HomeState) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Radar(Modifier.size(44.dp), active = state.scanning || state.hosts.isEmpty())
        Spacer(Modifier.width(12.dp))
        Column {
            val n = state.hosts.size
            Text(
                when {
                    n == 1 -> "PC encontrado"
                    n > 1 -> "$n PCs encontrados"
                    else -> "Buscando tu PC…"
                },
                style = MaterialTheme.typography.titleLarge,
            )
            Text(
                if (n > 0) "Toca uno para usar esta pantalla como monitor" else "Por cable USB y en tu red WiFi",
                color = Palette.TextMuted,
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
}

@Composable
private fun HostCard(host: Found, isLast: Boolean, onConnect: () -> Unit) {
    val cable = host.isCable
    val icon = when (host.transport) {
        Transport.CABLE, Transport.USB -> DlIcons.Usb
        Transport.WIFI -> DlIcons.Wifi
        Transport.ETHERNET -> DlIcons.Lan
        Transport.OTHER -> DlIcons.Desktop
    }
    NeonCard(glow = isLast || cable, onClick = onConnect) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconBadge(icon, if (cable) Palette.Success else Palette.Accent)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        host.title,
                        style = MaterialTheme.typography.titleMedium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false),
                    )
                    if (cable) Pill("Recomendado", Palette.Success)
                    if (isLast) Pill("Último", Palette.Accent)
                }
                Text(
                    buildString {
                        append(host.transport.label)
                        if (!cable) append(" · ").append(host.ip)
                        if (cable) append(" · menos retardo y carga la tablet")
                        if (!host.acceptsConfig) append(" · DeskLink antiguo en el PC")
                    },
                    color = Palette.TextMuted,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Spacer(Modifier.width(12.dp))
            GlowButton("Conectar", onConnect)
        }
    }
}

@Composable
private fun LastHostCard(address: String, name: String?, onConnect: () -> Unit) {
    NeonCard(onClick = onConnect) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconBadge(DlIcons.Desktop, Palette.TextMuted)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(name?.takeIf { it.isNotBlank() } ?: if (address == com.desklink.Link.CABLE_HOST) "PC por cable USB" else address,
                    style = MaterialTheme.typography.titleMedium)
                Text("Último PC usado · ahora no responde a la búsqueda", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
            }
            TextButton(onClick = onConnect) { Text("Probar", color = Palette.Accent) }
        }
    }
}

@Composable
private fun HelpCard(networks: String) {
    NeonCard {
        Column {
            Text("No encuentro tu PC", style = MaterialTheme.typography.titleMedium, color = Palette.Warning)
            VSpace(10.dp)
            HelpStep("1", "Abre DeskLink en el PC.")
            HelpStep("2", "Por cable: conecta el USB y acepta «Permitir depuración USB» en la tablet.")
            HelpStep("3", "Por WiFi: PC y tablet en la misma red, y permite DeskLink en el cortafuegos de Windows.")
            VSpace(8.dp)
            Text("Redes de esta tablet: $networks", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
            Text("Sigo buscando…", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun HelpStep(n: String, text: String) {
    Row(Modifier.padding(vertical = 4.dp), verticalAlignment = Alignment.Top) {
        Text(n, color = Palette.Accent, fontWeight = FontWeight.Black, modifier = Modifier.width(22.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun ManualDialog(onDismiss: () -> Unit, onConnect: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Palette.Surface,
        title = { Text("IP del PC") },
        text = {
            Column {
                Text(
                    "La ves en la ventana de DeskLink del PC (por ejemplo 192.168.1.20).",
                    color = Palette.TextMuted,
                    style = MaterialTheme.typography.bodySmall,
                )
                VSpace(12.dp)
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it.trim() },
                    singleLine = true,
                    placeholder = { Text("192.168.1.20") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    colors = OutlinedTextFieldDefaults.colors(focusedBorderColor = Palette.Accent, cursorColor = Palette.Accent),
                    textStyle = TextStyle(textAlign = TextAlign.Start, color = Palette.Text, fontSize = 18.sp),
                )
            }
        },
        confirmButton = { GlowButton("Conectar", { onConnect(text) }, enabled = text.isNotBlank()) },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancelar", color = Palette.TextMuted) } },
    )
}
