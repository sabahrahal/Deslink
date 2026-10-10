package com.desklink.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
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
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.desklink.Found
import com.desklink.HostGroups
import com.desklink.Link
import com.desklink.PcEntry
import com.desklink.Transport
import com.desklink.UsbDebugHint

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
    /** Enchufada por USB sin depuración USB: qué falta para que el cable funcione sin elegir nada. */
    val usbDebug: UsbDebugHint = UsbDebugHint.NONE,
    /** Enchufada con la depuración USB activa: el cable aparece en unos segundos, cuando el PC lo prepara. */
    val cableExpected: Boolean = false,
) {
    /** El PC ya responde por la red pero el cable aún no: se avisa de que espere. */
    val waitingCable: Boolean get() =
        cableExpected && scans < HostGroups.CABLE_WAIT_SCANS && hosts.isNotEmpty() && hosts.none { it.isCable }

    /** Un PC por tarjeta, con el cable USB y el Wi-Fi juntos. */
    val pcs: List<PcEntry> get() = HostGroups.group(hosts)
}

/**
 * Inicio: el nombre de la app, una tarjeta por PC encontrado (con «Cable USB» y «Wi-Fi» como dos opciones de la
 * misma tarjeta) y un enlace a Configuración avanzada. Nada más: calidad, sonido y táctil se eligen en el PC.
 */
@Composable
fun HomeScreen(
    state: HomeState,
    onConnect: (Found) -> Unit,
    onConnectAddress: (String) -> Unit,
    onCancelAuto: () -> Unit,
    onOpenAdvanced: () -> Unit,
    onOpenUsbDebug: () -> Unit = {},
) {
    // Camino elegido en cada tarjeta (true = cable USB). De serie, el cable.
    val preferUsb = remember { mutableStateMapOf<String, Boolean>() }
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        Column(
            Modifier
                .widthIn(max = 640.dp)
                .fillMaxWidth()
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 24.dp)
                .animateContentSize(),
        ) {
            Text("DeskLink", style = DlText.LargeTitle)
            Text("Tu tablet como segunda pantalla", style = DlText.Body, color = palette.textSecondary)
            VSpace(24.dp)

            val pcs = state.pcs
            state.autoConnecting?.let { f ->
                DlCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(Modifier.size(20.dp), color = palette.accent, strokeWidth = 2.dp)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text("Conectando con ${f.title}…", style = DlText.Headline)
                            Text("Es el último PC que usaste", style = DlText.Note, color = palette.textSecondary)
                        }
                        SecondaryButton("Cancelar", onCancelAuto)
                    }
                }
                VSpace(12.dp)
            }

            if (state.waitingCable && state.autoConnecting == null) {
                DlCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(Modifier.size(20.dp), color = palette.accent, strokeWidth = 2.dp)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text("Preparando el cable USB…", style = DlText.Headline)
                            Text("Tarda unos segundos. Espera y se conecta solo, o conecta ya por Wi-Fi.", style = DlText.Note, color = palette.textSecondary)
                        }
                    }
                }
                VSpace(12.dp)
            }

            if (state.usbDebug != UsbDebugHint.NONE) {
                UsbDebugCard(state.usbDebug, onOpenUsbDebug)
                VSpace(12.dp)
            }

            if (pcs.isEmpty()) {
                Searching(state)
            } else {
                SectionHeader(if (pcs.size == 1) "Tu PC" else "Tus PC")
                for (pc in pcs) {
                    val usb = preferUsb[pc.key] ?: true
                    PcCard(
                        pc = pc,
                        isLast = pc.matches(state.lastHost),
                        useUsb = usb,
                        onChooseUsb = { preferUsb[pc.key] = it },
                        onConnect = { onConnect(pc.route(usb)) },
                    )
                    VSpace(12.dp)
                }
            }

            val last = state.lastHost
            if (last != null && pcs.none { it.matches(last) } && state.scans > 0) {
                VSpace(4.dp)
                LastHostCard(last, state.lastHostName, onConnect = { onConnectAddress(last) })
            }

            VSpace(20.dp)
            LinkText("Configuración avanzada", onOpenAdvanced, Modifier.align(Alignment.CenterHorizontally))
        }
    }
}

