package com.desklink.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.desklink.AppSettings
import com.desklink.CodecChoice
import com.desklink.DisplaySpec
import com.desklink.Quality
import com.desklink.ResolutionScale

/**
 * Ajustes. Cada sección es una tarjeta; para un ajuste nuevo (táctil, audio, batería…) basta con añadir
 * su campo en [AppSettings] y una [SettingsSection] aquí.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun SettingsScreen(
    settings: AppSettings,
    display: DisplaySpec,
    hevcSupported: Boolean,
    versionName: String,
    onChange: (AppSettings) -> Unit,
    onOpenDiagnostics: () -> Unit,
    onBack: () -> Unit,
) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        Column(
            Modifier
                .widthIn(max = 720.dp)
                .fillMaxWidth()
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onBack) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Volver", tint = Palette.Text)
                }
                Spacer(Modifier.width(4.dp))
                Text("Ajustes", style = MaterialTheme.typography.headlineSmall)
            }
            VSpace(16.dp)

            SettingsSection("Imagen", "Se aplican al conectar. Con un DeskLink antiguo en el PC manda la configuración del PC.") {
                ChoiceGroup("Resolución del monitor") {
                    for (r in ResolutionScale.entries) {
                        val (w, h) = display.scaled(r.percent)
                        OptionChip(r.label, "${w}×$h", settings.resolution == r) { onChange(settings.copy(resolution = r)) }
                    }
                }
                Hint(settings.resolution.hint)
                ChoiceGroup("Calidad") {
                    for (q in Quality.entries) {
                        OptionChip(q.label, q.hint, settings.quality == q) { onChange(settings.copy(quality = q)) }
                    }
                }
                ChoiceGroup("Fluidez") {
                    for (f in AppSettings.fpsOptions(display.refreshHz)) {
                        OptionChip(
                            if (f == 0) "Automática" else "$f fps",
                            if (f == 0) "La del PC" else null,
                            settings.fps == f,
                        ) { onChange(settings.copy(fps = f)) }
                    }
                }
                Hint("Tu pantalla llega a ${display.refreshHz} Hz. Menos fps gastan menos batería.")
                ChoiceGroup("Códec de vídeo") {
                    for (c in CodecChoice.entries) {
                        val enabled = c != CodecChoice.HEVC || hevcSupported
                        OptionChip(c.label, c.hint, settings.codec == c, enabled) { onChange(settings.copy(codec = c)) }
                    }
                }
                if (!hevcSupported) Hint("Esta tablet no decodifica HEVC: se usa H.264.")
            }

            SettingsSection("Pantalla") {
                SwitchRow("Mostrar estadísticas", "Resolución, fps, Mbps, retardo y batería. También con 4 dedos a la vez.", settings.showStats) {
                    onChange(settings.copy(showStats = it))
                }
                HorizontalDivider(color = Palette.Outline)
                SwitchRow("Mantener la pantalla encendida", "Mientras estés conectado. Si se pierde el PC, se apaga sola a los 2 minutos.", settings.keepScreenOn) {
                    onChange(settings.copy(keepScreenOn = it))
                }
                HorizontalDivider(color = Palette.Outline)
                SwitchRow("Conectar solo al abrir", "Si encuentra el último PC que usaste, se conecta sin tocar nada.", settings.autoConnect) {
                    onChange(settings.copy(autoConnect = it))
                }
            }

            SettingsSection("Ayuda") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Diagnóstico y recomendaciones", style = MaterialTheme.typography.titleMedium)
                        Text("Mide la conexión, el WiFi y la batería con el último PC.", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
                    }
                    TextButton(onClick = onOpenDiagnostics) { Text("Abrir", color = Palette.Accent) }
                }
                HorizontalDivider(color = Palette.Outline)
                Text(
                    "DeskLink $versionName · pantalla ${display.width}×${display.height} a ${display.refreshHz} Hz",
                    color = Palette.TextMuted,
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(top = 10.dp),
                )
            }
        }
    }
}

@Composable
fun SettingsSection(title: String, subtitle: String? = null, content: @Composable () -> Unit) {
    Text(title.uppercase(), style = SectionLabel, modifier = Modifier.padding(start = 4.dp, bottom = 8.dp))
    NeonCard {
        Column {
            if (subtitle != null) {
                Text(subtitle, color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
                VSpace(8.dp)
            }
            content()
        }
    }
    VSpace(20.dp)
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ChoiceGroup(title: String, content: @Composable () -> Unit) {
    Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 10.dp, bottom = 8.dp))
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        content()
    }
}

@Composable
private fun Hint(text: String) {
    Text(text, color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(top = 6.dp))
}
