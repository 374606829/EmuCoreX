package com.sbro.emucorex.data

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import com.sbro.emucorex.core.DocumentPathResolver
import com.sbro.emucorex.core.EmulatorBridge
import com.sbro.emucorex.core.EmulatorStorage
import com.sbro.emucorex.core.NativeApp
import kotlinx.coroutines.flow.first
import java.io.File

data class TextureFileEntry(
    val fileName: String,
    val sizeBytes: Long
)

data class TextureImportResult(
    val copied: Int,
    val skipped: Int
)

class TextureRepository(private val context: Context) {
    private val preferences = AppPreferences(context)

    fun defaultTextureRoot(): File = EmulatorStorage.texturesDir(context)

    suspend fun textureRootPath(): String {
        return preferences.textureRootPath.first() ?: defaultTextureRoot().absolutePath
    }

    suspend fun loadTextureReplacements(): Boolean = preferences.loadTextureReplacements.first()

    suspend fun dumpReplaceableTextures(): Boolean = preferences.dumpReplaceableTextures.first()

    suspend fun syncConfiguredSettings() {
        applyTextureRoot(textureRootPath())
        applyLoadTextureReplacements(loadTextureReplacements())
        applyDumpReplaceableTextures(dumpReplaceableTextures())
    }

    suspend fun setLoadTextureReplacements(enabled: Boolean) {
        preferences.setLoadTextureReplacements(enabled)
        applyLoadTextureReplacements(enabled)
        if (enabled) reloadReplacementMap()
    }

    suspend fun setDumpReplaceableTextures(enabled: Boolean) {
        preferences.setDumpReplaceableTextures(enabled)
        applyDumpReplaceableTextures(enabled)
    }

    suspend fun setTextureRootFromTree(uri: Uri): Boolean {
        runCatching {
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
        }
        val resolved = DocumentPathResolver.resolveDirectoryPath(uri.toString()) ?: return false
        File(resolved).mkdirs()
        preferences.setTextureRootPath(resolved)
        applyTextureRoot(resolved)
        reloadReplacementMap()
        return true
    }

    suspend fun resetTextureRoot() {
        val root = defaultTextureRoot().absolutePath
        preferences.setTextureRootPath(root)
        applyTextureRoot(root)
        reloadReplacementMap()
    }

    fun listReplacementTextures(serial: String): List<TextureFileEntry> {
        val dir = replacementsDir(serial)
        return dir.listFiles { file ->
            file.isFile && isTextureFile(file.name)
        }?.sortedBy { it.name.lowercase() }
            ?.map { TextureFileEntry(fileName = it.name, sizeBytes = it.length()) }
            .orEmpty()
    }

    fun replacementsPath(serial: String): String = replacementsDir(serial).absolutePath

    fun dumpsPath(serial: String): String = dumpsDir(serial).absolutePath

    fun normalizeSerial(value: String): String {
        return value.trim().uppercase().replace(Regex("[^A-Z0-9._-]"), "_")
    }

    fun isValidSerial(value: String): Boolean {
        val serial = normalizeSerial(value)
        return serial.length >= 4 && serial.any { it.isLetter() } && serial.any { it.isDigit() }
    }

    fun importTextures(serial: String, sourceTreeUri: Uri): TextureImportResult {
        val normalizedSerial = normalizeSerial(serial)
        require(isValidSerial(normalizedSerial)) { "Invalid serial" }

        runCatching {
            context.contentResolver.takePersistableUriPermission(
                sourceTreeUri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        }
        val sourceRoot = DocumentFile.fromTreeUri(context, sourceTreeUri) ?: return TextureImportResult(0, 0)
        val targetDir = replacementsDir(normalizedSerial).apply { mkdirs() }
        var copied = 0
        var skipped = 0

        sourceRoot.listFiles().forEach { source ->
            val name = source.name.orEmpty()
            if (!source.isFile || !isTextureFile(name)) {
                skipped++
                return@forEach
            }
            val safeName = sanitizeTextureFileName(name)
            val target = File(targetDir, safeName)
            val ok = runCatching {
                context.contentResolver.openInputStream(source.uri)?.use { input ->
                    target.outputStream().use { output -> input.copyTo(output) }
                } != null
            }.getOrDefault(false)
            if (ok) copied++ else skipped++
        }

        reloadReplacementMap()
        return TextureImportResult(copied, skipped)
    }

    fun reloadReplacementMap() {
        runCatching { NativeApp.reloadTextureReplacements() }
    }

    private suspend fun applyLoadTextureReplacements(enabled: Boolean) {
        EmulatorBridge.setSetting("EmuCore/GS", "LoadTextureReplacements", "bool", enabled.toString())
    }

    private suspend fun applyDumpReplaceableTextures(enabled: Boolean) {
        EmulatorBridge.setSetting("EmuCore/GS", "DumpReplaceableTextures", "bool", enabled.toString())
    }

    private suspend fun applyTextureRoot(path: String) {
        EmulatorBridge.setSetting("Folders", "Textures", "string", path)
    }

    private fun replacementsDir(serial: String): File {
        return File(File(textureRootForFiles(), normalizeSerial(serial)), "replacements").apply { mkdirs() }
    }

    private fun dumpsDir(serial: String): File {
        return File(File(textureRootForFiles(), normalizeSerial(serial)), "dumps").apply { mkdirs() }
    }

    private fun textureRootForFiles(): File {
        val configured = runCatching { preferences.textureRootPath }.getOrNull()
        val path = runCatching {
            kotlinx.coroutines.runBlocking { configured?.first() }
        }.getOrNull()
        return if (path.isNullOrBlank() || path.startsWith("content://")) {
            defaultTextureRoot()
        } else {
            File(path).apply { mkdirs() }
        }
    }

    private fun isTextureFile(name: String): Boolean {
        val ext = name.substringAfterLast('.', "").lowercase()
        return ext == "png" || ext == "dds"
    }

    private fun sanitizeTextureFileName(value: String): String {
        return value.replace(Regex("[\\\\/:*?\"<>|]"), "_")
    }
}