/** Enchufada al PC sin depuración USB: cómo dejar el cable funcionando sin elegir «Anclaje USB» cada vez. */
@Composable
private fun UsbDebugCard(hint: UsbDebugHint, onOpen: () -> Unit) {
    DlCard(Modifier.testTag("usb-debug")) {
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text("Activa la depuración USB", style = DlText.Headline)
            Text(
                "Así el cable funciona solo al enchufarlo, sin elegir «Anclaje USB» ni ningún modo cada vez. Se hace una sola vez.",
                style = DlText.Body,
                color = palette.textSecondary,
            )
            if (hint == UsbDebugHint.ENABLE_DEVELOPER) {
                Text("1. En Información de la tablet, toca 7 veces «Número de compilación».", style = DlText.Body)
                Text("2. Vuelve atrás, entra en «Opciones de desarrollador» y activa «Depuración USB».", style = DlText.Body)
            } else {
                Text("1. En «Opciones de desarrollador», activa «Depuración USB».", style = DlText.Body)
            }
            Text(
                "${if (hint == UsbDebugHint.ENABLE_DEVELOPER) 3 else 2}. Al enchufar al PC, marca «Permitir siempre desde este ordenador» y acepta.",
                style = DlText.Body,
            )
            VSpace(4.dp)
            PrimaryButton("Abrir ajustes", onOpen)
        }
    }
}

@Composable
private fun Searching(state: HomeState) {
    var help by rememberSaveable { mutableStateOf(false) }
    DlCard {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.size(20.dp), color = palette.accent, strokeWidth = 2.dp)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text("Buscando tu PC…", style = DlText.Headline)
                    Text("Por cable USB y en tu red Wi-Fi", style = DlText.Note, color = palette.textSecondary)
                }
            }
            if (state.scans >= 2) {
                VSpace(8.dp)
                LinkText(if (help) "Ocultar ayuda" else "¿No aparece?", { help = !help })
                AnimatedVisibility(help) {
                    Column(Modifier.padding(start = 8.dp, top = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text("1. Abre DeskLink en el PC y pulsa Iniciar.", style = DlText.Body)
                        Text("2. Por cable: activa la depuración USB en la tablet, conecta el USB y acepta «Permitir depuración USB».", style = DlText.Body)
                        Text("3. Por Wi-Fi: PC y tablet en la misma red, y DeskLink permitido en el cortafuegos de Windows.", style = DlText.Body)
                        Text("Redes de esta tablet: ${state.networks}", style = DlText.Note, color = palette.textSecondary)
                    }
                }
            }
        }
    }
}

@Composable
private fun PcCard(pc: PcEntry, isLast: Boolean, useUsb: Boolean, onChooseUsb: (Boolean) -> Unit, onConnect: () -> Unit) {
    val route = pc.route(useUsb)
    DlCard(Modifier.testTag("pc:${pc.key}")) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(pc.name, style = DlText.Headline, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(
                        HostGroups.describe(route) + if (isLast) " · el último que usaste" else "",
                        style = DlText.Note,
                        color = palette.textSecondary,
                    )
                    if (!route.acceptsConfig) {
                        Text("El DeskLink del PC es antiguo: actualízalo.", style = DlText.Note, color = palette.warning)
                    }
                }
                Spacer(Modifier.width(12.dp))
                PrimaryButton("Conectar", onConnect)
            }
            if (pc.hasBoth) {
                VSpace(12.dp)
                val network = pc.network.first().transport
                Segmented(
                    options = listOf("Cable USB", if (network == Transport.WIFI) "Wi-Fi" else "Red"),
                    selected = if (useUsb) 0 else 1,
                    onSelect = { onChooseUsb(it == 0) },
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}

@Composable
private fun LastHostCard(address: String, name: String?, onConnect: () -> Unit) {
    DlCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    name?.takeIf { it.isNotBlank() } ?: if (address == Link.CABLE_HOST) "PC por cable USB" else address,
                    style = DlText.Headline,
                )
                Text("Último PC usado · ahora no responde", style = DlText.Note, color = palette.textSecondary)
            }
            SecondaryButton("Probar", onConnect)
        }
    }
}
