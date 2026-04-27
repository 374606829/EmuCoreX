// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Vulkan-side runtime that owns the compiled ReShade effect chain.
//
// Lifecycle:
//   - GSDeviceVK constructs one ReShadeChainVK after the Vulkan device is up.
//   - LoadPreset() parses the INI, compiles every listed *.fx with the
//     bundled ReShade FX compiler and builds Vulkan pipelines for the passes
//     that are supported on this MVP build (graphics-only; the compute
//     passes used by effects like CMAA_2 are skipped with a warning).
//   - Apply() is called once per frame inside GSDeviceVK::BeginPresent,
//     between the m_current ShaderReadOnly transition and the swapchain
//     render pass. It runs the effect chain into an intermediate render
//     target and rewrites m_current so the existing swapchain blit picks up
//     the post-processed image transparently.
//   - Any failure at any stage flips the chain into a "passthrough" state
//     and the same Apply() call is a no-op afterwards. The MVP document
//     explicitly allows this fallback.
//
// Threading:
//   - All public methods must be called on the GS thread (the same thread
//     that drives GSDeviceVK). No internal locking is required.

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "ReShadeFXCompiler.h"
#include "ReShadePresetINI.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class GSDeviceVK;
class GSTexture;
class GSTextureVK;

namespace ReShade
{
	/// Configuration options gathered from the host. Currently sourced from
	/// EmuFolders + GSConfig at the call site.
	struct ChainConfig
	{
		/// Absolute path to the preset INI (e.g. .../1.默认·适应大部分.ini).
		std::string preset_path;
		/// Optional list of additional include directories used to resolve
		/// `#include "ReShade.fxh"` etc. The directory containing the INI
		/// is always added implicitly.
		std::vector<std::string> extra_search_paths;
		/// Output debug info into the SPIR-V binaries. Off by default.
		bool debug_spirv = false;
	};

	/// Forward-declared internal data for the chain. Defined inside the .cpp
	/// so the header stays free of Vulkan glue and ReShade types.
	struct ChainImpl;

	/// Vulkan-side runtime that owns the compiled ReShade effect chain.
	class ChainVK
	{
	public:
		ChainVK();
		~ChainVK();

		ChainVK(const ChainVK&) = delete;
		ChainVK& operator=(const ChainVK&) = delete;

		/// Loads and compiles the given preset. Returns true if at least one
		/// effect compiled successfully and was wired up; otherwise the chain
		/// stays in passthrough mode and Apply() is a no-op.
		///
		/// Calling LoadPreset() again replaces the existing chain.
		bool LoadPreset(GSDeviceVK* device, const ChainConfig& config);

		/// Drops all effects, pipelines and intermediate RTs. The chain
		/// switches to passthrough mode. Safe to call multiple times.
		void Reset();

		/// Forces the chain to mark every effect as failed. Apply() becomes a
		/// strict no-op until the next LoadPreset() call. Used by the host
		/// when a fatal Vulkan error occurs and the chain shouldn't be
		/// trusted any more.
		void DisablePermanently();

		/// Returns true if the chain currently has at least one runnable
		/// effect.
		bool IsActive() const;

		/// Per-frame entry point. `current_io` is `m_current` from the GS
		/// device — on entry it must be in `ShaderReadOnly` layout. On exit
		/// it points to either the original texture (passthrough on failure)
		/// or to the chain output (post-processed). The chain owns the
		/// output texture and keeps it alive across frames.
		///
		/// `cmdbuffer` is the caller's recording command buffer. The chain
		/// will not start its own render passes inside an open RP; the
		/// caller is responsible for ending any active render pass before
		/// invoking Apply().
		///
		/// Apply() never throws. Any failure during a frame causes the
		/// chain to fall back to passthrough for that frame (or
		/// permanently, depending on severity).
		void Apply(VkCommandBuffer cmdbuffer, GSTextureVK*& current_io,
			uint32_t width, uint32_t height);

		/// Notifies the chain that the backbuffer dimensions changed (e.g.
		/// after a window resize). The chain destroys per-resolution
		/// resources and rebuilds them lazily on the next Apply().
		void NotifyBackbufferResized(uint32_t width, uint32_t height);

	private:
		std::unique_ptr<ChainImpl> m_impl;
	};
} // namespace ReShade
