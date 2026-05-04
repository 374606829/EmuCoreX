// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_ // disables MIPS opcode macros.

#include "common/Assertions.h"
#include "common/ByteSwap.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/ZipHelpers.h"

#include "Achievements.h"
#include "Config.h"
#include "core/runtime/GameDatabase.h"
#include "platform/host/Host.h"
#include "IopMem.h"
#include "Memory.h"
#include "Patch.h"
#include "R5900.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Patch
{
	// ── PC 全量移植：指针链表达式（§2 / §12.1） ─────────────────────────────
	// 加载时解析存储；apply 时按 EE/IOP 走对应 MemoryInterface 解析。
	// 两种形态：
	//   地址链 (address chain)：'(' 前是单个数字（如 '2(...)') → 结果是 *地址*
	//   值链  (value chain)：'(' 前为空或非单个数字 → 结果是最终地址 *读出来的值*
	struct PointerChainExpr
	{
		std::string prefix;          // '(' 之前的字符
		std::string suffix;          // ')' 之后的字符（仅地址链使用，会被当作 hex 串拼接到结果上；
		                             // 值链 + 算术偏移路径下保持为空，由 has_arith_offset/arith_* 表达）
		u32 base = 0;                // '(' 内第一个十六进制段
		std::vector<u32> offsets;    // '+' 分隔的剩余十六进制段
		bool is_value_chain = false; // §12.1.2：true → 值链；false → 地址链

		// 优化.md §12.3：值链常量算术偏移 — `(chain)+N` / `(chain)-N`
		// 仅 is_value_chain==true 时允许。N 同时被解析为 int64（适用整型/EXTENDED_T）
		// 和 double（适用 FLOAT_T）；选择哪一种由 ApplyPatch 按 patch_data_type 决定。
		// 设计意图：让 pnach 直接表达"读取指针链的值再 +1 写回"，无需借助 EXTENDED_T 的 +Y 增量码或脚本。
		bool has_arith_offset = false;
		bool arith_is_decimal = false; // 用户书写的 N 是否含小数点；用于 ApplyPatch 在非 FLOAT 类型上拒绝小数偏移
		int64_t arith_offset_int = 0;
		double arith_offset_float = 0.0;
	};

	template <typename EnumType, class ArrayType>
	static inline std::optional<EnumType> LookupEnumName(const std::string_view val, const ArrayType& arr)
	{
		for (size_t i = 0; i < arr.size(); i++)
		{
			if (val == arr[i])
				return static_cast<EnumType>(i);
		}
		return std::nullopt;
	}

	// 指针表达式解析；slot 形如 "[prefix](base[+off1+off2+...])[ suffix ]"
	// 没有匹配的小括号时返回 nullptr（视为静态十六进制）。
	static std::unique_ptr<PointerChainExpr> ParsePointerExpr(const std::string_view slot);

	// 指针链运行时解析（地址链）：返回最终地址；遇到空指针时返回 nullopt 表示该行整体跳过。
	static std::optional<u32> ResolvePointerChain(const PointerChainExpr& expr, MemoryInterface& mem);

	// §12.1：值指针链解析 — 行走链路得到地址后再读取 32 位值并返回。
	// 用于 data 槽或 F-gate Format B 的 cond(...) 比较目标。
	static u32 ResolveValuePointerChain(const PointerChainExpr& expr, MemoryInterface& mem);

	struct PatchGroup
	{
		std::string name;
		std::optional<float> override_aspect_ratio;
		std::optional<AspectRatioType> override_aspect_ratio_mode;
		std::optional<GSInterlaceMode> override_interlace_mode;
		std::vector<PatchCommand> patches;
		std::vector<DynamicPatch> dpatches;
	};

	struct PatchTextTable
	{
		int code;
		const char* text;
		void (*func)(PatchGroup* group, const std::string_view cmd, const std::string_view param);
	};

	// F-gate 块 / 槽：完整定义在文件作用域下面，这里前向声明，避免 ExtendedState 引用未知类型
	namespace FGate
	{
		struct Slot;
		struct Block;
	}

	struct ExtendedState
	{
		u32 skip_count = 0;
		u32 iteration_count = 0;
		u32 iteration_increment = 0;
		u32 prev_cheat_type = 0;
		u32 prev_cheat_addr = 0;
		u32 last_type = 0;
		bool null_pointer_encountered = false;

		// PC 全量移植 §12.2：F-gate 累加器。
		// 每次 ApplyPatches 进入循环前会重置，保证一组 F-block 的三行都来自同一遍
		// 顺序遍历，跨遍历不会错位。
		// phase: 0=未进入；1=已捕获 open；2=已捕获 close（等待第 3 行）
		int fgate_accum_phase = 0;
		// 缓冲块（Slot::ptr_chain 由本结构 own，析构时 reset() 释放）
		// 用 unique_ptr 避免在头里完整定义 Block。
		std::unique_ptr<FGate::Block> fgate_accum_block;
	};

	namespace PatchFunc
	{
		static void patch(PatchGroup* group, const std::string_view cmd, const std::string_view param);
		static void gsaspectratio(PatchGroup* group, const std::string_view cmd, const std::string_view param);
		static void gsinterlacemode(PatchGroup* group, const std::string_view cmd, const std::string_view param);
		static void dpatch(PatchGroup* group, const std::string_view cmd, const std::string_view param);
	} // namespace PatchFunc

	static void TrimPatchLine(std::string& buffer);
	static int PatchTableExecute(PatchGroup* group, const std::string_view lhs, const std::string_view rhs,
		const std::span<const PatchTextTable>& Table);
	static void LoadPatchLine(PatchGroup* group, const std::string_view line);
	static u32 LoadPatchesFromString(std::vector<PatchGroup>* patch_list, const std::string& patch_file);
	static bool OpenPatchesZip();
	static std::string GetPnachTemplate(
		const std::string_view serial, u32 crc, bool include_serial, bool add_wildcard, bool all_crcs);
	static std::vector<std::string> FindPatchFilesOnDisk(
		const std::string_view serial, u32 crc, bool cheats, bool all_crcs);

	static bool ContainsPatchName(const std::vector<PatchInfo>& patches, const std::string_view patchName);
	static bool ContainsPatchName(const std::vector<PatchGroup>& patches, const std::string_view patchName);

	template <typename F>
	static void EnumeratePnachFiles(const std::string_view serial, u32 crc, bool cheats, bool for_ui, const F& f);

	static bool PatchStringHasUnlabelledPatch(const std::string& pnach_data);
	static void ExtractPatchInfo(std::vector<PatchInfo>* dst, const std::string& pnach_data, u32* num_unlabelled_patches);
	static void ReloadEnabledLists();
	static u32 EnablePatches(const std::vector<PatchGroup>* patches, const std::vector<std::string>& enable_list, const std::vector<std::string>* enable_immediately_list);

	template <typename EEMemory, typename IOPMemory>
		requires std::is_base_of_v<MemoryInterface, EEMemory> &&
	             std::is_base_of_v<MemoryInterface, IOPMemory>
	void ApplyPatch(const PatchCommand* p, EEMemory& ee, IOPMemory& iop, ExtendedState& state);
	static void ApplyDynaPatch(const DynamicPatch& patch, u32 address);
	template <typename Memory>
		requires std::is_base_of_v<MemoryInterface, Memory>
	static void writeCheat(Memory& memory, ExtendedState& state);
	template <typename Memory>
		requires std::is_base_of_v<MemoryInterface, Memory>
	static void handle_extended_t(const PatchCommand* p, Memory& memory, ExtendedState& state);

	// Name of patches which will be auto-enabled based on global options.
	static constexpr std::string_view WS_PATCH_NAME = "Widescreen 16:9";
	static constexpr std::string_view NI_PATCH_NAME = "No-Interlacing";
	static constexpr std::string_view PATCHES_ZIP_NAME = "patches.zip";

	const char* PATCHES_CONFIG_SECTION = "Patches";
	const char* CHEATS_CONFIG_SECTION = "Cheats";
	const char* PATCH_ENABLE_CONFIG_KEY = "Enable";
	const char* PATCH_DISABLE_CONFIG_KEY = "Disable";

	static zip_t* s_patches_zip;
	static std::vector<PatchGroup> s_gamedb_patches;
	static std::vector<PatchGroup> s_game_patches;
	static std::vector<PatchGroup> s_cheat_patches;

	static u32 s_gamedb_counts = 0;
	static u32 s_patches_counts = 0;
	static u32 s_cheats_counts = 0;

	static std::vector<const PatchCommand*> s_active_patches;
	static std::vector<DynamicPatch> s_active_gamedb_dynamic_patches;
	static std::vector<DynamicPatch> s_active_pnach_dynamic_patches;
	static std::vector<std::string> s_enabled_cheats;
	static std::vector<std::string> s_enabled_patches;
	static std::vector<std::string> s_just_enabled_cheats;
	static std::vector<std::string> s_just_enabled_patches;
	static u32 s_patches_crc;
	static std::optional<float> s_override_aspect_ratio;
	static std::optional<AspectRatioType> s_override_aspect_ratio_mode;
	static std::optional<GSInterlaceMode> s_override_interlace_mode;

	static const PatchTextTable s_patch_commands[] = {
		{0, "patch", &Patch::PatchFunc::patch},
		{0, "gsaspectratio", &Patch::PatchFunc::gsaspectratio},
		{0, "gsinterlacemode", &Patch::PatchFunc::gsinterlacemode},
		{0, "dpatch", &Patch::PatchFunc::dpatch},
		{0, nullptr, nullptr},
	};
} // namespace Patch

// PatchCommand 析构 / 移动赋值的实现（在 .cpp 里，因为需要 PointerChainExpr 完整类型）
Patch::PatchCommand::~PatchCommand()
{
	if (data_ptr)
		std::free(data_ptr);
	delete ptr_addr;
	delete ptr_data;
}

Patch::PatchCommand& Patch::PatchCommand::operator=(PatchCommand&& p)
{
	if (this == &p)
		return *this;
	if (data_ptr)
		std::free(data_ptr);
	delete ptr_addr;
	delete ptr_data;
	std::memcpy(static_cast<void*>(this), &p, sizeof(*this));
	p.data_ptr = nullptr;
	p.ptr_addr = nullptr;
	p.ptr_data = nullptr;
	return *this;
}

