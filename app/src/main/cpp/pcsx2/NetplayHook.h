// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>

#include "common/Pcsx2Defs.h"

namespace NetplayHook
{
	using VSyncCallback = void (*)();
	/// LAN Boot Netplay: invoked once per frame until cleared; used to unblock shoryu Host/Join
	/// after the VM is running (equivalent to legacy IOP HandleIO first-poll notify).
	using LanBootVsyncCallback = void (*)();

	void SetVSyncCallback(VSyncCallback cb);
	VSyncCallback GetVSyncCallback();

	void SetLanBootVsyncCallback(LanBootVsyncCallback cb);
	LanBootVsyncCallback GetLanBootVsyncCallback();

	/// After pad input is polled each CPU frame; drives LAN netplay NextFrame (see legacy IOP cadence).
	using LanCpuFrameCallback = void (*)();
	void SetLanCpuFrameCallback(LanCpuFrameCallback cb);
	LanCpuFrameCallback GetLanCpuFrameCallback();

	/// 与 NetplayQuickJoinDialog::startFrameSync 一致：仅通过 VSync 回调读手柄并写回；LanCpuFrameCallback 置空。
	/// exclusive_pad_slot：可选，在 PollSources 之后清零另一侧 unified 槽（见 VMManager::PollInputOnCPUThread）；Quick Join 与局域网联机帧同步默认均传 nullopt。
	void ApplyFrameSyncInputHooks(VSyncCallback cb, std::optional<u32> exclusive_pad_slot = std::nullopt);

	/// 可选：本机只允许某一 unified 槽位（0=1P / 1=2P）保留 PollSources 后的物理输入，另一侧在 PollInputOnCPUThread 内被清零。nullopt=不限制。
	void SetLanExclusiveLocalPadSlot(std::optional<u32> unified_slot);
	std::optional<u32> GetLanExclusiveLocalPadSlot();

	void SetActive(bool active);
	bool IsActive();

	void SetPadOverride(bool enabled);
	bool IsPadOverrideActive();

	/// 帧同步物理输入中间缓冲区（对标 ARMSX2-online）。产品底线：
	/// 联机开启后，**本机所有物理输入通道（InputManager 键盘手柄 / UI 宏键）**
	/// 都只写到这里（SetPhysicalInput），不再直接落到 s_controllers[] 的 pad 0/1；
	/// 真正写 pad 的只有 NetplayFrameSyncClient::OnVSync —— 它每帧先 Clear 两侧 pad，
	/// 再把本机 local 写到独占槽、远端 remote 写到非独占槽。
	/// 这样 pad 状态在每个 VSync 边界是完全确定的，彻底消除"pad 0/1 被异步 SetControllerState
	/// 污染"引发的画面漂移 / 单边越权输入。
	static constexpr int MAX_PAD_INPUTS = 32;
	void SetPhysicalInput(u32 key, u8 raw_value);
	u8 GetPhysicalInput(u32 key);
	void ClearPhysicalInputs();

	void SetVerifier(bool v);
	bool IsVerifier();

	// Save-state blocking for anti-cheat during netplay.
	// Returns true when netplay is active AND no internal bypass is in effect.
	bool ShouldBlockUserSaveStateOperations();

	void PushInternalSaveStateBypass();
	void PopInternalSaveStateBypass();

	// RAII guard — push on construction, pop on destruction.
	// Only used around netplay's own save/load paths (sync archive).
	struct InternalSaveStateBypassScope
	{
		InternalSaveStateBypassScope() { PushInternalSaveStateBypass(); }
		~InternalSaveStateBypassScope() { PopInternalSaveStateBypass(); }
		InternalSaveStateBypassScope(const InternalSaveStateBypassScope&) = delete;
		InternalSaveStateBypassScope& operator=(const InternalSaveStateBypassScope&) = delete;
	};
} // namespace NetplayHook
