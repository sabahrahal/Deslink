package app.extendesk.ui

import androidx.annotation.StringRes
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import app.extendesk.R
import app.extendesk.UsbDebugHint

/** Un paso de la guía de inicio: título, explicación y consejos en frases cortas. */
/** Una página de la guía; los textos están en res/values/strings.xml. */
data class OnboardingPage(
    val id: String,
    @StringRes val title: Int,
    @StringRes val body: Int,
    val tips: List<Int> = emptyList(),
)

/**
 * Guía de inicio: sale la primera vez que se abre la app y luego desde Configuración avanzada > Ayuda. Recoge lo
 * que más dudas da: instalar el PC, el Wi-Fi y el cable (anclaje USB o depuración), la resolución y la escala de Windows, y el menú
 * durante la sesión.
 */
object Onboarding {
    val pages = listOf(
        OnboardingPage("bienvenida", R.string.onb_welcome_title, R.string.onb_welcome_body),
        OnboardingPage(
            "pc",
            R.string.onb_pc_title,
            R.string.onb_pc_body,
            listOf(R.string.onb_pc_tip_1, R.string.onb_pc_tip_2),
        ),
        OnboardingPage(
            "cable",
            R.string.onb_cable_title,
            R.string.onb_cable_body,
            listOf(R.string.onb_cable_tip_1, R.string.onb_cable_tip_2, R.string.onb_cable_tip_3),
        ),
        OnboardingPage(
            "tamano",
            R.string.onb_size_title,
            R.string.onb_size_body,
            listOf(R.string.onb_size_tip_1, R.string.onb_size_tip_2),
        ),
        OnboardingPage(
            "sesion",
            R.string.onb_session_title,
            R.string.onb_session_body,
            listOf(R.string.onb_session_tip_1, R.string.onb_session_tip_2, R.string.onb_session_tip_3),
        ),
    )
}

@Composable
fun OnboardingScreen(usbDebug: UsbDebugHint, onOpenUsbDebug: () -> Unit, onDone: () -> Unit, onOpenTether: () -> Unit = {}) {
    var index by rememberSaveable { mutableIntStateOf(0) }
    val pages = Onboarding.pages
    val last = index == pages.lastIndex
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        Column(
            Modifier
                .widthIn(max = 560.dp)
                .fillMaxWidth()
                .safeDrawingPadding()
                .padding(horizontal = 24.dp, vertical = 24.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                BrandMark(Modifier.weight(1f), iconHeight = 20.dp, textSize = DlType.HeadlineSize)
                if (!last) LinkText(stringResource(R.string.action_skip), onDone)
            }
            VSpace(16.dp)
            AnimatedContent(index, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "page", modifier = Modifier.weight(1f)) { i ->
                val page = pages[i]
                Column(
                    Modifier.verticalScroll(rememberScrollState()).testTag("onboarding:${page.id}"),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(stringResource(page.title), style = DlText.LargeTitle)
                    Text(stringResource(page.body), style = DlText.Body, color = palette.textSecondary)
                    if (page.tips.isNotEmpty()) {
                        DlCard {
                            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                for (tip in page.tips) Tip(stringResource(tip))
                            }
                        }
                    }
                    if (page.id == "cable") {
                        SecondaryButton(stringResource(R.string.open_tethering), onOpenTether)
                        if (usbDebug == UsbDebugHint.NONE) {
                            Text(stringResource(R.string.usb_debug_already_on), style = DlText.Note, color = palette.success)
                        } else {
                            SecondaryButton(stringResource(R.string.open_usb_debug_settings), onOpenUsbDebug)
                        }
                    }
                }
            }
            VSpace(16.dp)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    pages.indices.forEach { i ->
                        Box(
                            Modifier
                                .size(if (i == index) 8.dp else 6.dp)
                                .clip(CircleShape)
                                .background(if (i == index) palette.accent else palette.separator),
                        )
                    }
                }
                if (index > 0) {
                    LinkText(stringResource(R.string.action_back), { index-- })
                    Spacer(Modifier.size(8.dp))
                }
                PrimaryButton(stringResource(if (last) R.string.action_start else R.string.action_next), { if (last) onDone() else index++ })
            }
        }
    }
}

@Composable
private fun Tip(text: String) {
    Row {
        Text("•", style = DlText.Body, color = palette.accent, modifier = Modifier.padding(end = 8.dp))
        Text(text, style = DlText.Body)
    }
}
