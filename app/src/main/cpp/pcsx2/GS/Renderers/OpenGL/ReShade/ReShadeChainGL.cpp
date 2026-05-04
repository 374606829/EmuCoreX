// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// OpenGL/OpenGL ES side ReShade chain runtime. Mirrors `ReShade::ChainVK`
// closely; the high-level execution model (compile → classify → ping-pong
// → rewrite m_current) is identical, only the GPU API is swapped from
// Vulkan + SPIR-V to GL + GLSL.
//
// Per-effect resources owned by the runtime:
//   - One UBO (glGenBuffers + GL_UNIFORM_BUFFER) sized to the effect's
//     `total_uniform_size` and refilled every frame from the preset INI
//     plus the FX-declared initializers.
//   - One internal `GSTextureOGL` per non-semantic texture declared by
//     the effect (allocated lazily through GSDevice helpers so the
//     existing format mapping is reused).
//   - One `GLProgram` per pass (compiled from the bundled GLSL backend
//     output) with the leading `#version 430` line rewritten to whatever
//     the host context actually reports, plus precision qualifiers and
//     binding-qualifier removal when targeting GLES 3.0.
//   - One sampler object per `(filter, wrap_u, wrap_v, wrap_w, mipmap)`
//     combination, cached on the chain itself.
//
// Chain-wide resources:
//   - Two ping-pong RTs sized to the swap chain extent. Effects that
//     write the implicit "BackBuffer" target alternate between the two;
//     effects that target an internal RT write into their own texture.
//   - One scratch FBO that we re-attach as needed.
//   - One scratch VAO so the shader's `gl_VertexID`-driven full-screen
//     triangle works on Core profiles that require a VAO to be bound.
//
// Failure handling stays "passthrough on error": any pass failing to
// compile, link or build flips the parent effect's `runnable` to false,
// the rest of the chain keeps trying, and the final `Apply()` returns
// the original frame untouched if nothing succeeded.

#include "ReShadeChainGL.h"

#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
#include "GS/Renderers/OpenGL/GSTextureOGL.h"
#include "GS/Renderers/OpenGL/GLState.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/OpenGL/ReShade/ReShadeFXCompilerGL.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "effect_module.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <utility>

namespace
{
	constexpr const char* kLogTag = "ReShade";

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

	GSTexture::Format MapTextureFormat(reshadefx::texture_format fmt)
	{
		using FX = reshadefx::texture_format;
		switch (fmt)
		{
			case FX::r8:       return GSTexture::Format::UNorm8;
			case FX::r32f:     return GSTexture::Format::Float32;
			case FX::rgba8:    return GSTexture::Format::Color;
			case FX::rgba16:   return GSTexture::Format::ColorClip;
			case FX::rgba16f:  return GSTexture::Format::ColorHDR;
			case FX::rgb10a2:  return GSTexture::Format::ColorHQ;
			case FX::r16f:
			case FX::r16:
			case FX::rg8:
			case FX::rg16f:
			case FX::rg16:
			case FX::rg32f:
			case FX::rg11b10f:
			case FX::rgba32f:
				return GSTexture::Format::ColorHDR;
			default:
				return GSTexture::Format::Color;
		}
	}

	GLenum MapMinFilter(reshadefx::filter_mode m, bool has_mipmap)
	{
		using F = reshadefx::filter_mode;
		const bool min_linear =
			(m != F::min_mag_mip_point && m != F::min_mag_point_mip_linear &&
			 m != F::min_point_mag_linear_mip_point && m != F::min_point_mag_mip_linear);
		const bool mip_linear =
			(m != F::min_mag_mip_point && m != F::min_point_mag_linear_mip_point &&
			 m != F::min_linear_mag_mip_point && m != F::min_mag_linear_mip_point);

		if (!has_mipmap)
			return min_linear ? GL_LINEAR : GL_NEAREST;
		if (min_linear)
			return mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
		return mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
	}

	GLenum MapMagFilter(reshadefx::filter_mode m)
	{
		using F = reshadefx::filter_mode;
		switch (m)
		{
			case F::min_mag_mip_point:
			case F::min_mag_point_mip_linear:
			case F::min_linear_mag_mip_point:
			case F::min_linear_mag_point_mip_linear:
				return GL_NEAREST;
			default:
				return GL_LINEAR;
		}
	}

	GLenum MapAddressMode(reshadefx::texture_address_mode m)
	{
		using A = reshadefx::texture_address_mode;
		switch (m)
		{
			case A::wrap:   return GL_REPEAT;
			case A::mirror: return GL_MIRRORED_REPEAT;
			case A::clamp:  return GL_CLAMP_TO_EDGE;
			case A::border:
				// GLES 3.x only adds CLAMP_TO_BORDER through an extension. Fall
				// back to CLAMP_TO_EDGE so the chain at least produces a
				// reasonable image instead of failing to create the sampler.
				return GL_CLAMP_TO_EDGE;
			default:        return GL_CLAMP_TO_EDGE;
		}
	}

	uint64_t SamplerKey(const reshadefx::sampler_desc& d, bool has_mipmap)
	{
		uint64_t k = 0;
		k |= static_cast<uint64_t>(d.filter) & 0xFFu;
		k |= (static_cast<uint64_t>(d.address_u) & 0xFFu) << 8;
		k |= (static_cast<uint64_t>(d.address_v) & 0xFFu) << 16;
		k |= (static_cast<uint64_t>(d.address_w) & 0xFFu) << 24;
		k |= (has_mipmap ? 1ull : 0ull) << 32;
		return k;
	}

