// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Note about terminology:
// "Patch" in PCSX2 terminology refers to a single pnach style patch line, e.g. `patch=1,EE,001110e0,word,00000000`
// Such patches can appear in several places:
//  - At the "patches" folder or on the "patches.zip file inside the 'resources' folder
//    - UI name: "Patch", Controlled via Per-Game Settings -> Patches
//  - At the "cheats" folder
//    - UI name: "Cheats", Controlled via Per-Game Settings -> Cheats -> Enable Cheat
//  - At GameIndex.yaml inside a [patches] section
//    - UI name: "Enable Compatibility Patches", controlled via Advanced section -> Enable compatability settings
// Note: The file name has to be exactly "<Serial>_<CRC>.pnach" (For example "SLPS-25399_CD62245A.pnach")
// Note #2: the old sytle of cheats are also supported but arent supported by the UI

#include "Config.h"

#include "common/MemoryInterface.h"
#include "common/SmallString.h"

#include <string>
#include <string_view>
#include <vector>

class EEMemoryInterface;
class IOPMemoryInterface;

namespace Patch
{
	// 前向声明：指针表达式（§2/§12.1 of 金手指优化.md）
	// 完整定义在 Patch.cpp，PatchCommand 仅持有指针。
	struct PointerChainExpr;

	// "place" is the first number at a pnach line (patch=<place>,...), e.g.:
	// - patch=1,EE,001110e0,word,00000000 <-- place is 1
	// - patch=0,EE,0010BC88,word,48468800 <-- place is 0
	// In PCSX2 it indicates how/when/where the patch line should be applied. If
	// place is not one of the supported values then the patch line is never applied.
	// PCSX2 currently supports the following values:
	// 0 - apply the patch line once on game boot only
	// 1 - apply the patch line continuously (technically - on every vsync)
	// 2 - effect of 0 and 1 combined, see below
	// 3 - apply the patch line once on game boot or when enabled in the GUI
	// Note:
	// - while it may seem that a value of 1 does the same as 0, but also later
	//   continues to apply the patch on every vsync - it's not.
	//   The current (and past) behavior is that these patches are applied at different
	//   places at the code, and it's possible, depending on circumstances, that 0 patches
	//   will get applied before the first vsync and therefore earlier than 1 patches.
	enum patch_place_type : u8
	{
		PPT_ONCE_ON_LOAD = 0,
		PPT_CONTINUOUSLY = 1,
		PPT_COMBINED_0_1 = 2,
		PPT_ON_LOAD_OR_WHEN_ENABLED = 3,

		PPT_END_MARKER
	};

	enum patch_cpu_type : u8
	{
		CPU_EE,
		CPU_IOP
	};

	enum patch_data_type : u8
	{
		BYTE_T,
		SHORT_T,
		WORD_T,
		DOUBLE_T,
		EXTENDED_T,
		SHORT_BE_T,
		WORD_BE_T,
		DOUBLE_BE_T,
		BYTES_T,
		// PC 全量移植：新增 swap / float 两种数据类型
		SWAP_T,
		FLOAT_T
	};

	static constexpr std::array<const char*, 4> s_place_to_string = {{"0", "1", "2", "3"}};
	static constexpr std::array<const char*, 2> s_cpu_to_string = {{"EE", "IOP"}};
	static constexpr std::array<const char*, 11> s_type_to_string = {
		{"byte", "short", "word", "double", "extended", "beshort", "beword", "bedouble", "bytes", "swap", "float"}};

	struct PatchCommand
	{
		patch_place_type placetopatch;
		patch_cpu_type cpu;
		patch_data_type type;
		u32 addr;
		u64 data;
		u8* data_ptr;

		// PC 全量移植 §2/§12.1：指针链表达式（堆分配；为静态十六进制时为 nullptr）
		PointerChainExpr* ptr_addr; // 非空 → addr 在 apply 时由表达式解析得到
		PointerChainExpr* ptr_data; // 非空 → data 在 apply 时由表达式解析得到

		// PC 全量移植：randint(lo,hi) — apply 时随机化 data
		bool is_random;
		u64 rand_lo;
		u64 rand_hi;

		bool is_rand_float;
		float rand_float_lo;
		float rand_float_hi;

		// 含指针、所以默认/拷贝/析构都要显式管理
		PatchCommand() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
		PatchCommand(const PatchCommand& p) = delete;
		PatchCommand(PatchCommand&& p)
		{
			std::memcpy(static_cast<void*>(this), &p, sizeof(*this));
			p.data_ptr = nullptr;
			p.ptr_addr = nullptr;
			p.ptr_data = nullptr;
		}
		~PatchCommand();

		PatchCommand& operator=(const PatchCommand& p) = delete;
		PatchCommand& operator=(PatchCommand&& p);

