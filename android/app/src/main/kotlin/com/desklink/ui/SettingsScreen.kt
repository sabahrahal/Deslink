package com.desklink.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
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
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.desklink.AppSettings
import com.desklink.CodecChoice
import com.desklink.DisplaySpec
import com.desklink.PowerChoice
import com.desklink.PowerProfile
import com.desklink.Quality
import com.desklink.ResolutionScale
import com.desklink.TouchChoice

/**
 * Ajustes: el único sitio donde se configura DeskLink (la app del PC no tiene calidad ni táctil). Arriba lo que
 * casi todo el mundo toca (perfil, sonido y táctil); el resto va plegado en «Avanzado». Todo se manda al PC
 * al conectar.
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
    // Abierto de entrada si ya hay algo fijado ahí: que se vea qué está mandando sobre el perfil.
    var advancedOpen by rememberSaveable { mutableStateOf(settings.hasAdvancedOverrides) }
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
                Column {
                    Text("Ajustes", style = MaterialTheme.typography.headlineSmall)
                    Text("Se eligen aquí y se mandan al PC al conectar.", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
                }
            }
            VSpace(16.dp)

            SettingsSection("Perfil") {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (p in PowerChoice.entries) {
                        OptionChip(p.label, p.hint, settings.power == p) { onChange(settings.copy(power = p)) }
                    }
                }
                Hint(PowerProfile.fromId(settings.power.id).summary())
                if (settings.hasAdvancedOverrides) {
                    Hint("Tienes valores fijos en Avanzado: mandan sobre el perfil.", Palette.Warning)
                }
            }

            SettingsSection("Sonido") {
                SwitchRow("Sonido del PC en la tablet", "Lo que suena en el PC se oye aquí. Apagado, sigue sonando en el PC.", settings.audio) {
                    onChange(settings.copy(audio = it))
                }
            }

            SettingsSection("Táctil") {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (t in TouchChoice.entries) {
                        OptionChip(t.label, t.hint, settings.touch == t) { onChange(settings.copy(touch = t)) }
                    }
                }
            }

            AdvancedHeader(advancedOpen) { advancedOpen = !advancedOpen }
            AnimatedVisibility(advancedOpen, enter = expandVertically() + fadeIn(), exit = shrinkVertically() + fadeOut()) {
                Column {
                    SettingsSection("Imagen", "«Automática» deja que decida el perfil. Un valor fijo manda sobre él (salvo si la tablet baja de perfil para no descargarse por cable).") {
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
                                    if (f == 0) "La del perfil" else null,
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
                        if (settings.hasAdvancedOverrides) {
                            TextButton(
                                onClick = {
                                    val d = AppSettings()
                                    onChange(settings.copy(resolution = d.resolution, quality = d.quality, fps = d.fps, codec = d.codec))
                                },
                                modifier = Modifier.padding(top = 4.dp),
                            ) { Text("Volver a lo automático", color = Palette.Accent) }
                        }
                    }

                    SettingsSection("Batería") {
                        SwitchRow("Adaptar a la carga por USB", "Por cable, no gastar más de lo que entra: la tablet no se descarga.", settings.powerAdapt) {
                            onChange(settings.copy(powerAdapt = it))
                        }
                    }

                    SettingsSection("Pantalla y conexión") {
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

/** Cabecera plegable de «Avanzado». */
@Composable
private fun AdvancedHeader(open: Boolean, onToggle: () -> Unit) {
    val angle by animateFloatAsState(if (open) 180f else 0f, label = "chevron")
    NeonCard(onClick = onToggle) {
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text("Avanzado", style = MaterialTheme.typography.titleMedium)
                Text("Resolución, fps, calidad, códec, batería y pantalla", color = Palette.TextMuted, style = MaterialTheme.typography.bodySmall)
            }
            Icon(
                Icons.Filled.KeyboardArrowDown,
                contentDescription = if (open) "Plegar" else "Desplegar",
                tint = Palette.Accent,
                modifier = Modifier.rotate(angle),
            )
        }
    }
    VSpace(20.dp)
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
private fun Hint(text: String, color: Color = Palette.TextMuted) {
    Text(text, color = color, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(top = 6.dp))
}
