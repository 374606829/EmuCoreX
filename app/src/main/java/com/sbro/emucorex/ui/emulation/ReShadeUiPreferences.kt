package com.sbro.emucorex.ui.emulation

import android.content.Context
import android.content.SharedPreferences

/**
 * Tiny SharedPreferences wrapper for the ReShade UI panel.
 *
 * The UI panel deliberately uses a self-contained SharedPreferences file
 * instead of the application-wide DataStore, because:
 *
 *  - The state is local to the post-processing experiment and need not be
 *    backed up alongside core emulator settings.
 *  - It avoids inflating the central [com.sbro.emucorex.data.AppPreferences]
 *    schema before the post-processing feature has stabilised.
 */
internal object ReShadeUiPreferences {
    private const val FILE = "reshade_ui_prefs"
    private const val KEY_MASTER_ENABLED = "master_enabled"
    private const val KEY_LAST_PRESET_PATH = "last_preset_path"

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    fun isMasterEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_MASTER_ENABLED, false)

    fun setMasterEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_MASTER_ENABLED, enabled).apply()
    }

    fun lastPresetPath(context: Context): String? =
        prefs(context).getString(KEY_LAST_PRESET_PATH, null)

    fun setLastPresetPath(context: Context, path: String?) {
        val editor = prefs(context).edit()
        if (path.isNullOrBlank()) editor.remove(KEY_LAST_PRESET_PATH) else editor.putString(KEY_LAST_PRESET_PATH, path)
        editor.apply()
    }
}
