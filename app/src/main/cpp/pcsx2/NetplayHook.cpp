// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayHook.h"

static NetplayHook::VSyncCallback s_netplay_vsync_cb = nullptr;
static NetplayHook::LanBootVsyncCallback s_lan_boot_vsync_cb = nullptr;
static NetplayHook::LanCpuFrameCallback s_lan_cpu_frame_cb = nullptr;
static bool s_netplay_active = false;
static bool s_netplay_pad_override = false;
static bool s_netplay_verifier = false;
static int s_savestate_bypass_depth = 0;
static std::optional<u32> s_lan_exclusive_local_pad_slot;
static u8 s_physical_input[NetplayHook::MAX_PAD_INPUTS] = {};

void NetplayHook::SetVSyncCallback(VSyncCallback cb)
{
	s_netplay_vsync_cb = cb;
}

NetplayHook::VSyncCallback NetplayHook::GetVSyncCallback()
{
	return s_netplay_vsync_cb;
}

void NetplayHook::SetLanBootVsyncCallback(LanBootVsyncCallback cb)
{
	s_lan_boot_vsync_cb = cb;
}

NetplayHook::LanBootVsyncCallback NetplayHook::GetLanBootVsyncCallback()
{
	return s_lan_boot_vsync_cb;
}

void NetplayHook::SetLanCpuFrameCallback(LanCpuFrameCallback cb)
{
	s_lan_cpu_frame_cb = cb;
}

NetplayHook::LanCpuFrameCallback NetplayHook::GetLanCpuFrameCallback()
{
	return s_lan_cpu_frame_cb;
}

void NetplayHook::ApplyFrameSyncInputHooks(VSyncCallback cb, std::optional<u32> exclusive_pad_slot)
{
	SetLanExclusiveLocalPadSlot(exclusive_pad_slot);
	SetVSyncCallback(cb);
	SetLanCpuFrameCallback(nullptr);
	SetActive(true);
}

void NetplayHook::SetLanExclusiveLocalPadSlot(std::optional<u32> unified_slot)
{
	s_lan_exclusive_local_pad_slot = unified_slot;
}

std::optional<u32> NetplayHook::GetLanExclusiveLocalPadSlot()
{
	return s_lan_exclusive_local_pad_slot;
}

void NetplayHook::SetActive(bool active)
{
	s_netplay_active = active;
}

bool NetplayHook::IsActive()
{
	return s_netplay_active;
}

void NetplayHook::SetPadOverride(bool enabled)
{
	s_netplay_pad_override = enabled;
}

bool NetplayHook::IsPadOverrideActive()
{
	return s_netplay_pad_override;
}

void NetplayHook::SetPhysicalInput(u32 key, u8 raw_value)
{
	if (key < MAX_PAD_INPUTS)
		s_physical_input[key] = raw_value;
}

u8 NetplayHook::GetPhysicalInput(u32 key)
{
	if (key < MAX_PAD_INPUTS)
		return s_physical_input[key];
	return 0;
}

void NetplayHook::ClearPhysicalInputs()
{
	for (int i = 0; i < MAX_PAD_INPUTS; i++)
		s_physical_input[i] = 0;
}

void NetplayHook::SetVerifier(bool v)
{
	s_netplay_verifier = v;
}

bool NetplayHook::IsVerifier()
{
	return s_netplay_verifier;
}

bool NetplayHook::ShouldBlockUserSaveStateOperations()
{
	return s_netplay_active && s_savestate_bypass_depth == 0;
}

void NetplayHook::PushInternalSaveStateBypass()
{
	++s_savestate_bypass_depth;
}

void NetplayHook::PopInternalSaveStateBypass()
{
	if (s_savestate_bypass_depth > 0)
		--s_savestate_bypass_depth;
}
