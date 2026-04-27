// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Thin C++ wrapper that drives the vendored ReShade FX language frontend
// (reshadefx_static) and turns a single .fx source file into:
//
//   - a `reshadefx::effect_module` describing all techniques, passes,
//     uniforms, textures and samplers, and
//   - one SPIR-V binary blob per entry point.
//
// The wrapper deliberately stays headless: it never touches the filesystem
// beyond what `reshadefx::preprocessor` already does, never logs through any
// global ReShade hooks, and never holds Vulkan state. The returned binaries
// can therefore be cached, hashed or fed directly into VKShaderCache.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace reshadefx
{
	struct effect_module;
}

namespace ReShade
{
	/// Result of compiling a single .fx file.
	struct CompiledEffect
	{
		/// Identifier (filename without extension) used by the runtime when
		/// looking up cached compilations. Filled by `FXCompiler` from the
		/// input filename.
		std::string id;

		/// In-memory description of the compiled effect. Owns no Vulkan
		/// resources by itself.
		///
		/// Boxed via std::vector<uint8_t> instead of a unique_ptr<effect_module>
		/// so callers do not have to include the heavy ReShade headers.
		/// Use `module()` to access it.
		std::vector<uint8_t> module_storage;

		/// Mapping `entry_point_name -> SPIR-V binary` (raw bytes ready for
		/// `vkCreateShaderModule`). Each entry corresponds to one entry in
		/// `effect_module::entry_points`.
		std::unordered_map<std::string, std::vector<uint32_t>> spirv;

		/// Concatenation of preprocessor + parser errors, if any. Empty on
		/// successful compilation.
		std::string errors;

		/// Returns the embedded effect_module pointer, or nullptr if not yet
		/// initialized. The pointer is valid as long as `*this` is alive.
		reshadefx::effect_module* module();
		const reshadefx::effect_module* module() const;
	};

	/// Compilation request describing how a single .fx is to be compiled.
	struct FXCompileOptions
	{
		/// Absolute path to the .fx file to compile.
		std::string source_path;

		/// Additional include search paths (e.g. the directory containing
		/// ReShade.fxh and per-effect helpers like ArcaneBloom.fxh).
		std::vector<std::string> include_paths;

		/// Preprocessor `#define key value` pairs. The wrapper always
		/// pre-defines BUFFER_WIDTH/BUFFER_HEIGHT/BUFFER_RCP_*, __RESHADE__
		/// and __RENDERER__ based on the values below; entries here
		/// override those defaults.
		std::vector<std::pair<std::string, std::string>> defines;

		/// Backbuffer dimensions used to derive BUFFER_* macros.
		uint32_t buffer_width = 1280;
		uint32_t buffer_height = 720;

		/// Encodes the running renderer for __RENDERER__. Vulkan reports
		/// 0x20000 in PC ReShade, matching the value picked here.
		uint32_t renderer_id = 0x20000;

		/// When true, the backend emits debug info (OpLine/OpString) in the
		/// generated SPIR-V. Off in release.
		bool debug_info = false;
	};

	/// Compiles a single .fx file into SPIR-V binaries plus a description
	/// module. Returns true on success, false otherwise (with `out.errors`
	/// containing a human-readable error log).
	///
	/// The function never throws; on any internal exception (e.g. out-of-
	/// memory inside the FX backend) it returns false and populates
	/// `out.errors`.
	bool CompileFX(const FXCompileOptions& opts, CompiledEffect& out);
} // namespace ReShade
