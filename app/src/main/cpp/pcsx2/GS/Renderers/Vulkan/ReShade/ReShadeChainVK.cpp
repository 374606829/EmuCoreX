// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Vulkan-side ReShade chain runtime.
//
// This compilation unit implements the post-processing executor described in
// "最小MVP实施文档.md" §3 (P0). On top of the existing FX -> SPIR-V compile
// path, it now:
//
//   1. Builds per-effect Vulkan resources (UBO, sampler cache, descriptor
//      set layouts, pipeline layout, internal textures) at LoadPreset() time.
//
//   2. Builds one VkPipeline per pass, plus matching descriptor set layouts
//      for the per-pass combined-image-sampler bindings.
//
//   3. In Apply(), runs the technique pass list:
//        - For backbuffer-target passes, ping-pongs between two
//          chain-owned RTs that match the swap chain extent/format.
//        - For internal-RT passes, writes into the per-effect named
//          texture and (optionally) generates mipmaps.
//        - Fills the global UBO from the parsed preset INI parameters,
//          falling back to the FX-declared initializer when the INI does
//          not specify a value.
//
//   4. Rewrites the caller's `current_io` with the chain output so that
//      GSDeviceVK::BeginPresent picks up the post-processed image without
//      any further plumbing.
//
// Failure handling stays "passthrough on error": any pass failing to
// compile/build flips the parent effect's `runnable` to false, and the
// remaining effects continue. The whole chain is also revertible via
// Reset()/DisablePermanently(). The MVP document explicitly allows this
// fallback ("不得崩溃，可随时降级到原图").
//
// Threading: All public methods must be called on the GS thread (the same
// thread that drives GSDeviceVK).

#include "ReShadeChainVK.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Common/GSTexture.h"

#include "Config.h"

#include "common/Console.h"
#include "common/Path.h"

#include "effect_module.hpp"

#include "vk_mem_alloc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace
{
	constexpr const char* kLogTag = "ReShade";
	constexpr uint32_t kBackBufferSet = 0; // Set 0 = global UBO
	constexpr uint32_t kSamplerSet = 1;    // Set 1 = combined image samplers

	// Filename helper for SPIR-V dumps. Mirrors the GL-side equivalent so
	// users only have one diagnostic convention to remember.
	std::string SanitizePathComponent(const std::string& in)
	{
		std::string out;
		out.reserve(in.size());
		for (unsigned char c : in)
		{
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20)
				out.push_back('_');
			else
				out.push_back(static_cast<char>(c));
		}
		if (out.empty())
			out = "_";
		return out;
	}

	// Dumps a SPIR-V binary to disk so the user (or us) can later run it
	// through spirv-val / spirv-cross to figure out why the driver
	// rejected the corresponding pipeline. The dump path is logged with
	// the `[ReShade]` prefix so it surfaces on filtered logcat.
	std::string DumpSpirvToDisk(const std::string& effect_name,
		const std::string& pass_name,
		const char* stage_label,
		const std::vector<uint32_t>& spirv)
	{
		if (EmuFolders::Logs.empty() || spirv.empty())
			return std::string();
		const std::string filename = "reshade_bad_pipeline_" +
			SanitizePathComponent(effect_name) + "_" +
			SanitizePathComponent(pass_name) + "_" + stage_label + ".spv";
		const std::string path = Path::Combine(EmuFolders::Logs, filename);
		std::ofstream ofs(path, std::ios::out | std::ios::binary);
		if (!ofs.is_open())
			return std::string();
		ofs.write(reinterpret_cast<const char*>(spirv.data()),
			static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
		return path;
	}

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

	// Map ReShade FX texture_format to a GSTexture::Format. We pick the
	// closest superset that GSTextureVK can natively allocate; effects asking
	// for a more exotic format silently widen to RGBA8/RGBA16F so the chain
	// keeps producing pictures even for shaders we did not design around.
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
			// Single/dual channel float formats fall back to RGBA16F (HDR).
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

	VkFilter MapMagMinFilter(reshadefx::filter_mode m)
	{
		using F = reshadefx::filter_mode;
		// "min/mag" share the same filter selection in ReShade's enum encoding.
		switch (m)
		{
			case F::min_mag_mip_point:
			case F::min_mag_point_mip_linear:
				return VK_FILTER_NEAREST;
			default:
				return VK_FILTER_LINEAR;
		}
	}

	VkSamplerMipmapMode MapMipMode(reshadefx::filter_mode m)
	{
		using F = reshadefx::filter_mode;
		switch (m)
		{
			case F::min_mag_mip_point:
			case F::min_point_mag_linear_mip_point:
			case F::min_linear_mag_mip_point:
			case F::min_mag_linear_mip_point:
				return VK_SAMPLER_MIPMAP_MODE_NEAREST;
			default:
				return VK_SAMPLER_MIPMAP_MODE_LINEAR;
		}
	}

	VkSamplerAddressMode MapAddressMode(reshadefx::texture_address_mode m)
	{
		using A = reshadefx::texture_address_mode;
		switch (m)
		{
			case A::wrap:   return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case A::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case A::clamp:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case A::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			default:        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		}
	}

	uint64_t SamplerKey(const reshadefx::sampler_desc& d)
	{
		// 8 fields packed into 64 bits, lossy on lod_bias/min_lod/max_lod which
		// we round to coarse buckets (post-processing does not need precise lod).
		const auto biasBucket = [](float v) -> uint64_t {
			if (!std::isfinite(v))
				return 0;
			return static_cast<uint64_t>(static_cast<int32_t>(v * 16.0f) & 0xFFFF);
		};
		uint64_t k = 0;
		k |= static_cast<uint64_t>(d.filter) & 0xFFu;
		k |= (static_cast<uint64_t>(d.address_u) & 0xFFu) << 8;
		k |= (static_cast<uint64_t>(d.address_v) & 0xFFu) << 16;
		k |= (static_cast<uint64_t>(d.address_w) & 0xFFu) << 24;
		k |= (biasBucket(d.lod_bias) & 0xFFFFull) << 32;
		k |= (biasBucket(d.min_lod) & 0xFFFFull) << 48;
		return k;
	}
} // namespace

