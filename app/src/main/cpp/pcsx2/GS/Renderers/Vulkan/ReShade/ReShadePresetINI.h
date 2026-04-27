// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Minimal MVP parser for ReShade preset INI files (e.g. "1.默认·适应大部分.ini").
//
// The PC-side reference grammar parsed here is:
//
//   Techniques=name@File1.fx,name2@File2.fx,...      -> ordered effect chain
//   TechniqueSorting=...                             -> ignored in MVP
//
//   [SomeFile.fx]
//   key0=value
//   key1=floatA,floatB
//   ...
//
// The PC-side TechniqueSorting line is intentionally ignored (per the MVP
// document, the effect chain is fully driven by the comma-separated order in
// the Techniques= line).
//
// Parsing is forgiving: unknown sections / unknown keys are kept verbatim and
// can be queried later. We never throw; on a malformed file we return a
// description string and an empty chain so the caller can fall back to "no
// post-processing" without crashing.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ReShade
{
	/// Single entry parsed from the comma-separated Techniques= header line.
	struct TechniqueRef
	{
		/// The technique name as written in the FX file. May be empty for
		/// shorthand entries like "MyEffect.fx" (no "@" present), in which
		/// case the FX file usually contains a single technique whose name
		/// can be inferred at compile time.
		std::string technique_name;
		/// Effect filename relative to the preset's search root, e.g.
		/// "ArcaneBloom.fx".
		std::string effect_filename;
	};

	/// All key/value pairs of a single [filename.fx] section.
	using EffectParameters = std::unordered_map<std::string, std::string>;

	/// Parsed in-memory representation of a ReShade preset INI.
	struct Preset
	{
		/// Ordered list of (technique, effect file) pairs from the
		/// Techniques= line. This is the *only* source of truth for the
		/// effect chain order in the MVP.
		std::vector<TechniqueRef> chain;

		/// Per-effect uniform parameters, keyed by effect filename
		/// (e.g. "ArcaneBloom.fx" -> { "uExposure" -> "2.359000", ... }).
		std::unordered_map<std::string, EffectParameters> effect_params;

		/// True when a Techniques= entry was found and parsed (even if empty).
		bool has_techniques_line = false;

		/// Reset all state so the same Preset object can be reused.
		void Clear();

		/// Returns true if the given effect filename has at least one parsed
		/// uniform parameter section.
		bool HasParametersFor(const std::string& effect_filename) const;
	};

	/// Loads and parses the given INI file. Returns true on success.
	/// On failure, error_out (when non-null) contains a human-readable
	/// description and `out` is left empty.
	///
	/// The parser is intentionally lenient: any line that is not a section
	/// header and does not contain '=' is ignored (with a warning logged).
	bool ParsePresetINI(const std::string& ini_path, Preset& out, std::string* error_out);

	/// Convenience helpers to coerce parsed values into common types. They
	/// never throw; on a parse failure the supplied default is returned.
	bool   ToBool  (const std::string& s, bool   default_value);
	int    ToInt   (const std::string& s, int    default_value);
	float  ToFloat (const std::string& s, float  default_value);

	/// Splits comma-separated numeric values like "0.476000,0.683000" into a
	/// vector of floats. Stops at the first non-numeric entry.
	std::vector<float> ToFloatList(const std::string& s);
} // namespace ReShade
