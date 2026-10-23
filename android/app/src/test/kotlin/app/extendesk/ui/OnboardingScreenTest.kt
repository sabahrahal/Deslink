package app.extendesk.ui

import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.extendesk.UsbDebugHint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

/** La guía de inicio enseña cada paso y sus consejos donde deben (Robolectric, sin tablet). */
@RunWith(AndroidJUnit4::class)
@Config(sdk = [34], qualifiers = "es")
class OnboardingScreenTest {
    @get:Rule val compose = createComposeRule()

    private fun show(usb: UsbDebugHint = UsbDebugHint.ENABLE_DEVELOPER, onUsb: () -> Unit = {}, onDone: () -> Unit = {}) {
        compose.setContent {
            ExtendeskTheme(dark = true) { OnboardingScreen(usb, onUsb, onDone) }
        }
    }

    private fun next(times: Int) = repeat(times) {
        compose.onNodeWithText("Siguiente").performClick()
        compose.waitForIdle()
    }

    @Test
    fun walksEveryStepAndFinishes() {
        var done = false
        show(onDone = { done = true })
        compose.onNode(hasTestTag("onboarding:bienvenida")).assertIsDisplayed()
        compose.onAllNodesWithText("Atrás").assertCountEquals(0)
        next(Onboarding.pages.size - 1)
        compose.onNode(hasTestTag("onboarding:sesion")).assertIsDisplayed()
        compose.onAllNodesWithText("Saltar").assertCountEquals(0)
        compose.onNodeWithText("Empezar").performClick()
        assertTrue(done)
    }

    @Test
    fun skipEndsTheGuide() {
        var done = false
        show(onDone = { done = true })
        compose.onNodeWithText("Saltar").performClick()
        assertTrue(done)
    }

    @Test
    fun cableStepOpensSettingsUntilDebuggingIsOn() {
        var opened = 0
        show(onUsb = { opened++ })
        next(2)
        compose.onNode(hasTestTag("onboarding:cable")).assertIsDisplayed()
        compose.onNodeWithText("Abrir ajustes de depuración USB").performScrollTo().performClick()
        assertEquals(1, opened)
    }

    @Test
    fun cableStepOffersTetheringWithoutDeveloperMode() {
        var tether = 0
        compose.setContent {
            ExtendeskTheme(dark = true) { OnboardingScreen(UsbDebugHint.ENABLE_DEVELOPER, {}, {}, onOpenTether = { tether++ }) }
        }
        next(2)
        compose.onNodeWithText("Abrir «Anclaje USB»").performScrollTo().performClick()
        assertEquals(1, tether)
    }

    @Test
    fun cableStepSaysWhenDebuggingIsAlreadyOn() {
        show(usb = UsbDebugHint.NONE)
        next(2)
        compose.onNodeWithText("La depuración USB ya está activa en este dispositivo.").performScrollTo().assertIsDisplayed()
        compose.onAllNodesWithText("Abrir ajustes de depuración USB").assertCountEquals(0)
    }

    @Test
    fun sizeStepRecommendsScaleOverResolution() {
        show()
        next(3)
        compose.onNode(hasTestTag("onboarding:tamano")).assertIsDisplayed()
        compose.onNode(hasText("sube la «Escala»", substring = true)).performScrollTo().assertIsDisplayed()
        compose.onNode(hasText("con el dispositivo conectado", substring = true)).performScrollTo().assertIsDisplayed()
    }
}