// ── 指针链解析（§2 of 金手指优化.md） ─────────────────────────────────────
// 若 slot 内同时含 '(' 与 ')' 且 '(' 在 ')' 之前，按
//   [prefix](base_hex[+offset_hex]...)[suffix]
// 解析；否则返回 nullptr（视为静态十六进制）。
std::unique_ptr<Patch::PointerChainExpr> Patch::ParsePointerExpr(const std::string_view slot)
{
	const auto lp = slot.find('(');
	const auto rp = slot.find(')');
	if (lp == std::string_view::npos || rp == std::string_view::npos || lp >= rp)
		return nullptr;

	auto expr = std::make_unique<PointerChainExpr>();
	expr->prefix = std::string(slot.substr(0, lp));
	std::string after = std::string(slot.substr(rp + 1));
	expr->suffix = after; // 暂存，下面会按需重写为算术偏移分支

	const std::string_view inner = slot.substr(lp + 1, rp - lp - 1);
	const std::vector<std::string_view> segs(StringUtil::SplitString(inner, '+', false));
	if (segs.empty())
	{
		Console.Error(fmt::format("(Patch) Pointer expression has empty body: {}", slot));
		return nullptr;
	}

	const auto base = StringUtil::FromChars<u32>(segs[0], 16);
	if (!base.has_value())
	{
		Console.Error(fmt::format("(Patch) Pointer expression: invalid base '{}' in '{}'", segs[0], slot));
		return nullptr;
	}
	expr->base = base.value();

	for (size_t i = 1; i < segs.size(); i++)
	{
		const auto off = StringUtil::FromChars<u32>(segs[i], 16);
		if (!off.has_value())
		{
			Console.Error(fmt::format("(Patch) Pointer expression: invalid offset '{}' in '{}'", segs[i], slot));
			return nullptr;
		}
		expr->offsets.push_back(off.value());
	}

	// §12.1.2：地址链 vs 值链。
	// 地址链：prefix 恰好是 '0'-'9' 单字符（如 "2(...)"）
	// 值链：prefix 为空，或并非"恰好一个数字"
	if (expr->prefix.empty())
	{
		expr->is_value_chain = true;
	}
	else if (expr->prefix.size() == 1 && expr->prefix[0] >= '0' && expr->prefix[0] <= '9')
	{
		expr->is_value_chain = false;
	}
	else
	{
		expr->is_value_chain = true;
	}

	// 优化.md §12.3：值链常量算术偏移 `(chain)+N` / `(chain)-N`。
	// 仅在值链上允许（地址链结果是地址，做算术无意义）。识别条件：
	//   - is_value_chain == true
	//   - suffix 紧随 ')' 的第一个字符为 '+' 或 '-'
	//   - 之后为合法十进制数字（可含小数点）
	// 对应的 ApplyPatch 逻辑会按 patch_data_type 选择走 int64 还是 double。
	if (expr->is_value_chain && !after.empty() && (after[0] == '+' || after[0] == '-'))
	{
		std::string_view tail(after);
		while (!tail.empty() && (tail.back() == ' ' || tail.back() == '\t'))
			tail.remove_suffix(1);

		const bool negative = (tail[0] == '-');
		std::string_view num = tail.substr(1);
		bool seen_digit = false;
		bool seen_dot = false;
		bool malformed = num.empty();
		for (char c : num)
		{
			if (c >= '0' && c <= '9')
			{
				seen_digit = true;
			}
			else if (c == '.' && !seen_dot)
			{
				seen_dot = true;
			}
			else
			{
				malformed = true;
				break;
			}
		}
		if (!seen_digit)
			malformed = true;

		if (!malformed)
		{
			std::string_view fend;
			const auto dval = StringUtil::FromChars<double>(num, &fend);
			if (dval.has_value() && fend.empty())
			{
				expr->has_arith_offset = true;
				expr->arith_is_decimal = seen_dot;
				expr->arith_offset_float = negative ? -dval.value() : dval.value();
				if (!seen_dot)
				{
					const auto ival = StringUtil::FromChars<int64_t>(num, 10);
					if (ival.has_value())
						expr->arith_offset_int = negative ? -ival.value() : ival.value();
					else
						expr->arith_offset_int = static_cast<int64_t>(expr->arith_offset_float);
				}
				else
				{
					expr->arith_offset_int = static_cast<int64_t>(expr->arith_offset_float);
				}
				expr->suffix.clear(); // 算术偏移分支下，suffix 不再参与旧的 hex 拼接路径
			}
		}
	}

	return expr;
}

// ── 指针链运行时解析（地址链；§3 of 金手指优化.md） ──────────────────────
// 走法与 PC 完全一致：先 read32(base)，逐段 += 偏移；除最后一段外都要解引用。
// base 指针为 0 且仍有偏移，则视为该行作废，返回 nullopt。
std::optional<u32> Patch::ResolvePointerChain(const PointerChainExpr& expr, MemoryInterface& mem)
{
	u32 ptr = mem.Read32(expr.base);
	if (ptr == 0 && expr.offsets.size() > 0)
		return std::nullopt;

	const size_t n = expr.offsets.size();
	if (n != 0)
	{
		for (size_t i = 0; i < n; i++)
		{
			ptr += expr.offsets[i];
			if (i < n - 1)
				ptr = mem.Read32(ptr);
			// 最后一段不解引用 — ptr 即为最终地址
		}
	}

	// 拼接 prefix + 可能的 '0' 填充 + 计算结果 hex + suffix；再解析为 u32。
	std::string s = expr.prefix;
	if (expr.prefix.size() == 1 && expr.prefix[0] >= '0' && expr.prefix[0] <= '9')
		s += '0';
	s += fmt::format("{:X}", ptr);
	s += expr.suffix;

	const auto resolved = StringUtil::FromChars<u32>(s, 16);
	if (!resolved.has_value())
	{
		Console.Warning(fmt::format("(Patch) Pointer chain resolved string '{}' is not a valid hex u32, skipping line.", s));
		return std::nullopt;
	}
	return resolved;
}

// ── §12.1：值指针链解析 ─────────────────────────────────────────────────
// 与地址链同走链，区别是末段也解引用，结果为最终地址处的 32 位值。
u32 Patch::ResolveValuePointerChain(const PointerChainExpr& expr, MemoryInterface& mem)
{
	u32 ptr = mem.Read32(expr.base);
	if (ptr == 0 && expr.offsets.size() > 0)
		return 0;

	for (size_t i = 0; i < expr.offsets.size(); i++)
	{
		ptr += expr.offsets[i];
		ptr = mem.Read32(ptr);
	}
	return ptr;
}

// ── §12.2：F-gate 状态机完整实现（PC 等价） ──────────────────────────────
// 每个 F-gate 块由三行扩展行组成：
//   Line 1: open  condition (Fxxxxxxx, extended, <config>)
//   Line 2: close condition (Fyyyyyyy, extended, <config>)
//   Line 3: guarded patch — 仅在 gate 打开期间应用
namespace Patch::FGate
{
	struct Slot
	{
		u32 watch_addr = 0;
		u8 cond = 0;
		u32 compare_value = 0;
		u16 hold_frames = 0;
		bool use_ptr_chain = false;
		Patch::PointerChainExpr* ptr_chain = nullptr;

		// 运行时累计
		u32 streak = 0;
		bool latched = false;

		Slot() = default;
		Slot(const Slot&) = delete;
		Slot(Slot&& o) noexcept
		{
			watch_addr = o.watch_addr;
			cond = o.cond;
			compare_value = o.compare_value;
			hold_frames = o.hold_frames;
			use_ptr_chain = o.use_ptr_chain;
			ptr_chain = o.ptr_chain;
			streak = o.streak;
			latched = o.latched;
			o.ptr_chain = nullptr;
		}
		Slot& operator=(const Slot&) = delete;
		Slot& operator=(Slot&& o) noexcept
		{
			if (this != &o)
			{
				delete ptr_chain;
				watch_addr = o.watch_addr;
				cond = o.cond;
				compare_value = o.compare_value;
				hold_frames = o.hold_frames;
				use_ptr_chain = o.use_ptr_chain;
				ptr_chain = o.ptr_chain;
				streak = o.streak;
				latched = o.latched;
				o.ptr_chain = nullptr;
			}
			return *this;
		}
		~Slot() { delete ptr_chain; }
	};

	struct Block
	{
		Slot open_slot;
		Slot close_slot;
		Patch::PatchCommand guarded_cmd;
		bool has_guarded = false;
		bool valid = false;

		Block() = default;
		Block(const Block&) = delete;
		Block(Block&&) = default;
		Block& operator=(const Block&) = delete;
		Block& operator=(Block&&) = default;
	};

	static std::vector<Block> s_fgate_blocks;
	static std::unordered_set<u64> s_fgate_registered_keys;

	static u64 MakeBlockKey(u32 open_addr, u32 close_addr)
	{
		return (static_cast<u64>(open_addr) << 32) | static_cast<u64>(close_addr);
	}

	// D/E 风格条件比较
	static bool EvalCondition(u8 cond, u32 mem_val, u32 target_val)
	{
		switch (cond)
		{
			case 0: return mem_val == target_val;       // equal
			case 1: return mem_val != target_val;       // not equal
			case 2: return mem_val < target_val;        // less than
			case 3: return mem_val > target_val;        // greater than
			case 4: return (mem_val & target_val) != 0; // AND non-zero
			case 5: return (mem_val & target_val) == 0; // AND zero
			case 6: return (mem_val | target_val) != 0; // OR non-zero
			case 7: return (mem_val | target_val) == 0; // OR zero
			default: return false;
		}
	}

	static void Reset()
	{
		s_fgate_blocks.clear();
		s_fgate_registered_keys.clear();
	}

	// Parse Format A (8 hex chars) 或 Format B ("c(chain)hold")
	// Format A：[31:28]=cond，[27:16]=compare_value (12bit imm)，[15:0]=hold_frames
	static bool ParseConfig(const std::string_view config, Slot& slot)
	{
		const auto lp = config.find('(');
		if (lp != std::string_view::npos && lp >= 1)
		{
			const auto rp = config.find(')', lp);
			if (rp == std::string_view::npos)
			{
				Console.Error(fmt::format("(Patch) F-gate Format B: missing closing ')': {}", config));
				return false;
			}

			const std::string_view cond_str = config.substr(0, lp);
			if (cond_str.size() != 1)
			{
				Console.Error(fmt::format("(Patch) F-gate Format B: cond must be single hex char: {}", config));
				return false;
			}
			const auto cond_val = StringUtil::FromChars<u8>(cond_str, 16);
			if (!cond_val.has_value())
			{
				Console.Error(fmt::format("(Patch) F-gate Format B: invalid cond '{}': {}", cond_str, config));
				return false;
			}
			slot.cond = cond_val.value();

			const std::string_view chain_body = config.substr(lp + 1, rp - lp - 1);
			const std::vector<std::string_view> segs(StringUtil::SplitString(chain_body, '+', false));
			if (segs.empty())
			{
				Console.Error(fmt::format("(Patch) F-gate Format B: empty pointer chain: {}", config));
				return false;
			}

			auto* pce = new Patch::PointerChainExpr();
			const auto base = StringUtil::FromChars<u32>(segs[0], 16);
			if (!base.has_value())
			{
				delete pce;
				Console.Error(fmt::format("(Patch) F-gate Format B: invalid chain base: {}", config));
				return false;
			}
			pce->base = base.value();
			pce->is_value_chain = true;
			for (size_t i = 1; i < segs.size(); i++)
			{
				const auto off = StringUtil::FromChars<u32>(segs[i], 16);
				if (!off.has_value())
				{
					delete pce;
					Console.Error(fmt::format("(Patch) F-gate Format B: invalid chain offset: {}", config));
					return false;
				}
				pce->offsets.push_back(off.value());
			}
			slot.ptr_chain = pce;
			slot.use_ptr_chain = true;

			const std::string_view hold_str = config.substr(rp + 1);
			if (hold_str.empty())
			{
				Console.Error(fmt::format("(Patch) F-gate Format B: missing hold_frames: {}", config));
				return false;
			}
			const auto hold = StringUtil::FromChars<u16>(hold_str, 16);
			if (!hold.has_value())
			{
				Console.Error(fmt::format("(Patch) F-gate Format B: invalid hold_frames '{}': {}", hold_str, config));
				return false;
			}
			slot.hold_frames = hold.value();
			return true;
		}

		// Format A
		if (config.size() != 8)
		{
			Console.Error(fmt::format("(Patch) F-gate Format A: expected 8 hex chars, got {}: {}", config.size(), config));
			return false;
		}
		const auto word = StringUtil::FromChars<u32>(config, 16);
		if (!word.has_value())
		{
			Console.Error(fmt::format("(Patch) F-gate Format A: invalid hex word: {}", config));
			return false;
		}
		const u32 w = word.value();
		slot.cond = static_cast<u8>((w >> 28) & 0xF);
		slot.compare_value = (w >> 16) & 0xFFF;
		slot.hold_frames = static_cast<u16>(w & 0xFFFF);
		slot.use_ptr_chain = false;
		slot.ptr_chain = nullptr;
		return true;
	}

	// 每帧累计观察值，命中次数达到 hold_frames 时锁存
	static void UpdateSlotStreak(Slot& slot, MemoryInterface& mem)
	{
		const u32 masked_addr = slot.watch_addr & 0x0FFFFFFF;
		const u32 mem_val = mem.Read16(masked_addr);

		u32 target;
		if (slot.use_ptr_chain && slot.ptr_chain)
			target = Patch::ResolveValuePointerChain(*slot.ptr_chain, mem);
		else
			target = slot.compare_value;

		if (EvalCondition(slot.cond, mem_val, target))
			slot.streak++;
		else
			slot.streak = 0;
	}
} // namespace Patch::FGate

void Patch::TrimPatchLine(std::string& buffer)
{
	StringUtil::StripWhitespace(&buffer);
	if (std::strncmp(buffer.c_str(), "//", 2) == 0)
	{
		// comment
		buffer.clear();
	}

	// check for comments at the end of a line
	const std::string::size_type pos = buffer.find("//");
	if (pos != std::string::npos)
		buffer.erase(pos);
}

bool Patch::ContainsPatchName(const std::vector<PatchGroup>& patch_list, const std::string_view patch_name)
{
	return std::find_if(patch_list.begin(), patch_list.end(), [&patch_name](const PatchGroup& patch) {
		return patch.name == patch_name;
	}) != patch_list.end();
}

