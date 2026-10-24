package app.extendesk.ui

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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.extendesk.Found
import app.extendesk.HostGroups
import app.extendesk.Link
import app.extendesk.PcEntry
import app.extendesk.R
import app.extendesk.UsbDebugHint

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
    /** Enchufada por USB sin depuración USB: falta activar el anclaje para que el cable funcione. */
    val usbDebug: UsbDebugHint = UsbDebugHint.NONE,
    /** Enchufada con la depuración USB activa: el cable aparece en unos segundos, cuando el PC lo prepara. */
    val cableExpected: Boolean = false,
) {
    /** Enchufada pero el PC aún no llega por el cable: un solo paso, activar el anclaje USB. */
    val showCableStep: Boolean get() = usbDebug != UsbDebugHint.NONE && pcs.none { it.usb != null }

    /** El PC ya responde por la red pero el cable aún no: se avisa de que espere. */
    val waitingCable: Boolean get() =
        cableExpected && scans < HostGroups.CABLE_WAIT_SCANS && hosts.isNotEmpty() && hosts.none { it.isCable }

    /** Un PC por tarjeta, con el cable USB y el Wi-Fi juntos. */
    val pcs: List<PcEntry> get() = HostGroups.group(hosts)
}

/**
 * Inicio: el nombre de la app, una tarjeta por PC encontrado (conecta por cable USB si llega por él, si no por Wi-Fi:
 * no hay nada que elegir) y un enlace a Configuración avanzada. Nada más: calidad y sonido se eligen en el PC.
 */
@Composable
fun HomeScreen(
    state: HomeState,
    onConnect: (Found) -> Unit,
    onConnectAddress: (String) -> Unit,
    onCancelAuto: () -> Unit,
    onOpenAdvanced: () -> Unit,
    onOpenUsbDebug: () -> Unit = {},
    onOpenTether: () -> Unit = {},
) {
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
            BrandMark()
            VSpace(6.dp)
            Text(stringResource(R.string.home_tagline), style = DlText.Body, color = palette.textSecondary)
            VSpace(24.dp)

            val pcs = state.pcs
            state.autoConnecting?.let { f ->
                DlCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(Modifier.size(20.dp), color = palette.accent, strokeWidth = 2.dp)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.home_connecting_to, f.title), style = DlText.Headline)
                            Text(stringResource(R.string.home_last_pc_used), style = DlText.Note, color = palette.textSecondary)
                        }
                        SecondaryButton(stringResource(R.string.action_cancel), onCancelAuto)
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
                            Text(stringResource(R.string.home_preparing_cable), style = DlText.Headline)
                            Text(stringResource(R.string.home_preparing_cable_note), style = DlText.Note, color = palette.textSecondary)
                        }
                    }
                }
                VSpace(12.dp)
            }

            if (state.showCableStep) {
                CableStepCard(state.usbDebug, onOpenTether, onOpenUsbDebug)
                VSpace(12.dp)
            }

            if (pcs.isEmpty()) {
                Searching(state)
            } else {
                SectionHeader(stringResource(if (pcs.size == 1) R.string.home_your_pc else R.string.home_your_pcs))
                for (pc in pcs) {
                    PcCard(pc = pc, isLast = pc.matches(state.lastHost), onConnect = { onConnect(pc.route()) })
                    VSpace(12.dp)
                }
            }

            val last = state.lastHost
            if (last != null && pcs.none { it.matches(last) } && state.scans > 0) {
                VSpace(4.dp)
                LastHostCard(last, state.lastHostName, onConnect = { onConnectAddress(last) })
            }

            VSpace(20.dp)
            LinkText(stringResource(R.string.advanced_settings), onOpenAdvanced, Modifier.align(Alignment.CenterHorizontally))
        }
    }
}

/**
 * Enchufada al PC y el cable aún no va: un solo paso, activar «Anclaje USB» (sin modo desarrollador). La depuración
 * USB queda como consejo opcional, plegado: con ella el cable conecta solo al enchufarlo, sin repetir el paso.
 */
@Composable
private fun CableStepCard(hint: UsbDebugHint, onOpenTether: () -> Unit, onOpenUsbDebug: () -> Unit) {
    var more by rememberSaveable { mutableStateOf(false) }
    DlCard(Modifier.testTag("cable-step")) {
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(stringResource(R.string.cable_title), style = DlText.Headline)
            Text(
                stringResource(R.string.cable_body),
                style = DlText.Body,
                color = palette.textSecondary,
            )
            VSpace(4.dp)
            PrimaryButton(stringResource(R.string.open_tethering), onOpenTether)
            VSpace(4.dp)
            LinkText(stringResource(if (more) R.string.action_hide else R.string.cable_auto_optional), { more = !more })
            AnimatedVisibility(more) {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(stringResource(R.string.cable_debug_intro), style = DlText.Body)
                    if (hint == UsbDebugHint.ENABLE_DEVELOPER) {
                        Text(stringResource(R.string.cable_debug_step_build), style = DlText.Body)
                        Text(stringResource(R.string.cable_debug_step_dev), style = DlText.Body)
                    } else {
                        Text(stringResource(R.string.cable_debug_step_dev_only), style = DlText.Body)
                    }
                    Text(
                        stringResource(R.string.cable_debug_step_allow, if (hint == UsbDebugHint.ENABLE_DEVELOPER) 3 else 2),
                        style = DlText.Body,
                    )
                    SecondaryButton(stringResource(R.string.open_usb_debug_settings), onOpenUsbDebug)
                }
            }
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
                    Text(stringResource(R.string.searching_pc), style = DlText.Headline)
                    Text(stringResource(R.string.searching_pc_note), style = DlText.Note, color = palette.textSecondary)
                }
            }
            if (state.scans >= 2) {
                VSpace(8.dp)
                LinkText(stringResource(if (help) R.string.hide_help else R.string.not_showing_up), { help = !help })
                AnimatedVisibility(help) {
                    Column(Modifier.padding(start = 8.dp, top = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(stringResource(R.string.search_help_1), style = DlText.Body)
                        Text(stringResource(R.string.search_help_2), style = DlText.Body)
                        Text(stringResource(R.string.search_help_3), style = DlText.Body)
                        Text(stringResource(R.string.device_networks, state.networks), style = DlText.Note, color = palette.textSecondary)
                    }
                }
            }
        }
    }
}

@Composable
private fun PcCard(pc: PcEntry, isLast: Boolean, onConnect: () -> Unit) {
    val route = pc.route()
    DlCard(Modifier.testTag("pc:${pc.key}")) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(pc.name, style = DlText.Headline, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(
                    HostGroups.describe(route) + if (isLast) stringResource(R.string.pc_last_used_suffix) else "",
                    style = DlText.Note,
                    color = palette.textSecondary,
                )
                if (!route.acceptsConfig) {
                    Text(stringResource(R.string.pc_outdated), style = DlText.Note, color = palette.warning)
                }
            }
            Spacer(Modifier.width(12.dp))
            PrimaryButton(stringResource(R.string.action_connect), onConnect)
        }
    }
}

@Composable
private fun LastHostCard(address: String, name: String?, onConnect: () -> Unit) {
    DlCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    name?.takeIf { it.isNotBlank() } ?: if (address == Link.CABLE_HOST) stringResource(R.string.pc_by_usb_cable) else address,
                    style = DlText.Headline,
                )
                Text(stringResource(R.string.last_pc_not_responding), style = DlText.Note, color = palette.textSecondary)
            }
            SecondaryButton(stringResource(R.string.action_try), onConnect)
        }
    }
}
