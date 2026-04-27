package com.sbro.emucorex.ui.emulation

import android.content.Context
import com.sbro.emucorex.core.EmulatorStorage
import java.io.File

/**
 * Path resolution and master-switch plumbing for the ReShade post-processing
 * pipeline.
 *
 * The native (Vulkan) side currently boots the chain by reading
 * `<DataRoot>/shaders/reshade/preset.ini`. The UI here exposes a master
 * switch and a preset library; both are implemented purely on top of that
 * single canonical file:
 *
 *   - When the master switch is OFF, [preset.ini] is removed (or moved to a
 *     hidden backup), causing the native chain to fall back to passthrough.
 *   - When the master switch is ON and the user has selected a preset from
 *     the library, the chosen file's contents are mirrored into
 *     [preset.ini], so a subsequent emulator boot picks them up.
 *
 * Doing the wiring on the file system (rather than across JNI) keeps the
 * native interface untouched while still giving the UI complete control
 * over what the next session will load.
 */
internal object ReShadeStorage {

    private const val SUBDIR = "shaders/reshade"
    private const val ACTIVE_PRESET_NAME = "preset.ini"
    private const val DISABLED_BACKUP_NAME = ".preset.ini.disabled"

    fun reshadeRoot(context: Context): File =
        File(EmulatorStorage.dataRoot(context), SUBDIR).apply { mkdirs() }

    fun activePresetFile(context: Context): File =
        File(reshadeRoot(context), ACTIVE_PRESET_NAME)

    private fun disabledBackupFile(context: Context): File =
        File(reshadeRoot(context), DISABLED_BACKUP_NAME)

    /** True if any `.ini` files exist under the ReShade root. */
    fun hasAnyPresets(context: Context): Boolean =
        listPresets(context).isNotEmpty()

    /**
     * Recursively list every `.ini` candidate under the ReShade root.
     * The active mirror file is intentionally excluded so the user is
     * never offered to "load preset.ini" itself - that file is managed by
     * the UI as an output, not a source of truth.
     */
    fun listPresets(context: Context): List<File> {
        val root = reshadeRoot(context)
        if (!root.isDirectory) return emptyList()
        return root.walkTopDown()
            .maxDepth(4)
            .filter { it.isFile && it.name.endsWith(".ini", ignoreCase = true) }
            .filter { !it.name.equals(ACTIVE_PRESET_NAME, ignoreCase = true) }
            .filter { !it.name.equals(DISABLED_BACKUP_NAME, ignoreCase = true) }
            .sortedBy { it.relativeTo(root).path.lowercase() }
            .toList()
    }

    /**
     * Apply the master toggle. When [enabled] is true, [sourceText] (already
     * serialised from the UI's current state) is written to the canonical
     * preset path. When [enabled] is false, the canonical file is hidden
     * away as a backup so the next toggle can restore it without forcing the
     * user to reselect the preset.
     */
    fun applyMasterSwitch(context: Context, enabled: Boolean, sourceText: String?) {
        val active = activePresetFile(context)
        val backup = disabledBackupFile(context)
        if (enabled) {
            if (sourceText != null) {
                active.parentFile?.mkdirs()
                active.writeText(sourceText, Charsets.UTF_8)
            } else if (backup.exists() && !active.exists()) {
                backup.renameTo(active)
            }
        } else {
            if (active.exists()) {
                if (backup.exists()) backup.delete()
                active.renameTo(backup)
            }
        }
    }

    /**
     * Mirror the freshly-edited preset INI to the canonical "active" file
     * when the master switch is on. No-op when disabled.
     */
    fun mirrorActiveIfEnabled(context: Context, enabled: Boolean, presetText: String) {
        if (!enabled) return
        val active = activePresetFile(context)
        active.parentFile?.mkdirs()
        active.writeText(presetText, Charsets.UTF_8)
    }
}
