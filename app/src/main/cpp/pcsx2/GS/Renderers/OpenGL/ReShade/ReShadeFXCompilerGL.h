// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// OpenGL/OpenGL ES counterpart of `ReShadeFXCompiler`. Drives the same
// vendored `reshadefx_static` language frontend, but selects the GLSL
// backend (`create_codegen_glsl`) so each entry point comes out as a
// self-contained GLSL source string instead of a SPIR-V binary.
//
// The wrapper deliberately stays headless: it never touches GL state,
// never allocates GPU resources and never logs through device-specific
// hooks. The resulting strings can therefore be cached, hashed or fed
// directly into `glShaderSource` after the chain runtime applies the
// `#version`/precision rewrites required by the target context (see
// `ReShadeChainGL`).

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
	/// Result of compiling a single .fx file for the GLSL backend.
	struct CompiledEffectGL
	{
		/// Identifier (filename without extension) used by the runtime when
		/// looking up cached compilations. Filled by `CompileFXGLSL` from
		/// the input filename.
		std::string id;

		/// In-memory description of the compiled effect (uniforms, samplers,
		/// textures, techniques, passes). Owns no GL handles by itself.
		///
		/// Boxed via std::vector<uint8_t> instead of unique_ptr<effect_module>
		/// so callers do not have to include the heavy ReShade headers.
		/// Use `module()` to access it.
		std::vector<uint8_t> module_storage;

		/// Mapping `entry_point_name -> GLSL source string`. Each entry
		/// corresponds to one entry in `effect_module::entry_points`.
		///
		/// The strings start with a single `#version 430` line (the upstream
		/// reshadefx GLSL backend hard-codes it) followed by the rest of the
		/// shader. The chain runtime is responsible for rewriting the version
		/// header to `#version 320 es` / `#version 310 es` and prepending
		/// the appropriate precision qualifiers when targeting GLES.
		std::unordered_map<std::string, std::string> glsl;

		/// Concatenation of preprocessor + parser errors, if any. Empty on
		/// successful compilation.
		std::string errors;

		/// Returns the embedded effect_module pointer, or nullptr if not yet
		/// initialized. The pointer is valid as long as `*this` is alive.
		reshadefx::effect_module* module();
		const reshadefx::effect_module* module() const;
	};

	/// Compilation request describing how a single .fx is to be compiled
	/// for the GLSL backend.
	struct FXCompileOptionsGL
	{
		/// Absolute path to the .fx file to compile.
		std::string source_path;

		/// Additional include search paths (e.g. the directory containing
		/// ReShade.fxh and per-effect helpers).
		std::vector<std::string> include_paths;

		/// Preprocessor `#define key value` pairs. The wrapper always
		/// pre-defines BUFFER_WIDTH/BUFFER_HEIGHT/BUFFER_RCP_*, __RESHADE__
		/// and __RENDERER__ based on the values below; entries here
		/// override those defaults.
		std::vector<std::pair<std::string, std::string>> defines;

		/// Backbuffer dimensions used to derive BUFFER_* macros.
		uint32_t buffer_width = 1280;
		uint32_t buffer_height = 720;

		/// Encodes the running renderer for __RENDERER__. PC ReShade reports
		/// 0x10000 for OpenGL, so we pick the same value to keep effects
		/// that branch on the renderer ID happy.
		uint32_t renderer_id = 0x10000;

		/// When true, the GLSL backend appends source line comments. Off in
		/// release.
		bool debug_info = false;
	};

	/// Compiles a single .fx file into per-entry-point GLSL strings plus a
	/// description module. Returns true on success, false otherwise (with
	/// `out.errors` containing a human-readable error log).
	///
	/// The function never throws; on any internal exception (e.g. STL OOM
	/// inside the FX backend) std::terminate is called - the FX code base
	/// has no `throw`/`catch`, so this is safe in practice and the
	/// `-fno-exceptions` build flag enforces fail-loud behaviour.
	bool CompileFXGLSL(const FXCompileOptionsGL& opts, CompiledEffectGL& out);
} // namespace ReShade