int Patch::PatchTableExecute(PatchGroup* group, const std::string_view lhs, const std::string_view rhs,
	const std::span<const PatchTextTable>& Table)
{
	int i = 0;

	while (Table[i].text)
	{
		if (lhs.compare(Table[i].text) == 0)
		{
			if (Table[i].func)
				Table[i].func(group, lhs, rhs);
			break;
		}
		i++;
	}

	return Table[i].code;
}

// This routine is for executing the commands of the ini file.
void Patch::LoadPatchLine(PatchGroup* group, const std::string_view line)
{
	std::string_view key, value;
	StringUtil::ParseAssignmentString(line, &key, &value);

	PatchTableExecute(group, key, value, s_patch_commands);
}

u32 Patch::LoadPatchesFromString(std::vector<PatchGroup>* patch_list, const std::string& patch_file)
{
	const size_t before = patch_list->size();

	PatchGroup current_patch_group;
	const auto add_current_patch = [patch_list, &current_patch_group]() {
		if (!current_patch_group.patches.empty())
		{
			// Ungrouped/legacy patches should merge with other ungrouped patches.
			if (current_patch_group.name.empty())
			{
				const std::vector<PatchGroup>::iterator ungrouped_patch = std::find_if(patch_list->begin(), patch_list->end(),
					[](const PatchGroup& pg) { return pg.name.empty(); });
				if (ungrouped_patch != patch_list->end())
				{
					Console.WriteLn(Color_Gray, fmt::format(
						"Patch: Merging {} new patch commands into ungrouped list.", current_patch_group.patches.size()));

					ungrouped_patch->patches.reserve(ungrouped_patch->patches.size() + current_patch_group.patches.size());
					for (PatchCommand& cmd : current_patch_group.patches)
						ungrouped_patch->patches.push_back(std::move(cmd));
				}
				else
				{
					// Always add ungrouped patches, no sense to compare empty names.
					patch_list->push_back(std::move(current_patch_group));
				}

				return;
			}
		}

		if (current_patch_group.patches.empty() && current_patch_group.dpatches.empty())
			return;

		// Don't show patches with duplicate names, prefer the first loaded.
		if (!ContainsPatchName(*patch_list, current_patch_group.name))
		{
			patch_list->push_back(std::move(current_patch_group));
		}
		else
		{
			Console.WriteLn(Color_Gray, fmt::format(
				"Patch: Skipped loading patch '{}' since a patch with a duplicate name was already loaded.",
				current_patch_group.name));
		}
	};

	std::istringstream ss(patch_file);
	std::string line;
	while (std::getline(ss, line))
	{
		TrimPatchLine(line);
		if (line.empty())
			continue;

		if (line.front() == '[')
		{
			if (line.length() < 2 || line.back() != ']')
			{
				Console.Error(fmt::format("Malformed patch line: {}", line.c_str()));
				continue;
			}

			if (!current_patch_group.name.empty() || !current_patch_group.patches.empty() || !current_patch_group.dpatches.empty())
			{
				add_current_patch();
				current_patch_group = {};
			}

			current_patch_group.name = line.substr(1, line.length() - 2);
			if (current_patch_group.name.empty())
				Console.Error(fmt::format("Malformed patch name: {}", line));

			continue;
		}

		LoadPatchLine(&current_patch_group, line);
	}

	if (!current_patch_group.name.empty() || !current_patch_group.patches.empty() || !current_patch_group.dpatches.empty())
		add_current_patch();

	return static_cast<u32>(patch_list->size() - before);
}

bool Patch::OpenPatchesZip()
{
	if (s_patches_zip)
		return true;

	const std::string filename = Path::Combine(EmuFolders::Resources, PATCHES_ZIP_NAME);

	zip_error ze = {};
	zip_source_t* zs = zip_source_file_create(filename.c_str(), 0, 0, &ze);
	if (zs && !(s_patches_zip = zip_open_from_source(zs, ZIP_RDONLY, &ze)))
	{
		static bool warning_shown = false;
		if (!warning_shown)
		{
			Host::AddIconOSDMessage("PatchesZipOpenWarning", ICON_FA_BANDAGE,
				fmt::format(TRANSLATE_FS("Patch", "Failed to open {}. Built-in game patches are not available."),
					PATCHES_ZIP_NAME),
				Host::OSD_ERROR_DURATION);
			warning_shown = true;
		}

		// have to clean up source
		Console.Error("Failed to open %s: %s", filename.c_str(), zip_error_strerror(&ze));
		zip_source_free(zs);
		return false;
	}

	std::atexit([]() { zip_close(s_patches_zip); });
	return true;
}

std::string Patch::GetPnachTemplate(const std::string_view serial, u32 crc, bool include_serial, bool add_wildcard, bool all_crcs)
{
	pxAssert(!all_crcs || (include_serial && add_wildcard));
	if (!serial.empty())
	{
		if (all_crcs)
			return fmt::format("{}_*.pnach", serial);
		else if (include_serial)
			return fmt::format("{}_{:08X}{}.pnach", serial, crc, add_wildcard ? "*" : "");
	}
	return fmt::format("{:08X}{}.pnach", crc, add_wildcard ? "*" : "");
}

std::vector<std::string> Patch::FindPatchFilesOnDisk(const std::string_view serial, u32 crc, bool cheats, bool all_crcs)
{
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(cheats ? EmuFolders::Cheats.c_str() : EmuFolders::Patches.c_str(),
		GetPnachTemplate(serial, crc, true, true, all_crcs).c_str(),
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);

	std::vector<std::string> ret;
	ret.reserve(files.size());

	for (FILESYSTEM_FIND_DATA& fd : files)
		ret.push_back(std::move(fd.FileName));

	// and patches without serials
	FileSystem::FindFiles(cheats ? EmuFolders::Cheats.c_str() : EmuFolders::Patches.c_str(),
		GetPnachTemplate(serial, crc, false, true, false).c_str(), FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES,
		&files);
	ret.reserve(ret.size() + files.size());
	for (FILESYSTEM_FIND_DATA& fd : files)
		ret.push_back(std::move(fd.FileName));

	return ret;
}

bool Patch::ContainsPatchName(const std::vector<PatchInfo>& patches, const std::string_view patchName)
{
	return std::find_if(patches.begin(), patches.end(), [&patchName](const PatchInfo& patch) {
		return patch.name == patchName;
	}) != patches.end();
}

template <typename F>
void Patch::EnumeratePnachFiles(const std::string_view serial, u32 crc, bool cheats, bool for_ui, const F& f)
{
	// Prefer files on disk over the zip.
	std::vector<std::string> disk_patch_files;
	if (for_ui || !Achievements::IsHardcoreModeActive())
		disk_patch_files = FindPatchFilesOnDisk(serial, crc, cheats, for_ui);

	bool unlabeled_patch_found = false;
	if (!disk_patch_files.empty())
	{
		for (const std::string& file : disk_patch_files)
		{
			std::optional<std::string> contents = FileSystem::ReadFileToString(file.c_str());
			if (contents.has_value())
			{
				// Catch if unlabeled patches are being loaded so we can disable ZIP patches to prevent conflicts.
				if (PatchStringHasUnlabelledPatch(contents.value()))
				{
					unlabeled_patch_found = true;
					Console.WriteLn(fmt::format("Patch: Disabling any bundled '{}' patches due to unlabeled patch being loaded. (To avoid conflicts)", PATCHES_ZIP_NAME));
				}

				f(std::move(file), std::move(contents.value()));
			}
		}
	}

	// Otherwise fall back to the zip.
	if (cheats || unlabeled_patch_found || !OpenPatchesZip())
		return;

	// Prefer filename with serial.
	std::string zip_filename = GetPnachTemplate(serial, crc, true, false, false);
	std::optional<std::string> pnach_data(ReadFileInZipToString(s_patches_zip, zip_filename.c_str()));
	if (!pnach_data.has_value())
	{
		zip_filename = GetPnachTemplate(serial, crc, false, false, false);
		pnach_data = ReadFileInZipToString(s_patches_zip, zip_filename.c_str());
	}
	if (pnach_data.has_value())
		f(std::move(zip_filename), std::move(pnach_data.value()));
}

bool Patch::PatchStringHasUnlabelledPatch(const std::string& pnach_data)
{
	std::istringstream ss(pnach_data);
	std::string line;
	bool foundPatch = false, foundLabel = false;

	while (std::getline(ss, line))
	{
		TrimPatchLine(line);
		if (line.empty())
			continue;

		if (line.length() > 2 && line.front() == '[' && line.back() == ']')
		{
			if (!foundPatch)
				return false;
			foundLabel = true;
			continue;
		}

		std::string_view key, value;
		StringUtil::ParseAssignmentString(line, &key, &value);
		if (key == "patch")
		{
			if (!foundLabel)
				return true;

			foundPatch = true;
		}
	}
	return false;
}

void Patch::ExtractPatchInfo(std::vector<PatchInfo>* dst, const std::string& pnach_data, u32* num_unlabelled_patches)
{
	std::istringstream ss(pnach_data);
	std::string line;
	PatchInfo current_patch;

	std::optional<patch_place_type> last_place;
	bool unknown_place = false;

	while (std::getline(ss, line))
	{
		TrimPatchLine(line);
		if (line.empty())
			continue;

		const bool has_patch = !current_patch.name.empty();

		if (line.length() > 2 && line.front() == '[' && line.back() == ']')
		{
			if (has_patch)
			{
				if (std::none_of(dst->begin(), dst->end(),
						[&current_patch](const PatchInfo& pi) { return (pi.name == current_patch.name); }))
				{
					// Don't show patches with duplicate names, prefer the first loaded.
					if (!ContainsPatchName(*dst, current_patch.name))
					{
						dst->push_back(std::move(current_patch));
					}
					else
					{
						Console.WriteLn(Color_Gray, fmt::format("Patch: Skipped reading patch '{}' since a patch with a duplicate name was already loaded.", current_patch.name));
					}
				}
				current_patch = {};
			}

			current_patch.name = line.substr(1, line.length() - 2);
			last_place = std::nullopt;
			unknown_place = false;
			continue;
		}

		std::string_view key, value;
		StringUtil::ParseAssignmentString(line, &key, &value);

		// Just ignore other directives, who knows what rubbish people have in here.
		// Use comment for description if it hasn't been otherwise specified.
		if (key == "author")
		{
			current_patch.author = value;
		}
		else if (key == "description")
		{
			current_patch.description = value;
		}
		else if (key == "comment" && current_patch.description.empty())
		{
			current_patch.description = value;
		}
		else if (key == "patch")
		{
			if (!has_patch && num_unlabelled_patches)
				(*num_unlabelled_patches)++;

			// Try to extract the place value of the patch lines so we can
			// display it in the GUI if they all match. TODO: Don't duplicate
			// all this parsing logic twice.
			if (unknown_place)
				continue;

			std::string::size_type comma_pos = value.find(",");
			if (comma_pos == std::string::npos)
				comma_pos = 0;
			const std::string_view padded_place = value.substr(0, comma_pos);
			const std::string_view place_string = StringUtil::StripWhitespace(padded_place);
			const std::optional<patch_place_type> place = LookupEnumName<patch_place_type>(
				place_string, s_place_to_string);
			if (!place.has_value() || (last_place.has_value() && place != last_place))
			{
				// This group contains patch lines with different or invalid
				// place values.
				current_patch.place = std::nullopt;
				unknown_place = true;
				continue;
			}

			current_patch.place = place;
			last_place = place;
		}
		else if (key == "dpatch")
		{
			current_patch.place = std::nullopt;
			unknown_place = true;
		}
	}

	// Last one.
	if (!current_patch.name.empty() && std::none_of(dst->begin(), dst->end(), [&current_patch](const PatchInfo& pi) {
			return (pi.name == current_patch.name);
		}))
	{
		dst->push_back(std::move(current_patch));
	}
}

std::string_view Patch::PatchInfo::GetNamePart() const
{
	const std::string::size_type pos = name.rfind('\\');
	std::string_view ret = name;
	if (pos != std::string::npos)
		ret = ret.substr(pos + 1);
	return ret;
}

std::string_view Patch::PatchInfo::GetNameParentPart() const
{
	const std::string::size_type pos = name.rfind('\\');
	std::string_view ret;
	if (pos != std::string::npos)
		ret = std::string_view(name).substr(0, pos);
	return ret;
}

