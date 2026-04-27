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
    int protocol_version = 0x0200;
    int sync_delay = 0;

    std::string v2_base_url;
    std::string rest_api_base_url;

    bool is_verifier = false;
    std::string host_player_id;
};

extern NetplayRoomState g_NetplayRoomState;
