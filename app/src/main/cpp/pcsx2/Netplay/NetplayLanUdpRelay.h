// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Netplay/NetplayUdpSocket.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// 纯 LAN 补充层（pcsx2-online 工程内无对应类）：云端 UDP 服务负责合并广播时，客户端只发 0x0001；
/// 无服务端时由房主本类在 udp_server_port 上收包并按 NetplayFrameSyncClient 已实现的**同一** 0x0002 格式回送。
/// 帧内字段、CRC、player_id 规则仍以 pcsx2-online 的 NetplayFrameSync 为准，本类仅做传输层中继。
class NetplayLanUdpRelay
{
public:
	NetplayLanUdpRelay();
	~NetplayLanUdpRelay();

	bool Start(unsigned short port, int protocol_version, std::string room_id);
	void Stop();

	bool IsRunning() const { return m_running.load(); }
	static void InjectLoopbackPacket(const uint8_t* data, int len);

private:
	struct InjectedPacket
	{
		std::vector<uint8_t> data;
	};

	void ThreadMain();

	std::atomic<bool> m_running{false};
	std::thread m_thread;
	std::mutex m_injected_mutex;
	std::vector<InjectedPacket> m_injected_packets;
	unsigned short m_port = 0;
	int m_protocol_version = 0x0200;
	std::string m_room_id = "LAN";
	np_socket_t m_sock = NP_INVALID_SOCKET;
};
