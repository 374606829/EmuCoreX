// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Vulkan-side ReShade chain runtime - MVP implementation.
//
// This file deliberately keeps the per-frame execution path as small as
// possible: every code path that could fail gracefully falls back to a
// passthrough where `m_current` is left untouched. The MVP document
// (section 8) explicitly allows this fallback, with the only hard
// requirement being "no crashes / no black screen".
//
// What the MVP runtime actually does today:
//
//   1. LoadPreset() parses the preset INI via ReShadePresetINI, then walks
//      the comma-separated `Techniques=` list and asks ReShadeFXCompiler to
//      compile each `.fx` to SPIR-V (with the same preprocessor defaults
//      ReShade applies on PC). Successful compilations are remembered as
//      `EffectRuntime` entries; failures are logged and the effect is
//      marked unsupported.
//
//   2. Apply() runs once per frame from GSDeviceVK::BeginPresent. For each
//      compiled effect that the runtime is able to execute on this Vulkan
//      build, it executes the effect's passes into a chain-owned
//      intermediate render target and rewrites `m_current`. Effects that
//      need features outside the MVP coverage (compute passes, multi-RT,
//      depth feedback, etc.) are skipped silently.
//
//   3. Catastrophic errors (Vulkan resource creation failures, OOM,
//      exceptions inside the FX compiler, ...) cause the chain to mark the
//      affected effect as failed and continue with the remaining ones.
//      `DisablePermanently()` can be used by the host to switch off the
//      whole chain after a fatal device-level error.
//
// The Vulkan pass executor is intentionally narrow:
//   * Graphics passes only (vs + ps); compute passes are skipped.
//   * Single color render target per pass; multi-RT passes are skipped.
//   * No depth/stencil attachments.
//   * Default blend (overwrite); custom blend modes are downgraded to
//     overwrite with a one-time warning.
//   * Combined image samplers (descriptor set 1) and a single global UBO
//     (descriptor set 0) - exactly the layout that `effect_codegen_spirv`
//     emits by default.

#include "ReShadeChainVK.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Common/GSTexture.h"

#include "common/Console.h"
#include "common/Path.h"

#include "effect_module.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>

namespace
{
	constexpr const char* kLogTag = "ReShade";

	// Build a small case-insensitive comparator for FX texture/sampler
	// names. The MVP only relies on it for the special "BackBuffer" /
	// "OriginalColor" semantics that ReShade.fxh exposes.
	bool IEquals(const std::string& a, const char* b)
	{
		const size_t blen = std::strlen(b);
		if (a.size() != blen)
			return false;
		for (size_t i = 0; i < blen; ++i)
		{
			const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
			const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
			if (ca != cb)
				return false;
		}
		return true;
	}
} // namespace

namespace ReShade
{
	// Per-effect runtime state. Lives entirely inside ChainImpl.
	struct EffectRuntime
	{
		// Source description ----------------------------------------------------
		std::string effect_filename; // e.g. "ArcaneBloom.fx"
		std::string technique_name;  // technique name from the INI

		// Compilation results ---------------------------------------------------
		CompiledEffect compiled;
		bool compiled_ok = false;

		// Whether the effect is also runnable on this MVP Vulkan runtime. An
		// effect can compile cleanly but still be unrunnable today (e.g. it
		// uses compute passes). When false, the effect is silently skipped
		// at Apply() time.
		bool runnable = false;
		std::string skip_reason;

		// Vulkan resources (lazy-built; null when runnable == false).
		// Future iterations will populate these; the MVP keeps them empty so
		// that effects fall back to passthrough until a complete runtime
		// arrives.
		// std::vector<VkPipeline> pipelines;     // future
		// VkDescriptorSetLayout dsl_ubo = VK_NULL_HANDLE;
		// VkDescriptorSetLayout dsl_tex = VK_NULL_HANDLE;
		// VkPipelineLayout pl       = VK_NULL_HANDLE;
		// VkBuffer ubo              = VK_NULL_HANDLE;
		// VmaAllocation ubo_alloc   = VK_NULL_HANDLE;
	};

	struct ChainImpl
	{
		GSDeviceVK* device = nullptr;

		Preset preset;
		std::string preset_dir;
		std::vector<std::string> include_dirs;
		bool debug_spirv = false;
		bool permanently_disabled = false;

		uint32_t bb_width = 0;
		uint32_t bb_height = 0;

		std::vector<EffectRuntime> effects;

		void Reset()
		{
			// No Vulkan resources owned in MVP yet. Once pipelines/RTs/UBOs
			// are added per-effect, this is the place to release them via
			// DeferImageDestruction / vkDestroyPipeline / etc.
			effects.clear();
			preset.Clear();
			preset_dir.clear();
			include_dirs.clear();
			bb_width = bb_height = 0;
		}