	// Helper: returns true if `line` is a `#extension <name>` directive
	// (whitespace tolerant). Does not match `#extension all : ...`
	// (which is fine but irrelevant to our needs).
	bool LineIsExtensionDirective(const std::string& line)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			++i;
		return line.compare(i, 10, "#extension") == 0;
	}

	bool LineMentionsToken(const std::string& line, const char* token)
	{
		return line.find(token) != std::string::npos;
	}

	// Walk over `body` line by line. For each line that is an
	// `#extension <name>` directive, run `visit(line)`; if `visit`
	// returns true, the line is removed from `body` and `out_collected`
	// receives the original (verbatim) text. Useful for either stripping
	// desktop-only extensions, or extracting extensions to relocate
	// them above the precision qualifiers we inject (so GLES drivers
	// see the spec-mandated order).
	void ProcessExtensionDirectives(std::string& body,
		std::string* out_collected,
		const std::function<bool(const std::string&)>& visit)
	{
		size_t pos = 0;
		while (pos < body.size())
		{
			const size_t line_end = body.find('\n', pos);
			const size_t cut_end = (line_end == std::string::npos) ? body.size() : line_end + 1;
			std::string line = body.substr(pos, cut_end - pos);

			if (LineIsExtensionDirective(line) && visit(line))
			{
				if (out_collected)
				{
					if (!line.empty() && line.back() != '\n')
						line.push_back('\n');
					*out_collected += line;
				}
				body.erase(pos, cut_end - pos);
				continue;
			}
			pos = cut_end;
		}
	}

	// Replace the leading `#version XXX[ es]` line of a reshadefx GLSL
	// blob (always `#version 430\n`) with the version line that matches
	// the host context, plus the GLES precision qualifiers ES requires.
	//
	// We also drop / shim a few desktop-only `#extension` directives the
	// upstream codegen emits unconditionally, since GLES drivers refuse
	// to compile shaders that reference unknown extensions even at
	// `: enable` severity.
	std::string AdaptShaderForContext(const std::string& src,
		bool is_gles, int gles_major, int gles_minor, int desktop_major, int desktop_minor,
		bool is_fragment)
	{
		std::string out;
		out.reserve(src.size() + 512);

		const size_t newline = src.find('\n');
		std::string body = (newline == std::string::npos) ? std::string() : src.substr(newline + 1);

		if (is_gles)
		{
			// First, deal with the `#extension` directives the codegen
			// emits inside the body. GLSL ES requires every #extension
			// directive to come BEFORE any non-preprocessor token, so
			// we extract them up-front and put them right after our
			// `#version` line. Desktop-only extensions are dropped: the
			// driver would otherwise refuse to compile the shader.
			std::string relocated_extensions;
			ProcessExtensionDirectives(body, &relocated_extensions,
				[](const std::string& line) -> bool {
					// Drop GL_ARB_derivative_control (desktop only — no
					// GLES counterpart) and GL_NV_gpu_shader5 (only used
					// for 16-bit types, which we disabled). Both are
					// handled by `: enable` semantics or by our shim
					// `#define`s below.
					if (LineMentionsToken(line, "GL_ARB_derivative_control"))
						return true;
					if (LineMentionsToken(line, "GL_NV_gpu_shader5"))
						return true;
					return true; // Always relocate other extensions too.
				});
			// The visitor returns `true` for every extension directive,
			// so `body` no longer contains any extension lines. The ones
			// we kept are now in `relocated_extensions`. We then drop
			// any line that matched a desktop-only extension by simply
			// not re-emitting it.

			// We gate `LoadPreset()` on GLES 3.1+ in `DetectContextLevels`,
			// so we can rely on `layout(binding = N)` being legal here and
			// keep the shader text mostly verbatim.
			if (gles_major > 3 || (gles_major == 3 && gles_minor >= 2))
				out = "#version 320 es\n";
			else
				out = "#version 310 es\n";

			// Re-emit the surviving extension directives, but drop the
			// desktop-only ones as they would crash the compile.
			std::string ext_lines = relocated_extensions;
			const auto drop_line_if = [&ext_lines](const char* token) {
				size_t pos = 0;
				while ((pos = ext_lines.find(token, pos)) != std::string::npos)
				{
					size_t line_start = ext_lines.rfind('\n', pos);
					line_start = (line_start == std::string::npos) ? 0 : (line_start + 1);
					const size_t line_end = ext_lines.find('\n', pos);
					const size_t cut_end = (line_end == std::string::npos)
						? ext_lines.size() : line_end + 1;
					ext_lines.erase(line_start, cut_end - line_start);
					pos = line_start;
				}
			};
			drop_line_if("GL_ARB_derivative_control");
			drop_line_if("GL_NV_gpu_shader5");
			// ANGLE / several GLES stacks reject `#extension
			// GL_EXT_control_flow_attributes` outright when the device does
			// not expose the extension, even though the only use in the
			// generated code is guarded by `#if GL_EXT_control_flow_attributes`
			// (undefined macro → 0 → the [[branch]] attributes are skipped).
			// Dropping the extension line keeps the shader valid; the
			// preprocessor guard still suppresses the attributes.
			drop_line_if("GL_EXT_control_flow_attributes");
			out += ext_lines;

			// ES requires explicit precision qualifiers on every floating
			// point and integer declaration. `highp` matches the upstream
			// 4.x default the FX shaders were tested against; mediump
			// would be faster on tile-based GPUs but risks visible banding
			// on legacy bloom / colour-grading effects.
			out += "precision highp float;\n";
			out += "precision highp int;\n";
			out += "precision highp sampler2D;\n";
			if (gles_major > 3 || (gles_major == 3 && gles_minor >= 1))
				out += "precision highp sampler2DMS;\n";

			// Coarse/fine derivative shims so effects that asked for them
			// behave on GLES (which only exposes the regular dFdx/dFdy).
			out +=
				"#define dFdxCoarse(x) dFdx(x)\n"
				"#define dFdxFine(x) dFdx(x)\n"
				"#define dFdyCoarse(y) dFdy(y)\n"
				"#define dFdyFine(y) dFdy(y)\n"
				"#define fwidthCoarse(p) fwidth(p)\n"
				"#define fwidthFine(p) fwidth(p)\n";
		}
		else
		{
			// Desktop GL: keep the upstream `#version 430` (which the FX
			// frontend already targets). If the host context is older than
			// 4.30 the shader will simply fail to compile, the runtime
			// then marks that effect as un-runnable - the MVP fallback.
			out = "#version 430\n";
			(void)desktop_major;
			(void)desktop_minor;
		}

		out += body;
		(void)is_fragment;
		return out;
	}
} // namespace

namespace ReShade
{
	// ----------------------------------------------------------------------
	// Lightweight GL program wrapper. We don't use GSDeviceOGL's GLProgram
	// because it routes its compile/link error log through Console.Error
	// without our `[ReShade]` prefix, which makes the diagnostics
	// invisible to anyone filtering the logcat by tag. This wrapper does
	// the same job but lets us capture and re-emit the full info log with
	// the proper prefix and dump the failing GLSL to disk for offline
	// inspection.
	struct ChainProgramGL
	{
		GLuint id = 0;

		ChainProgramGL() = default;
		ChainProgramGL(const ChainProgramGL&) = delete;
		ChainProgramGL& operator=(const ChainProgramGL&) = delete;
		ChainProgramGL(ChainProgramGL&& o) noexcept { id = o.id; o.id = 0; }
		ChainProgramGL& operator=(ChainProgramGL&& o) noexcept
		{
			if (this != &o)
			{
				Destroy();
				id = o.id;
				o.id = 0;
			}
			return *this;
		}
		~ChainProgramGL() { Destroy(); }

		bool IsValid() const { return id != 0; }

		void Destroy()
		{
			if (id != 0)
			{
				glDeleteProgram(id);
				id = 0;
			}
		}

		void Bind() const { glUseProgram(id); }

		void BindUniformBlock(const char* name, GLuint binding) const
		{
			const GLuint block_index = glGetUniformBlockIndex(id, name);
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(id, block_index, binding);
		}
	};

