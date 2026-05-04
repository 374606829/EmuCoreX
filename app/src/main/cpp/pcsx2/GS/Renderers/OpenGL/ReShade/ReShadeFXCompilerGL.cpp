// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// GLSL output path of the vendored ReShade FX language frontend. Mirrors
// `ReShadeFXCompiler` (Vulkan/SPIR-V) line-for-line, except the codegen
// backend is `create_codegen_glsl(/*vulkan_semantics=*/false, ...)` and the
// per-entry-point payload is plain text (not a SPIR-V binary).
//
// The output is *desktop* GLSL 4.30 because that is what the upstream
// reshadefx GLSL backend hard-codes. The chain runtime (`ReShadeChainGL`)
// is in charge of converting the leading `#version 430` line to
// `#version 320 es` / `#version 310 es` for Android targets and prepending
// the precision qualifiers ES requires - the compiler intentionally does
// not bake any backend-specific assumptions in.

#include "ReShadeFXCompilerGL.h"

#include "common/Console.h"

#include "effect_codegen.hpp"
#include "effect_module.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <new>

namespace
{
	// Allocate / destroy a heap-resident effect_module inside the
	// CompiledEffectGL's byte buffer so we don't have to expose
	// reshadefx::effect_module in our public header.
	reshadefx::effect_module* AllocateModule(std::vector<uint8_t>& storage)
	{
		storage.clear();
		storage.resize(sizeof(reshadefx::effect_module));
		return new (storage.data()) reshadefx::effect_module();
	}

	void DestroyModule(std::vector<uint8_t>& storage)
	{
		if (storage.size() < sizeof(reshadefx::effect_module))
			return;
		auto* m = reinterpret_cast<reshadefx::effect_module*>(storage.data());
		m->~effect_module();
		storage.clear();
	}
} // namespace

namespace ReShade
{
	reshadefx::effect_module* CompiledEffectGL::module()
	{
		if (module_storage.size() < sizeof(reshadefx::effect_module))
			return nullptr;
		return reinterpret_cast<reshadefx::effect_module*>(module_storage.data());
	}

	const reshadefx::effect_module* CompiledEffectGL::module() const
	{
		if (module_storage.size() < sizeof(reshadefx::effect_module))
			return nullptr;
		return reinterpret_cast<const reshadefx::effect_module*>(module_storage.data());
	}

	bool CompileFXGLSL(const FXCompileOptionsGL& opts, CompiledEffectGL& out)
	{
		// Reset any leftovers from a previous run on the same struct.
		DestroyModule(out.module_storage);
		out.glsl.clear();
		out.errors.clear();
		out.id.clear();

		// NOTE: the entire EmuCoreX native build (including reshadefx_static)
		// runs with `-fno-exceptions` from BuildParameters.cmake, so we
		// cannot wrap the reshadefx calls in try/catch here. Upstream
		// reshadefx never uses `throw`/`catch` and reports problems through
		// `errors()` strings, so this is safe in practice.

		std::filesystem::path src(opts.source_path);
		out.id = src.stem().string();

		reshadefx::preprocessor pp;

		// Default macros that ReShade always exposes; the explicit entries
		// in `opts.defines` override these.
		pp.add_macro_definition("__RESHADE__", "50000");
		pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
		pp.add_macro_definition("__VENDOR__", "0");
		pp.add_macro_definition("__DEVICE__", "0");
		pp.add_macro_definition("__APPLICATION__", "0xEC0FE000"); // 'EmuCoreX'
		pp.add_macro_definition("__RENDERER__", std::to_string(opts.renderer_id));
		pp.add_macro_definition("BUFFER_WIDTH", std::to_string(opts.buffer_width));
		pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(opts.buffer_height));
		pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
		pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
		pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
		pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");

		for (const auto& kv : opts.defines)
			pp.add_macro_definition(kv.first, kv.second);

		for (const std::string& inc : opts.include_paths)
			pp.add_include_path(std::filesystem::path(inc));

		if (!pp.append_file(src))
		{
			out.errors = pp.errors();
			if (out.errors.empty())
				out.errors = "Preprocessor failed without an error message";
			return false;
		}

		// vulkan_semantics=false → real OpenGL bindings (set=0/binding=N
		// uniform blocks, separate sampler bindings, no SPIR-V-isms).
		// uniforms_to_spec_constants=false: classic UBO at binding 0.
		// flip_vert_y=false: GLES uses the same Y-up convention we feed via
		// our 3-vert fullscreen triangle, no implicit flip needed.
		std::unique_ptr<reshadefx::codegen> backend(
			reshadefx::create_codegen_glsl(
				/*vulkan_semantics=*/false,
				/*debug_info=*/opts.debug_info,
				/*uniforms_to_spec_constants=*/false,
				/*enable_16bit_types=*/false,
				/*flip_vert_y=*/false));

		reshadefx::parser parser;
		if (!parser.parse(pp.output(), backend.get()))
		{
			out.errors = pp.errors();
			out.errors += parser.errors();
			if (out.errors.empty())
				out.errors = "Parser failed without an error message";
			return false;
		}

		reshadefx::effect_module* dst = AllocateModule(out.module_storage);
		*dst = std::move(backend->module());

		// Emit per-entry-point GLSL. The upstream backend writes the
		// shader text into the `binary` parameter (despite its name) and
		// leaves `assembly` empty.
		for (const auto& ep : dst->entry_points)
		{
			std::string text, assembly_unused, errs;
			if (!backend->assemble_code_for_entry_point(ep.first, text, assembly_unused, errs))
			{
				out.errors += errs;
				if (out.errors.empty())
					out.errors = "GLSL assembly failed for entry point " + ep.first;
				return false;
			}

			if (text.empty())
			{
				out.errors += "Generated GLSL for entry point '" + ep.first + "' is empty";
				return false;
			}

			out.glsl.emplace(ep.first, std::move(text));
		}

		return true;
	}
} // namespace ReShade