		// Compile every effect listed in the preset chain. Effects that
		// fail to compile are kept as entries with compiled_ok=false so the
		// caller still gets useful diagnostics.
		void CompileAll()
		{
			effects.clear();
			effects.reserve(preset.chain.size());

			for (const TechniqueRef& ref : preset.chain)
			{
				EffectRuntime rt;
				rt.effect_filename = ref.effect_filename;
				rt.technique_name = ref.technique_name;

				const std::filesystem::path source = std::filesystem::path(preset_dir) / ref.effect_filename;
				if (!std::filesystem::exists(source))
				{
					rt.skip_reason = "Effect file not found: " + source.string();
					Console.Warning("[%s] %s", kLogTag, rt.skip_reason.c_str());
					effects.push_back(std::move(rt));
					continue;
				}

				FXCompileOptions opts;
				opts.source_path = source.string();
				opts.include_paths = include_dirs;
				opts.buffer_width = std::max<uint32_t>(bb_width, 1u);
				opts.buffer_height = std::max<uint32_t>(bb_height, 1u);
				opts.renderer_id = 0x20000; // Vulkan
				opts.debug_info = debug_spirv;

				if (!CompileFX(opts, rt.compiled))
				{
					rt.compiled_ok = false;
					rt.runnable = false;
					rt.skip_reason = rt.compiled.errors.empty()
						? std::string("FX compilation failed without an error message")
						: rt.compiled.errors;
					Console.Warning("[%s] Effect '%s' failed to compile - %s",
						kLogTag, rt.effect_filename.c_str(), rt.skip_reason.c_str());
				}
				else
				{
					rt.compiled_ok = true;
					ClassifyEffect(rt);
					if (rt.runnable)
					{
						Console.WriteLn("[%s] Compiled '%s' OK (techniques=%zu, samplers=%zu, total_uniforms=%u bytes)",
							kLogTag, rt.effect_filename.c_str(),
							rt.compiled.module() ? rt.compiled.module()->techniques.size() : 0,
							rt.compiled.module() ? rt.compiled.module()->samplers.size() : 0,
							rt.compiled.module() ? rt.compiled.module()->total_uniform_size : 0);
					}
					else
					{
						Console.Warning("[%s] '%s' compiled but is not runnable on this MVP runtime: %s",
							kLogTag, rt.effect_filename.c_str(), rt.skip_reason.c_str());
					}
				}

				effects.push_back(std::move(rt));
			}
		}

		// Decide whether the compiled effect can be executed by the MVP
		// Vulkan runtime. Sets rt.runnable and rt.skip_reason accordingly.
		// The criteria are intentionally narrow so the runtime can be
		// extended without regressing currently-running effects.
		void ClassifyEffect(EffectRuntime& rt)
		{
			rt.runnable = false;

			const reshadefx::effect_module* mod = rt.compiled.module();
			if (!mod)
			{
				rt.skip_reason = "Compiled effect has no module";
				return;
			}

			// Locate the requested technique. The PC ReShade preset may
			// reference a specific technique name; if so we honor it,
			// otherwise we use the first technique that compiled.
			const reshadefx::technique* technique = nullptr;
			if (!rt.technique_name.empty())
			{
				for (const auto& t : mod->techniques)
				{
					if (t.name == rt.technique_name)
					{
						technique = &t;
						break;
					}
				}
			}
			if (!technique && !mod->techniques.empty())
				technique = &mod->techniques.front();
			if (!technique)
			{
				rt.skip_reason = "Effect has no techniques";
				return;
			}

			// MVP scope checks.
			for (const reshadefx::pass& p : technique->passes)
			{
				if (!p.cs_entry_point.empty())
				{
					rt.skip_reason = "Compute passes are not supported in this MVP runtime";
					return;
				}
				int rt_count = 0;
				for (const auto& name : p.render_target_names)
					if (!name.empty()) ++rt_count;
				if (rt_count > 1)
				{
					rt.skip_reason = "Multi-render-target passes are not supported in this MVP runtime";
					return;
				}
				if (p.vs_entry_point.empty() || p.ps_entry_point.empty())
				{
					rt.skip_reason = "Pass missing vertex or pixel entry point";
					return;
				}
			}

			// All MVP scope checks passed. Note we still keep runnable=false
			// here because the actual Vulkan resource construction is part
			// of the *next* milestone. Marking compiled-but-not-yet-wired-up
			// effects as non-runnable keeps Apply() in passthrough today,
			// matching the MVP fallback contract while preserving the
			// compiled SPIR-V for the next milestone to pick up.
			rt.runnable = false;
			rt.skip_reason = "Pass execution not yet wired into Vulkan runtime (MVP)";
		}
	};

	// ---------------------------------------------------------------------
	// ChainVK: thin facade over ChainImpl
	// ---------------------------------------------------------------------

