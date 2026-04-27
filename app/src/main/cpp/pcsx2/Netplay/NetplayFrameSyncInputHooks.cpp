// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay/NetplayFrameSyncInputHooks.h"

#include <optional>

#include "Netplay/NetplayFrameSyncClient.h"
#include "pcsx2/NetplayHook.h"

static void NetplayFrameSyncQuickJoinStyleVSyncCallback()
{
	if (g_NetplayFrameSync)
		g_NetplayFrameSync->OnVSync();
}

void NetplayFrameSyncRegisterQuickJoinStyleInputHooks(std::optional<uint32_t> exclusive_local_pad_slot)
{
	NetplayHook::ApplyFrameSyncInputHooks(&NetplayFrameSyncQuickJoinStyleVSyncCallback, exclusive_local_pad_slot);
}

void NetplayFrameSyncUnregisterQuickJoinStyleInputHooks()
{
	NetplayHook::SetVSyncCallback(nullptr);
	NetplayHook::SetLanBootVsyncCallback(nullptr);
	NetplayHook::SetLanCpuFrameCallback(nullptr);
	NetplayHook::SetLanExclusiveLocalPadSlot(std::nullopt);
	NetplayHook::SetActive(false);
}