	// ----------------------------------------------------------------------
	// Per-pass GPU resources.
	// ----------------------------------------------------------------------
	struct PassRuntimeGL
	{
		ChainProgramGL program;
		bool program_ok = false;

		// One entry per FX sampler bound by this pass. `binding` is the
		// `entry_point_binding` slot the codegen baked into the GLSL
		// `layout(binding = N)` qualifier, so we drive
		// glActiveTexture / glBindTexture / glBindSampler off it directly
		// (rather than off the iteration index, which would mis-bind any
		// pass that uses non-contiguous sampler slots).
		struct SamplerBinding
		{
			GLuint binding = 0;
			std::string texture_name; // FX texture unique_name
			std::string texture_semantic;
			GLuint sampler_object = 0;
			bool srgb = false;
			bool has_mipmap = false;
		};
		std::vector<SamplerBinding> sampler_bindings;

		// Per-pass attachment description.
		bool writes_backbuffer = true;
		std::string rt_name;
		uint32_t viewport_width = 0;
		uint32_t viewport_height = 0;
		uint32_t num_vertices = 3;
		bool generate_mipmaps = false;
		bool clear_render_target = false;

		// Pass name (for diagnostics).
		std::string pass_name;
	};

	// ----------------------------------------------------------------------
	// Per-effect runtime state.
	// ----------------------------------------------------------------------
	struct EffectRuntimeGL
	{
		std::string effect_filename;
		std::string technique_name;

		CompiledEffectGL compiled;
		bool compiled_ok = false;

		bool runnable = false;
		std::string skip_reason;

		GLuint ubo = 0;
		uint32_t ubo_size = 0;
		std::vector<uint8_t> ubo_staging;

		std::unordered_map<std::string, std::unique_ptr<GSTextureOGL>> textures;

		std::vector<PassRuntimeGL> passes;
	};

	// ----------------------------------------------------------------------
	// ChainImplGL
	// ----------------------------------------------------------------------
	struct ChainImplGL
	{
		GSDeviceOGL* device = nullptr;

		Preset preset;
		std::string preset_dir;
		std::vector<std::string> include_dirs;
		bool debug_glsl = false;
		bool permanently_disabled = false;
		bool disabled_unsupported_context = false;

		bool is_gles = false;
		int gles_major = 0;
		int gles_minor = 0;
		int desktop_major = 0;
		int desktop_minor = 0;

		uint32_t bb_width = 0;
		uint32_t bb_height = 0;

		// Two backbuffer-sized RTs alternated for BackBuffer-target passes.
		std::unique_ptr<GSTextureOGL> ping_pong[2];

		// 1x1 R32F texture filled with 1.0 (far plane) for effects that
		// read the DepthBuffer semantic. The MVP never exposes real depth,
		// so the chain feeds a neutral value so depth-aware effects don't
		// produce garbage.
		std::unique_ptr<GSTextureOGL> dummy_depth;

		// Scratch FBO + VAO recreated lazily on first use.
		GLuint fbo = 0;
		GLuint vao = 0;

		// Sampler cache shared across effects.
		std::unordered_map<uint64_t, GLuint> sampler_cache;

		std::vector<EffectRuntimeGL> effects;

		// ------------------------------------------------------------------
		// Lifetime
		// ------------------------------------------------------------------
		void Reset();
		void DestroyShared();
		void DestroyEffect(EffectRuntimeGL& rt);

		bool DetectContextLevels();

		// Sampler / texture / UBO helpers.
		GLuint GetSampler(const reshadefx::sampler_desc& d, bool has_mipmap);
		bool EnsureScratchObjects();
		bool EnsurePingPong(uint32_t width, uint32_t height);
		bool EnsureDummyDepth();

		// Compile + GPU resource setup.
		void CompileAndBuild();
		bool BuildEffect(EffectRuntimeGL& rt);
		bool BuildEffectTextures(EffectRuntimeGL& rt);
		bool BuildEffectUbo(EffectRuntimeGL& rt);
		bool BuildEffectPasses(EffectRuntimeGL& rt);

		// Per-frame.
		void FillUniformBuffer(EffectRuntimeGL& rt);
		GLuint ResolveSampledTexture(const std::string& tex_name,
			const std::string& tex_semantic,
			GSTextureOGL* current_back_buffer,
			const EffectRuntimeGL& rt) const;
		GSTextureOGL* FindEffectTexture(const std::string& key, const EffectRuntimeGL& rt) const;

		bool ApplyEffect(EffectRuntimeGL& rt, GSTextureOGL*& cur_color);

		void RestoreDeviceState();

		// Compile + link a vertex/fragment program. On failure, the GL
		// info log is captured, prefixed with `[ReShade]` and printed to
		// the host log; the full GLSL source is also written to
		// `<EmuFolders::Logs>/reshade_bad_shader_<effect>_<pass>_<stage>.glsl`
		// so it can be inspected after the fact. Returns true on
		// success, false on any compile or link error.
		bool BuildPassProgram(ChainProgramGL& out_program,
			const std::string& vs_src, const std::string& fs_src,
			const std::string& effect_name, const std::string& pass_name,
			std::string* out_error);
	};

	namespace
	{
		// Keep dump filenames filesystem-friendly (POSIX + NTFS): drop
		// path separators and any byte that is a known forbidden char on
		// Windows. We keep most extended characters (so technique /
		// effect names with CJK glyphs survive) and rely on UTF-8 paths.
		std::string SanitizePathComponent(const std::string& in)
		{
			std::string out;
			out.reserve(in.size());
			for (unsigned char c : in)
			{
				if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
					c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20)
				{
					out.push_back('_');
				}
				else
				{
					out.push_back(static_cast<char>(c));
				}
			}
			if (out.empty())
				out = "_";
			return out;
		}

		// Returns the dumped path on success, empty string otherwise. The
		// caller can include the path in its log line so the user knows
		// where to look for the offending GLSL.
		std::string DumpBadShaderToDisk(const std::string& effect_name,
			const std::string& pass_name,
			const char* stage_label,
			const std::string& source,
			const std::string& info_log)
		{
			if (EmuFolders::Logs.empty())
				return std::string();
			const std::string filename = "reshade_bad_shader_" +
				SanitizePathComponent(effect_name) + "_" +
				SanitizePathComponent(pass_name) + "_" + stage_label + ".glsl";
			const std::string path = Path::Combine(EmuFolders::Logs, filename);
			std::ofstream ofs(path, std::ios::out | std::ios::binary);
			if (!ofs.is_open())
				return std::string();
			ofs.write(source.data(), static_cast<std::streamsize>(source.size()));
			ofs << "\n\n// ----- GL info log -----\n";
			ofs.write(info_log.data(), static_cast<std::streamsize>(info_log.size()));
			return path;
		}

