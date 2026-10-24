package app.extendesk

import android.content.Context
import android.content.SharedPreferences

/**
 * Las SharedPreferences de la app (ajustes, perfil de consumo, lo que se aprende del dispositivo). Antes del nombre
 * Extendesk se llamaban «desklink»: la primera vez que se abren se pasa lo guardado allí y se borra el archivo viejo.
 */
object Prefs {
    const val NAME = "extendesk"
    private const val LEGACY_NAME = "desklink"

    fun open(context: Context): SharedPreferences {
        val prefs = context.getSharedPreferences(NAME, Context.MODE_PRIVATE)
        val legacy = context.getSharedPreferences(LEGACY_NAME, Context.MODE_PRIVATE)
        val old = legacy.all
        if (old.isNotEmpty()) {
            if (prefs.all.isEmpty()) {
                val edit = prefs.edit()
                for ((key, value) in old) {
                    @Suppress("UNCHECKED_CAST")
                    when (value) {
                        is Boolean -> edit.putBoolean(key, value)
                        is Int -> edit.putInt(key, value)
                        is Long -> edit.putLong(key, value)
                        is Float -> edit.putFloat(key, value)
                        is String -> edit.putString(key, value)
                        is Set<*> -> edit.putStringSet(key, value as Set<String>)
                    }
                }
                edit.commit()
            }
            legacy.edit().clear().commit()
            context.deleteSharedPreferences(LEGACY_NAME)
        }
        return prefs
    }
}
