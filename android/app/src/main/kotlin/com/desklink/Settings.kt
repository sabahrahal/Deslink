package com.desklink

import android.content.Context

/** Ajustes guardados de la app (SharedPreferences "desklink"). Las pantallas de ajustes leen y escriben aquí. */
object Settings {
    const val PREFS = "desklink"

    /** Sonido del PC en la tablet (por defecto apagado: el PC lo sigue oyendo por sus altavoces). */
    const val KEY_AUDIO = "audio_enabled"

    fun audioEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_AUDIO, false)

    fun setAudioEnabled(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_AUDIO, enabled).apply()
    }
}