		// Truncate long info logs so logcat (which has a per-line limit)
		// keeps the most useful prefix.
		std::string ClipForLog(const std::string& s, size_t max_len = 480)
		{
			if (s.size() <= max_len)
				return s;
			return s.substr(0, max_len) + " ...(truncated)";
		}
	} // namespace

	// ----------------------------------------------------------------------
	// Reset / destruction
	// ----------------------------------------------------------------------
	void ChainImplGL::DestroyEffect(EffectRuntimeGL& rt)
	{
		for (PassRuntimeGL& p : rt.passes)
		{
			if (p.program.IsValid())
				p.program.Destroy();
		}
		rt.passes.clear();

		if (rt.ubo != 0)
		{
			glDeleteBuffers(1, &rt.ubo);
			rt.ubo = 0;
		}
		rt.ubo_size = 0;
		rt.ubo_staging.clear();

		rt.textures.clear();
		rt.runnable = false;
	}

	void ChainImplGL::DestroyShared()
	{
		for (auto& kv : sampler_cache)
		{
			if (kv.second != 0)
				glDeleteSamplers(1, &kv.second);
		}
		sampler_cache.clear();

		ping_pong[0].reset();
		ping_pong[1].reset();
		dummy_depth.reset();

		if (fbo != 0)
		{
			glDeleteFramebuffers(1, &fbo);
			fbo = 0;
		}
		if (vao != 0)
		{
			glDeleteVertexArrays(1, &vao);
			vao = 0;
		}
	}

	void ChainImplGL::Reset()
	{
		for (EffectRuntimeGL& rt : effects)
			DestroyEffect(rt);
		effects.clear();
		DestroyShared();

		preset.Clear();
		preset_dir.clear();
		include_dirs.clear();
		bb_width = bb_height = 0;
	}

	bool ChainImplGL::DetectContextLevels()
	{
		if (!device)
			return false;
		is_gles = device->IsGLESDevice();
		gles_major = gles_minor = 0;
		desktop_major = desktop_minor = 0;
		if (is_gles)
		{
			// The chain relies on `layout(binding = N)` being legal in
			// shader source so it can avoid the per-frame reflection +
			// glUniformBlockBinding / glUniform1i dance entirely. That
			// qualifier is core in GLES 3.1 and later; on GLES 3.0 we
			// would need an extra strip-and-rebind path that is not
			// worth the maintenance burden for the MVP. The chain
			// reports a clear "context too old" warning in that case
			// and stays in passthrough.
			if (GLAD_GL_ES_VERSION_3_2)
			{
				gles_major = 3;
				gles_minor = 2;
			}
			else if (GLAD_GL_ES_VERSION_3_1)
			{
				gles_major = 3;
				gles_minor = 1;
			}
			else
			{
				return false;
			}
		}
		else
		{
			// Desktop targets `#version 430`. We do not bother probing
			// the exact reported version; if the host driver is older the
			// shader compile step will surface a clear error and the
			// chain will fall back to passthrough on its own.
			desktop_major = 4;
			desktop_minor = 3;
		}
		return true;
	}

	GLuint ChainImplGL::GetSampler(const reshadefx::sampler_desc& d, bool has_mipmap)
	{
		const uint64_t key = SamplerKey(d, has_mipmap);
		auto it = sampler_cache.find(key);
		if (it != sampler_cache.end())
			return it->second;

		GLuint s = 0;
		glGenSamplers(1, &s);
		if (s == 0)
			return 0;

		glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, MapMinFilter(d.filter, has_mipmap));
		glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, MapMagFilter(d.filter));
		glSamplerParameteri(s, GL_TEXTURE_WRAP_S, MapAddressMode(d.address_u));
		glSamplerParameteri(s, GL_TEXTURE_WRAP_T, MapAddressMode(d.address_v));
		glSamplerParameteri(s, GL_TEXTURE_WRAP_R, MapAddressMode(d.address_w));
		if (has_mipmap && std::isfinite(d.lod_bias))
		{
			// glSamplerParameterf(GL_TEXTURE_LOD_BIAS) is desktop-only,
			// so we stay default on GLES.
			if (!is_gles)
				glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, d.lod_bias);
		}
		sampler_cache[key] = s;
		return s;
	}

	bool ChainImplGL::EnsureScratchObjects()
	{
		if (fbo == 0)
			glGenFramebuffers(1, &fbo);
		if (vao == 0)
			glGenVertexArrays(1, &vao);
		return fbo != 0 && vao != 0;
	}

	bool ChainImplGL::EnsurePingPong(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return false;
		const bool size_changed = (bb_width != width || bb_height != height);
		const bool need_alloc = !ping_pong[0] || !ping_pong[1] || size_changed;
		if (!need_alloc)
			return true;

		ping_pong[0].reset();
		ping_pong[1].reset();
		bb_width = width;
		bb_height = height;

		for (int i = 0; i < 2; ++i)
		{
			GSTexture* tex = device->CreateRenderTarget(static_cast<int>(width),
				static_cast<int>(height), GSTexture::Format::Color, /*clear=*/false, /*prefer_reuse=*/false);
			if (!tex)
			{
				ping_pong[0].reset();
				ping_pong[1].reset();
				return false;
			}
			ping_pong[i].reset(static_cast<GSTextureOGL*>(tex));
		}
		return true;
	}

	bool ChainImplGL::EnsureDummyDepth()
	{
		if (dummy_depth)
			return true;
		GSTexture* tex = device->CreateTexture(1, 1, 1, GSTexture::Format::Float32, /*prefer_reuse=*/false);
		if (!tex)
			return false;
		dummy_depth.reset(static_cast<GSTextureOGL*>(tex));
		const float far_value = 1.0f;
		const GSVector4i rect(0, 0, 1, 1);
		dummy_depth->Update(rect, &far_value, sizeof(float), 0);
		return true;
	}

	// ----------------------------------------------------------------------
	// Effect compilation + GPU resource construction
	// ----------------------------------------------------------------------
	void ChainImplGL::CompileAndBuild()
	{
		effects.clear();
		effects.reserve(preset.chain.size());

		for (const TechniqueRef& ref : preset.chain)
		{
			EffectRuntimeGL rt;
			rt.effect_filename = ref.effect_filename;
			rt.technique_name = ref.technique_name;

			const std::filesystem::path source =
				std::filesystem::path(preset_dir) / ref.effect_filename;
			if (!std::filesystem::exists(source))
			{
				rt.skip_reason = "Effect file not found: " + source.string();
				Console.Warning("[%s] %s", kLogTag, rt.skip_reason.c_str());
				effects.push_back(std::move(rt));
				continue;
			}

			FXCompileOptionsGL opts;
			opts.source_path = source.string();
			opts.include_paths = include_dirs;
			opts.buffer_width = bb_width ? bb_width : 1920u;
			opts.buffer_height = bb_height ? bb_height : 1080u;
			// PC ReShade uses 0x10000 for OpenGL renderers regardless of
			// desktop-vs-ES, mirror that so effects branching on
			// __RENDERER__ behave the same.
			opts.renderer_id = 0x10000;
			opts.debug_info = debug_glsl;

			if (!CompileFXGLSL(opts, rt.compiled))
			{
				rt.compiled_ok = false;
				rt.runnable = false;
				rt.skip_reason = rt.compiled.errors.empty()
					? std::string("FX compilation failed without an error message")
					: rt.compiled.errors;
				Console.Warning("[%s] Effect '%s' failed to compile - %s",
					kLogTag, rt.effect_filename.c_str(), rt.skip_reason.c_str());
				effects.push_back(std::move(rt));
				continue;
			}
			rt.compiled_ok = true;

			if (!BuildEffect(rt))
			{
				DestroyEffect(rt);
				rt.runnable = false;
				if (rt.skip_reason.empty())
					rt.skip_reason = "GL resource construction failed";
				Console.Warning("[%s] Effect '%s' compiled but cannot run: %s",
					kLogTag, rt.effect_filename.c_str(), rt.skip_reason.c_str());
			}
			else
			{
				rt.runnable = true;
				const reshadefx::effect_module* mod = rt.compiled.module();
				Console.WriteLn("[%s] Compiled '%s' OK (techniques=%zu, samplers=%zu, total_uniforms=%u bytes), runnable=true",
					kLogTag, rt.effect_filename.c_str(),
					mod ? mod->techniques.size() : 0,
					mod ? mod->samplers.size() : 0,
					mod ? mod->total_uniform_size : 0);
			}

			effects.push_back(std::move(rt));
		}
	}

	bool ChainImplGL::BuildEffect(EffectRuntimeGL& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
		{
			rt.skip_reason = "Compiled effect has no module";
			return false;
		}

		// Pick the requested technique or fall back to the first one.
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
			return false;
		}

		// MVP scope checks: graphics passes only, no MRT, full VS+PS.
		for (const reshadefx::pass& p : technique->passes)
		{
			if (!p.cs_entry_point.empty())
			{
				rt.skip_reason = "Compute passes are not supported in this MVP runtime";
				return false;
			}
			int rt_count = 0;
			for (const auto& name : p.render_target_names)
				if (!name.empty()) ++rt_count;
			if (rt_count > 1)
			{
				rt.skip_reason = "Multi-render-target passes are not supported in this MVP runtime";
				return false;
			}
			if (p.vs_entry_point.empty() || p.ps_entry_point.empty())
			{
				rt.skip_reason = "Pass missing vertex or pixel entry point";
				return false;
			}
		}

		if (!BuildEffectTextures(rt))
			return false;
		if (!BuildEffectUbo(rt))
			return false;
		if (!BuildEffectPasses(rt))
			return false;
		return true;
	}

	bool ChainImplGL::BuildEffectTextures(EffectRuntimeGL& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
			return false;

		for (const reshadefx::texture& t : mod->textures)
		{
			if (!t.semantic.empty())
				continue;

			const GSTexture::Format gfx_fmt = MapTextureFormat(t.format);
			const int width = static_cast<int>(std::max<uint32_t>(t.width, 1));
			const int height = static_cast<int>(std::max<uint32_t>(t.height, 1));
			const int levels = static_cast<int>(std::max<uint16_t>(t.levels, 1));

			GSTexture* tex = device->CreateRenderTarget(width, height, gfx_fmt, /*clear=*/false, /*prefer_reuse=*/false);
			if (!tex)
			{
				rt.skip_reason = "Failed to allocate internal texture: " + t.unique_name;
				return false;
			}
			// Use the GL device's mipmap path instead of allocating extra
			// levels manually; effects that ask for them call
			// GenerateMipmap explicitly via the pass description.
			(void)levels;
			rt.textures[t.unique_name] = std::unique_ptr<GSTextureOGL>(static_cast<GSTextureOGL*>(tex));
		}
		return true;
	}

	bool ChainImplGL::BuildEffectUbo(EffectRuntimeGL& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		uint32_t size = mod ? mod->total_uniform_size : 0;
		if (size == 0)
			size = 16;
		size = (size + 15u) & ~15u;
		rt.ubo_size = size;
		rt.ubo_staging.assign(size, 0);

		glGenBuffers(1, &rt.ubo);
		if (rt.ubo == 0)
		{
			rt.skip_reason = "Failed to allocate UBO";
			return false;
		}
		glBindBuffer(GL_UNIFORM_BUFFER, rt.ubo);
		glBufferData(GL_UNIFORM_BUFFER, size, rt.ubo_staging.data(), GL_DYNAMIC_DRAW);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		return true;
	}

	bool ChainImplGL::BuildEffectPasses(EffectRuntimeGL& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
			return false;

		const reshadefx::technique* technique = nullptr;
		if (!rt.technique_name.empty())
		{
			for (const auto& t : mod->techniques)
				if (t.name == rt.technique_name) { technique = &t; break; }
		}
		if (!technique && !mod->techniques.empty())
			technique = &mod->techniques.front();
		if (!technique)
			return false;

		rt.passes.reserve(technique->passes.size());
		for (const reshadefx::pass& p : technique->passes)
		{
			rt.passes.emplace_back();
			PassRuntimeGL& pr = rt.passes.back();
			pr.pass_name = p.name;
			pr.writes_backbuffer = p.render_target_names[0].empty();
			pr.rt_name = p.render_target_names[0];
			pr.viewport_width = p.viewport_width;
			pr.viewport_height = p.viewport_height;
			pr.num_vertices = std::max(1u, p.num_vertices);
			pr.generate_mipmaps = p.generate_mipmaps && !pr.writes_backbuffer;
			pr.clear_render_target = p.clear_render_targets;

			// Locate VS / PS GLSL strings.
			const auto vs_it = rt.compiled.glsl.find(p.vs_entry_point);
			const auto ps_it = rt.compiled.glsl.find(p.ps_entry_point);
			if (vs_it == rt.compiled.glsl.end() || ps_it == rt.compiled.glsl.end())
			{
				rt.skip_reason = "Compiled effect missing entry point GLSL for pass " + p.name;
				return false;
			}

			const std::string vs_src = AdaptShaderForContext(vs_it->second,
				is_gles, gles_major, gles_minor, desktop_major, desktop_minor, /*is_fragment=*/false);
			const std::string ps_src = AdaptShaderForContext(ps_it->second,
				is_gles, gles_major, gles_minor, desktop_major, desktop_minor, /*is_fragment=*/true);

			std::string build_error;
			if (!BuildPassProgram(pr.program, vs_src, ps_src, rt.effect_filename, p.name, &build_error))
			{
				rt.skip_reason = build_error.empty()
					? std::string("Failed to compile/link pass ") + p.name
					: build_error;
				return false;
			}
			pr.program_ok = true;

			// Wire up the global UBO at the binding slot the codegen
			// emitted (`layout(std140, column_major, binding = 0)`). We
			// call `BindUniformBlock` rather than relying on layout
			// because some drivers (especially ANGLE / Mali) treat the
			// implicit GL_UNIFORM_BUFFER binding differently.
			pr.program.BindUniformBlock("_Globals", 0);

			// Resolve sampler bindings. The codegen emits `layout(binding = N)`
			// for each referenced sampler in the entry-point's GLSL output,
			// so the texture unit `N` already routes to the right uniform.
			// We just need the GL sampler object and the texture-of-interest
			// per binding slot.
			pr.sampler_bindings.reserve(p.sampler_bindings.size());
			for (const reshadefx::sampler_binding& sb : p.sampler_bindings)
			{
				if (sb.index >= mod->samplers.size())
					continue;
				const reshadefx::sampler& smp = mod->samplers[sb.index];
				PassRuntimeGL::SamplerBinding bind;
				bind.binding = sb.entry_point_binding;
				bind.texture_name = smp.texture_name;
				bind.srgb = smp.srgb;

				bool has_mipmap = false;
				for (const reshadefx::texture& t : mod->textures)
				{
					if (t.unique_name == smp.texture_name || t.name == smp.texture_name)
					{
						bind.texture_semantic = t.semantic;
						has_mipmap = (t.levels > 1);
						break;
					}
				}
				bind.has_mipmap = has_mipmap;
				bind.sampler_object = GetSampler(smp, has_mipmap);
				if (bind.sampler_object == 0)
				{
					rt.skip_reason = "Failed to create sampler object";
					return false;
				}

				pr.sampler_bindings.push_back(std::move(bind));
			}
		}
		return true;
	}

	// ----------------------------------------------------------------------
	// UBO fill: preset INI -> uniform offsets/size
	// ----------------------------------------------------------------------
	void ChainImplGL::FillUniformBuffer(EffectRuntimeGL& rt)
	{
		if (rt.ubo == 0 || rt.ubo_size == 0)
			return;
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
			return;

		const auto params_it = preset.effect_params.find(rt.effect_filename);
		const EffectParameters* params = (params_it != preset.effect_params.end())
			? &params_it->second
			: nullptr;

		std::fill(rt.ubo_staging.begin(), rt.ubo_staging.end(), uint8_t{0});
		uint8_t* ubo = rt.ubo_staging.data();

		for (const reshadefx::uniform& u : mod->uniforms)
		{
			const uint32_t off = u.offset;
			const uint32_t size = u.size;
			if (off + size > rt.ubo_size)
				continue;

			std::vector<float> floats;
			std::vector<int32_t> ints;
			std::vector<uint32_t> uints;
			bool got_value = false;
			if (params)
			{
				auto pit = params->find(u.name);
				if (pit == params->end() && !u.unique_name.empty())
					pit = params->find(u.unique_name);
				if (pit != params->end())
				{
					const std::vector<float> raw = ToFloatList(pit->second);
					if (!raw.empty())
					{
						floats = raw;
						ints.resize(raw.size());
						uints.resize(raw.size());
						for (size_t i = 0; i < raw.size(); ++i)
						{
							ints[i] = static_cast<int32_t>(raw[i]);
							uints[i] = static_cast<uint32_t>(raw[i] < 0 ? 0 : raw[i]);
						}
						got_value = true;
					}
				}
			}

			if (!got_value && u.has_initializer_value)
			{
				const uint32_t comps = std::max(1u, u.type.components());
				floats.resize(comps);
				ints.resize(comps);
				uints.resize(comps);
				for (uint32_t i = 0; i < comps && i < 16; ++i)
				{
					floats[i] = u.initializer_value.as_float[i];
					ints[i] = u.initializer_value.as_int[i];
					uints[i] = u.initializer_value.as_uint[i];
				}
				got_value = true;
			}

			if (!got_value)
			{
				std::memset(ubo + off, 0, size);
				continue;
			}

			const uint32_t cols = std::max(1u, u.type.cols);
			const uint32_t rows = std::max(1u, u.type.rows);
			const bool is_int = u.type.is_integral();
			const bool is_bool = u.type.is_boolean();
			const uint32_t row_stride = (cols > 1) ? 16u : 0u;

			const auto write_scalar = [&](uint32_t byte_offset, uint32_t idx) {
				if (byte_offset + 4u > rt.ubo_size)
					return;
				if (is_bool)
				{
					const uint32_t v = (idx < floats.size() && floats[idx] != 0.0f) ? 1u : 0u;
					std::memcpy(ubo + byte_offset, &v, sizeof(uint32_t));
				}
				else if (is_int)
				{
					const int32_t v = (idx < ints.size()) ? ints[idx] : 0;
					std::memcpy(ubo + byte_offset, &v, sizeof(int32_t));
				}
				else
				{
					const float v = (idx < floats.size()) ? floats[idx] : 0.0f;
					std::memcpy(ubo + byte_offset, &v, sizeof(float));
				}
			};

			if (cols > 1)
			{
				for (uint32_t c = 0; c < cols; ++c)
					for (uint32_t r = 0; r < rows; ++r)
					{
						const uint32_t comp_index = r * cols + c;
						write_scalar(off + c * row_stride + r * 4u, comp_index);
					}
			}
			else
			{
				for (uint32_t r = 0; r < rows; ++r)
					write_scalar(off + r * 4u, r);
			}
		}

		glBindBuffer(GL_UNIFORM_BUFFER, rt.ubo);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, rt.ubo_size, rt.ubo_staging.data());
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	GSTextureOGL* ChainImplGL::FindEffectTexture(const std::string& key, const EffectRuntimeGL& rt) const
	{
		auto it = rt.textures.find(key);
		if (it != rt.textures.end())
			return it->second.get();
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
			return nullptr;
		for (const reshadefx::texture& t : mod->textures)
		{
			if (t.name == key)
			{
				auto by_unique = rt.textures.find(t.unique_name);
				if (by_unique != rt.textures.end())
					return by_unique->second.get();
			}
		}
		return nullptr;
	}

	GLuint ChainImplGL::ResolveSampledTexture(const std::string& tex_name,
		const std::string& tex_semantic,
		GSTextureOGL* current_back_buffer,
		const EffectRuntimeGL& rt) const
	{
		if (IEquals(tex_semantic, "COLOR") && current_back_buffer)
			return current_back_buffer->GetID();
		if (IEquals(tex_semantic, "DEPTH") && dummy_depth)
			return dummy_depth->GetID();
		GSTextureOGL* tex = FindEffectTexture(tex_name, rt);
		return tex ? tex->GetID() : 0;
	}

	// ----------------------------------------------------------------------
	// Per-frame execution
	// ----------------------------------------------------------------------
	bool ChainImplGL::ApplyEffect(EffectRuntimeGL& rt, GSTextureOGL*& cur_color)
	{
		if (!rt.runnable || !cur_color || !device)
			return false;

		FillUniformBuffer(rt);

		GSTextureOGL* sampled_back_buffer = cur_color;

		for (size_t pass_idx = 0; pass_idx < rt.passes.size(); ++pass_idx)
		{
			PassRuntimeGL& pr = rt.passes[pass_idx];

			GSTextureOGL* dst = nullptr;
			if (pr.writes_backbuffer)
			{
				dst = (sampled_back_buffer == ping_pong[0].get())
					? ping_pong[1].get()
					: ping_pong[0].get();
			}
			else
			{
				dst = FindEffectTexture(pr.rt_name, rt);
			}
			if (!dst)
				return false;

			// Bind FBO + colour attachment.
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->GetID(), 0);
			const GLenum draw_buffers[1] = {GL_COLOR_ATTACHMENT0};
			glDrawBuffers(1, draw_buffers);

			if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				return false;

			const uint32_t vp_w = pr.viewport_width ? pr.viewport_width : static_cast<uint32_t>(dst->GetWidth());
			const uint32_t vp_h = pr.viewport_height ? pr.viewport_height : static_cast<uint32_t>(dst->GetHeight());
			glViewport(0, 0, static_cast<GLsizei>(vp_w), static_cast<GLsizei>(vp_h));

			// Disable scissor + colour clipping the renderer may have left
			// dangling, then optionally clear.
			glDisable(GL_SCISSOR_TEST);
			glDisable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_STENCIL_TEST);
			glDepthMask(GL_FALSE);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			if (pr.clear_render_target)
			{
				glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}

			pr.program.Bind();
			glBindVertexArray(vao);

			// Bind samplers. Each `binding` slot already has a matching
			// `layout(binding = N) uniform sampler2D ...;` in the GLSL
			// program, so the only thing left is gluing a GSTexture +
			// sampler object onto texture unit N.
			for (const PassRuntimeGL::SamplerBinding& sb : pr.sampler_bindings)
			{
				const GLuint tex_id = ResolveSampledTexture(sb.texture_name, sb.texture_semantic,
					sampled_back_buffer, rt);
				glActiveTexture(GL_TEXTURE0 + sb.binding);
				glBindTexture(GL_TEXTURE_2D, tex_id);
				glBindSampler(sb.binding, sb.sampler_object);
			}

			// Bind the global UBO.
			glBindBufferRange(GL_UNIFORM_BUFFER, 0, rt.ubo, 0, rt.ubo_size);

			glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(pr.num_vertices));

			if (pr.generate_mipmaps && !pr.writes_backbuffer)
				dst->GenerateMipmap();

			// Bookkeeping for the chain's own state machine.
			if (pr.writes_backbuffer)
				sampled_back_buffer = dst;
		}

		// Unbind sampler objects we set so PresentRect later doesn't see
		// stale samplers attached to its texture units. We only touched
		// the units mentioned in `pr.sampler_bindings`, but the highest
		// binding observed is conservatively bounded by 16 (ReShade FX
		// itself never uses more than 16 samplers per entry point), and
		// detaching extras is harmless.
		for (size_t i = 0; i < 16; ++i)
			glBindSampler(static_cast<GLuint>(i), 0);

		cur_color = sampled_back_buffer;
		return true;
	}

	// ----------------------------------------------------------------------
	// State restore: snap GLState back to a known-clean baseline so the
	// PresentRect blit (and the rest of GSDeviceOGL) does not have to
	// guess which knobs the chain just turned. We *do not* attempt to
	// preserve every dirty bit individually - the existing OGL renderer
	// re-applies its own state on each draw, the only thing we owe it is
	// a consistent FBO + VAO + viewport to start from.
	// ----------------------------------------------------------------------
	void ChainImplGL::RestoreDeviceState()
	{
		if (!device)
			return;

		// Default framebuffer for the swap chain blit. We hit
		// glBindFramebuffer directly and patch the cached GLState value
		// in place — GSDeviceOGL::OMSetFBO is private to the device, so
		// we cannot call it from here.
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		GLState::fbo = 0;

		// VAO: GSDeviceOGL caches the VAO in `GLState::vao`; force it to
		// re-bind on the next draw by writing an obviously-invalid value.
		GLState::vao = static_cast<GLuint>(-1);

		// Texture units: chain code only touched units 0..N (one per
		// sampler binding), so just zero the cached IDs here. Subsequent
		// PSSetShaderResource calls will re-bind whatever the renderer
		// needs.
		for (size_t i = 0; i < std::size(GLState::tex_unit); ++i)
			GLState::tex_unit[i] = 0;

		// We disabled scissor testing for the chain passes; re-enable it
		// so the renderer's scissor-aware draws keep working. The exact
		// rect is restored on the next SetScissor() call.
		glEnable(GL_SCISSOR_TEST);

		// Active texture unit: leave at GL_TEXTURE0 because that matches
		// the GSDeviceOGL convention for sampler slot 0.
		glActiveTexture(GL_TEXTURE0);
	}

	// ----------------------------------------------------------------------
	// Diagnostic shader compile + link path. Mirrors what GLProgram does
	// internally but emits errors with the `[ReShade]` prefix and dumps
	// the failing GLSL beside the bad-shader log so the chain failure
	// mode is fully observable from logcat.
	// ----------------------------------------------------------------------
	bool ChainImplGL::BuildPassProgram(ChainProgramGL& out_program,
		const std::string& vs_src, const std::string& fs_src,
		const std::string& effect_name, const std::string& pass_name,
		std::string* out_error)
	{
		out_program.Destroy();

		const auto compile_stage = [&](GLenum stage, const char* stage_label,
								   const std::string& source) -> GLuint {
			GLuint sh = glCreateShader(stage);
			if (sh == 0)
			{
				Console.Warning("[%s] %s/%s %s glCreateShader returned 0 (out of memory or invalid context)",
					kLogTag, effect_name.c_str(), pass_name.c_str(), stage_label);
				if (out_error)
					*out_error = std::string("glCreateShader(") + stage_label + ") returned 0";
				return 0;
			}
			const GLchar* src_ptr = source.data();
			const GLint src_len = static_cast<GLint>(source.size());
			glShaderSource(sh, 1, &src_ptr, &src_len);
			glCompileShader(sh);

			GLint status = GL_FALSE;
			glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
			GLint info_len = 0;
			glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &info_len);

			if (status == GL_FALSE)
			{
				std::string info_log(static_cast<size_t>(std::max(info_len, 1)), '\0');
				if (info_len > 0)
					glGetShaderInfoLog(sh, info_len, nullptr, info_log.data());
				// Trim trailing NULs/whitespace so the log line is tidy.
				while (!info_log.empty() && (info_log.back() == '\0' || info_log.back() == '\n' ||
											 info_log.back() == '\r' || info_log.back() == ' '))
					info_log.pop_back();

				const std::string dump_path = DumpBadShaderToDisk(
					effect_name, pass_name, stage_label, source, info_log);
				Console.Warning("[%s] %s/%s %s shader compile failed: %s",
					kLogTag, effect_name.c_str(), pass_name.c_str(), stage_label,
					ClipForLog(info_log).c_str());
				if (!dump_path.empty())
					Console.Warning("[%s]   full source dumped to: %s", kLogTag, dump_path.c_str());
				if (out_error)
				{
					*out_error = std::string("Compile failed (") + stage_label + "): " +
						ClipForLog(info_log, 200);
				}
				glDeleteShader(sh);
				return 0;
			}
			return sh;
		};

		GLuint vs = compile_stage(GL_VERTEX_SHADER, "vs", vs_src);
		if (vs == 0)
			return false;
		GLuint fs = compile_stage(GL_FRAGMENT_SHADER, "fs", fs_src);
		if (fs == 0)
		{
			glDeleteShader(vs);
			return false;
		}

		GLuint program = glCreateProgram();
		if (program == 0)
		{
			glDeleteShader(vs);
			glDeleteShader(fs);
			if (out_error)
				*out_error = "glCreateProgram returned 0";
			return false;
		}

		glAttachShader(program, vs);
		glAttachShader(program, fs);
		glLinkProgram(program);

		// Shaders can be flagged for deletion immediately after linking;
		// they will be released once the program detaches them.
		glDeleteShader(vs);
		glDeleteShader(fs);

		GLint link_status = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &link_status);
		GLint info_len = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);

		if (link_status == GL_FALSE)
		{
			std::string info_log(static_cast<size_t>(std::max(info_len, 1)), '\0');
			if (info_len > 0)
				glGetProgramInfoLog(program, info_len, nullptr, info_log.data());
			while (!info_log.empty() && (info_log.back() == '\0' || info_log.back() == '\n' ||
										 info_log.back() == '\r' || info_log.back() == ' '))
				info_log.pop_back();

			Console.Warning("[%s] %s/%s program link failed: %s",
				kLogTag, effect_name.c_str(), pass_name.c_str(),
				ClipForLog(info_log).c_str());
			// Save both stages so the user can correlate varyings.
			const std::string vs_dump = DumpBadShaderToDisk(
				effect_name, pass_name, "vs_link_failed", vs_src, info_log);
			const std::string fs_dump = DumpBadShaderToDisk(
				effect_name, pass_name, "fs_link_failed", fs_src, info_log);
			if (!vs_dump.empty())
				Console.Warning("[%s]   vertex shader dumped to: %s", kLogTag, vs_dump.c_str());
			if (!fs_dump.empty())
				Console.Warning("[%s]   fragment shader dumped to: %s", kLogTag, fs_dump.c_str());
			if (out_error)
				*out_error = std::string("Link failed: ") + ClipForLog(info_log, 200);
			glDeleteProgram(program);
			return false;
		}

		out_program.id = program;
		return true;
	}

	// ----------------------------------------------------------------------
	// ChainGL: thin facade over ChainImplGL
	// ----------------------------------------------------------------------
	ChainGL::ChainGL() : m_impl(std::make_unique<ChainImplGL>()) {}
	ChainGL::~ChainGL() = default;

	bool ChainGL::LoadPreset(GSDeviceOGL* device, const ChainConfigGL& config)
	{
		if (!m_impl)
			m_impl = std::make_unique<ChainImplGL>();

		m_impl->Reset();
		m_impl->permanently_disabled = false;
		m_impl->disabled_unsupported_context = false;
		m_impl->device = device;
		m_impl->debug_glsl = config.debug_glsl;

		if (!m_impl->DetectContextLevels())
		{
			Console.WriteLn("[%s] OpenGL context too old (need GLES 3.1+ or desktop GL 3.3+);"
				" chain disabled, falling back to passthrough",
				kLogTag);
			m_impl->disabled_unsupported_context = true;
			return false;
		}

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

		if (!m_impl->EnsureScratchObjects())
		{
			Console.Warning("[%s] Failed to allocate scratch FBO / VAO; chain disabled", kLogTag);
			return false;
		}

		if (!m_impl->EnsureDummyDepth())
			Console.Warning("[%s] Failed to allocate dummy depth texture; depth-reading effects will sample garbage", kLogTag);

		m_impl->CompileAndBuild();

		size_t compiled = 0, runnable = 0;
		for (const EffectRuntimeGL& rt : m_impl->effects)
		{
			if (rt.compiled_ok) ++compiled;
			if (rt.runnable)    ++runnable;
		}
		Console.WriteLn("[%s] Preset loaded: %zu effects parsed, %zu compiled, %zu runnable (rest fall back to passthrough)",
			kLogTag, m_impl->effects.size(), compiled, runnable);

		return runnable > 0;
	}

	void ChainGL::Reset()
	{
		if (m_impl)
			m_impl->Reset();
	}

	void ChainGL::DisablePermanently()
	{
		if (!m_impl)
			return;
		m_impl->permanently_disabled = true;
		m_impl->Reset();
	}

	bool ChainGL::IsActive() const
	{
		if (!m_impl || m_impl->permanently_disabled || m_impl->disabled_unsupported_context)
			return false;
		for (const EffectRuntimeGL& rt : m_impl->effects)
			if (rt.runnable)
				return true;
		return false;
	}

	void ChainGL::Apply(GSTextureOGL*& current_io, uint32_t width, uint32_t height)
	{
		if (!m_impl || !current_io)
			return;
		if (m_impl->permanently_disabled || m_impl->disabled_unsupported_context)
			return;
		if (!IsActive())
			return;

		if (!m_impl->EnsurePingPong(width, height))
		{
			Console.Warning("[%s] Failed to allocate ping-pong RTs; chain disabled", kLogTag);
			DisablePermanently();
			return;
		}

		GSTextureOGL* cur = current_io;
		for (EffectRuntimeGL& rt : m_impl->effects)
		{
			if (!rt.runnable)
				continue;
			if (!m_impl->ApplyEffect(rt, cur))
			{
				Console.Warning("[%s] Effect '%s' failed at runtime; disabling for this session",
					kLogTag, rt.effect_filename.c_str());
				rt.runnable = false;
				rt.skip_reason = "Runtime apply failed";
				cur = current_io; // skip the rest, fall back to original frame
			}
		}

		current_io = cur;
		m_impl->RestoreDeviceState();
	}

	void ChainGL::NotifyBackbufferResized(uint32_t width, uint32_t height)
	{
		if (!m_impl)
			return;
		m_impl->bb_width = width;
		m_impl->bb_height = height;
		m_impl->ping_pong[0].reset();
		m_impl->ping_pong[1].reset();
	}
} // namespace ReShade
