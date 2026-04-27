// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Android 适配版：去掉 Qt 依赖，用 std::string 替代 QString。
// 与 PC 版 NetplayLanSettings.h 语义一致。

#pragma once

#include <string>
#include <cstdint>

enum NetplayLanMode : int
{
    ConnectMode,
    HostMode,
    ObserveMode
};

struct NetplayLanSettings
{
    bool IsEnabled = false;
    /// true：仅大厅握手（无 VM），双方进房后再选 ISO 并启动；false：已进入游戏阶段 shoryu+输入同步
    bool LobbyPhaseOnly = false;
    /// 大厅已点过 Start、VM 已起后第二次 Host：对话框已是 PhaseReady，不能再 WaitForConfirmation
    bool SkipHostWaitAfterLobby = false;

    std::string Username;
    NetplayLanMode Mode = ConnectMode;
    unsigned int HostPort = 7500;
    std::string HostAddress;
    unsigned int ListenPort = 7500;
    bool SaveReplay = false;
    bool ReadonlyMemcard = false;
    bool ClientOnlyDelay = true;
    bool MemcardSync = true;
    unsigned int NumPlayers = 2;

    void SanityCheck()
    {
        if (HostPort > 65535 || HostPort < 1)
            HostPort = 7500;
        if (ListenPort > 65535 || ListenPort < 1)
            ListenPort = 7500;
        // 局域网联机固定 2 人（1P 房主 / 2P 成员），与 Quick Join 一致
        NumPlayers = 2;
    }
};

// Global LAN netplay settings instance
extern NetplayLanSettings g_LanNetplaySettings;