		bool operator==(const PatchCommand& p) const
		{
			// 指针链补丁不参与去重，只比较普通字段
			return placetopatch == p.placetopatch && cpu == p.cpu && type == p.type &&
			       addr == p.addr && data == p.data && data_ptr == p.data_ptr &&
			       ptr_addr == p.ptr_addr && ptr_data == p.ptr_data &&
			       is_random == p.is_random && rand_lo == p.rand_lo && rand_hi == p.rand_hi &&
			       is_rand_float == p.is_rand_float && rand_float_lo == p.rand_float_lo && rand_float_hi == p.rand_float_hi;
		}
		bool operator!=(const PatchCommand& p) const { return !(*this == p); }

		SmallString ToString() const
		{
			return SmallString::from_format("{},{},{},{:08x},{:x}{}{}",
				s_place_to_string[static_cast<u8>(placetopatch)],
				s_cpu_to_string[static_cast<u8>(cpu)],
				s_type_to_string[static_cast<u8>(type)], addr, data,
				ptr_addr ? " [ptr-addr]" : "", ptr_data ? " [ptr-data]" : "");
		}
	};
	// 16(addr/data/data_ptr) + 24(基础枚举/place/cpu/type 与 padding) + 16(两个指针)
	// + 1(bool)+15(对齐)+8(lo)+8(hi) → 实际由编译器决定。这里仅校验它是 8 字节对齐的合理大小，
	// 而不再固定为 24，否则增加字段会触发编译错误。
	static_assert(sizeof(PatchCommand) >= 24 && (sizeof(PatchCommand) % 8) == 0,
		"PatchCommand size must be 8-byte aligned");

	struct DynamicPatchEntry
	{
		u32 offset;
		u32 value;
	};

	struct DynamicPatch
	{
		std::vector<DynamicPatchEntry> pattern;
		std::vector<DynamicPatchEntry> replacement;
	};

	struct PatchInfo
	{
		std::string name;
		std::string description;
		std::string author;

		// This is only populated if all the patch lines in a given group have
		// the same place value.
		std::optional<patch_place_type> place;

		std::string_view GetNamePart() const;
		std::string_view GetNameParentPart() const;
	};

	// Config sections/keys to use to enable patches.
	extern const char* PATCHES_CONFIG_SECTION;
	extern const char* CHEATS_CONFIG_SECTION;
	extern const char* PATCH_ENABLE_CONFIG_KEY;
	extern const char* PATCH_DISABLE_CONFIG_KEY;

	extern std::vector<PatchInfo> GetPatchInfo(const std::string_view serial, u32 crc, bool cheats, bool showAllCRCS, u32* num_unlabelled_patches);

	/// Returns the path to a new cheat/patch pnach for the specified serial and CRC.
	extern std::string GetPnachFilename(const std::string_view serial, u32 crc, bool cheats);

	/// Reloads cheats/patches. If verbose is set, the number of patches loaded will be shown in the OSD.
	extern void ReloadPatches(const std::string& serial, u32 crc, bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed);

	extern void UpdateActivePatches(bool reload_enabled_list, bool verbose, bool verbose_if_changed, bool apply_new_patches);

	/// 局域网联机「公平」模式（房主开启）：不向活动补丁列表注入 cheats 目录金手指（本会话）。
	extern void SetFairPlayLanDisableDiskCheats(bool disable);
	extern bool GetFairPlayLanDisableDiskCheats();
	extern void ApplyPatchSettingOverrides();
	extern bool ReloadPatchAffectingOptions();
	extern void UnloadPatches();

	/// Functions for Dynamic EE patching.
	extern void LoadDynamicPatches(const std::vector<DynamicPatch>& patches);
	extern void ApplyDynamicPatches(u32 pc);

	/// Apply all loaded patches that should be applied when the entry point is
	/// being recompiled.
	extern void ApplyBootPatches();

	/// Apply all loaded patches that should be applied during vsync.
	extern void ApplyVsyncPatches();

	// PC 全量移植 §12.2：F-gate 状态机 / 帧级处理
	// 与 PC `Patch::ApplyFGateUpdates` 等价；ApplyVsyncPatches 内部尾部调用。
	// EE 上下文内部自构造（与 ApplyVsyncPatches 一致），无需调用方传参。
	extern void ApplyFGateUpdates();

	// PC 全量移植 §12.2：复位 F-gate 状态（包括已注册块、累加器、去重集）。
	// UnloadPatches 的最后一步会调用，确保跨游戏 / 重载补丁时不残留旧 gate。
	extern void ResetFGateState();

	/// Apply the patches from the provided list which have place values that
	/// match the one specified.
	extern void ApplyPatches(
		const std::vector<const PatchCommand*>& patches,
		patch_place_type place,
		EEMemoryInterface& ee,
		IOPMemoryInterface& iop);
	extern void ApplyPatches(
		const std::vector<const PatchCommand*>& patches,
		patch_place_type place,
		MemoryInterface& ee,
		MemoryInterface& iop);

	// Get the total counts of the active game patches.
	extern u32 GetActiveGameDBPatchesCount();
	extern u32 GetActivePatchesCount();
	extern u32 GetActiveCheatsCount();
	extern u32 GetAllActivePatchesCount();

	extern bool IsGloballyToggleablePatch(const PatchInfo& patch_info);

	extern const char* PlaceToString(std::optional<patch_place_type> place);
} // namespace Patch
