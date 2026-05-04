// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// OpenGL/OpenGL ES counterpart of `ReShade::ChainVK`.
//
// Lifecycle:
//   - `GSDeviceOGL` constructs one `ReShadeChainGL` after the GL context is
//     up. The chain is cheap to instantiate; it does no GPU work until
//     `LoadPreset()` is called.
//   - `LoadPreset()` parses the same INI format as the Vulkan side, runs
//     each listed *.fx through the bundled FX -> GLSL compiler and builds
//     `GLProgram`s + FBO render targets for the passes the MVP runtime
//     supports (graphics-only; CMAA_2 / compute / MRT are skipped with a
//     warning, mirroring `ChainVK`).
//   - `Apply()` is called once per frame inside `GSDeviceOGL::BeginPresent`
//     after `m_current` has the freshly merged frame. It runs the pass
//     list into a chain-owned ping-pong FBO and rewrites `current_io` so
//     that the existing `PresentRect` blit transparently picks up the
//     post-processed image.
//   - Any failure at any stage flips the chain into a passthrough state
//     and the same `Apply()` call is a no-op afterwards. The MVP document
//     explicitly allows this fallback ("不得崩溃，可随时降级到原图").
//
// Threading:
//   - All public methods must run on the GS thread, the same one that
//     owns the EGL/WGL context. No internal locking is required.

#pragma once

#include "glad/gl.h"

#include "GS/Renderers/Vulkan/ReShade/ReShadePresetINI.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GSDeviceOGL;
class GSTexture;
class GSTextureOGL;

namespace ReShade
{
	/// Configuration options gathered from the host. Fields mirror
	/// `ReShade::ChainConfig` (Vulkan side) so callers can share the same
	/// glue code on both backends.
	struct ChainConfigGL
	{
		/// Absolute path to the preset INI (e.g. .../1.默认·适应大部分.ini).
		std::string preset_path;
		/// Optional list of additional include directories used to resolve
		/// `#include "ReShade.fxh"` etc. The directory containing the INI
		/// is always added implicitly.
		std::vector<std::string> extra_search_paths;
		/// Append `#line` directives and source comments to the compiled
		/// GLSL. Off by default.
		bool debug_glsl = false;
	};

	/// Forward-declared internal data for the chain. Defined inside the
	/// .cpp so the header stays free of GL-specific glue.
	struct ChainImplGL;

	class ChainGL
	{
	public:
		ChainGL();
		~ChainGL();

		ChainGL(const ChainGL&) = delete;
		ChainGL& operator=(const ChainGL&) = delete;

		/// Loads and compiles the given preset. Returns true if at least
		/// one effect compiled successfully and was wired up; otherwise
		/// the chain stays in passthrough mode and `Apply()` is a no-op.
		///
		/// Calling `LoadPreset()` again replaces the existing chain.
		bool LoadPreset(GSDeviceOGL* device, const ChainConfigGL& config);

		/// Drops all effects, programs and intermediate RTs. The chain
		/// switches to passthrough mode. Safe to call multiple times.
		void Reset();

		/// Forces the chain to disable itself for the rest of the
		/// session. Used by the host when a fatal GL error makes the
		/// chain untrustworthy.
		void DisablePermanently();

		/// True if the chain currently has at least one runnable effect.
		bool IsActive() const;

		/// Per-frame entry point. `current_io` is `m_current` from the
		/// GS device on entry and points at one of the chain's
		/// intermediate RTs (or stays the same texture on passthrough)
		/// on exit. `width` / `height` are the swap chain extent (used
		/// to size the ping-pong RTs).
		///
		/// `Apply()` never throws. Any failure during a frame causes the
		/// chain to fall back to passthrough for that frame (or
		/// permanently, depending on severity).
		void Apply(GSTextureOGL*& current_io, uint32_t width, uint32_t height);

		/// Notifies the chain that the backbuffer dimensions changed.
		/// The chain destroys per-resolution resources and rebuilds them
		/// lazily on the next `Apply()` call.
		void NotifyBackbufferResized(uint32_t width, uint32_t height);

	private:
		std::unique_ptr<ChainImplGL> m_impl;
	};
} // namespace ReShade
