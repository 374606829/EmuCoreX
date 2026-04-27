package com.sbro.emucorex.ui.emulation

import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateMap
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.sbro.emucorex.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.roundToInt

// =================================================================================
// In-memory model used by the pane.
// =================================================================================

private data class LoadedPreset(
    val sourceFile: File,
    val preset: ReShadePresetParser.Preset,
    val uniformsByEffect: Map<String, List<ReShadePresetParser.UniformDef>>,
    val warnings: List<String>
)

private const val INI_DECIMALS = 6

/** Small numeric formatter that avoids scientific notation in the saved INI. */
private fun formatIniFloat(value: Float): String {
    val rounded = Math.round(value.toDouble() * 1_000_000.0) / 1_000_000.0
    return "%.${INI_DECIMALS}f".format(rounded).let { s ->
        if (!s.contains('.')) s
        else s.trimEnd('0').let { if (it.endsWith('.')) it + "0" else it }
    }
}

private fun formatIniInt(value: Int): String = value.toString()

private fun formatIniBool(value: Boolean): String = if (value) "1" else "0"

private fun parseIniFloats(raw: String, count: Int): FloatArray {
    val out = FloatArray(count)
    val parts = raw.split(',')
    if (parts.size == 1) {
        val v = parts[0].trim().toFloatOrNull() ?: return out
        for (i in 0 until count) out[i] = v
    } else {
        for (i in 0 until count) {
            out[i] = parts.getOrNull(i)?.trim()?.toFloatOrNull() ?: 0f
        }
    }
    return out
}

private fun parseIniBool(raw: String): Boolean {
    val t = raw.trim().lowercase()
    return t == "1" || t == "true" || t == "yes" || t == "on"
}

private fun encodeIniFloats(values: FloatArray): String =
    values.joinToString(",") { formatIniFloat(it) }

private fun encodeIniInts(values: IntArray): String =
    values.joinToString(",") { formatIniInt(it) }

private fun encodeIniInts(values: FloatArray): String =
    values.joinToString(",") { formatIniInt(it.roundToInt()) }

private fun normalizeRange(min: Float, max: Float): Pair<Float, Float> {
    val lo = if (min.isFinite()) min else 0f
    val hi = if (max.isFinite()) max else 1f
    return if (lo < hi) lo to hi else lo to (lo + 1f)
}

// =================================================================================
// Public entry point
// =================================================================================

