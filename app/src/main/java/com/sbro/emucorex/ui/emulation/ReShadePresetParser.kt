package com.sbro.emucorex.ui.emulation

import java.io.File

/**
 * Pure-Kotlin parser for ReShade preset INI files and the uniform metadata
 * embedded in `.fx` shader sources.
 *
 * The parser is intentionally tolerant: any malformed input is reported as
 * a soft warning rather than throwing, and recognised entries are returned
 * even when surrounding lines fail to parse.
 *
 * The output of [parsePresetIni] is structured so that:
 *  - The section ordering of the original file is preserved.
 *  - Comments and unknown keys are kept verbatim in [Preset.lines] so that
 *    a later [serializePreset] call does not clobber user content.
 *  - The list of techniques (i.e. the effects to run) is exposed as a
 *    structured list, alongside the per-effect parameter sections.
 */
internal object ReShadePresetParser {

    // ---------------------------------------------------------------------
    // Public data classes
    // ---------------------------------------------------------------------

    /** A single entry from the `Techniques=` line, e.g. `ArcaneBloom@ArcaneBloom.fx`. */
    data class TechniqueRef(
        val techniqueName: String,
        val effectFilename: String
    )

    /** Possible parsed types of a uniform value. */
    enum class UniformType { Float, Int, Bool, Float2, Float3, Float4, Int2, Int3, Int4, Unknown }

    /** Hint about the desired widget for a uniform. */
    enum class UniformWidget { Slider, Drag, Checkbox, Combo, Color, Radio, List, Input, Other }

    /**
     * Metadata extracted from a single `uniform` declaration in a `.fx` file.
     */
    data class UniformDef(
        val effectFilename: String,
        val name: String,
        val type: UniformType,
        val components: Int,
        val widget: UniformWidget,
        val label: String,
        val tooltip: String,
        val category: String,
        val min: FloatArray,
        val max: FloatArray,
        val step: FloatArray,
        val defaults: FloatArray,
        val comboItems: List<String>,
        val isSystem: Boolean
    ) {
        val key: String get() = "${effectFilename}::${name}"
    }

    /**
     * In-memory, mutable representation of a parsed preset INI.
     */
    class Preset {
        /** Raw lines from the original file, used to round-trip safely. */
        val lines: MutableList<String> = mutableListOf()

        /** Ordered list of techniques to apply (parsed from `Techniques=`). */
        val techniques: MutableList<TechniqueRef> = mutableListOf()

        /** Per-effect parameter map. Outer key is `<filename>` (case-preserved). */
        val effectParameters: LinkedHashMap<String, LinkedHashMap<String, String>> = LinkedHashMap()

        /** All parse warnings collected; never null. */
        val warnings: MutableList<String> = mutableListOf()

        /** Unique list of effect filenames referenced by [techniques] (preserves order). */
        val effectFilenames: List<String>
            get() = techniques.map { it.effectFilename }.distinct()
    }

    // ---------------------------------------------------------------------
    // Parsing
    // ---------------------------------------------------------------------

