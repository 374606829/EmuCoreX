// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ReShadeFXCompiler.h"

#include "common/Console.h"

#include "effect_codegen.hpp"
#include "effect_module.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>

namespace
{
	// Allocate / destroy a heap-resident effect_module inside the
	// CompiledEffect's byte buffer so we don't have to expose
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

	// SPIR-V code is emitted by `assemble_code_for_entry_point` as a binary
	// std::string. Copy it to a vector<uint32_t> so the caller can feed it
	// directly into Vulkan, after sanity-checking the magic header.
	std::vector<uint32_t> ToSpvBinary(const std::string& binary)
	{
		std::vector<uint32_t> out;
		if (binary.size() < 4 || (binary.size() % 4) != 0)
			return out;
		out.resize(binary.size() / 4);
		std::memcpy(out.data(), binary.data(), binary.size());
		// SPIR-V magic number per spec is 0x07230203 (host endianness).
		if (out.empty() || out[0] != 0x07230203u)
			return {};
		return out;
	}
} // namespace

namespace ReShade
{
	reshadefx::effect_module* CompiledEffect::module()
	{
		if (module_storage.size() < sizeof(reshadefx::effect_module))
			return nullptr;
		return reinterpret_cast<reshadefx::effect_module*>(module_storage.data());
	}

	const reshadefx::effect_module* CompiledEffect::module() const
	{
		if (module_storage.size() < sizeof(reshadefx::effect_module))
			return nullptr;
		return reinterpret_cast<const reshadefx::effect_module*>(module_storage.data());
	}

	bool CompileFX(const FXCompileOptions& opts, CompiledEffect& out)
	{
		// Make sure any previous run is fully torn down (in case the caller
		// reuses the same CompiledEffect object).
		DestroyModule(out.module_storage);
		out.spirv.clear();
		out.errors.clear();
		out.id.clear();

		// NOTE: the entire EmuCoreX native build (including reshadefx_static)
		// runs with `-fno-exceptions` from BuildParameters.cmake, so we
		// cannot wrap the reshadefx calls in try/catch here. Upstream
		// reshadefx never uses `throw`/`catch` and reports problems through
		// `errors()` strings, so this is safe in practice. The only
		// remaining failure mode is STL OOM (e.g. std::bad_alloc), which
		// would call std::terminate - acceptable per the MVP "fail loud
		// rather than render garbage" contract.

		std::filesystem::path src(opts.source_path);
		out.id = src.stem().string();

		reshadefx::preprocessor pp;

		// Default macros that ReShade always exposes; the explicit
		// entries in `opts.defines` override these.
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

		// We always target Vulkan SPIR-V semantics: combined samplers,
		// no spec-constant promotion, no debug info unless asked.
		std::unique_ptr<reshadefx::codegen> backend(
			reshadefx::create_codegen_spirv(
				/*vulkan_semantics=*/true,
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

		// Emit per-entry-point SPIR-V binaries. We rely on the
		// post-parse contents of `_module.entry_points` instead of
		// re-walking techniques so that vertex/pixel/compute shaders
		// that are referenced by multiple passes are emitted once.
		for (const auto& ep : dst->entry_points)
		{
			std::string binary, assembly, errs;
			if (!backend->assemble_code_for_entry_point(ep.first, binary, assembly, errs))
			{
				out.errors += errs;
				if (out.errors.empty())
					out.errors = "SPIR-V assembly failed for entry point " + ep.first;
				return false;
			}

			std::vector<uint32_t> spv = ToSpvBinary(binary);
			if (spv.empty())
			{
				out.errors += "Generated SPIR-V for entry point '" + ep.first +
					"' is malformed (size=" + std::to_string(binary.size()) + ")";
				return false;
			}
			out.spirv.emplace(ep.first, std::move(spv));
		}

		return true;
	}
} // namespace ReShade
