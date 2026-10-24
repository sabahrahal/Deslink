package app.extendesk

import android.content.Context
import androidx.annotation.StringRes

/**
 * Textos de res/values/strings.xml (y de cada values-<idioma>) fuera de Compose (avisos, mensajes de la sesión, estadísticas, diagnóstico). En
 * Compose se usa stringResource. [ExtendeskApp] lo prepara al arrancar, también en las pruebas con Robolectric.
 */
object Strings {
    private lateinit var context: Context

    fun init(context: Context) {
        this.context = context.applicationContext
    }

    fun get(@StringRes id: Int, vararg args: Any): String = context.resources.getString(id, *args)
}