std::vector<Patch::PatchInfo> Patch::GetPatchInfo(const std::string_view serial, u32 crc, bool cheats, bool showAllCRCS, u32* num_unlabelled_patches)
{
	std::vector<PatchInfo> ret;

	if (num_unlabelled_patches)
		*num_unlabelled_patches = 0;

	EnumeratePnachFiles(serial, crc, cheats, showAllCRCS,
		[&ret, num_unlabelled_patches](const std::string& filename, const std::string& pnach_data) {
			ExtractPatchInfo(&ret, pnach_data, num_unlabelled_patches);
		});

	return ret;
}

std::string Patch::GetPnachFilename(const std::string_view serial, u32 crc, bool cheats)
{
	return Path::Combine(cheats ? EmuFolders::Cheats : EmuFolders::Patches, GetPnachTemplate(serial, crc, true, false, false));
}

void Patch::ReloadEnabledLists()
{
	const std::vector<std::string> prev_enabled_cheats = std::move(s_enabled_cheats);
	if (EmuConfig.EnableCheats && !Achievements::IsHardcoreModeActive())
		s_enabled_cheats = Host::GetStringListSetting(CHEATS_CONFIG_SECTION, PATCH_ENABLE_CONFIG_KEY);
	else
		s_enabled_cheats = {};

	const std::vector<std::string> prev_enabled_patches = std::exchange(s_enabled_patches, Host::GetStringListSetting(PATCHES_CONFIG_SECTION, PATCH_ENABLE_CONFIG_KEY));
	const std::vector<std::string> disabled_patches = Host::GetStringListSetting(PATCHES_CONFIG_SECTION, PATCH_DISABLE_CONFIG_KEY);

	// Name based matching for widescreen/NI settings.
	if (EmuConfig.EnableWideScreenPatches)
	{
		if (std::none_of(s_enabled_patches.begin(), s_enabled_patches.end(),
				[](const std::string& it) { return (it == WS_PATCH_NAME); }))
		{
			s_enabled_patches.emplace_back(WS_PATCH_NAME);
		}
	}
	if (EmuConfig.EnableNoInterlacingPatches)
	{
		if (std::none_of(s_enabled_patches.begin(), s_enabled_patches.end(),
				[](const std::string& it) { return (it == NI_PATCH_NAME); }))
		{
			s_enabled_patches.emplace_back(NI_PATCH_NAME);
		}
	}

	for (auto it = s_enabled_patches.begin(); it != s_enabled_patches.end();)
	{
		if (std::find(disabled_patches.begin(), disabled_patches.end(), *it) != disabled_patches.end())
		{
			it = s_enabled_patches.erase(it);
		}
		else
		{
			++it;
		}
	}

	s_just_enabled_cheats.clear();
	s_just_enabled_patches.clear();
	for (const auto& p : s_enabled_cheats)
	{
		if (std::find(prev_enabled_cheats.begin(), prev_enabled_cheats.end(), p) == prev_enabled_cheats.end())
		{
			s_just_enabled_cheats.emplace_back(p);
		}
	}
	for (const auto& p : s_enabled_patches)
	{
		if (std::find(prev_enabled_patches.begin(), prev_enabled_patches.end(), p) == prev_enabled_patches.end())
		{
			s_just_enabled_patches.emplace_back(p);
		}
	}
}

u32 Patch::EnablePatches(const std::vector<PatchGroup>* patches, const std::vector<std::string>& enable_list, const std::vector<std::string>* enable_immediately_list)
{
	u32 count = 0;
	for (const PatchGroup& p : *patches)
	{
		// For compatibility, we auto enable anything that's not labelled.
		// Also for gamedb patches.
		if (!p.name.empty() && std::find(enable_list.begin(), enable_list.end(), p.name) == enable_list.end())
			continue;

		Console.WriteLn(Color_Green, fmt::format("Enabled patch: {}",
										 p.name.empty() ? std::string_view("<unknown>") : std::string_view(p.name)));

		// Note: We DO NOT insert a nullptr here to reset extended code state between groups.
		// PS2 cheat engines (and the PC emulator) concatenate all active cheats into a single list
		// without isolating them. Many cheats rely on D/E-codes in one group controlling patches in another.
		// State isolation is only performed between major categories (GameDB vs Game Patches vs Cheats).

		for (const PatchCommand& ip : p.patches)
		{
			// print the actual patch lines only in verbose mode (even in devel)
			if (Log::GetMaxLevel() >= LOGLEVEL_DEV)
				DevCon.WriteLnFmt("  {}", ip.ToString());

			s_active_patches.push_back(&ip);
		}

		for (const DynamicPatch& dp : p.dpatches)
		{
			s_active_pnach_dynamic_patches.push_back(dp);
		}

		if (p.override_aspect_ratio.has_value())
			s_override_aspect_ratio = p.override_aspect_ratio;
		if (p.override_aspect_ratio_mode.has_value())
			s_override_aspect_ratio_mode = p.override_aspect_ratio_mode;
		if (p.override_interlace_mode.has_value())
			s_override_interlace_mode = p.override_interlace_mode;

		// Count unlabelled patches once per command, or one patch per group.
		count += p.name.empty() ? (static_cast<u32>(p.patches.size()) + static_cast<u32>(p.dpatches.size())) : 1;
	}

	// Apply PPT_ON_LOAD_OR_WHEN_ENABLED patches immediately.
	if (enable_immediately_list && !enable_immediately_list->empty())
	{
		// Don't pass pointers to patch objects themselves here just in case the
		// patches are reloaded twice in a row before this event makes it.
		Host::RunOnCPUThread([patches, enable_immediately_list]() {
			for (const PatchGroup& group : *patches)
			{
				const bool apply_immediately = std::find(
												   enable_immediately_list->begin(),
												   enable_immediately_list->end(),
												   group.name) != enable_immediately_list->end();
				if (!apply_immediately)
					continue;

				EEMemoryInterface ee;
				IOPMemoryInterface iop;
				ExtendedState state;

				for (const PatchCommand& command : group.patches)
				{
					if (command.placetopatch != PPT_ON_LOAD_OR_WHEN_ENABLED)
						continue;

					ApplyPatch(&command, ee, iop, state);
				}
			}
		});
	}

	return count;
}

void Patch::ReloadPatches(const std::string& serial, u32 crc, bool reload_files, bool reload_enabled_list, bool verbose,
	bool verbose_if_changed)
{
	reload_files |= (s_patches_crc != crc);
	s_patches_crc = crc;

	if (reload_files)
	{
		s_gamedb_patches.clear();

		const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(serial);
		if (game)
		{
			const std::string* patches = game->findPatch(crc);
			if (patches)
			{
				const u32 patch_count = LoadPatchesFromString(&s_gamedb_patches, *patches);
				if (patch_count > 0)
					Console.WriteLn(Color_Green, fmt::format("Found {} game patches in GameDB.", patch_count));
			}

			LoadDynamicPatches(game->dynaPatches);
		}

		s_game_patches.clear();
		EnumeratePnachFiles(
			serial, s_patches_crc, false, false, [](const std::string& filename, const std::string& pnach_data) {
				const u32 patch_count = LoadPatchesFromString(&s_game_patches, pnach_data);
				if (patch_count > 0)
					Console.WriteLn(Color_Green, fmt::format("Found {} game patches in {}.", patch_count, filename));
			});

		s_cheat_patches.clear();
		EnumeratePnachFiles(
			serial, s_patches_crc, true, false, [](const std::string& filename, const std::string& pnach_data) {
				const u32 patch_count = LoadPatchesFromString(&s_cheat_patches, pnach_data);
				if (patch_count > 0)
					Console.WriteLn(Color_Green, fmt::format("Found {} cheats in {}.", patch_count, filename));
			});
	}

	UpdateActivePatches(reload_enabled_list, verbose, verbose_if_changed, false);
}

static bool s_lan_fair_play_disable_disk_cheats = false;

void Patch::SetFairPlayLanDisableDiskCheats(bool disable)
{
	s_lan_fair_play_disable_disk_cheats = disable;
}

bool Patch::GetFairPlayLanDisableDiskCheats()
{
	return s_lan_fair_play_disable_disk_cheats;
}

void Patch::UpdateActivePatches(bool reload_enabled_list, bool verbose, bool verbose_if_changed, bool apply_new_patches)
{
	if (reload_enabled_list)
		ReloadEnabledLists();

	const size_t prev_count = s_active_patches.size();
	s_active_patches.clear();
	s_override_aspect_ratio.reset();
	s_override_aspect_ratio_mode.reset();
	s_override_interlace_mode.reset();
	s_active_pnach_dynamic_patches.clear();

	SmallString message;
	u32 gp_count = 0;
	if (EmuConfig.EnablePatches)
	{
		gp_count = EnablePatches(&s_gamedb_patches, std::vector<std::string>(), nullptr);
		s_gamedb_counts = gp_count;
		if (gp_count > 0)
			message.append(TRANSLATE_PLURAL_STR("Patch", "%n GameDB patches are active.", "OSD Message", gp_count));
	}

	// dual-space isolation — reset D/E/F state at GameDB -> Game patches boundary
	s_active_patches.emplace_back(nullptr);

	const u32 p_count = EnablePatches(
		&s_game_patches, s_enabled_patches, apply_new_patches ? &s_just_enabled_patches : nullptr);
	s_patches_counts = p_count;
	if (p_count > 0)
		message.append_format("{}{}", message.empty() ? "" : "\n",
			TRANSLATE_PLURAL_STR("Patch", "%n game patches are active.", "OSD Message", p_count));

	// dual-space isolation — reset D/E/F state at Game patches -> Cheats boundary
	s_active_patches.emplace_back(nullptr);

	u32 c_count = 0;
	const bool cheats_disk_allowed = EmuConfig.EnableCheats && !GetFairPlayLanDisableDiskCheats();
	if (cheats_disk_allowed)
		c_count = EnablePatches(
			&s_cheat_patches, s_enabled_cheats, apply_new_patches ? &s_just_enabled_cheats : nullptr);
	s_cheats_counts = c_count;
	if (c_count > 0)
		message.append_format("{}{}", message.empty() ? "" : "\n",
			TRANSLATE_PLURAL_STR("Patch", "%n cheat patches are active.", "OSD Message", c_count));

	// Display message on first boot when we load patches.
	// Except when it's just GameDB.
	const bool just_gamedb = (p_count == 0 && c_count == 0 && gp_count > 0);
	if (verbose || (verbose_if_changed && prev_count != s_active_patches.size() && !just_gamedb))
	{
		if (!message.empty())
		{
			Host::AddIconOSDMessage("LoadPatches", ICON_FA_BANDAGE, message, Host::OSD_INFO_DURATION);
		}
		else
		{
			Host::AddIconOSDMessage("LoadPatches", ICON_FA_BANDAGE,
				TRANSLATE_SV(
					"Patch", "No cheats or patches (widescreen, compatibility or others) are found / enabled."),
				Host::OSD_INFO_DURATION);
		}
	}

	if ((!s_active_gamedb_dynamic_patches.empty() || !s_active_pnach_dynamic_patches.empty()) && Cpu)
		Cpu->Reset();
}

void Patch::ApplyPatchSettingOverrides()
{
	// Switch to the requested aspect ratio if widescreen patches are enabled, and AR is auto.
	if (EmuConfig.GS.AspectRatio == AspectRatioType::RAuto4_3_3_2)
	{
		if (s_override_aspect_ratio_mode.has_value())
		{
			EmuConfig.GS.AspectRatio = s_override_aspect_ratio_mode.value();
			EmuConfig.CurrentCustomAspectRatio = 0.0f;

			Console.WriteLn(Color_Gray, fmt::format("Patch: Setting aspect ratio mode to {} by patch request.",
									 Pcsx2Config::GSOptions::AspectRatioNames[static_cast<u8>(s_override_aspect_ratio_mode.value())]));
		}
		else if (s_override_aspect_ratio.has_value())
		{
			EmuConfig.CurrentCustomAspectRatio = s_override_aspect_ratio.value();

			Console.WriteLn(Color_Gray,
				fmt::format("Patch: Setting aspect ratio to {} by patch request.", s_override_aspect_ratio.value()));
		}
	}

	// Disable interlacing in GS if active.
	if (s_override_interlace_mode.has_value() && EmuConfig.GS.InterlaceMode == GSInterlaceMode::Automatic)
	{
		Console.WriteLn(Color_Gray, fmt::format("Patch: Setting deinterlace mode to {} by patch request.",
										static_cast<int>(s_override_interlace_mode.value())));
		EmuConfig.GS.InterlaceMode = s_override_interlace_mode.value();
	}
}