namespace ReShade
{
	// ----------------------------------------------------------------------
	// Per-pass GPU resources.
	// ----------------------------------------------------------------------
	struct PassRuntime
	{
		VkShaderModule vs = VK_NULL_HANDLE;
		VkShaderModule ps = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkRenderPass render_pass = VK_NULL_HANDLE;

		// One layout per pass because the sampler binding count varies.
		VkDescriptorSetLayout dsl_ubo = VK_NULL_HANDLE;
		VkDescriptorSetLayout dsl_tex = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

		bool writes_backbuffer = true;
		std::string rt_name; // empty when writes_backbuffer == true
		uint32_t viewport_width = 0;
		uint32_t viewport_height = 0;
		uint32_t num_vertices = 3;
		bool generate_mipmaps = false;
		VkFormat color_format = VK_FORMAT_UNDEFINED;
		bool clear_render_target = false;

		struct SamplerBinding
		{
			uint32_t binding = 0;
			std::string texture_name; // resolved at apply time
			std::string texture_semantic;
			VkSampler sampler = VK_NULL_HANDLE;
			bool srgb = false;
		};
		std::vector<SamplerBinding> sampler_bindings;
	};

	// ----------------------------------------------------------------------
	// Per-effect runtime state.
	// ----------------------------------------------------------------------
	struct EffectRuntime
	{
		std::string effect_filename;
		std::string technique_name;

		CompiledEffect compiled;
		bool compiled_ok = false;

		bool runnable = false;
		std::string skip_reason;

		// UBO (size = effect_module.total_uniform_size, persistent-mapped).
		VkBuffer ubo = VK_NULL_HANDLE;
		VmaAllocation ubo_alloc = VK_NULL_HANDLE;
		void* ubo_mapped = nullptr;
		uint32_t ubo_size = 0;

		// Internal named textures, keyed by texture.unique_name.
		std::unordered_map<std::string, std::unique_ptr<GSTextureVK>> textures;

		std::vector<PassRuntime> passes;
	};

	// ----------------------------------------------------------------------
	// ChainImpl
	// ----------------------------------------------------------------------
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
		bool dimensions_dirty = true;

		// Two backbuffer-sized textures used in ping-pong fashion for passes
		// that target the implicit "BackBuffer" render target.
		std::unique_ptr<GSTextureVK> ping_pong[2];
		VkFormat ping_pong_format = VK_FORMAT_UNDEFINED;

		// 1x1 dummy depth-as-color texture for effects that read the
		// (currently unsupported) DepthBuffer semantic.
		std::unique_ptr<GSTextureVK> dummy_depth;

		// Render-pass cache: one per (format, clear) combination.
		std::unordered_map<uint64_t, VkRenderPass> render_pass_cache;

		// Sampler cache shared across effects.
		std::unordered_map<uint64_t, VkSampler> sampler_cache;

		// Dedicated descriptor pool for the chain so the global GS pool
		// (which only stocks UNIFORM_BUFFER_DYNAMIC) does not need to be
		// extended with regular UNIFORM_BUFFER capacity.
		VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

		// Pending descriptor sets to free when their fence has completed.
		struct PendingDS
		{
			uint64_t fence_counter = 0;
			std::vector<VkDescriptorSet> sets;
		};
		std::deque<PendingDS> pending_descriptor_frees;

		std::vector<EffectRuntime> effects;

		// ------------------------------------------------------------------
		// Lifetime
		// ------------------------------------------------------------------
		void Reset();

		// Returns a cached render pass (single color attachment, LOAD/STORE)
		// for the given format. Created lazily.
		VkRenderPass GetRenderPass(VkFormat fmt, bool clear);

		// Creates (or fetches) a Vulkan sampler matching the given descriptor.
		VkSampler GetSampler(const reshadefx::sampler_desc& d);

		// Creates a VkShaderModule from the SPIR-V bytecode of the named
		// entry point, or VK_NULL_HANDLE if the entry point is not present.
		VkShaderModule CreateShaderModule(const CompiledEffect& ce, const std::string& entry_point);

		// Compiles every effect in the preset's `Techniques=` list and
		// builds the GPU resources for those that pass the MVP scope checks.
		void CompileAndBuild();

		// Builds per-effect Vulkan resources. Returns true on full success.
		bool BuildEffect(EffectRuntime& rt);
		bool BuildEffectTextures(EffectRuntime& rt);
		bool BuildEffectUbo(EffectRuntime& rt);
		bool BuildEffectPasses(EffectRuntime& rt);

		// Releases Vulkan resources owned by an effect. Safe to call multiple
		// times; the EffectRuntime is left in a "compiled-but-not-runnable"
		// state.
		void DestroyEffect(EffectRuntime& rt);

		// Releases the chain-wide caches (samplers, render passes,
		// ping-pong RTs, dummy depth).
		void DestroyShared();