    fun parsePresetIni(text: String): Preset {
        val preset = Preset()
        val rawLines = text.split('\n')
        var currentSection: String? = null
        rawLines.forEachIndexed { idx, original ->
            val line = original.trimEnd('\r')
            preset.lines.add(line)
            val trimmed = line.trim()
            if (trimmed.isEmpty() || trimmed.startsWith(';') || trimmed.startsWith('#')) {
                return@forEachIndexed
            }
            if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
                currentSection = trimmed.substring(1, trimmed.length - 1).trim()
                if (currentSection != null && !preset.effectParameters.containsKey(currentSection)) {
                    preset.effectParameters[currentSection!!] = LinkedHashMap()
                }
                return@forEachIndexed
            }
            val eq = trimmed.indexOf('=')
            if (eq <= 0) {
                preset.warnings.add("Line ${idx + 1}: ignored (no '=')")
                return@forEachIndexed
            }
            val key = trimmed.substring(0, eq).trim()
            val value = trimmed.substring(eq + 1).trim()
            val section = currentSection
            if (section == null) {
                if (key.equals("Techniques", ignoreCase = true)) {
                    parseTechniquesValue(value, preset)
                }
                // other top-level keys (TechniqueSorting, PreprocessorDefinitions, ...) are
                // round-tripped via [Preset.lines] but otherwise ignored by the UI.
                return@forEachIndexed
            }
            preset.effectParameters.getOrPut(section) { LinkedHashMap() }[key] = value
        }
        return preset
    }

    private fun parseTechniquesValue(rawValue: String, into: Preset) {
        rawValue.split(',').forEach { part ->
            val token = part.trim().trim('"').trim()
            if (token.isEmpty()) return@forEach
            val at = token.indexOf('@')
            if (at <= 0 || at >= token.length - 1) {
                into.warnings.add("Techniques entry has no '@<file.fx>': '$token'")
                return@forEach
            }
            val techName = token.substring(0, at).trim()
            val file = token.substring(at + 1).trim()
            if (techName.isNotEmpty() && file.isNotEmpty()) {
                into.techniques.add(TechniqueRef(techName, file))
            }
        }
    }

    // ---------------------------------------------------------------------
    // FX uniform metadata extraction
    // ---------------------------------------------------------------------

    private val UNIFORM_REGEX = Regex(
        """uniform\s+(\w+)\s+([A-Za-z_][\w]*)\s*<([\s\S]*?)>\s*(?:=\s*([\s\S]*?))?;""",
        RegexOption.MULTILINE
    )

    fun parseFxUniforms(effectFilename: String, fxText: String): List<UniformDef> {
        val cleaned = stripCommentsKeepingLines(fxText)
        val list = mutableListOf<UniformDef>()
        UNIFORM_REGEX.findAll(cleaned).forEach { m ->
            val typeWord = m.groupValues[1]
            val name = m.groupValues[2]
            val annotationBlock = m.groupValues[3]
            val defaultText = m.groupValues.getOrElse(4) { "" }.trim()
            val (type, components) = decodeType(typeWord)
            val annotations = parseAnnotations(annotationBlock)
            val widget = decodeWidget(annotations["ui_type"])
            val label = annotations["ui_label"].orEmpty().ifEmpty { name }
            val tooltip = annotations["ui_tooltip"].orEmpty()
            val category = annotations["ui_category"].orEmpty()
            val isSystem = annotations.containsKey("source")
            val min = readVecFloat(annotations["ui_min"], components, defaultFill = 0f)
            val max = readVecFloat(annotations["ui_max"], components, defaultFill = 1f)
            val step = readVecFloat(annotations["ui_step"], components, defaultFill = 0f)
            val defaults = parseDefaultLiteral(defaultText, components, type)
            val comboItems = parseComboItems(annotations["ui_items"])
            list.add(
                UniformDef(
                    effectFilename = effectFilename,
                    name = name,
                    type = type,
                    components = components,
                    widget = widget,
                    label = label,
                    tooltip = tooltip,
                    category = category,
                    min = min,
                    max = max,
                    step = step,
                    defaults = defaults,
                    comboItems = comboItems,
                    isSystem = isSystem
                )
            )
        }
        return list
    }

    private fun decodeType(word: String): Pair<UniformType, Int> = when (word.lowercase()) {
        "float" -> UniformType.Float to 1
        "float2" -> UniformType.Float2 to 2
        "float3" -> UniformType.Float3 to 3
        "float4" -> UniformType.Float4 to 4
        "int" -> UniformType.Int to 1
        "uint" -> UniformType.Int to 1
        "int2" -> UniformType.Int2 to 2
        "int3" -> UniformType.Int3 to 3
        "int4" -> UniformType.Int4 to 4
        "bool" -> UniformType.Bool to 1
        else -> UniformType.Unknown to 1
    }

    private fun decodeWidget(raw: String?): UniformWidget = when (raw?.lowercase()) {
        "slider" -> UniformWidget.Slider
        "drag" -> UniformWidget.Drag
        "checkbox" -> UniformWidget.Checkbox
        "combo" -> UniformWidget.Combo
        "color" -> UniformWidget.Color
        "radio" -> UniformWidget.Radio
        "list" -> UniformWidget.List
        "input" -> UniformWidget.Input
        else -> UniformWidget.Other
    }

    private fun parseAnnotations(block: String): Map<String, String> {
        val map = LinkedHashMap<String, String>()
        var i = 0
        val s = block
        while (i < s.length) {
            while (i < s.length && (s[i].isWhitespace() || s[i] == ';')) i++
            if (i >= s.length) break
            val nameStart = i
            while (i < s.length && (s[i].isLetterOrDigit() || s[i] == '_')) i++
            if (i == nameStart) {
                i++; continue
            }
            val key = s.substring(nameStart, i).lowercase()
            while (i < s.length && s[i].isWhitespace()) i++
            if (i >= s.length || s[i] != '=') {
                while (i < s.length && s[i] != ';') i++
                continue
            }
            i++ // consume '='
            while (i < s.length && s[i].isWhitespace()) i++
            val sb = StringBuilder()
            while (i < s.length && s[i] != ';') {
                if (s[i] == '"') {
                    i++
                    while (i < s.length && s[i] != '"') {
                        if (s[i] == '\\' && i + 1 < s.length) {
                            sb.append(decodeEscape(s[i + 1]))
                            i += 2
                            continue
                        }
                        sb.append(s[i]); i++
                    }
                    if (i < s.length) i++
                } else {
                    sb.append(s[i]); i++
                }
            }
            map[key] = sb.toString().trim()
        }
        return map
    }

    private fun decodeEscape(c: Char): String = when (c) {
        'n' -> "\n"
        'r' -> "\r"
        't' -> "\t"
        '\\' -> "\\"
        '"' -> "\""
        '0' -> "\u0000"
        else -> c.toString()
    }

    private fun parseComboItems(raw: String?): List<String> {
        if (raw.isNullOrEmpty()) return emptyList()
        return raw.split('\u0000').filter { it.isNotEmpty() }
    }

    private fun readVecFloat(raw: String?, components: Int, defaultFill: Float): FloatArray {
        val out = FloatArray(components) { defaultFill }
        if (raw.isNullOrEmpty()) return out
        val literal = stripVecCtor(raw)
        val parts = literal.split(',')
        if (parts.size == 1) {
            val v = parseLooseFloat(parts[0]) ?: defaultFill
            for (i in 0 until components) out[i] = v
        } else {
            for (i in 0 until components) {
                val v = parts.getOrNull(i)?.let(::parseLooseFloat) ?: defaultFill
                out[i] = v
            }
        }
        return out
    }

    private fun parseDefaultLiteral(raw: String, components: Int, type: UniformType): FloatArray {
        if (raw.isEmpty()) return FloatArray(components)
        if (type == UniformType.Bool) {
            val b = raw.trim().equals("true", ignoreCase = true) || raw.trim() == "1"
            return floatArrayOf(if (b) 1f else 0f)
        }
        return readVecFloat(raw, components, 0f)
    }

    private fun stripVecCtor(raw: String): String {
        val t = raw.trim()
        val open = t.indexOf('(')
        val close = t.lastIndexOf(')')
        if (open in 0 until close) return t.substring(open + 1, close)
        return t
    }

    private fun parseLooseFloat(token: String?): Float? {
        if (token == null) return null
        val cleaned = token.trim()
            .removeSuffix("f").removeSuffix("F")
            .removeSuffix("h").removeSuffix("H")
        if (cleaned.isEmpty()) return null
        return cleaned.toFloatOrNull()
    }

    /**
     * Replace `//` and `/* */` comments with spaces while preserving newlines so
     * that line counts in the regex still align with the original source. This
     * keeps annotations like `ui_tooltip = "// not a comment"` intact because
     * the body inside double quotes is left alone.
     */
    private fun stripCommentsKeepingLines(src: String): String {
        val sb = StringBuilder(src.length)
        var i = 0
        var inString = false
        while (i < src.length) {
            val c = src[i]
            if (inString) {
                sb.append(c)
                if (c == '\\' && i + 1 < src.length) {
                    sb.append(src[i + 1]); i += 2; continue
                }
                if (c == '"') inString = false
                i++; continue
            }
            if (c == '"') { sb.append(c); inString = true; i++; continue }
            if (c == '/' && i + 1 < src.length) {
                val n = src[i + 1]
                if (n == '/') {
                    while (i < src.length && src[i] != '\n') { sb.append(' '); i++ }
                    continue
                }
                if (n == '*') {
                    sb.append("  "); i += 2
                    while (i + 1 < src.length && !(src[i] == '*' && src[i + 1] == '/')) {
                        sb.append(if (src[i] == '\n') '\n' else ' ')
                        i++
                    }
                    if (i + 1 < src.length) { sb.append("  "); i += 2 }
                    continue
                }
            }
            sb.append(c); i++
        }
        return sb.toString()
    }

    /**
     * Locate the on-disk path for an effect filename, given the directory the
     * INI lives in plus any extra search paths. Returns `null` if the file
     * cannot be found.
     */
    fun resolveEffectFile(effectFilename: String, presetDir: File, extraSearchDirs: List<File>): File? {
        val candidates = mutableListOf<File>()
        candidates += File(presetDir, effectFilename)
        candidates += File(presetDir, "reshade-shaders/Shaders/$effectFilename")
        for (extra in extraSearchDirs) candidates += File(extra, effectFilename)
        candidates.firstOrNull { it.isFile }?.let { return it }
        // Fall back to a recursive search rooted at presetDir; bounded depth.
        return walkFor(presetDir, effectFilename, maxDepth = 6)
            ?: extraSearchDirs.firstNotNullOfOrNull { walkFor(it, effectFilename, maxDepth = 6) }
    }

    private fun walkFor(root: File, name: String, maxDepth: Int): File? {
        if (!root.isDirectory) return null
        return root.walkTopDown().maxDepth(maxDepth)
            .firstOrNull { it.isFile && it.name.equals(name, ignoreCase = true) }
    }

    // ---------------------------------------------------------------------
    // Serialisation
    // ---------------------------------------------------------------------

    /**
     * Render the [preset] back into INI text, applying [overrides] on top of
     * the section/key map already parsed. The original [Preset.lines] are
     * walked to preserve unknown keys, comments and ordering.
     *
     * - For each known `[section]` block in the original file, every override
     *   `(section, key) -> value` already present in the file is updated in
     *   place. New `(section, key)` overrides are appended at the end of
     *   that section.
     * - Sections whose name appears in [overrides] but not in the file are
     *   appended at the end of the document.
     */
    fun serializePreset(
        preset: Preset,
        overrides: Map<String, Map<String, String>>
    ): String {
        if (preset.lines.isEmpty() && overrides.isEmpty()) return ""

        val out = StringBuilder()
        val seenSections = mutableSetOf<String>()
        val pendingPerSection = HashMap<String, LinkedHashMap<String, String>>().also { dst ->
            for ((sec, map) in overrides) {
                dst[sec] = LinkedHashMap(map)
            }
        }

        var currentSection: String? = null
        val sectionBuffers = LinkedHashMap<String?, MutableList<String>>()
        sectionBuffers[null] = mutableListOf()
        val sectionOrder = mutableListOf<String?>(null)

        for (raw in preset.lines) {
            val trimmed = raw.trim()
            if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
                currentSection = trimmed.substring(1, trimmed.length - 1).trim()
                if (!sectionBuffers.containsKey(currentSection)) {
                    sectionBuffers[currentSection] = mutableListOf()
                    sectionOrder.add(currentSection)
                }
                sectionBuffers[currentSection]!!.add(raw)
                continue
            }
            sectionBuffers[currentSection]!!.add(raw)
        }

        for (sec in sectionOrder) {
            val buf = sectionBuffers[sec]!!
            val ovr = sec?.let(pendingPerSection::remove) ?: LinkedHashMap()
            seenSections.add(sec ?: "")
            for (line in buf) {
                val trimmed = line.trim()
                if (trimmed.isEmpty() || trimmed.startsWith(';') || trimmed.startsWith('#')
                    || (trimmed.startsWith('[') && trimmed.endsWith(']'))
                ) {
                    out.append(line).append('\n')
                    continue
                }
                val eq = trimmed.indexOf('=')
                if (eq <= 0) {
                    out.append(line).append('\n')
                    continue
                }
                val key = trimmed.substring(0, eq).trim()
                if (ovr.containsKey(key)) {
                    val replacement = ovr.remove(key)!!
                    val indent = line.substring(0, line.indexOf(trimmed.first { !it.isWhitespace() }))
                    out.append("${indent}${key}=${replacement}").append('\n')
                } else {
                    out.append(line).append('\n')
                }
            }
            if (ovr.isNotEmpty()) {
                if (out.isNotEmpty() && out.last() != '\n') out.append('\n')
                for ((k, v) in ovr) out.append("${k}=${v}").append('\n')
            }
        }

        // Sections that appeared only in overrides need to be appended.
        for ((sec, ovr) in pendingPerSection) {
            if (sec.isEmpty() || ovr.isEmpty()) continue
            if (out.isNotEmpty() && out.last() != '\n') out.append('\n')
            out.append('\n').append('[').append(sec).append(']').append('\n')
            for ((k, v) in ovr) out.append("${k}=${v}").append('\n')
        }

        return out.toString()
    }
}