bool Patch::ReloadPatchAffectingOptions()
{
	// Restore the aspect ratio + interlacing setting the user had set before reloading the patch,
	// as the custom patch settings only apply if the "Auto" settings are selected.

	const AspectRatioType current_ar = EmuConfig.GS.AspectRatio;
	const GSInterlaceMode current_interlace = EmuConfig.GS.InterlaceMode;
	const float custom_aspect_ratio = EmuConfig.CurrentCustomAspectRatio;

	// This is pretty gross, but we're not using a config layer, so...
	AspectRatioType new_ar = Pcsx2Config::GSOptions::DEFAULT_ASPECT_RATIO;
	const std::string ar_value = Host::GetStringSettingValue("EmuCore/GS", "AspectRatio",
		Pcsx2Config::GSOptions::AspectRatioNames[static_cast<u8>(EmuConfig.GS.AspectRatio)]);
	for (u32 i = 0; i < static_cast<u32>(AspectRatioType::MaxCount); i++)
	{
		if (ar_value == Pcsx2Config::GSOptions::AspectRatioNames[i])
		{
			new_ar = static_cast<AspectRatioType>(i);
			break;
		}
	}
	EmuConfig.GS.AspectRatio = new_ar;
	EmuConfig.GS.InterlaceMode = static_cast<GSInterlaceMode>(Host::GetIntSettingValue(
		"EmuCore/GS", "deinterlace_mode", static_cast<int>(Pcsx2Config::GSOptions::DEFAULT_INTERLACE_MODE)));

	ApplyPatchSettingOverrides();

	// Return true if any config setting changed
	return current_ar != EmuConfig.GS.AspectRatio || custom_aspect_ratio != EmuConfig.CurrentCustomAspectRatio || current_interlace != EmuConfig.GS.InterlaceMode;
}

void Patch::UnloadPatches()
{
	s_override_interlace_mode = {};
	s_override_aspect_ratio = {};
	s_patches_crc = 0;
	s_active_patches = {};
	s_active_pnach_dynamic_patches = {};
	s_active_gamedb_dynamic_patches = {};
	s_enabled_patches = {};
	s_enabled_cheats = {};
	decltype(s_cheat_patches)().swap(s_cheat_patches);
	decltype(s_game_patches)().swap(s_game_patches);
	decltype(s_gamedb_patches)().swap(s_gamedb_patches);

	// PC 全量移植 §12.2：跨游戏 / 重载补丁时清除已注册的 F-gate 块与去重表，
	// 否则旧 gate 会残留在新游戏的帧推进里，导致写入错乱。
	ResetFGateState();
}

// PatchFunc Functions.
void Patch::PatchFunc::patch(PatchGroup* group, const std::string_view cmd, const std::string_view param)
{
#define PATCH_ERROR(fstring, ...) \
	Console.Error(fmt::format("(Patch) Error Parsing: {}={}: " fstring, cmd, param, __VA_ARGS__))

	// [0]=PlaceToPatch,[1]=CpuType,[2]=MemAddr,[3]=OperandSize,[4]=WriteValue
	// 当 value 形如 randint(lo,hi) 时，逗号会让 SplitString 多分一段，所以允许 6 段。
	const std::vector<std::string_view> pieces(StringUtil::SplitString(param, ',', false));
	if (pieces.size() < 5 || pieces.size() > 6)
	{
		PATCH_ERROR("Expected 5 data parameters; only found {}", pieces.size());
		return;
	}

	std::string reconstructed_value;
	std::string_view value_field = pieces[4];
	if (pieces.size() == 6)
	{
		reconstructed_value = fmt::format("{},{}", pieces[4], pieces[5]);
		value_field = reconstructed_value;
	}

	const std::optional<patch_place_type> placetopatch = LookupEnumName<patch_place_type>(pieces[0], s_place_to_string);
	const std::optional<patch_cpu_type> cpu = LookupEnumName<patch_cpu_type>(pieces[1], s_cpu_to_string);
	const std::optional<patch_data_type> type = LookupEnumName<patch_data_type>(pieces[3], s_type_to_string);

	if (!placetopatch.has_value())
	{
		PATCH_ERROR("Invalid 'place' value '{}' (0: on boot only, 1: continuously, 2: on boot and continuously, 3: on boot and when enabled in the GUI)", pieces[0]);
		return;
	}
	if (!cpu.has_value())
	{
		PATCH_ERROR("Unrecognized CPU Target: '{}'", pieces[1]);
		return;
	}
	if (!type.has_value())
	{
		PATCH_ERROR("Unrecognized Operand Size: '{}'", pieces[3]);
		return;
	}

	// ── 地址槽（pieces[2]）：可能是指针表达式或静态十六进制 ──
	std::unique_ptr<PointerChainExpr> ptr_addr_expr = ParsePointerExpr(pieces[2]);
	u32 addr_val = 0;
	if (!ptr_addr_expr)
	{
		std::string_view addr_end;
		const std::optional<u32> addr = StringUtil::FromChars<u32>(pieces[2], 16, &addr_end);
		if (!addr.has_value() || !addr_end.empty())
		{
			PATCH_ERROR("Malformed address '{}', a hex number without prefix (e.g. 0123ABCD) is expected", pieces[2]);
			return;
		}
		addr_val = addr.value();
	}

	// ── 值槽（value_field）：randint(lo,hi) / float / 指针表达式 / 静态 hex ──
	bool is_random = false;
	bool is_rand_float = false;
	u64 rand_lo = 0, rand_hi = 0;
	float rand_float_lo = 0.f, rand_float_hi = 0.f;
	std::unique_ptr<PointerChainExpr> ptr_data_expr;
	std::optional<u64> data;
	u8* data_ptr = nullptr;

	if (value_field.size() > 8 && value_field.substr(0, 8) == std::string_view("randint(") && value_field.back() == ')')
	{
		const auto inner = value_field.substr(8, value_field.size() - 9);
		const auto comma = inner.find(',');
		if (comma != std::string_view::npos)
		{
			auto lo_sv = inner.substr(0, comma);
			auto hi_sv = inner.substr(comma + 1);
			while (!lo_sv.empty() && (lo_sv.front() == ' ' || lo_sv.front() == '\t'))
				lo_sv.remove_prefix(1);
			while (!lo_sv.empty() && (lo_sv.back() == ' ' || lo_sv.back() == '\t'))
				lo_sv.remove_suffix(1);
			while (!hi_sv.empty() && (hi_sv.front() == ' ' || hi_sv.front() == '\t'))
				hi_sv.remove_prefix(1);
			while (!hi_sv.empty() && (hi_sv.back() == ' ' || hi_sv.back() == '\t'))
				hi_sv.remove_suffix(1);
			const auto lo = StringUtil::FromChars<u64>(lo_sv, 16);
			const auto hi = StringUtil::FromChars<u64>(hi_sv, 16);
			if (lo.has_value() && hi.has_value())
			{
				is_random = true;
				rand_lo = lo.value();
				rand_hi = hi.value();
				if (rand_lo > rand_hi)
					std::swap(rand_lo, rand_hi);
				data = 0;
			}
			else
			{
				PATCH_ERROR("Invalid randint hex parameters in '{}'", value_field);
				return;
			}
		}
		else
		{
			PATCH_ERROR("Invalid randint syntax '{}', expected randint(lo,hi)", value_field);
			return;
		}
	}
	else if (value_field.size() > 10 && value_field.substr(0, 10) == std::string_view("randfloat(") && value_field.back() == ')')
	{
		if (type.value() != FLOAT_T)
		{
			PATCH_ERROR("randfloat(lo,hi) requires operand type 'float', not '{}'", pieces[3]);
			return;
		}
		const auto inner = value_field.substr(10, value_field.size() - 11);
		const auto comma = inner.find(',');
		if (comma == std::string_view::npos)
		{
			PATCH_ERROR("Invalid randfloat syntax '{}', expected randfloat(lo,hi) with decimal bounds", value_field);
			return;
		}
		auto lo_sv = inner.substr(0, comma);
		auto hi_sv = inner.substr(comma + 1);
		while (!lo_sv.empty() && (lo_sv.front() == ' ' || lo_sv.front() == '\t'))
			lo_sv.remove_prefix(1);
		while (!lo_sv.empty() && (lo_sv.back() == ' ' || lo_sv.back() == '\t'))
			lo_sv.remove_suffix(1);
		while (!hi_sv.empty() && (hi_sv.front() == ' ' || hi_sv.front() == '\t'))
			hi_sv.remove_prefix(1);
		while (!hi_sv.empty() && (hi_sv.back() == ' ' || hi_sv.back() == '\t'))
			hi_sv.remove_suffix(1);
		std::string_view fend;
		const auto lo_f = StringUtil::FromChars<float>(lo_sv, &fend);
		if (!lo_f.has_value() || !fend.empty())
		{
			PATCH_ERROR("Invalid randfloat lower bound in '{}'", value_field);
			return;
		}
		const auto hi_f = StringUtil::FromChars<float>(hi_sv, &fend);
		if (!hi_f.has_value() || !fend.empty())
		{
			PATCH_ERROR("Invalid randfloat upper bound in '{}'", value_field);
			return;
		}
		float flo = lo_f.value();
		float fhi = hi_f.value();
		is_rand_float = true;
		rand_float_lo = flo;
		rand_float_hi = fhi;
		if (rand_float_lo > rand_float_hi)
			std::swap(rand_float_lo, rand_float_hi);
		data = 0;
	}
	else
	{
		ptr_data_expr = ParsePointerExpr(value_field);
		if (ptr_data_expr && ptr_data_expr->has_arith_offset)
		{
			// 优化.md §12.3：值链算术偏移
			// 仅 FLOAT_T 接受小数偏移；BYTES_T 不接受任何 (chain)+N（写入是变长字节数组，
			// "加一个数"没有明确含义）；其它整型 (BYTE/SHORT/WORD/DOUBLE/EXTENDED/...) 仅接受整数偏移。
			if (type.value() == BYTES_T)
			{
				PATCH_ERROR("Pointer-chain arithmetic offset '(chain)+N' is not supported for type 'bytes' in '{}'",
					value_field);
				return;
			}
			if (ptr_data_expr->arith_is_decimal && type.value() != FLOAT_T)
			{
				PATCH_ERROR("Pointer-chain arithmetic offset '(chain)+N.M' with decimal point requires "
					"operand type 'float', not '{}' (offset='{}')",
					pieces[3], ptr_data_expr->arith_offset_float);
				return;
			}
		}
	}

	if (!is_random && !is_rand_float && !ptr_data_expr)
	{
		if (type.value() == FLOAT_T)
		{
			std::string_view float_end;
			const auto fval = StringUtil::FromChars<float>(value_field, &float_end);
			if (!fval.has_value() || !float_end.empty())
			{
				PATCH_ERROR("Malformed float value '{}', a decimal number (e.g. 1.5) is expected", value_field);
				return;
			}
			float f = fval.value();
			u32 bits;
			std::memcpy(&bits, &f, sizeof(bits));
			data = bits;
		}
		else if (type.value() != BYTES_T)
		{
			// 支持负十六进制：-1 → 0xFFFF...FF
			std::string_view data_str = value_field;
			bool is_negative = false;
			if (!data_str.empty() && data_str[0] == '-')
			{
				is_negative = true;
				data_str = data_str.substr(1);
			}

			std::string_view data_end;
			data = StringUtil::FromChars<u64>(data_str, 16, &data_end);
			if (!data.has_value() || !data_end.empty())
			{
				PATCH_ERROR("Malformed data '{}', a hex number without prefix (e.g. 0123ABCD or -1) is expected", value_field);
				return;
			}
			if (is_negative)
				data = static_cast<u64>(-static_cast<s64>(data.value()));
		}
		else
		{
			std::optional<std::vector<u8>> bytes = StringUtil::DecodeHex(value_field);
			if (!bytes.has_value() || bytes->empty())
			{
				PATCH_ERROR("Malformed data '{}', a hex string without prefix (e.g. 0123ABCD) is expected", value_field);
				return;
			}
			data = bytes->size();
			data_ptr = static_cast<u8*>(std::malloc(bytes->size()));
			std::memcpy(data_ptr, bytes->data(), bytes->size());
		}
	}
	else if (!is_random && !is_rand_float)
	{
		// 指针表达式作为数据槽 — 占位 0，apply 时再解析
		data = 0;
	}

	PatchCommand iPatch;
	iPatch.placetopatch = placetopatch.value();
	iPatch.cpu = cpu.value();
	iPatch.addr = addr_val;
	iPatch.type = type.value();
	iPatch.data = data.value();
	iPatch.data_ptr = data_ptr;
	iPatch.ptr_addr = ptr_addr_expr.release();
	iPatch.ptr_data = ptr_data_expr.release();
	iPatch.is_random = is_random;
	iPatch.rand_lo = rand_lo;
	iPatch.rand_hi = rand_hi;
	iPatch.is_rand_float = is_rand_float;
	iPatch.rand_float_lo = rand_float_lo;
	iPatch.rand_float_hi = rand_float_hi;
	group->patches.push_back(std::move(iPatch));

#undef PATCH_ERROR
}