		// Lazily creates a descriptor pool dedicated to the chain.
		bool EnsureDescriptorPool();

		// Releases descriptor sets whose fence has completed.
		void RecycleDescriptorSets();
		// Allocates a single descriptor set and queues it for deferred free.
		VkDescriptorSet AllocateTransientDescriptorSet(VkDescriptorSetLayout layout);

		// Make sure the ping-pong RTs match the requested backbuffer size.
		bool EnsurePingPong(uint32_t width, uint32_t height);
		bool EnsureDummyDepth();

		// Per-frame: write the per-effect UBO from preset values and FX defaults.
		void FillUniformBuffer(const EffectRuntime& rt);

		// Resolve a named texture to a (view, layout) pair for sampling.
		// Falls back to the chain's "current backbuffer" or the dummy depth
		// texture when the texture has a special semantic.
		void ResolveSampledImage(const std::string& tex_name,
			const std::string& tex_semantic,
			GSTextureVK* current_back_buffer,
			VkImageView& out_view, VkImageLayout& out_layout, GSTextureVK** out_tex,
			const EffectRuntime& rt) const;

		// Looks up a texture in the effect's internal map by its
		// `unique_name` first, then by its public `name`.
		GSTextureVK* FindEffectTexture(const std::string& key, const EffectRuntime& rt) const;

		// Generate mipmaps for an internal texture using vkCmdBlitImage.
		void GenerateMipmaps(VkCommandBuffer cmdbuf, GSTextureVK* tex);

