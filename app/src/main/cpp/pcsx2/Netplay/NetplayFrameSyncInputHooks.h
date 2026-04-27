// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Quick Join 与局域网联机帧同步共用的输入钩子（NetplayQuickJoinDialog::startFrameSync 的单一实现源）。

#pragma once

#include <cstdint>
#include <optional>

/// 与 NetplayQuickJoinDialog::startFrameSync 相同：NetplayHook::ApplyFrameSyncInputHooks，仅 VSync→g_NetplayFrameSync->OnVSync。
/// \param exclusive_local_pad_slot  当联机中需要强制约束"本端只能驱动某一侧 pad"时传入：
///        房主=0 (1P)，成员=1 (2P)。PollInputOnCPUThread 会在每帧 PollSources 之后把另一侧的
///        pad 强制清零，从协议上封掉"本端误写对端 pad → 帧同步 read pad 读到脏数据"的通路。
///        Android 侧进一步在 JNI setPadButton / setPadParams 里做 0→slot 的重定向，保证
///        UI 上的虚拟按键无论房主/成员都落到正确一侧。
void NetplayFrameSyncRegisterQuickJoinStyleInputHooks(std::optional<uint32_t> exclusive_local_pad_slot = std::nullopt);

/// 撤销商述钩子（VSync/LanBoot/LanCpu/Active/exclusive），不断开帧同步客户端本身。
void NetplayFrameSyncUnregisterQuickJoinStyleInputHooks();