void Patch::PatchFunc::gsaspectratio(PatchGroup* group, const std::string_view cmd, const std::string_view param)
{
	std::string str(param);
	std::istringstream ss(str);
	uint dividend, divisor;
	char delimiter;
	float aspect_ratio = 0.f;

	ss >> dividend >> delimiter >> divisor;
	if (!ss.fail() && delimiter == ':' && divisor != 0)
	{
		aspect_ratio = static_cast<float>(dividend) / static_cast<float>(divisor);
	}

	if (aspect_ratio > 0.f)
	{
		group->override_aspect_ratio = aspect_ratio;
		return;
	}

	for (u32 i = 0; i < static_cast<u32>(AspectRatioType::MaxCount); i++)
	{
		const char* const name = Pcsx2Config::GSOptions::AspectRatioNames[i];
		if (name && StringUtil::compareNoCase(param, name))
		{
			group->override_aspect_ratio_mode = static_cast<AspectRatioType>(i);
			return;
		}
	}

	Console.Error(fmt::format("Patch error: {} is an unknown aspect ratio.", param));
}

void Patch::PatchFunc::gsinterlacemode(PatchGroup* group, const std::string_view cmd, const std::string_view param)
{
	const std::optional<int> interlace_mode = StringUtil::FromChars<int>(param);
	if (!interlace_mode.has_value() || interlace_mode.value() < 0 ||
		interlace_mode.value() >= static_cast<int>(GSInterlaceMode::Count))
	{
		Console.Error(fmt::format("Patch error: {} is an unknown interlace mode.", param));
		return;
	}

	group->override_interlace_mode = static_cast<GSInterlaceMode>(interlace_mode.value());
}

void Patch::PatchFunc::dpatch(PatchGroup* group, const std::string_view cmd, const std::string_view param)
{
#define PATCH_ERROR(fstring, ...) \
	Console.Error(fmt::format("(dPatch) Error Parsing: {}={}: " fstring, cmd, param, __VA_ARGS__))

	// [0]=version/type,[1]=number of patterns,[2]=number of replacements
	// Each pattern or replacement is [3]=offset,[4]=hex

	const std::vector<std::string_view> pieces(StringUtil::SplitString(param, ',', false));
	if (pieces.size() < 3)
	{
		PATCH_ERROR("Expected at least 3 data parameters; only found {}", pieces.size());
		return;
	}


	std::string_view patterns_end, replacements_end;

	// Implemented for possible future use so we don't have to break backcompat
	std::optional<u32> dpatch_type = StringUtil::FromChars<u32>(pieces[0]);

	std::optional<u32> num_patterns = StringUtil::FromChars<u32>(pieces[1], 16, &patterns_end);
	std::optional<u32> num_replacements = StringUtil::FromChars<u32>(pieces[2], 16, &replacements_end);

	if (!dpatch_type.has_value())
	{
		PATCH_ERROR("Malformed version/type '{}', a decimal number(e.g. 0,1,2) is expected", pieces[0]);
		return;
	}

	if (dpatch_type.value() != 0)
	{
		PATCH_ERROR("Unsupported version/type '{}', only 0 is currently supported", pieces[0]);
		return;
	}

	if (!num_patterns.has_value())
	{
		PATCH_ERROR("Malformed number of patterns '{}', a decimal number is expected", pieces[1]);
		return;
	}

	if (!num_replacements.has_value())
	{
		PATCH_ERROR("Malformed number of replacements '{}', a decimal number is expected", pieces[2]);
		return;
	}

	if (pieces.size() != ((num_patterns.value() * 2) + (num_replacements.value() * 2) + 3))
	{
		PATCH_ERROR("Expected 2 fields for each {} patterns and {} replacements; found {}", num_patterns.value(), num_replacements.value(), pieces.size() - 2);
		return;
	}

	DynamicPatch dpatch;
	for (u32 i = 0; i < num_patterns.value(); i++)
	{
		std::optional<u32> offset = StringUtil::FromChars<u32>(pieces[3 + (i * 2)], 16);
		std::optional<u32> value = StringUtil::FromChars<u32>(pieces[4 + (i * 2)], 16);
		if (!offset.has_value())
		{
			PATCH_ERROR("Malformed offset '{}', a hex number without prefix (e.g. 0123ABCD) is expected", pieces[3 + (i * 2)]);
			return;
		}
		if (!value.has_value())
		{
			PATCH_ERROR("Malformed value '{}', a hex number without prefix (e.g. 0123ABCD) is expected", pieces[4 + (i * 2)]);
			return;
		}

		DynamicPatchEntry pattern;
		pattern.offset = offset.value();
		pattern.value = value.value();

		dpatch.pattern.push_back(pattern);
	}

	for (u32 i = 0; i < num_replacements.value(); i++)
	{
		std::optional<u32> offset = StringUtil::FromChars<u32>(pieces[3 + (num_patterns.value() * 2) + (i * 2)], 16);
		std::optional<u32> value = StringUtil::FromChars<u32>(pieces[4 + (num_patterns.value() * 2) + (i * 2)], 16);
		if (!offset.has_value())
		{
			PATCH_ERROR("Malformed offset '{}', a hex number without prefix (e.g. 0123ABCD) is expected", pieces[3 + (num_patterns.value() * 2) + (i * 2)]);
			return;
		}
		if (!value.has_value())
		{
			PATCH_ERROR("Malformed value '{}', a hex number without prefix (e.g. 0123ABCD) is expected", pieces[4 + (num_patterns.value() * 2) + (i * 2)]);
			return;
		}

		DynamicPatchEntry replacement;
		replacement.offset = offset.value();
		replacement.value = value.value();

		dpatch.replacement.push_back(replacement);
	}

	group->dpatches.push_back(dpatch);
}

void Patch::ApplyBootPatches()
{
	EEMemoryInterface ee;
	IOPMemoryInterface iop;
	ApplyPatches(s_active_patches, PPT_ONCE_ON_LOAD, ee, iop);
	ApplyPatches(s_active_patches, PPT_COMBINED_0_1, ee, iop);
	ApplyPatches(s_active_patches, PPT_ON_LOAD_OR_WHEN_ENABLED, ee, iop);
}

void Patch::ApplyVsyncPatches()
{
	EEMemoryInterface ee;
	IOPMemoryInterface iop;
	ApplyPatches(s_active_patches, PPT_CONTINUOUSLY, ee, iop);
	ApplyPatches(s_active_patches, PPT_COMBINED_0_1, ee, iop);

	// PC 全量移植 §12.2：在每帧补丁应用之后，统一推进 F-gate 状态机。
	ApplyFGateUpdates();
}

// PC 全量移植 §12.2：F-gate 每帧推进 / 守门打开时调度被守护补丁
void Patch::ApplyFGateUpdates()
{
	if (FGate::s_fgate_blocks.empty())
		return;

	EEMemoryInterface ee;
	IOPMemoryInterface iop;
	ExtendedState gated_state;
	for (auto& block : FGate::s_fgate_blocks)
	{
		if (!block.valid || !block.has_guarded)
			continue;

		FGate::UpdateSlotStreak(block.open_slot, ee);
		if (!block.open_slot.latched && block.open_slot.streak >= block.open_slot.hold_frames &&
			block.open_slot.hold_frames > 0)
		{
			block.open_slot.latched = true;
			block.close_slot.streak = 0;
		}

		FGate::UpdateSlotStreak(block.close_slot, ee);
		if (block.open_slot.latched && block.close_slot.streak >= block.close_slot.hold_frames &&
			block.close_slot.hold_frames > 0)
		{
			block.open_slot.latched = false;
			block.open_slot.streak = 0;
		}

		if (block.open_slot.latched)
		{
			// 守护行是独立补丁，不应受 D/E 残留 SkipCount/PrevCheatType 影响
			gated_state = {};
			ApplyPatch(&block.guarded_cmd, ee, iop, gated_state);
		}
	}
}

void Patch::ResetFGateState()
{
	FGate::Reset();
}

void Patch::ApplyPatches(
	const std::vector<const PatchCommand*>& patches,
	patch_place_type place,
	EEMemoryInterface& ee,
	IOPMemoryInterface& iop)
{
	ExtendedState state;

	for (const PatchCommand* patch : patches)
	{
		if (!patch)
		{
			state = {};
			continue;
		}

		if (patch->placetopatch != place)
			continue;

		ApplyPatch(patch, ee, iop, state);
	}
}

void Patch::ApplyPatches(
	const std::vector<const PatchCommand*>& patches,
	patch_place_type place,
	MemoryInterface& ee,
	MemoryInterface& iop)
{
	ExtendedState state;

	for (const PatchCommand* patch : patches)
	{
		if (!patch)
		{
			state = {};
			continue;
		}

		if (patch->placetopatch != place)
			continue;

		ApplyPatch(patch, ee, iop, state);
	}
}

u32 Patch::GetActiveGameDBPatchesCount()
{
	return s_gamedb_counts;
}

u32 Patch::GetActivePatchesCount()
{
	return s_patches_counts;
}

u32 Patch::GetActiveCheatsCount()
{
	return s_cheats_counts;
}

u32 Patch::GetAllActivePatchesCount()
{
	return s_gamedb_counts + s_patches_counts + s_cheats_counts;
}

bool Patch::IsGloballyToggleablePatch(const PatchInfo& patch_info)
{
	return patch_info.name == WS_PATCH_NAME || patch_info.name == NI_PATCH_NAME;
}

void Patch::ApplyDynamicPatches(u32 pc)
{
	for (const auto& dynpatch : s_active_pnach_dynamic_patches)
		ApplyDynaPatch(dynpatch, pc);
	for (const auto& dynpatch : s_active_gamedb_dynamic_patches)
		ApplyDynaPatch(dynpatch, pc);
}

void Patch::LoadDynamicPatches(const std::vector<DynamicPatch>& patches)
{
	for (const DynamicPatch& it : patches)
		s_active_gamedb_dynamic_patches.push_back(it);
}

template <typename Memory>
	requires std::is_base_of_v<MemoryInterface, Memory>
void Patch::writeCheat(Memory& memory, ExtendedState& state)
{
	switch (state.last_type)
	{
		case 0x0:
			memory.IdempotentWrite8(state.prev_cheat_addr, state.iteration_increment & 0xFF);
			break;
		case 0x1:
			memory.IdempotentWrite16(state.prev_cheat_addr, state.iteration_increment & 0xFFFF);
			break;
		case 0x2:
			memory.IdempotentWrite32(state.prev_cheat_addr, state.iteration_increment);
			break;
		default:
			break;
	}
}

template <typename Memory>
	requires std::is_base_of_v<MemoryInterface, Memory>