		// Run a single compiled effect for one frame.
		bool ApplyEffect(VkCommandBuffer cmdbuf, EffectRuntime& rt,
			GSTextureVK*& cur_color);
	};

	// ----------------------------------------------------------------------
	// Reset / destruction
	// ----------------------------------------------------------------------
	void ChainImpl::DestroyEffect(EffectRuntime& rt)
	{
		if (!device)
			return;
		const VkDevice dev = device->GetDevice();
		const VmaAllocator alloc = device->GetAllocator();

		for (PassRuntime& p : rt.passes)
		{
			if (p.pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(dev, p.pipeline, nullptr);
			if (p.pipeline_layout != VK_NULL_HANDLE)
				vkDestroyPipelineLayout(dev, p.pipeline_layout, nullptr);
			if (p.dsl_ubo != VK_NULL_HANDLE)
				vkDestroyDescriptorSetLayout(dev, p.dsl_ubo, nullptr);
			if (p.dsl_tex != VK_NULL_HANDLE)
				vkDestroyDescriptorSetLayout(dev, p.dsl_tex, nullptr);
			if (p.vs != VK_NULL_HANDLE)
				vkDestroyShaderModule(dev, p.vs, nullptr);
			if (p.ps != VK_NULL_HANDLE)
				vkDestroyShaderModule(dev, p.ps, nullptr);
		}
		rt.passes.clear();

		if (rt.ubo != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(alloc, rt.ubo, rt.ubo_alloc);
			rt.ubo = VK_NULL_HANDLE;
			rt.ubo_alloc = VK_NULL_HANDLE;
			rt.ubo_mapped = nullptr;
		}

		// GSTextureVK destructor handles defer/destroy of the underlying VkImage.
		rt.textures.clear();

		rt.runnable = false;
	}

	void ChainImpl::DestroyShared()
	{
		if (!device)
			return;
		const VkDevice dev = device->GetDevice();

		// Reset the descriptor pool wholesale: this implicitly invalidates
		// every set we ever handed out, so the pending-free queue can
		// just be cleared without explicit vkFreeDescriptorSets calls.
		pending_descriptor_frees.clear();
		if (descriptor_pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(dev, descriptor_pool, nullptr);
			descriptor_pool = VK_NULL_HANDLE;
		}

		for (auto& kv : sampler_cache)
			vkDestroySampler(dev, kv.second, nullptr);
		sampler_cache.clear();

		for (auto& kv : render_pass_cache)
			vkDestroyRenderPass(dev, kv.second, nullptr);
		render_pass_cache.clear();

		ping_pong[0].reset();
		ping_pong[1].reset();
		ping_pong_format = VK_FORMAT_UNDEFINED;
		dummy_depth.reset();
		dimensions_dirty = true;
	}

	bool ChainImpl::EnsureDescriptorPool()
	{
		if (descriptor_pool != VK_NULL_HANDLE)
			return true;
		// Sized for ~64 effect passes per frame, three frames in flight.
		// Each pass takes 1 UBO descriptor + up to 16 combined image samplers.
		constexpr uint32_t kMaxSets = 64u * 3u * 2u; // ubo + tex per pass
		const VkDescriptorPoolSize pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64u * 3u},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64u * 3u * 16u},
		};
		VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		ci.maxSets = kMaxSets;
		ci.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
		ci.pPoolSizes = pool_sizes;

		const VkResult r = vkCreateDescriptorPool(device->GetDevice(), &ci, nullptr, &descriptor_pool);
		if (r != VK_SUCCESS)
		{
			descriptor_pool = VK_NULL_HANDLE;
			return false;
		}
		return true;
	}

	void ChainImpl::Reset()
	{
		for (EffectRuntime& rt : effects)
			DestroyEffect(rt);
		effects.clear();
		DestroyShared();

		preset.Clear();
		preset_dir.clear();
		include_dirs.clear();
		bb_width = bb_height = 0;
	}

	// ----------------------------------------------------------------------
	// Cached helpers
	// ----------------------------------------------------------------------
	VkRenderPass ChainImpl::GetRenderPass(VkFormat fmt, bool clear)
	{
		const uint64_t key = (static_cast<uint64_t>(fmt) << 1) | (clear ? 1u : 0u);
		auto it = render_pass_cache.find(key);
		if (it != render_pass_cache.end())
			return it->second;

		VkAttachmentDescription att{};
		att.format = fmt;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub{};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &ref;

		VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rpi.attachmentCount = 1;
		rpi.pAttachments = &att;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &sub;

		VkRenderPass rp = VK_NULL_HANDLE;
		if (vkCreateRenderPass(device->GetDevice(), &rpi, nullptr, &rp) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		render_pass_cache[key] = rp;
		return rp;
	}

	VkSampler ChainImpl::GetSampler(const reshadefx::sampler_desc& d)
	{
		const uint64_t key = SamplerKey(d);
		auto it = sampler_cache.find(key);
		if (it != sampler_cache.end())
			return it->second;

		VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		ci.magFilter = MapMagMinFilter(d.filter);
		ci.minFilter = MapMagMinFilter(d.filter);
		ci.mipmapMode = MapMipMode(d.filter);
		ci.addressModeU = MapAddressMode(d.address_u);
		ci.addressModeV = MapAddressMode(d.address_v);
		ci.addressModeW = MapAddressMode(d.address_w);
		ci.mipLodBias = std::isfinite(d.lod_bias) ? d.lod_bias : 0.0f;
		ci.anisotropyEnable = (d.filter == reshadefx::filter_mode::anisotropic) ? VK_TRUE : VK_FALSE;
		ci.maxAnisotropy = ci.anisotropyEnable ? 4.0f : 1.0f;
		ci.compareEnable = VK_FALSE;
		ci.minLod = std::max(0.0f, std::isfinite(d.min_lod) ? d.min_lod : 0.0f);
		ci.maxLod = std::isfinite(d.max_lod) ? d.max_lod : VK_LOD_CLAMP_NONE;
		ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		ci.unnormalizedCoordinates = VK_FALSE;

		VkSampler s = VK_NULL_HANDLE;
		if (vkCreateSampler(device->GetDevice(), &ci, nullptr, &s) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		sampler_cache[key] = s;
		return s;
	}

	VkShaderModule ChainImpl::CreateShaderModule(const CompiledEffect& ce, const std::string& entry_point)
	{
		const auto it = ce.spirv.find(entry_point);
		if (it == ce.spirv.end() || it->second.empty())
			return VK_NULL_HANDLE;

		VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = it->second.size() * sizeof(uint32_t);
		ci.pCode = it->second.data();

		VkShaderModule m = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device->GetDevice(), &ci, nullptr, &m) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return m;
	}

	// ----------------------------------------------------------------------
	// Per-frame transient resources
	// ----------------------------------------------------------------------
	void ChainImpl::RecycleDescriptorSets()
	{
		if (!device || descriptor_pool == VK_NULL_HANDLE)
			return;
		const uint64_t completed = device->GetCompletedFenceCounter();
		while (!pending_descriptor_frees.empty())
		{
			PendingDS& head = pending_descriptor_frees.front();
			if (head.fence_counter > completed)
				break;
			if (!head.sets.empty())
			{
				vkFreeDescriptorSets(device->GetDevice(), descriptor_pool,
					static_cast<uint32_t>(head.sets.size()), head.sets.data());
			}
			pending_descriptor_frees.pop_front();
		}
	}

	VkDescriptorSet ChainImpl::AllocateTransientDescriptorSet(VkDescriptorSetLayout layout)
	{
		if (!EnsureDescriptorPool())
			return VK_NULL_HANDLE;

		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = descriptor_pool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &layout;

		VkDescriptorSet set = VK_NULL_HANDLE;
		if (vkAllocateDescriptorSets(device->GetDevice(), &ai, &set) != VK_SUCCESS)
			return VK_NULL_HANDLE;

		const uint64_t fence_counter = device->GetCurrentFenceCounter();
		if (pending_descriptor_frees.empty() ||
			pending_descriptor_frees.back().fence_counter != fence_counter)
		{
			pending_descriptor_frees.push_back({fence_counter, {}});
		}
		pending_descriptor_frees.back().sets.push_back(set);
		return set;
	}

	bool ChainImpl::EnsurePingPong(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return false;
		const VkFormat fmt = device->LookupNativeFormat(GSTexture::Format::Color);
		const bool size_changed = (bb_width != width || bb_height != height);
		const bool need_alloc = !ping_pong[0] || !ping_pong[1] || size_changed || ping_pong_format != fmt;
		if (!need_alloc)
			return true;

		ping_pong[0].reset();
		ping_pong[1].reset();
		bb_width = width;
		bb_height = height;
		ping_pong_format = fmt;

		for (int i = 0; i < 2; ++i)
		{
			ping_pong[i] = GSTextureVK::Create(GSTexture::Type::RenderTarget, GSTexture::Format::Color,
				static_cast<int>(width), static_cast<int>(height), 1);
			if (!ping_pong[i])
			{
				ping_pong[0].reset();
				ping_pong[1].reset();
				return false;
			}
		}
		return true;
	}

	bool ChainImpl::EnsureDummyDepth()
	{
		if (dummy_depth)
			return true;
		// 1x1 R32F texture filled with 1.0; ReShade FX shaders that read the
		// DepthBuffer semantic see a far-plane sample everywhere, which is
		// neutral for most depth-only effects.
		dummy_depth = GSTextureVK::Create(GSTexture::Type::Texture, GSTexture::Format::Float32, 1, 1, 1);
		if (!dummy_depth)
			return false;

		const float far_value = 1.0f;
		const GSVector4i rect(0, 0, 1, 1);
		dummy_depth->Update(rect, &far_value, sizeof(float), 0);
		return true;
	}

	// ----------------------------------------------------------------------
	// Effect compilation + GPU resource construction
	// ----------------------------------------------------------------------
	void ChainImpl::CompileAndBuild()
	{
		effects.clear();
		effects.reserve(preset.chain.size());

		for (const TechniqueRef& ref : preset.chain)
		{
			EffectRuntime rt;
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

			FXCompileOptions opts;
			opts.source_path = source.string();
			opts.include_paths = include_dirs;
			// At LoadPreset time we don't yet know the swapchain extent
			// (Apply() has not run); fall back to 1920x1080 which matches
			// most modern device defaults. Effects sample via BUFFER_WIDTH /
			// BUFFER_RCP_WIDTH which is a small per-pixel offset error if
			// the real swapchain differs - non-fatal for the MVP.
			opts.buffer_width = bb_width ? bb_width : 1920u;
			opts.buffer_height = bb_height ? bb_height : 1080u;
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
				effects.push_back(std::move(rt));
				continue;
			}
			rt.compiled_ok = true;

			if (!BuildEffect(rt))
			{
				DestroyEffect(rt);
				rt.runnable = false;
				if (rt.skip_reason.empty())
					rt.skip_reason = "Vulkan resource construction failed";
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

	bool ChainImpl::BuildEffect(EffectRuntime& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
		{
			rt.skip_reason = "Compiled effect has no module";
			return false;
		}

		// Locate the requested technique (or fall back to the first one).
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

		// MVP scope checks: graphics passes only, no MRT, full VS+PS coverage.
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

		// Build pipelines for the chosen technique only - other techniques
		// in the same module are unused at runtime today.
		rt.passes.reserve(technique->passes.size());
		for (const reshadefx::pass& p : technique->passes)
		{
			rt.passes.emplace_back();
			PassRuntime& pr = rt.passes.back();

			pr.writes_backbuffer = p.render_target_names[0].empty();
			pr.rt_name = p.render_target_names[0];
			pr.viewport_width = p.viewport_width;
			pr.viewport_height = p.viewport_height;
			pr.num_vertices = std::max(1u, p.num_vertices);
			pr.generate_mipmaps = p.generate_mipmaps && !pr.writes_backbuffer;
			pr.clear_render_target = p.clear_render_targets;

			pr.color_format = pr.writes_backbuffer
				? device->LookupNativeFormat(GSTexture::Format::Color)
				: VK_FORMAT_UNDEFINED;

			if (!pr.writes_backbuffer)
			{
				GSTextureVK* dst = FindEffectTexture(pr.rt_name, rt);
				if (!dst)
				{
					rt.skip_reason = "Pass references unknown render target: " + pr.rt_name;
					return false;
				}
				pr.color_format = dst->GetVkFormat();
			}

			pr.vs = CreateShaderModule(rt.compiled, p.vs_entry_point);
			pr.ps = CreateShaderModule(rt.compiled, p.ps_entry_point);
			if (pr.vs == VK_NULL_HANDLE || pr.ps == VK_NULL_HANDLE)
			{
				rt.skip_reason = "Failed to create shader modules for pass " + p.name;
				return false;
			}

			pr.render_pass = GetRenderPass(pr.color_format, pr.clear_render_target);
			if (pr.render_pass == VK_NULL_HANDLE)
			{
				rt.skip_reason = "Failed to create render pass for pass " + p.name;
				return false;
			}

			// Resolve sampler bindings for this pass.
			pr.sampler_bindings.reserve(p.sampler_bindings.size());
			uint32_t max_binding = 0;
			for (const reshadefx::sampler_binding& sb : p.sampler_bindings)
			{
				if (sb.index >= mod->samplers.size())
					continue;
				const reshadefx::sampler& smp = mod->samplers[sb.index];
				PassRuntime::SamplerBinding bind;
				bind.binding = sb.entry_point_binding;
				bind.texture_name = smp.texture_name;
				bind.srgb = smp.srgb;

				// Look up texture semantic by walking module textures.
				for (const reshadefx::texture& t : mod->textures)
				{
					if (t.unique_name == smp.texture_name || t.name == smp.texture_name)
					{
						bind.texture_semantic = t.semantic;
						break;
					}
				}
				bind.sampler = GetSampler(smp);
				if (bind.sampler == VK_NULL_HANDLE)
				{
					rt.skip_reason = "Failed to create Vulkan sampler";
					return false;
				}
				max_binding = std::max(max_binding, bind.binding + 1);
				pr.sampler_bindings.push_back(std::move(bind));
			}

			// Descriptor set layouts.
			Vulkan::DescriptorSetLayoutBuilder dsl_b;
			dsl_b.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			pr.dsl_ubo = dsl_b.Create(device->GetDevice());
			if (pr.dsl_ubo == VK_NULL_HANDLE)
			{
				rt.skip_reason = "Failed to create UBO descriptor set layout";
				return false;
			}

			Vulkan::DescriptorSetLayoutBuilder tex_b;
			for (uint32_t i = 0; i < max_binding; ++i)
				tex_b.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
					VK_SHADER_STAGE_FRAGMENT_BIT);
			pr.dsl_tex = (max_binding == 0)
				? tex_b.Create(device->GetDevice())  // empty set is OK
				: tex_b.Create(device->GetDevice());
			if (pr.dsl_tex == VK_NULL_HANDLE)
			{
				// An empty layout is legal but the builder may still fail
				// to allocate; skip the texture set entirely in that case.
				rt.skip_reason = "Failed to create sampler descriptor set layout";
				return false;
			}

			Vulkan::PipelineLayoutBuilder pl_b;
			pl_b.AddDescriptorSet(pr.dsl_ubo);
			pl_b.AddDescriptorSet(pr.dsl_tex);
			pr.pipeline_layout = pl_b.Create(device->GetDevice());
			if (pr.pipeline_layout == VK_NULL_HANDLE)
			{
				rt.skip_reason = "Failed to create pipeline layout";
				return false;
			}

			Vulkan::GraphicsPipelineBuilder gpb;
			gpb.SetPipelineLayout(pr.pipeline_layout);
			gpb.SetRenderPass(pr.render_pass, 0);
			gpb.SetVertexShader(pr.vs);
			gpb.SetFragmentShader(pr.ps);
			gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
			gpb.SetMultisamples(VK_SAMPLE_COUNT_1_BIT);
			gpb.SetNoDepthTestState();
			gpb.SetNoStencilState();
			gpb.SetBlendAttachment(0, false,
				VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
				VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
			gpb.SetDynamicViewportAndScissorState();

			pr.pipeline = gpb.Create(device->GetDevice(), VK_NULL_HANDLE, true);
			if (pr.pipeline == VK_NULL_HANDLE)
			{
				// `vkCreateGraphicsPipelines()` already logged the precise
				// VkResult through `LOG_VULKAN_ERROR` (visible in logcat
				// without the `[ReShade]` filter). We additionally dump
				// the SPIR-V for both stages and surface a `[ReShade]`
				// breadcrumb so the user can correlate the two.
				const std::vector<uint32_t> empty_vec;
				const auto find_spirv = [&](const std::string& ep) -> const std::vector<uint32_t>& {
					const auto it = rt.compiled.spirv.find(ep);
					return (it != rt.compiled.spirv.end()) ? it->second : empty_vec;
				};
				const std::string vs_dump = DumpSpirvToDisk(rt.effect_filename, p.name, "vs",
					find_spirv(p.vs_entry_point));
				const std::string fs_dump = DumpSpirvToDisk(rt.effect_filename, p.name, "fs",
					find_spirv(p.ps_entry_point));

				Console.Warning("[%s] %s/%s vkCreateGraphicsPipelines failed; see preceding"
					" 'vkCreateGraphicsPipelines() failed:' line for the VkResult code",
					kLogTag, rt.effect_filename.c_str(), p.name.c_str());
				if (!vs_dump.empty())
					Console.Warning("[%s]   vertex SPIR-V dumped to: %s", kLogTag, vs_dump.c_str());
				if (!fs_dump.empty())
					Console.Warning("[%s]   fragment SPIR-V dumped to: %s", kLogTag, fs_dump.c_str());

				rt.skip_reason = "Failed to create pipeline for pass " + p.name;
				return false;
			}
		}

		return true;
	}

	bool ChainImpl::BuildEffectTextures(EffectRuntime& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
			return false;

		for (const reshadefx::texture& t : mod->textures)
		{
			// Special semantics (BackBuffer/DepthBuffer) are resolved at
			// apply time; we don't allocate storage for them.
			if (!t.semantic.empty())
				continue;

			const GSTexture::Format gfx_fmt = MapTextureFormat(t.format);
			const int width = static_cast<int>(std::max<uint32_t>(t.width, 1));
			const int height = static_cast<int>(std::max<uint32_t>(t.height, 1));
			const int levels = static_cast<int>(std::max<uint16_t>(t.levels, 1));

			std::unique_ptr<GSTextureVK> tex = GSTextureVK::Create(
				GSTexture::Type::RenderTarget, gfx_fmt, width, height, levels);
			if (!tex)
			{
				rt.skip_reason = "Failed to allocate internal texture: " + t.unique_name;
				return false;
			}
			// Store under both unique_name and name so passes can resolve
			// by either.
			rt.textures[t.unique_name] = std::move(tex);
		}
		return true;
	}

	bool ChainImpl::BuildEffectUbo(EffectRuntime& rt)
	{
		const reshadefx::effect_module* mod = rt.compiled.module();
		rt.ubo_size = mod ? mod->total_uniform_size : 0;
		if (rt.ubo_size == 0)
		{
			// Even when the effect has no uniforms, ReShade SPIR-V always
			// references a UBO at set=0 binding=0. We allocate a dummy 16-byte
			// buffer so descriptor writes still succeed.
			rt.ubo_size = 16;
		}
		// std140 alignment: round up to 16.
		rt.ubo_size = (rt.ubo_size + 15u) & ~15u;

		const VkBufferCreateInfo bci{
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
			rt.ubo_size,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_SHARING_MODE_EXCLUSIVE
		};
		VmaAllocationCreateInfo aci{};
		aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VmaAllocationInfo ai{};
		const VkResult res = vmaCreateBuffer(device->GetAllocator(), &bci, &aci,
			&rt.ubo, &rt.ubo_alloc, &ai);
		if (res != VK_SUCCESS)
		{
			rt.skip_reason = "Failed to allocate UBO";
			return false;
		}
		rt.ubo_mapped = ai.pMappedData;
		// Zero-initialize so unspecified slots have deterministic content.
		std::memset(rt.ubo_mapped, 0, rt.ubo_size);
		return true;
	}

	GSTextureVK* ChainImpl::FindEffectTexture(const std::string& key, const EffectRuntime& rt) const
	{
		auto it = rt.textures.find(key);
		if (it != rt.textures.end())
			return it->second.get();
		// Fall back to public `name` matching by walking module textures.
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

	void ChainImpl::ResolveSampledImage(const std::string& tex_name,
		const std::string& tex_semantic,
		GSTextureVK* current_back_buffer,
		VkImageView& out_view, VkImageLayout& out_layout, GSTextureVK** out_tex,
		const EffectRuntime& rt) const
	{
		out_view = VK_NULL_HANDLE;
		out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (out_tex)
			*out_tex = nullptr;

		// COLOR semantic == BackBuffer.
		if (IEquals(tex_semantic, "COLOR") && current_back_buffer)
		{
			out_view = current_back_buffer->GetView();
			out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if (out_tex)
				*out_tex = current_back_buffer;
			return;
		}

		// DEPTH semantic == dummy depth (for the MVP we never expose game depth).
		if (IEquals(tex_semantic, "DEPTH") && dummy_depth)
		{
			out_view = dummy_depth->GetView();
			out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if (out_tex)
				*out_tex = dummy_depth.get();
			return;
		}

		GSTextureVK* tex = FindEffectTexture(tex_name, rt);
		if (tex)
		{
			out_view = tex->GetView();
			out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if (out_tex)
				*out_tex = tex;
		}
	}

	// ----------------------------------------------------------------------
	// UBO fill: preset INI -> uniform offsets/size
	// ----------------------------------------------------------------------
	void ChainImpl::FillUniformBuffer(const EffectRuntime& rt)
	{
		if (!rt.ubo_mapped || rt.ubo_size == 0)
			return;

		const reshadefx::effect_module* mod = rt.compiled.module();
		if (!mod)
			return;

		const auto params_it = preset.effect_params.find(rt.effect_filename);
		const EffectParameters* params = (params_it != preset.effect_params.end())
			? &params_it->second
			: nullptr;

		uint8_t* ubo = static_cast<uint8_t*>(rt.ubo_mapped);

		for (const reshadefx::uniform& u : mod->uniforms)
		{
			const uint32_t off = u.offset;
			const uint32_t size = u.size;
			if (off + size > rt.ubo_size)
				continue;

			// 1. Preset INI override.
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

			// 2. Fall back to FX-declared initializer.
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

			// 3. Pack into the UBO. ReShade SPIR-V codegen places vectors
			//    contiguously; matrices are column-major with stride 16.
			const uint32_t cols = std::max(1u, u.type.cols);
			const uint32_t rows = std::max(1u, u.type.rows);
			const bool is_int = u.type.is_integral();
			const bool is_bool = u.type.is_boolean();
			const uint32_t row_stride = (cols > 1) ? 16u : 0u; // vector vs matrix layout

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
				// Matrix: column-major, with each column padded to 16 bytes.
				for (uint32_t c = 0; c < cols; ++c)
					for (uint32_t r = 0; r < rows; ++r)
					{
						const uint32_t comp_index = r * cols + c;
						write_scalar(off + c * row_stride + r * 4u, comp_index);
					}
			}
			else
			{
				// Scalar / vector: contiguous components.
				for (uint32_t r = 0; r < rows; ++r)
					write_scalar(off + r * 4u, r);
			}
		}
	}

	// ----------------------------------------------------------------------
	// Mipmap generation
	// ----------------------------------------------------------------------
	void ChainImpl::GenerateMipmaps(VkCommandBuffer cmdbuf, GSTextureVK* tex)
	{
		if (!tex)
			return;
		// Use the GSTextureVK helper which handles barriers + blit chain.
		// We must be outside any active render pass; the chain's Apply()
		// guarantees that by EndRenderPass before calling here.
		tex->GenerateMipmap();
	}

	// ----------------------------------------------------------------------
	// Per-frame execution
	// ----------------------------------------------------------------------
	bool ChainImpl::ApplyEffect(VkCommandBuffer cmdbuf, EffectRuntime& rt,
		GSTextureVK*& cur_color)
	{
		if (!rt.runnable || !cur_color)
			return false;

		FillUniformBuffer(rt);

		// Each ping-pong texture starts with the same content as cur_color
		// for the first BackBuffer-target pass that reads BackBuffer. To
		// avoid a copy we just sample directly from cur_color until the
		// first BackBuffer-target pass writes a new ping-pong.
		GSTextureVK* sampled_back_buffer = cur_color;

		for (size_t pass_idx = 0; pass_idx < rt.passes.size(); ++pass_idx)
		{
			const PassRuntime& pr = rt.passes[pass_idx];

			// Pick the destination texture.
			GSTextureVK* dst = nullptr;
			if (pr.writes_backbuffer)
			{
				// Choose the ping-pong slot that is not currently sampled.
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

			// Layout transitions: source must be ShaderReadOnly, dest must
			// be ColorAttachment.
			if (sampled_back_buffer)
				sampled_back_buffer->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ShaderReadOnly);
			for (const PassRuntime::SamplerBinding& sb : pr.sampler_bindings)
			{
				GSTextureVK* tex = nullptr;
				VkImageView view; VkImageLayout layout;
				ResolveSampledImage(sb.texture_name, sb.texture_semantic,
					sampled_back_buffer, view, layout, &tex, rt);
				if (tex && tex != sampled_back_buffer && tex != dst)
					tex->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ShaderReadOnly);
			}
			dst->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ColorAttachment);
			dst->SetState(GSTexture::State::Dirty);

			// Allocate descriptor sets.
			const VkDescriptorSet set_ubo = AllocateTransientDescriptorSet(pr.dsl_ubo);
			const VkDescriptorSet set_tex = AllocateTransientDescriptorSet(pr.dsl_tex);
			if (set_ubo == VK_NULL_HANDLE || set_tex == VK_NULL_HANDLE)
				return false;

			Vulkan::DescriptorSetUpdateBuilder dsub;
			dsub.AddBufferDescriptorWrite(set_ubo, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				rt.ubo, 0, rt.ubo_size);
			for (const PassRuntime::SamplerBinding& sb : pr.sampler_bindings)
			{
				VkImageView view = VK_NULL_HANDLE;
				VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				ResolveSampledImage(sb.texture_name, sb.texture_semantic,
					sampled_back_buffer, view, layout, nullptr, rt);
				if (view == VK_NULL_HANDLE && dummy_depth)
					view = dummy_depth->GetView();
				if (view == VK_NULL_HANDLE)
					return false;
				dsub.AddCombinedImageSamplerDescriptorWrite(set_tex, sb.binding, view, sb.sampler, layout);
			}
			dsub.Update(device->GetDevice(), true);

			// Begin render pass.
			const uint32_t vp_w = pr.viewport_width ? pr.viewport_width : static_cast<uint32_t>(dst->GetWidth());
			const uint32_t vp_h = pr.viewport_height ? pr.viewport_height : static_cast<uint32_t>(dst->GetHeight());
			const VkClearValue clear{}; // black

			VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			rpi.renderPass = pr.render_pass;
			rpi.framebuffer = dst->GetFramebuffer(false);
			if (rpi.framebuffer == VK_NULL_HANDLE)
				return false;
			rpi.renderArea.offset = {0, 0};
			rpi.renderArea.extent = {static_cast<uint32_t>(dst->GetWidth()), static_cast<uint32_t>(dst->GetHeight())};
			rpi.clearValueCount = pr.clear_render_target ? 1u : 0u;
			rpi.pClearValues = pr.clear_render_target ? &clear : nullptr;
			vkCmdBeginRenderPass(cmdbuf, &rpi, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport{0, 0, static_cast<float>(vp_w), static_cast<float>(vp_h), 0.0f, 1.0f};
			VkRect2D scissor{{0, 0}, {vp_w, vp_h}};
			vkCmdSetViewport(cmdbuf, 0, 1, &viewport);
			vkCmdSetScissor(cmdbuf, 0, 1, &scissor);

			vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pr.pipeline);
			const VkDescriptorSet sets[2] = {set_ubo, set_tex};
			vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pr.pipeline_layout, 0, 2, sets, 0, nullptr);
			vkCmdDraw(cmdbuf, pr.num_vertices, 1, 0, 0);
			vkCmdEndRenderPass(cmdbuf);

			// Bookkeeping.
			dst->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ShaderReadOnly);
			if (pr.generate_mipmaps && !pr.writes_backbuffer)
				GenerateMipmaps(cmdbuf, dst);

			if (pr.writes_backbuffer)
				sampled_back_buffer = dst;
		}

		cur_color = sampled_back_buffer;
		return true;
	}

	// ----------------------------------------------------------------------
	// ChainVK: thin facade over ChainImpl
	// ----------------------------------------------------------------------
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

		if (!m_impl->EnsureDummyDepth())
			Console.Warning("[%s] Failed to allocate dummy depth texture; depth-reading effects will sample garbage", kLogTag);

		m_impl->CompileAndBuild();

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
		if (!m_impl || !current_io || !cmdbuffer)
			return;
		if (m_impl->permanently_disabled)
			return;
		if (!IsActive())
			return;

		// Free finished descriptor sets before allocating new ones for this
		// frame. Cheap walk over a small queue.
		m_impl->RecycleDescriptorSets();

		if (!m_impl->EnsurePingPong(width, height))
		{
			Console.Warning("[%s] Failed to allocate ping-pong RTs; chain disabled", kLogTag);
			DisablePermanently();
			return;
		}

		GSTextureVK* cur = current_io;
		for (EffectRuntime& rt : m_impl->effects)
		{
			if (!rt.runnable)
				continue;
			if (!m_impl->ApplyEffect(cmdbuffer, rt, cur))
			{
				Console.Warning("[%s] Effect '%s' failed at runtime; disabling for this session",
					kLogTag, rt.effect_filename.c_str());
				rt.runnable = false;
				rt.skip_reason = "Runtime apply failed";
				// Keep going: surviving effects can still post-process.
				cur = current_io;
			}
		}

		// Make sure the final texture is sampleable for the present pass.
		if (cur)
			cur->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
		current_io = cur;
	}

	void ChainVK::NotifyBackbufferResized(uint32_t width, uint32_t height)
	{
		if (!m_impl)
			return;
		// We keep the old size for one more frame so EnsurePingPong sees the
		// change and reallocates. Setting bb_width/height to 0 would force
		// CompileAndBuild's BUFFER_WIDTH macros to be wrong on next preset
		// load, so we update them instead.
		m_impl->bb_width = width;
		m_impl->bb_height = height;
		m_impl->ping_pong[0].reset();
		m_impl->ping_pong[1].reset();
	}
} // namespace ReShade
