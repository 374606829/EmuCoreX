package com.sbro.emucorex.core

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import com.sbro.emucorex.MainActivity
import com.sbro.emucorex.R
import com.sbro.emucorex.data.GameItem

object GameLaunchShortcut {
    const val ACTION_LAUNCH_GAME = "com.sbro.emucorex.action.LAUNCH_GAME"
    const val EXTRA_GAME_PATH = "com.sbro.emucorex.extra.GAME_PATH"
    const val EXTRA_SAVE_SLOT = "com.sbro.emucorex.extra.SAVE_SLOT"
    const val EXTRA_BOOT_BIOS = "com.sbro.emucorex.extra.BOOT_BIOS"
    const val EXTRA_AUTOTEST_MODE = "com.sbro.emucorex.extra.AUTOTEST_MODE"
    const val EXTRA_ENABLE_EE_RECOMPILER = "com.sbro.emucorex.extra.ENABLE_EE_RECOMPILER"
    const val EXTRA_ENABLE_IOP_RECOMPILER = "com.sbro.emucorex.extra.ENABLE_IOP_RECOMPILER"
    const val EXTRA_ENABLE_VU0_RECOMPILER = "com.sbro.emucorex.extra.ENABLE_VU0_RECOMPILER"
    const val EXTRA_ENABLE_VU1_RECOMPILER = "com.sbro.emucorex.extra.ENABLE_VU1_RECOMPILER"

    private const val SCHEME = "emucorex"
    private const val HOST = "launch"

    data class LaunchRequest(
        val gamePath: String? = null,
        val saveSlot: Int? = null,
        val bootBios: Boolean = false,
        val autotestMode: Boolean = false,
        val enableEeRecompiler: Boolean? = null,
        val enableIopRecompiler: Boolean? = null,
        val enableVu0Recompiler: Boolean? = null,
        val enableVu1Recompiler: Boolean? = null
    )

    fun requestPinnedShortcut(
        context: Context,
        game: GameItem,
        saveSlot: Int? = null
    ): Boolean {
        val normalizedSaveSlot = normalizeSaveSlot(saveSlot)
        val shortcutId = buildString {
            append("game:")
            append(game.path.hashCode())
            normalizedSaveSlot?.let {
                append(":")
                append(it)
            }
        }
        val launchIntent = Intent(context, MainActivity::class.java).apply {
            action = ACTION_LAUNCH_GAME
            putExtra(EXTRA_GAME_PATH, game.path)
            normalizedSaveSlot?.let { putExtra(EXTRA_SAVE_SLOT, it) }
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        }
        val shortcut = ShortcutInfoCompat.Builder(context, shortcutId)
            .setShortLabel(game.title.take(40))
            .setLongLabel(game.title)
            .setIcon(IconCompat.createWithResource(context, R.mipmap.ic_launcher))
            .setIntent(launchIntent)
            .build()
        return ShortcutManagerCompat.requestPinShortcut(context, shortcut, null)
    }

    fun buildLaunchUri(
        gamePath: String? = null,
        saveSlot: Int? = null,
        bootBios: Boolean = false
    ): Uri {
        val normalizedSaveSlot = normalizeSaveSlot(saveSlot)
        return Uri.Builder()
            .scheme(SCHEME)
            .authority(HOST)
            .apply {
                gamePath?.let { appendQueryParameter("gamePath", it) }
                normalizedSaveSlot?.let { appendQueryParameter("saveSlot", it.toString()) }
                if (bootBios) appendQueryParameter("bootBios", "true")
            }
            .build()
    }

    fun parseLaunchRequest(intent: Intent?): LaunchRequest? {
        intent ?: return null
        val data = intent.data
        val gamePathFromData = when {
            data == null -> null
            data.scheme == SCHEME && data.host == HOST -> data.getQueryParameter("gamePath")
            data.scheme == "content" || data.scheme == "file" -> data.toString()
            else -> null
        }
        val gamePath = intent.getStringExtra(EXTRA_GAME_PATH) ?: gamePathFromData
        val saveSlot = when {
            intent.hasExtra(EXTRA_SAVE_SLOT) -> normalizeSaveSlot(intent.getIntExtra(EXTRA_SAVE_SLOT, -1))
            data?.scheme == SCHEME && data.host == HOST -> normalizeSaveSlot(data.getQueryParameter("saveSlot")?.toIntOrNull())
            else -> null
        }
        val bootBios = intent.getBooleanExtra(EXTRA_BOOT_BIOS, false) ||
            (data?.scheme == SCHEME && data.host == HOST && data.getQueryParameter("bootBios") == "true")
        val autotestMode = intent.getBooleanExtra(EXTRA_AUTOTEST_MODE, false) ||
            (data?.scheme == SCHEME && data.host == HOST && data.getQueryParameter("autotest") == "true")
        if (gamePath.isNullOrBlank() && !bootBios) return null
        return LaunchRequest(
            gamePath = gamePath,
            saveSlot = saveSlot,
            bootBios = bootBios,
            autotestMode = autotestMode,
            enableEeRecompiler = optionalBooleanExtra(intent, EXTRA_ENABLE_EE_RECOMPILER)
                ?: optionalBooleanQuery(data, "enableEeRecompiler"),
            enableIopRecompiler = optionalBooleanExtra(intent, EXTRA_ENABLE_IOP_RECOMPILER)
                ?: optionalBooleanQuery(data, "enableIopRecompiler"),
            enableVu0Recompiler = optionalBooleanExtra(intent, EXTRA_ENABLE_VU0_RECOMPILER)
                ?: optionalBooleanQuery(data, "enableVu0Recompiler"),
            enableVu1Recompiler = optionalBooleanExtra(intent, EXTRA_ENABLE_VU1_RECOMPILER)
                ?: optionalBooleanQuery(data, "enableVu1Recompiler")
        )
    }

    fun clearLaunchRequest(intent: Intent?) {
        intent ?: return
        intent.removeExtra(EXTRA_GAME_PATH)
        intent.removeExtra(EXTRA_SAVE_SLOT)
        intent.removeExtra(EXTRA_BOOT_BIOS)
        intent.removeExtra(EXTRA_AUTOTEST_MODE)
        intent.removeExtra(EXTRA_ENABLE_EE_RECOMPILER)
        intent.removeExtra(EXTRA_ENABLE_IOP_RECOMPILER)
        intent.removeExtra(EXTRA_ENABLE_VU0_RECOMPILER)
        intent.removeExtra(EXTRA_ENABLE_VU1_RECOMPILER)
        if (intent.data?.scheme == SCHEME && intent.data?.host == HOST) {
            intent.data = null
        }
    }

    private fun optionalBooleanExtra(intent: Intent, key: String): Boolean? {
        return if (intent.hasExtra(key)) intent.getBooleanExtra(key, false) else null
    }

    private fun optionalBooleanQuery(data: Uri?, key: String): Boolean? {
        if (data?.scheme != SCHEME || data.host != HOST) return null
        return when (data.getQueryParameter(key)?.lowercase()) {
            "true", "1", "yes", "on" -> true
            "false", "0", "no", "off" -> false
            else -> null
        }
    }

    private fun normalizeSaveSlot(slot: Int?): Int? {
        return when {
            slot == null -> null
            slot in 1..10 -> slot
            slot in 0..9 -> slot + 1
            else -> null
        }
    }
}