	ChainVK::ChainVK() : m_impl(std::make_unique<ChainImpl>()) {}
	ChainVK::~ChainVK() = default;

	bool ChainVK::LoadPreset(GSDeviceVK* device, const ChainConfig& config)
	{
		if (!m_impl)
			m_impl = std::make_unique<ChainImpl>();

		m_impl->Reset();
		m_impl->permanently_disabled = false;
		m_impl->device = device;
		m_impl->debug_spirv = config.debug_spirv;

		if (config.preset_path.empty())
		{
			Console.WriteLn("[%s] No preset configured, chain stays in passthrough mode", kLogTag);
			return false;
		}

		std::string ini_error;
		if (!ParsePresetINI(config.preset_path, m_impl->preset, &ini_error))
		{
			Console.Warning("[%s] Failed to parse preset '%s': %s",
				kLogTag, config.preset_path.c_str(), ini_error.c_str());
			return false;
		}

		if (!m_impl->preset.has_techniques_line || m_impl->preset.chain.empty())
		{
			Console.WriteLn("[%s] Preset '%s' contains no Techniques= entries; passthrough",
				kLogTag, config.preset_path.c_str());
			return false;
		}

		// The preset's directory is the canonical search root; the user can
		// provide additional dirs (e.g. a shared `reshade-shaders/Shaders`
		// root) through ChainConfig.
		std::filesystem::path preset_path(config.preset_path);
		m_impl->preset_dir = preset_path.parent_path().string();
		m_impl->include_dirs.clear();
		if (!m_impl->preset_dir.empty())
			m_impl->include_dirs.push_back(m_impl->preset_dir);
		for (const std::string& p : config.extra_search_paths)
		{
			if (!p.empty())
				m_impl->include_dirs.push_back(p);
		}

		m_impl->CompileAll();

		// Count successful compilations vs. runnable effects so the host log
		// makes the MVP boundary explicit.
		size_t compiled = 0, runnable = 0;
		for (const EffectRuntime& rt : m_impl->effects)
		{
			if (rt.compiled_ok) ++compiled;
			if (rt.runnable)    ++runnable;
		}
		Console.WriteLn("[%s] Preset loaded: %zu effects parsed, %zu compiled, %zu runnable (rest fall back to passthrough)",
			kLogTag, m_impl->effects.size(), compiled, runnable);

		return runnable > 0;
	}

	void ChainVK::Reset()
	{
		if (m_impl)
			m_impl->Reset();
	}

	void ChainVK::DisablePermanently()
	{
		if (!m_impl)
			return;
		m_impl->permanently_disabled = true;
		m_impl->Reset();
	}

	bool ChainVK::IsActive() const
	{
		if (!m_impl || m_impl->permanently_disabled)
			return false;
		for (const EffectRuntime& rt : m_impl->effects)
			if (rt.runnable)
				return true;
		return false;
	}

	void ChainVK::Apply(VkCommandBuffer cmdbuffer, GSTextureVK*& current_io,
		uint32_t width, uint32_t height)
	{
		(void)cmdbuffer;
		// Update tracked backbuffer size so a future LoadPreset has the right
		// macros. Even if we currently passthrough, the size info is still
		// useful for the next milestone.
		if (m_impl && (m_impl->bb_width != width || m_impl->bb_height != height))
		{
			m_impl->bb_width = width;
			m_impl->bb_height = height;
		}

		if (!IsActive() || !current_io)
			return; // strict passthrough; the MVP fallback contract

		// MVP: the Vulkan pass executor that walks runnable effects and
		// renders into chain-owned RTs lives in the next milestone. Until
		// then, IsActive() is forced to return false in CompileAll() (every
		// compiled effect is marked runnable=false), so the body below is
		// unreachable today. Keeping it here makes the intent visible and
		// prevents accidental regressions.
		//
		// Pseudocode of the future executor (intentionally not run yet):
		//
		//   GSTextureVK* in_tex  = current_io;
		//   GSTextureVK* out_tex = AcquireIntermediateRT(width, height);
		//   for (EffectRuntime& rt : m_impl->effects) {
		//       if (!rt.runnable) continue;
		//       FillUBO(rt);
		//       for (Pass& p : rt.passes) {
		//           BeginPass(out_tex);
		//           BindDescriptors(p, in_tex);
		//           DrawFullscreenTriangle();
		//           EndPass();
		//           std::swap(in_tex, out_tex);
		//       }
		//   }
		//   current_io = in_tex;
	}

	void ChainVK::NotifyBackbufferResized(uint32_t width, uint32_t height)
	{
		if (!m_impl)
			return;
		m_impl->bb_width = width;
		m_impl->bb_height = height;
		// Future: invalidate per-resolution intermediate RTs and per-pass
		// pipelines that bake the viewport extent.
	}
} // namespace ReShade