@Composable
internal fun ReShadePane(
    sectionTitleColor: Color,
    sectionLabelTopPadding: androidx.compose.ui.unit.Dp,
    sectionLabelInset: androidx.compose.ui.unit.Dp
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var masterEnabled by remember { mutableStateOf(ReShadeUiPreferences.isMasterEnabled(context)) }
    var loaded by remember { mutableStateOf<LoadedPreset?>(null) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var loading by remember { mutableStateOf(false) }
    var showPicker by remember { mutableStateOf(false) }
    var dirty by remember { mutableStateOf(false) }
    val edits: SnapshotStateMap<String, String> = remember { mutableStateMapOf() }

    val savedToast = stringResource(id = R.string.reshade_save_toast)
    val errorToast = stringResource(id = R.string.reshade_error_toast)
    val masterOn = stringResource(id = R.string.reshade_master_on_toast)
    val masterOff = stringResource(id = R.string.reshade_master_off_toast)
    val noPresetsHint = stringResource(id = R.string.reshade_no_presets_hint)

    // Auto-load the last selected preset on first composition.
    LaunchedEffect(Unit) {
        val last = ReShadeUiPreferences.lastPresetPath(context)
        if (loaded == null && last != null) {
            val candidate = File(last)
            if (candidate.isFile) {
                loadPreset(context, candidate, scope, onStart = { loading = true }, onResult = { lp, err ->
                    loading = false
                    if (lp != null) {
                        loaded = lp
                        loadError = null
                        edits.clear()
                        dirty = false
                    } else {
                        loadError = err
                    }
                })
            }
        }
    }

    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        ReShadeSectionTitle(
            text = stringResource(id = R.string.reshade_tab).uppercase(),
            color = sectionTitleColor,
            topPadding = sectionLabelTopPadding,
            horizontalInset = sectionLabelInset
        )

        // ---------------- Master switch -----------------------------------
        ReShadeToggleRow(
            title = stringResource(id = R.string.reshade_master_switch),
            subtitle = stringResource(id = R.string.reshade_master_switch_help),
            checked = masterEnabled,
            onCheckedChange = { newValue ->
                masterEnabled = newValue
                ReShadeUiPreferences.setMasterEnabled(context, newValue)
                val text = loaded?.let { lp ->
                    ReShadePresetParser.serializePreset(lp.preset, buildOverrides(lp, edits))
                }
                ReShadeStorage.applyMasterSwitch(context, newValue, text)
                Toast.makeText(context, if (newValue) masterOn else masterOff, Toast.LENGTH_SHORT).show()
            }
        )

        // ---------------- Preset picker row -------------------------------
        ReShadeRow {
            Text(
                text = loaded?.sourceFile?.name ?: stringResource(id = R.string.reshade_no_preset),
                style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium),
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.weight(1f)
            )
            TextButton(onClick = {
                if (ReShadeStorage.hasAnyPresets(context)) {
                    showPicker = true
                } else {
                    Toast.makeText(context, noPresetsHint, Toast.LENGTH_LONG).show()
                }
            }) {
                Text(stringResource(id = R.string.reshade_pick_preset))
            }
        }

        if (loading) {
            Text(
                text = stringResource(id = R.string.reshade_loading),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 4.dp)
            )
        }
        if (loadError != null) {
            Text(
                text = loadError!!,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(horizontal = 4.dp)
            )
        }

        val activeLoad = loaded
        if (activeLoad != null) {
            ReShadeSectionTitle(
                text = stringResource(id = R.string.reshade_techniques).uppercase(),
                color = sectionTitleColor,
                topPadding = sectionLabelTopPadding,
                horizontalInset = sectionLabelInset
            )
            Surface(
                shape = RoundedCornerShape(16.dp),
                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.30f),
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.05f))
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 14.dp, vertical = 10.dp),
                    verticalArrangement = Arrangement.spacedBy(2.dp)
                ) {
                    if (activeLoad.preset.techniques.isEmpty()) {
                        Text(
                            stringResource(id = R.string.reshade_no_techniques),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    } else {
                        activeLoad.preset.techniques.forEach { tech ->
                            Text(
                                text = "• ${tech.techniqueName} @ ${tech.effectFilename}",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurface
                            )
                        }
                    }
                }
            }

            // ---------------- Per-effect parameter sliders ------------------
            activeLoad.uniformsByEffect.forEach { (effectFilename, defs) ->
                if (defs.isEmpty()) return@forEach
                ReShadeSectionTitle(
                    text = effectFilename.uppercase(),
                    color = sectionTitleColor,
                    topPadding = sectionLabelTopPadding,
                    horizontalInset = sectionLabelInset
                )
                defs.forEach { def ->
                    ReShadeUniformControl(
                        def = def,
                        currentRaw = currentRawFor(def, activeLoad, edits),
                        onChanged = { newRaw ->
                            edits[def.key] = newRaw
                            dirty = true
                        }
                    )
                }
            }

            // ---------------- Save row --------------------------------------
            Spacer(Modifier.height(4.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = if (dirty) stringResource(id = R.string.reshade_dirty)
                    else stringResource(id = R.string.reshade_clean),
                    style = MaterialTheme.typography.bodySmall,
                    color = if (dirty) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f)
                )
                TextButton(
                    onClick = {
                        if (loaded != null) {
                            edits.clear()
                            dirty = false
                        }
                    }
                ) { Text(stringResource(id = R.string.reshade_revert)) }
                TextButton(
                    onClick = {
                        val current = loaded ?: return@TextButton
                        scope.launch {
                            val ok = withContext(Dispatchers.IO) {
                                runCatching {
                                    val text = ReShadePresetParser.serializePreset(
                                        current.preset,
                                        buildOverrides(current, edits)
                                    )
                                    current.sourceFile.parentFile?.mkdirs()
                                    current.sourceFile.writeText(text, Charsets.UTF_8)
                                    ReShadeStorage.mirrorActiveIfEnabled(context, masterEnabled, text)
                                    // Refold the in-memory representation so future edits
                                    // diff against the just-saved baseline.
                                    val reparsed = ReShadePresetParser.parsePresetIni(text)
                                    val merged = mergeUniformsAfterSave(current, reparsed)
                                    loaded = merged
                                    edits.clear()
                                    dirty = false
                                    true
                                }.getOrElse { false }
                            }
                            Toast.makeText(
                                context,
                                if (ok) savedToast else errorToast,
                                Toast.LENGTH_SHORT
                            ).show()
                        }
                    },
                    enabled = dirty
                ) { Text(stringResource(id = R.string.reshade_save)) }
            }

            if (activeLoad.warnings.isNotEmpty()) {
                Text(
                    text = activeLoad.warnings.joinToString("\n").take(800),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 4.dp)
                )
            }
        }
    }

    if (showPicker) {
        val candidates = remember { ReShadeStorage.listPresets(context) }
        AlertDialog(
            onDismissRequest = { showPicker = false },
            containerColor = MaterialTheme.colorScheme.surface,
            shape = RoundedCornerShape(24.dp),
            title = {
                Text(
                    text = stringResource(id = R.string.reshade_pick_preset),
                    style = MaterialTheme.typography.titleLarge
                )
            },
            text = {
                if (candidates.isEmpty()) {
                    Text(
                        stringResource(id = R.string.reshade_no_presets_hint),
                        style = MaterialTheme.typography.bodyMedium
                    )
                } else {
                    val scrollState = rememberScrollState()
                    val root = remember { ReShadeStorage.reshadeRoot(context) }
                    Column(
                        modifier = Modifier
                            .heightIn(max = 360.dp)
                            .verticalScroll(scrollState),
                        verticalArrangement = Arrangement.spacedBy(2.dp)
                    ) {
                        candidates.forEach { file ->
                            TextButton(
                                onClick = {
                                    showPicker = false
                                    loadPreset(context, file, scope,
                                        onStart = { loading = true; loadError = null },
                                        onResult = { lp, err ->
                                            loading = false
                                            if (lp != null) {
                                                loaded = lp
                                                edits.clear()
                                                dirty = false
                                                ReShadeUiPreferences.setLastPresetPath(context, file.absolutePath)
                                                if (masterEnabled) {
                                                    ReShadeStorage.mirrorActiveIfEnabled(
                                                        context, true,
                                                        ReShadePresetParser.serializePreset(lp.preset, emptyMap())
                                                    )
                                                }
                                            } else {
                                                loadError = err
                                            }
                                        }
                                    )
                                },
                                modifier = Modifier.fillMaxWidth()
                            ) {
                                Text(
                                    text = file.relativeTo(root).path.ifEmpty { file.name },
                                    style = MaterialTheme.typography.bodyMedium
                                )
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showPicker = false }) {
                    Text(stringResource(id = R.string.cancel))
                }
            }
        )
    }
}

// =================================================================================
// Helpers - merging current INI values, overrides, post-save state.
// =================================================================================

private fun currentRawFor(
    def: ReShadePresetParser.UniformDef,
    loaded: LoadedPreset,
    edits: Map<String, String>
): String {
    edits[def.key]?.let { return it }
    val sectionValues = loaded.preset.effectParameters[def.effectFilename]
    val raw = sectionValues?.get(def.name)
    if (raw != null) return raw
    return when (def.type) {
        ReShadePresetParser.UniformType.Bool -> formatIniBool(def.defaults.firstOrNull()?.let { it >= 0.5f } ?: false)
        ReShadePresetParser.UniformType.Int,
        ReShadePresetParser.UniformType.Int2,
        ReShadePresetParser.UniformType.Int3,
        ReShadePresetParser.UniformType.Int4 -> encodeIniInts(def.defaults)
        else -> encodeIniFloats(def.defaults)
    }
}

private fun buildOverrides(
    loaded: LoadedPreset,
    edits: Map<String, String>
): Map<String, Map<String, String>> {
    if (edits.isEmpty()) return emptyMap()
    val out = LinkedHashMap<String, LinkedHashMap<String, String>>()
    for ((key, value) in edits) {
        val sep = key.indexOf("::")
        if (sep <= 0) continue
        val effect = key.substring(0, sep)
        val name = key.substring(sep + 2)
        out.getOrPut(effect) { LinkedHashMap() }[name] = value
    }
    // Make sure the set of overridden sections at least exists in the
    // serializer's view of the world; otherwise we'd skip writing them.
    for (eff in loaded.uniformsByEffect.keys) {
        if (out.containsKey(eff) && !loaded.preset.effectParameters.containsKey(eff)) {
            // Section is brand-new; the serializer will append it.
        }
    }
    return out
}

private fun mergeUniformsAfterSave(
    previous: LoadedPreset,
    reparsed: ReShadePresetParser.Preset
): LoadedPreset = LoadedPreset(
    sourceFile = previous.sourceFile,
    preset = reparsed,
    uniformsByEffect = previous.uniformsByEffect,
    warnings = reparsed.warnings.toList()
)

private fun loadPreset(
    context: android.content.Context,
    file: File,
    scope: kotlinx.coroutines.CoroutineScope,
    onStart: () -> Unit,
    onResult: (LoadedPreset?, String?) -> Unit
) {
    onStart()
    scope.launch {
        val outcome = withContext(Dispatchers.IO) {
            runCatching {
                val text = file.readText(Charsets.UTF_8)
                val preset = ReShadePresetParser.parsePresetIni(text)
                val byEffect = LinkedHashMap<String, List<ReShadePresetParser.UniformDef>>()
                val presetDir = file.parentFile ?: ReShadeStorage.reshadeRoot(context)
                val extra = listOf(
                    File(ReShadeStorage.reshadeRoot(context), "reshade-shaders/Shaders"),
                    File(ReShadeStorage.reshadeRoot(context), "Shaders")
                )
                for (effect in preset.effectFilenames) {
                    val resolved = ReShadePresetParser.resolveEffectFile(effect, presetDir, extra)
                    if (resolved != null && resolved.isFile) {
                        val src = runCatching { resolved.readText(Charsets.UTF_8) }.getOrNull().orEmpty()
                        if (src.isNotEmpty()) {
                            val defs = ReShadePresetParser.parseFxUniforms(effect, src)
                                .filter { !it.isSystem && it.type != ReShadePresetParser.UniformType.Unknown }
                            byEffect[effect] = defs
                        } else {
                            byEffect[effect] = emptyList()
                            preset.warnings.add("Effect file is empty: ${resolved.absolutePath}")
                        }
                    } else {
                        byEffect[effect] = emptyList()
                        preset.warnings.add("Effect file not found: $effect")
                    }
                }
                LoadedPreset(file, preset, byEffect, preset.warnings.toList())
            }
        }
        val result = outcome.getOrNull()
        val err = outcome.exceptionOrNull()?.message
        onResult(result, err)
    }
}

// =================================================================================
// Local re-implementations of the visual primitives. We do not depend on the
// `private` helpers in EmulationScreen.kt to keep coupling shallow.
// =================================================================================

@Composable
private fun ReShadeSectionTitle(
    text: String,
    color: Color,
    topPadding: androidx.compose.ui.unit.Dp,
    horizontalInset: androidx.compose.ui.unit.Dp
) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelSmall.copy(
            fontWeight = FontWeight.ExtraBold,
            letterSpacing = 1.sp
        ),
        color = color,
        modifier = Modifier.padding(top = topPadding, start = horizontalInset, end = horizontalInset)
    )
}

