// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Android 适配版：去掉 Qt 依赖，用 std::string 替代 QString。
// 与 PC 版 NetplayRoomState.h 字段语义完全一致（协议不可分叉）。

#pragma once

#include <string>
#include <cstdint>

struct NetplayRoomState
{
    std::string room_id;
    std::string player_id;
    bool is_host = false;
    std::string session_token;

    std::string udp_server_addr;
    unsigned short udp_server_port = 0;
    uint64_t initial_frame_id = 0;
    uint64_t frame_interval_us = 16667;
    int frame_cache_size = 100;
    // 与 PC 端 NetplayFrameSyncClient 的 wire 版本对齐（0x0200）。
    // §A 的 cheat RNG 0x000A 是新增 msgType，wire 格式不变；
    // PC 那侧的 `if (msgType != 0x0001) return;` 会自然忽略，安卓↔安卓两端的 0x000A
    // handler 仍可享受 RNG 同步——即「同 wire / 能力软降级」。详见 优化.md §B.7。
    int protocol_version = 0x0200;
    int sync_delay = 0;

    std::string v2_base_url;
    std::string rest_api_base_url;

    bool is_verifier = false;
    std::string host_player_id;
};

extern NetplayRoomState g_NetplayRoomState;
