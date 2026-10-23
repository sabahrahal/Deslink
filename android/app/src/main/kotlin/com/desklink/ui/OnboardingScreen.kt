package com.desklink.ui

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
import androidx.compose.ui.unit.dp
import com.desklink.UsbDebugHint

/** Un paso de la guía de inicio: título, explicación y consejos en frases cortas. */
data class OnboardingPage(val id: String, val title: String, val body: String, val tips: List<String> = emptyList())

/**
 * Guía de inicio: sale la primera vez que se abre la app y luego desde Configuración avanzada > Ayuda. Recoge lo
 * que más dudas da: instalar el PC, el Wi-Fi y el cable (anclaje USB o depuración), la resolución y la escala de Windows, y el menú
 * durante la sesión.
 */
object Onboarding {
    val pages = listOf(
        OnboardingPage(
            "bienvenida",
            "Tu dispositivo como segunda pantalla",
            "Extendesk convierte el dispositivo en una pantalla más del PC, por Wi-Fi o por cable USB. Son tres cosas y listo.",
        ),
        OnboardingPage(
            "pc",
            "Instala Extendesk en el PC",
            "Ejecuta Extendesk-Setup.exe en el PC. Instala también la pantalla extra, que es lo que permite usar el dispositivo como un monitor más.",
            listOf(
                "Abre Extendesk en el PC y pulsa Iniciar.",
                "La calidad, el sonido y el táctil se eligen en la ventana del PC.",
            ),
        ),
        OnboardingPage(
            "cable",
            "Conecta el dispositivo",
            "Lo más fácil es el Wi-Fi: con el PC y el dispositivo en la misma red no hay que configurar nada. Abre Extendesk en los dos y pulsa Conectar.",
            listOf(
                "Wi-Fi: la misma red en el PC y en el dispositivo.",
                "Cable, sin complicaciones: enchufa el dispositivo y activa «Anclaje USB» en sus ajustes.",
                "Cable, más fluido (opcional): activa la depuración USB una sola vez (Información del dispositivo, toca 7 veces «Número de compilación», y en «Opciones de desarrollador» activa «Depuración USB»).",
            ),
        ),
        OnboardingPage(
            "tamano",
            "Que se vea a tu gusto",
            "El dispositivo arranca a su resolución, que es la más nítida. El tamaño de todo se elige en Windows: Configuración > Pantalla, eligiendo la pantalla DeskLink.",
            listOf(
                "Para ver todo más grande, sube la «Escala» (por ejemplo a 200 %) en vez de bajar la resolución.",
                "Si prefieres otra resolución, cámbiala con el dispositivo conectado: Extendesk la recuerda.",
            ),
        ),
        OnboardingPage(
            "sesion",
            "Durante la sesión",
            "El botón azul ☰ de la esquina abre el menú: estadísticas y desconectar.",
            listOf("Si te tapa algo, arrástralo a otro sitio y se queda ahí."),
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
                if (!last) LinkText("Saltar", onDone)
            }
            VSpace(16.dp)
            AnimatedContent(index, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "page", modifier = Modifier.weight(1f)) { i ->
                val page = pages[i]
                Column(
                    Modifier.verticalScroll(rememberScrollState()).testTag("onboarding:${page.id}"),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(page.title, style = DlText.LargeTitle)
                    Text(page.body, style = DlText.Body, color = palette.textSecondary)
                    if (page.tips.isNotEmpty()) {
                        DlCard {
                            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                for (tip in page.tips) Tip(tip)
                            }
                        }
                    }
                    if (page.id == "cable") {
                        SecondaryButton("Abrir «Anclaje USB»", onOpenTether)
                        if (usbDebug == UsbDebugHint.NONE) {
                            Text("La depuración USB ya está activa en este dispositivo.", style = DlText.Note, color = palette.success)
                        } else {
                            SecondaryButton("Abrir ajustes de depuración USB", onOpenUsbDebug)
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
                    LinkText("Atrás", { index-- })
                    Spacer(Modifier.size(8.dp))
                }
                PrimaryButton(if (last) "Empezar" else "Siguiente", { if (last) onDone() else index++ })
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