void Patch::handle_extended_t(const PatchCommand* p, Memory& memory, ExtendedState& state)
{
	switch (state.prev_cheat_type)
		{
			case 0x3040: // vvvvvvvv 00000000 Inc
			{
				u32 mem = memory.Read32(state.prev_cheat_addr);
				memory.Write32(state.prev_cheat_addr, mem + (p->addr));
				state.prev_cheat_type = 0;
				break;
			}

			case 0x3050: // vvvvvvvv 00000000 Dec
			{
				u32 mem = memory.Read32(state.prev_cheat_addr);
				memory.Write32(state.prev_cheat_addr, mem - (p->addr));
				state.prev_cheat_type = 0;
				break;
			}

			case 0x4000: // vvvvvvvv iiiiiiii
				for (u32 i = 0; i < state.iteration_count; i++)
				{
					memory.IdempotentWrite32((u32)(state.prev_cheat_addr + (i * state.iteration_increment)), (u32)(p->addr + ((u32)p->data * i)));
				}
				state.prev_cheat_type = 0;
				break;

			case 0x5000: // bbbbbbbb 00000000
				for (u32 i = 0; i < state.iteration_count; i++)
				{
					u8 mem = memory.Read8(state.prev_cheat_addr + i);
					memory.IdempotentWrite8((p->addr + i) & 0x0FFFFFFF, mem);
				}
				state.prev_cheat_type = 0;
				break;

			case 0x6000: // 000Xnnnn iiiiiiii
			{
				state.null_pointer_encountered = false;

				// Get Number of pointers
				if (((u32)p->addr & 0x0000FFFF) == 0)
					state.iteration_count = 1;
				else
					state.iteration_count = (u32)p->addr & 0x0000FFFF;

				// Read first pointer
				state.last_type = ((u32)p->addr & 0x000F0000) >> 16;
				u32 mem = memory.Read32(state.prev_cheat_addr);
				if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) == 0)
					state.null_pointer_encountered = true;

				state.prev_cheat_addr = mem + (u32)p->data;
				state.iteration_count--;

				// Check if needed to read another pointer
				if (state.iteration_count == 0)
				{
					state.prev_cheat_type = 0;
					if (!state.null_pointer_encountered)
						writeCheat(memory, state);
				}
				else
				{
					state.prev_cheat_type = 0x6001;
				}
			}
			break;

			case 0x6001: // 000Xnnnn iiiiiiii
			{
				// Read first pointer
				u32 mem = 0;
				if (!state.null_pointer_encountered)
				{
					mem = memory.Read32(state.prev_cheat_addr & 0x0FFFFFFF);
					if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) == 0)
						state.null_pointer_encountered = true;
				}

				state.prev_cheat_addr = mem + (u32)p->addr;
				state.iteration_count--;

				// Check if needed to read another pointer
				if (state.iteration_count == 0)
				{
					state.prev_cheat_type = 0;
					if (!state.null_pointer_encountered)
						writeCheat(memory, state);
				}
				else
				{
					if (!state.null_pointer_encountered)
					{
						mem = memory.Read32(state.prev_cheat_addr);
						if (((mem & 0x0FFFFFFF) & 0x3FFFFFFC) == 0)
							state.null_pointer_encountered = true;
					}
					else
					{
						mem = 0;
					}

					state.prev_cheat_addr = mem + (u32)p->data;
					state.iteration_count--;
					if (state.iteration_count == 0)
					{
						state.prev_cheat_type = 0;
						if (!state.null_pointer_encountered)
							writeCheat(memory, state);
					}
				}
			}
			break;

			default:
				if ((p->addr & 0xF0000000) == 0x00000000) // 0aaaaaaa 0000000vv
				{
					memory.IdempotentWrite8(p->addr & 0x0FFFFFFF, (u8)p->data & 0x000000FF);
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xF0000000) == 0x10000000) // 1aaaaaaa 0000vvvv
				{
					memory.IdempotentWrite16(p->addr & 0x0FFFFFFF, (u16)p->data & 0x0000FFFF);
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xF0000000) == 0x20000000) // 2aaaaaaa vvvvvvvv
				{
					memory.IdempotentWrite32(p->addr & 0x0FFFFFFF, (u32)p->data);
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xFFFF0000) == 0x30000000) // 300000vv 0aaaaaaa Inc
				{
					u8 mem = memory.Read8((u32)p->data);
					memory.Write8((u32)p->data, mem + (p->addr & 0x000000FF));
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xFFFF0000) == 0x30100000) // 301000vv 0aaaaaaa Dec
				{
					u8 mem = memory.Read8((u32)p->data);
					memory.Write8((u32)p->data, mem - (p->addr & 0x000000FF));
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xFFFF0000) == 0x30200000) // 3020vvvv 0aaaaaaa Inc
				{
					u16 mem = memory.Read16((u32)p->data);
					memory.Write16((u32)p->data, mem + (p->addr & 0x0000FFFF));
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xFFFF0000) == 0x30300000) // 3030vvvv 0aaaaaaa Dec
				{
					u16 mem = memory.Read16((u32)p->data);
					memory.Write16((u32)p->data, mem - (p->addr & 0x0000FFFF));
					state.prev_cheat_type = 0;
				}
				else if ((p->addr & 0xFFFF0000) == 0x30400000) // 30400000 0aaaaaaa Inc + Another line
				{
					state.prev_cheat_type = 0x3040;
					state.prev_cheat_addr = (u32)p->data;
				}
				else if ((p->addr & 0xFFFF0000) == 0x30500000) // 30500000 0aaaaaaa Inc + Another line
				{
					state.prev_cheat_type = 0x3050;
					state.prev_cheat_addr = (u32)p->data;
				}
				else if ((p->addr & 0xF0000000) == 0x40000000) // 4aaaaaaa nnnnssss + Another line
				{
					state.iteration_count = ((u32)p->data & 0xFFFF0000) >> 16;
					state.iteration_increment = ((u32)p->data & 0x0000FFFF) * 4;
					state.prev_cheat_addr = (u32)p->addr & 0x0FFFFFFF;
					state.prev_cheat_type = 0x4000;
				}
				else if ((p->addr & 0xF0000000) == 0x50000000) // 5sssssss nnnnnnnn + Another line
				{
					state.prev_cheat_addr = (u32)p->addr & 0x0FFFFFFF;
					state.iteration_count = ((u32)p->data);
					state.prev_cheat_type = 0x5000;
				}
				else if ((p->addr & 0xF0000000) == 0x60000000) // 6aaaaaaa 000000vv + Another line/s
				{
					state.prev_cheat_addr = (u32)p->addr & 0x0FFFFFFF;
					state.iteration_increment = ((u32)p->data);
					state.iteration_count = 0;
					state.prev_cheat_type = 0x6000;
				}
				else if ((p->addr & 0xF0000000) == 0x70000000)
				{
					if ((p->data & 0x00F00000) == 0x00000000) // 7aaaaaaa 000000vv
					{
						u8 mem = memory.Read8((u32)p->addr & 0x0FFFFFFF);
						memory.Write8((u32)p->addr & 0x0FFFFFFF, (u8)(mem | (p->data & 0x000000FF)));
					}
					else if ((p->data & 0x00F00000) == 0x00100000) // 7aaaaaaa 0010vvvv
					{
						u16 mem = memory.Read16((u32)p->addr & 0x0FFFFFFF);
						memory.Write16((u32)p->addr & 0x0FFFFFFF, (u16)(mem | (p->data & 0x0000FFFF)));
					}
					else if ((p->data & 0x00F00000) == 0x00200000) // 7aaaaaaa 002000vv
					{
						u8 mem = memory.Read8((u32)p->addr & 0x0FFFFFFF);
						memory.Write8((u32)p->addr & 0x0FFFFFFF, (u8)(mem & (p->data & 0x000000FF)));
					}
					else if ((p->data & 0x00F00000) == 0x00300000) // 7aaaaaaa 0030vvvv
					{
						u16 mem = memory.Read16((u32)p->addr & 0x0FFFFFFF);
						memory.Write16((u32)p->addr & 0x0FFFFFFF, (u16)(mem & (p->data & 0x0000FFFF)));
					}
					else if ((p->data & 0x00F00000) == 0x00400000) // 7aaaaaaa 004000vv
					{
						u8 mem = memory.Read8((u32)p->addr & 0x0FFFFFFF);
						memory.Write8((u32)p->addr & 0x0FFFFFFF, (u8)(mem ^ (p->data & 0x000000FF)));
					}
					else if ((p->data & 0x00F00000) == 0x00500000) // 7aaaaaaa 0050vvvv
					{
						u16 mem = memory.Read16((u32)p->addr & 0x0FFFFFFF);
						memory.Write16((u32)p->addr & 0x0FFFFFFF, (u16)(mem ^ (p->data & 0x0000FFFF)));
					}
				}
				else if ((p->addr & 0xF0000000) == 0xD0000000 || (p->addr & 0xF0000000) == 0xE0000000)
				{
					u32 addr = (u32)p->addr;
					u32 data = (u32)p->data;

					// Since D-codes now have the additional functionality present in PS2rd which
					// incorporates E-code-like functionality by making use of the unused bits in
					// D-codes, the E-codes are now just converted to D-codes to reduce bloat.

					if ((addr & 0xF0000000) == 0xE0000000)
					{
						// Ezyyvvvv taaaaaaa  ->  Daaaaaaa yytzvvvv
						addr = 0xD0000000 | ((u32)p->data & 0x0FFFFFFF);
						data = 0x00000000 | ((u32)p->addr & 0x0000FFFF);
						data = data | ((u32)p->addr & 0x00FF0000) << 8;
						data = data | ((u32)p->addr & 0x0F000000) >> 8;
						data = data | ((u32)p->data & 0xF0000000) >> 8;
					}

					const u8 type = (data & 0x000F0000) >> 16;
					const u8 cond = (data & 0x00F00000) >> 20;

					if (cond == 0) // Daaaaaaa yy0zvvvv
					{
						if (type == 0) // Daaaaaaa yy00vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (mem != (data & 0x0000FFFF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy0100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (mem != (data & 0x000000FF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 1) // Daaaaaaa yy1zvvvv
					{
						if (type == 0) // Daaaaaaa yy10vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (mem == (data & 0x0000FFFF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy1100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (mem == (data & 0x000000FF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 2) // Daaaaaaa yy2zvvvv
					{
						if (type == 0) // Daaaaaaa yy20vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (mem >= (data & 0x0000FFFF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy2100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (mem >= (data & 0x000000FF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 3) // Daaaaaaa yy3zvvvv
					{
						if (type == 0) // Daaaaaaa yy30vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (mem <= (data & 0x0000FFFF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy3100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (mem <= (data & 0x000000FF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 4) // Daaaaaaa yy4zvvvv
					{
						if (type == 0) // Daaaaaaa yy40vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (mem & (data & 0x0000FFFF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy4100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (mem & (data & 0x000000FF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 5) // Daaaaaaa yy5zvvvv
					{
						if (type == 0) // Daaaaaaa yy50vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (!(mem & (data & 0x0000FFFF)))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy5100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (!(mem & (data & 0x000000FF)))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 6) // Daaaaaaa yy6zvvvv
					{
						if (type == 0) // Daaaaaaa yy60vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (mem | (data & 0x0000FFFF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy6100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (mem | (data & 0x000000FF))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
					else if (cond == 7) // Daaaaaaa yy7zvvvv
					{
						if (type == 0) // Daaaaaaa yy70vvvv
						{
							u16 mem = memory.Read16(addr & 0x0FFFFFFF);
							if (!(mem | (data & 0x0000FFFF)))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
						else if (type == 1) // Daaaaaaa yy7100vv
						{
							u8 mem = memory.Read8(addr & 0x0FFFFFFF);
							if (!(mem | (data & 0x000000FF)))
							{
								state.skip_count = (data & 0xFF000000) >> 24;
								if (!state.skip_count)
								{
									state.skip_count = 1;
								}
							}
							state.prev_cheat_type = 0;
						}
					}
				}
				else if ((p->addr & 0xF0000000) == 0xF0000000)
				{
					// ── §12.2: F-gate accumulation ─────────────────────────
					// 把 F-block 的前两行（open / close 条件）累加到 state.fgate_accum_block。
					// 第三行（被守护的补丁）由 ApplyPatch 的入口在解析指针链 *之前* 捕获，
					// 这样才能保留原始的 ptr_addr/ptr_data，让守门打开时能完整执行。
					const u32 f_watch_addr = (u32)p->addr & 0x0FFFFFFF;
					const std::string config_str = fmt::format("{:X}", (u64)p->data);

					// Format A 需要 8 位 hex；不足则补前导 0
					std::string padded_config;
					if (config_str.size() < 8 && config_str.find('(') == std::string::npos)
						padded_config = std::string(8 - config_str.size(), '0') + config_str;
					else
						padded_config = config_str;

					if (state.fgate_accum_phase == 0)
					{
						state.fgate_accum_block = std::make_unique<FGate::Block>();
						state.fgate_accum_block->open_slot.watch_addr = f_watch_addr;
						if (FGate::ParseConfig(padded_config, state.fgate_accum_block->open_slot))
							state.fgate_accum_phase = 1;
						else
						{
							state.fgate_accum_block.reset();
							state.fgate_accum_phase = 0;
						}
					}
					else if (state.fgate_accum_phase == 1)
					{
						state.fgate_accum_block->close_slot.watch_addr = f_watch_addr;
						if (FGate::ParseConfig(padded_config, state.fgate_accum_block->close_slot))
							state.fgate_accum_phase = 2;
						else
						{
							state.fgate_accum_block.reset();
							state.fgate_accum_phase = 0;
						}
					}

					state.prev_cheat_type = 0;
				}
		}
}

template <typename EEMemory, typename IOPMemory>
	requires std::is_base_of_v<MemoryInterface, EEMemory> &&
             std::is_base_of_v<MemoryInterface, IOPMemory>
void Patch::ApplyPatch(const PatchCommand* p, EEMemory& ee, IOPMemory& iop, ExtendedState& state)
{
	// §12.2：F-gate 第三行捕获。必须在指针链解析 *之前*，否则 ptr_addr/ptr_data 会丢。
	if (state.fgate_accum_phase == 2 && p->type == EXTENDED_T)
	{
		// 真正的 0xFXXXXXXX 仍然是 F-code，不能当作守护行；只有非 F-code 的 EXTENDED_T 行
		// 才视为 guarded。带指针链的地址（addr 是占位 0）也认为不是 F-code。
		bool is_f_code = false;
		if (!p->ptr_addr)
			is_f_code = (p->addr & 0xF0000000) == 0xF0000000;

		if (!is_f_code && state.fgate_accum_block)
		{
			const u64 dedup_key = FGate::MakeBlockKey(
				state.fgate_accum_block->open_slot.watch_addr,
				state.fgate_accum_block->close_slot.watch_addr);

			if (FGate::s_fgate_registered_keys.count(dedup_key) == 0)
			{
				FGate::Block& blk = *state.fgate_accum_block;
				// 深拷贝 PatchCommand POD 字段（不接管 data_ptr / ptr_*）
				std::memcpy(static_cast<void*>(&blk.guarded_cmd), p, sizeof(PatchCommand));
				blk.guarded_cmd.data_ptr = nullptr;
				blk.guarded_cmd.ptr_addr = nullptr;
				blk.guarded_cmd.ptr_data = nullptr;
				if (p->ptr_addr)
					blk.guarded_cmd.ptr_addr = new PointerChainExpr(*p->ptr_addr);
				if (p->ptr_data)
					blk.guarded_cmd.ptr_data = new PointerChainExpr(*p->ptr_data);
				blk.has_guarded = true;
				blk.valid = true;
				FGate::s_fgate_blocks.push_back(std::move(blk));
				FGate::s_fgate_registered_keys.insert(dedup_key);
			}
			state.fgate_accum_block.reset();
			state.fgate_accum_phase = 0;
			// 守护行不当帧应用，等 gate 打开时由 ApplyFGateUpdates 调度
			return;
		}
	}

	// D/E-code 的 SkipCount 在指针链解析 *之前* 统一消耗，避免提前 return（指针链失败）
	// 让 SkipCount 残留泄露到下一行。
	if (state.skip_count > 0)
	{
		state.skip_count--;
		return;
	}

	// ── 解析指针链（地址 / 值） + randint ────────────────────────────────
	u32 effective_addr = p->addr;
	u64 effective_data = p->data;

	MemoryInterface& chain_mem = (p->cpu == CPU_IOP) ? static_cast<MemoryInterface&>(iop)
	                                                : static_cast<MemoryInterface&>(ee);

	if (p->ptr_addr)
	{
		const auto resolved = ResolvePointerChain(*p->ptr_addr, chain_mem);
		if (!resolved.has_value())
			return;
		effective_addr = resolved.value();
	}
	if (p->ptr_data)
	{
		if (p->ptr_data->is_value_chain)
		{
			const u32 raw = ResolveValuePointerChain(*p->ptr_data, chain_mem);
			if (p->ptr_data->has_arith_offset)
			{
				// 优化.md §12.3：值链常量算术偏移。
				// FLOAT_T  → 把 raw 当 IEEE-754 32 位 float，加上 arith_offset_float，再转回 bits。
				// 其它整型 → 64 位补码加，截断到目标位宽由后续 case 分支负责。
				// EXTENDED_T 也走整数加，因为 handle_extended_t 期望的就是 32 位 data。
				if (p->type == FLOAT_T)
				{
					float fv;
					std::memcpy(&fv, &raw, sizeof(fv));
					fv = fv + static_cast<float>(p->ptr_data->arith_offset_float);
					u32 bits;
					std::memcpy(&bits, &fv, sizeof(bits));
					effective_data = bits;
				}
				else
				{
					const int64_t signed_raw = static_cast<int64_t>(static_cast<int32_t>(raw));
					effective_data = static_cast<u64>(signed_raw + p->ptr_data->arith_offset_int);
				}
			}
			else
			{
				effective_data = raw;
			}
		}
		else
		{
			const auto resolved = ResolvePointerChain(*p->ptr_data, chain_mem);
			if (!resolved.has_value())
				return;
			effective_data = resolved.value();
		}
	}

	if (p->is_rand_float || p->is_random)
	{
		if (p->is_rand_float)
		{
			static thread_local std::mt19937_64 s_rng(std::random_device{}());
			int lo_t = static_cast<int>(std::lround(static_cast<double>(p->rand_float_lo) * 10.0));
			int hi_t = static_cast<int>(std::lround(static_cast<double>(p->rand_float_hi) * 10.0));
			if (lo_t > hi_t)
				std::swap(lo_t, hi_t);
			std::uniform_int_distribution<int> distf(lo_t, hi_t);
			const float f = static_cast<float>(distf(s_rng)) * 0.1f;
			u32 bits;
			std::memcpy(&bits, &f, sizeof(bits));
			effective_data = bits;
		}
		else
		{
			static thread_local std::mt19937_64 s_rng(std::random_device{}());
			std::uniform_int_distribution<u64> dist(p->rand_lo, p->rand_hi);
			effective_data = dist(s_rng);
		}
	}

	// 若任一字段被替换过，构造一个临时 PatchCommand 再分发；data_ptr 是借用，临时拷贝
	// 不能在销毁时释放它（PatchCommand 析构会 free）。所以构造后必须把 data_ptr 设为
	// nullptr 再让其析构。
	PatchCommand resolved_cmd;
	const PatchCommand* pp = p;
	if (p->ptr_addr || p->ptr_data || p->is_random || p->is_rand_float)
	{
		resolved_cmd.placetopatch = p->placetopatch;
		resolved_cmd.cpu = p->cpu;
		resolved_cmd.type = p->type;
		resolved_cmd.addr = effective_addr;
		resolved_cmd.data = effective_data;
		resolved_cmd.data_ptr = p->data_ptr; // 借用，析构前置 nullptr
		resolved_cmd.ptr_addr = nullptr;
		resolved_cmd.ptr_data = nullptr;
		pp = &resolved_cmd;
	}

	switch (pp->cpu)
	{
		case CPU_EE:
		{
			switch (pp->type)
			{
				case BYTE_T:
				{
					ee.IdempotentWrite8(pp->addr, static_cast<u8>(pp->data));
					break;
				}
				case SHORT_T:
				{
					ee.IdempotentWrite16(pp->addr, static_cast<u16>(pp->data));
					break;
				}
				case WORD_T:
				case FLOAT_T: // PC 全量移植：float 字面量在解析阶段已转 32-bit IEEE-754 hex
				{
					ee.IdempotentWrite32(pp->addr, static_cast<u32>(pp->data));
					break;
				}
				case DOUBLE_T:
				{
					ee.IdempotentWrite64(pp->addr, pp->data);
					break;
				}
				case EXTENDED_T:
				{
					handle_extended_t(pp, ee, state);
					break;
				}
				case SHORT_BE_T:
				{
					u16 value = ByteSwap(static_cast<u16>(pp->data));
					ee.IdempotentWrite16(pp->addr, value);
					break;
				}
				case WORD_BE_T:
				{
					u32 value = ByteSwap(static_cast<u32>(pp->data));
					ee.IdempotentWrite32(pp->addr, value);
					break;
				}
				case DOUBLE_BE_T:
				{
					u64 value = ByteSwap(pp->data);
					ee.IdempotentWrite64(pp->addr, value);
					break;
				}
				case BYTES_T:
				{
					ee.IdempotentWriteBytes(pp->addr, pp->data_ptr, static_cast<u32>(pp->data));
					break;
				}
				case SWAP_T:
				{
					// PC 全量移植：交换两地址处的 32 位值。addr=A，data=B（均已被指针链解析）。
					// 在 0x4000（4-code 循环）下还要按迭代步进同时对两侧偏移做交换。
					const u32 addr_a = pp->addr;
					const u32 addr_b = static_cast<u32>(pp->data);
					if (state.prev_cheat_type == 0x4000)
					{
						for (u32 i = 0; i < state.iteration_count; i++)
						{
							const u32 a = addr_a + (i * state.iteration_increment);
							const u32 b = addr_b + (i * state.iteration_increment);
							const u32 va = ee.Read32(a);
							const u32 vb = ee.Read32(b);
							if (va != vb)
							{
								ee.Write32(a, vb);
								ee.Write32(b, va);
							}
						}
						state.prev_cheat_type = 0;
					}
					else
					{
						const u32 va = ee.Read32(addr_a);
						const u32 vb = ee.Read32(addr_b);
						if (va != vb)
						{
							ee.Write32(addr_a, vb);
							ee.Write32(addr_b, va);
						}
					}
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		case CPU_IOP:
		{
			switch (pp->type)
			{
				case BYTE_T:
				{
					iop.IdempotentWrite8(pp->addr, static_cast<u8>(pp->data));
					break;
				}
				case SHORT_T:
				{
					iop.IdempotentWrite16(pp->addr, static_cast<u16>(pp->data));
					break;
				}
				case WORD_T:
				case FLOAT_T:
				{
					iop.IdempotentWrite32(pp->addr, static_cast<u32>(pp->data));
					break;
				}
				case BYTES_T:
				{
					iop.IdempotentWriteBytes(pp->addr, pp->data_ptr, static_cast<u32>(pp->data));
					break;
				}
				case SWAP_T:
				{
					const u32 addr_a = pp->addr;
					const u32 addr_b = static_cast<u32>(pp->data);
					if (state.prev_cheat_type == 0x4000)
					{
						for (u32 i = 0; i < state.iteration_count; i++)
						{
							const u32 a = addr_a + (i * state.iteration_increment);
							const u32 b = addr_b + (i * state.iteration_increment);
							const u32 va = iop.Read32(a);
							const u32 vb = iop.Read32(b);
							if (va != vb)
							{
								iop.Write32(a, vb);
								iop.Write32(b, va);
							}
						}
						state.prev_cheat_type = 0;
					}
					else
					{
						const u32 va = iop.Read32(addr_a);
						const u32 vb = iop.Read32(addr_b);
						if (va != vb)
						{
							iop.Write32(addr_a, vb);
							iop.Write32(addr_b, va);
						}
					}
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		default:
		{
			break;
		}
	}

	// resolved_cmd 借用了 p->data_ptr，析构前置 nullptr，避免 ~PatchCommand 调用 free
	if (pp == &resolved_cmd)
		resolved_cmd.data_ptr = nullptr;
}

void Patch::ApplyDynaPatch(const DynamicPatch& patch, u32 address)
{
	for (const auto& pattern : patch.pattern)
	{
		if (*static_cast<u32*>(PSM(address + pattern.offset)) != pattern.value)
			return;
	}

	Console.WriteLn("Applying Dynamic Patch to address 0x%08X", address);
	// If everything passes, apply the patch.
	for (const auto& replacement : patch.replacement)
	{
		memWrite32(address + replacement.offset, replacement.value);
	}
}

const char* Patch::PlaceToString(std::optional<patch_place_type> place)
{
	if (!place.has_value())
		//: Time when a patch is applied.
		return TRANSLATE("Patch", "Unknown");

	switch (*place)
	{
		case Patch::PPT_ONCE_ON_LOAD:
			//: Time when a patch is applied.
			return TRANSLATE("Patch", "Only On Startup");
		case Patch::PPT_CONTINUOUSLY:
			//: Time when a patch is applied.
			return TRANSLATE("Patch", "Every Frame");
		case Patch::PPT_COMBINED_0_1:
			//: Time when a patch is applied.
			return TRANSLATE("Patch", "On Startup & Every Frame");
		case Patch::PPT_ON_LOAD_OR_WHEN_ENABLED:
			//: Time when a patch is applied.
			return TRANSLATE("Patch", "On Startup & When Enabled");
		default:
		{
		}
	}

	return "";
}