@Composable
private fun ReShadeRow(content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit) {
    Surface(
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.05f))
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            content = content
        )
    }
}

@Composable
private fun ReShadeToggleRow(
    title: String,
    subtitle: String?,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Surface(
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.05f))
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium),
                    color = MaterialTheme.colorScheme.onSurface
                )
                if (!subtitle.isNullOrEmpty()) {
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
            Switch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = MaterialTheme.colorScheme.primary,
                    checkedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.4f)
                )
            )
        }
    }
}

@Composable
private fun ReShadeUniformControl(
    def: ReShadePresetParser.UniformDef,
    currentRaw: String,
    onChanged: (String) -> Unit
) {
    Surface(
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.30f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.05f))
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = def.label.ifEmpty { def.name },
                        style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium),
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    if (def.category.isNotEmpty()) {
                        Text(
                            text = def.category,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                Spacer(Modifier.width(8.dp))
                Text(
                    text = previewValueText(def, currentRaw),
                    style = MaterialTheme.typography.bodySmall.copy(fontWeight = FontWeight.Medium),
                    color = MaterialTheme.colorScheme.primary
                )
            }
            renderUniformBody(def, currentRaw, onChanged)
            if (def.tooltip.isNotEmpty()) {
                Text(
                    text = def.tooltip,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

@Composable
private fun renderUniformBody(
    def: ReShadePresetParser.UniformDef,
    raw: String,
    onChanged: (String) -> Unit
) {
    when (def.type) {
        ReShadePresetParser.UniformType.Bool -> {
            val current = parseIniBool(raw)
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Spacer(Modifier.weight(1f))
                Switch(
                    checked = current,
                    onCheckedChange = { onChanged(formatIniBool(it)) }
                )
            }
        }
        ReShadePresetParser.UniformType.Float,
        ReShadePresetParser.UniformType.Int -> {
            val isInt = def.type == ReShadePresetParser.UniformType.Int
            val cur = parseIniFloats(raw, 1)[0]
            val (lo, hi) = normalizeRange(def.min[0], def.max[0])
            ScalarSlider(
                value = cur,
                min = lo,
                max = hi,
                isInt = isInt,
                onChanged = { v ->
                    val out = if (isInt) formatIniInt(v.roundToInt()) else formatIniFloat(v)
                    onChanged(out)
                }
            )
        }
        ReShadePresetParser.UniformType.Float2,
        ReShadePresetParser.UniformType.Float3,
        ReShadePresetParser.UniformType.Float4 -> {
            val values = parseIniFloats(raw, def.components)
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for (i in 0 until def.components) {
                    val (lo, hi) = normalizeRange(
                        def.min.getOrElse(i) { 0f },
                        def.max.getOrElse(i) { 1f }
                    )
                    ScalarSlider(
                        labelPrefix = componentLabel(def.components, i),
                        value = values[i],
                        min = lo,
                        max = hi,
                        isInt = false,
                        onChanged = { v ->
                            val copy = values.copyOf()
                            copy[i] = v
                            onChanged(encodeIniFloats(copy))
                        }
                    )
                }
            }
        }
        ReShadePresetParser.UniformType.Int2,
        ReShadePresetParser.UniformType.Int3,
        ReShadePresetParser.UniformType.Int4 -> {
            val values = parseIniFloats(raw, def.components)
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for (i in 0 until def.components) {
                    val (lo, hi) = normalizeRange(
                        def.min.getOrElse(i) { 0f },
                        def.max.getOrElse(i) { 100f }
                    )
                    ScalarSlider(
                        labelPrefix = componentLabel(def.components, i),
                        value = values[i],
                        min = lo,
                        max = hi,
                        isInt = true,
                        onChanged = { v ->
                            val copy = values.copyOf()
                            copy[i] = v.roundToInt().toFloat()
                            onChanged(encodeIniInts(copy))
                        }
                    )
                }
            }
        }
        ReShadePresetParser.UniformType.Unknown -> {
            Text(
                text = raw,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun ScalarSlider(
    labelPrefix: String? = null,
    value: Float,
    min: Float,
    max: Float,
    isInt: Boolean,
    onChanged: (Float) -> Unit
) {
    val clamped = value.coerceIn(min, max)
    var local by remember(value, min, max) { mutableStateOf(clamped) }
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        if (labelPrefix != null) {
            Text(
                text = "$labelPrefix  ${if (isInt) local.roundToInt().toString() else "%.3f".format(local)}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Slider(
            value = local,
            valueRange = min..max,
            steps = if (isInt) {
                val span = (max - min).toInt()
                if (span in 2..63) span - 1 else 0
            } else 0,
            onValueChange = { local = it },
            onValueChangeFinished = { onChanged(local) },
            colors = SliderDefaults.colors(
                thumbColor = MaterialTheme.colorScheme.primary,
                activeTrackColor = MaterialTheme.colorScheme.primary
            )
        )
    }
}

private fun componentLabel(components: Int, index: Int): String = when (components) {
    2 -> arrayOf("X", "Y")[index]
    3 -> arrayOf("X", "Y", "Z")[index]
    4 -> arrayOf("X", "Y", "Z", "W")[index]
    else -> "[$index]"
}

private fun previewValueText(def: ReShadePresetParser.UniformDef, raw: String): String {
    return when (def.type) {
        ReShadePresetParser.UniformType.Bool -> if (parseIniBool(raw)) "ON" else "OFF"
        ReShadePresetParser.UniformType.Float -> {
            val v = parseIniFloats(raw, 1)[0]
            "%.3f".format(v)
        }
        ReShadePresetParser.UniformType.Int -> {
            val v = parseIniFloats(raw, 1)[0]
            v.roundToInt().toString()
        }
        ReShadePresetParser.UniformType.Float2,
        ReShadePresetParser.UniformType.Float3,
        ReShadePresetParser.UniformType.Float4 -> {
            val v = parseIniFloats(raw, def.components)
            v.joinToString(",") { "%.2f".format(it) }
        }
        ReShadePresetParser.UniformType.Int2,
        ReShadePresetParser.UniformType.Int3,
        ReShadePresetParser.UniformType.Int4 -> {
            val v = parseIniFloats(raw, def.components)
            v.joinToString(",") { it.roundToInt().toString() }
        }
        ReShadePresetParser.UniformType.Unknown -> raw.take(24)
    }
}

@Composable
private fun stringResource(id: Int): String = androidx.compose.ui.res.stringResource(id = id)
