// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// LAN 房主 UDP 中继：非 pcsx2-online 客户端逻辑的一部分；广播包构造须与 NetplayFrameSyncClient::ParseBroadcastPacket 一致。

#include "NetplayLanUdpRelay.h"

#include "Netplay/NetplayFrameSyncClient.h"
#include "common/Console.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::mutex s_active_relay_mutex;
	NetplayLanUdpRelay* s_active_relay = nullptr;

	uint64_t NowMs()
	{
		return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
	}

	uint32_t CalcCrc32(const uint8_t* data, size_t len)
	{
		uint32_t crc = 0xFFFFFFFFu;
		for (size_t i = 0; i < len; i++)
		{
			crc ^= data[i];
			for (int j = 0; j < 8; j++)
			{
				uint32_t mask = -(crc & 1u);
				crc = (crc >> 1) ^ (0xEDB88320u & mask);
			}
		}
		return ~crc;
	}

	struct InputSample
	{
		uint16_t index = 0;
		uint8_t is_pressed = 0;
		uint8_t range_value = 0;
	};

	struct PeerAddr
	{
		std::string ip;
		unsigned short port = 0;
	};

	bool ParseInputPayload(const std::vector<uint8_t>& pl, uint64_t& frame_id, std::vector<InputSample>& out_samples)
	{
		if (pl.size() < 1 + 8 + 8)
			return false;
		int p = 0;
		frame_id = 0;
		for (int i = 0; i < 8; i++)
			frame_id = (frame_id << 8) | pl[p++];
		const uint8_t count = pl[p++];
		out_samples.clear();
		for (uint8_t i = 0; i < count; i++)
		{
			if (p + 6 > (int)pl.size())
				return false;
			uint16_t idx = (uint16_t)((pl[p] << 8) | pl[p + 1]);
			p += 2;
			InputSample s;
			s.index = idx;
			s.is_pressed = pl[p++];
			s.range_value = pl[p++];
			p += 2;
			out_samples.push_back(s);
		}
		if (p + 8 > (int)pl.size())
			return false;
		return true;
	}

	bool ParseOuter(const uint8_t* d, int len, uint16_t& msgType, uint16_t& protocol_ver, std::string& room,
		std::string& player, std::vector<uint8_t>& inner, uint32_t& crc_out)
	{
		if (len < 16)
			return false;
		int pos = 0;
		const uint32_t magic = (static_cast<uint32_t>(d[pos]) << 24) | (static_cast<uint32_t>(d[pos + 1]) << 16) |
			(static_cast<uint32_t>(d[pos + 2]) << 8) | static_cast<uint32_t>(d[pos + 3]);
		pos += 4;
		if (magic != 0x50435358u)
			return false;
		protocol_ver = static_cast<uint16_t>((d[pos] << 8) | d[pos + 1]);
		pos += 2;
		msgType = static_cast<uint16_t>((d[pos] << 8) | d[pos + 1]);
		pos += 2;
		const uint16_t roomLen = static_cast<uint16_t>((d[pos] << 8) | d[pos + 1]);
		pos += 2;
		const uint16_t playerLen = static_cast<uint16_t>((d[pos] << 8) | d[pos + 1]);
		pos += 2;
		crc_out = static_cast<uint32_t>((static_cast<uint32_t>(d[pos]) << 24) | (static_cast<uint32_t>(d[pos + 1]) << 16) |
			(static_cast<uint32_t>(d[pos + 2]) << 8) | static_cast<uint32_t>(d[pos + 3]));
		pos += 4;
		if (pos + roomLen + playerLen > len)
			return false;
		room.assign(reinterpret_cast<const char*>(d + pos), roomLen);
		pos += roomLen;
		player.assign(reinterpret_cast<const char*>(d + pos), playerLen);
		pos += playerLen;
		if (pos > len)
			return false;
		inner.assign(d + pos, d + len);
		return true;
	}

	void Put16(std::vector<uint8_t>& b, uint16_t v)
	{
		b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
		b.push_back(static_cast<uint8_t>(v & 0xFF));
	}

	void Put32(std::vector<uint8_t>& b, uint32_t v)
	{
		b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
		b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
		b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
		b.push_back(static_cast<uint8_t>(v & 0xFF));
	}

	void Put64(std::vector<uint8_t>& b, uint64_t v)
	{
		for (int i = 7; i >= 0; i--)
			b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
	}

	void PadPlayerId32(std::vector<uint8_t>& out, const std::string& pid)
	{
		const size_t n = std::min<size_t>(pid.size(), 32u);
		for (size_t i = 0; i < n; i++)
			out.push_back(static_cast<uint8_t>(pid[i]));
		for (size_t i = n; i < 32u; i++)
			out.push_back(0);
	}

	bool BuildBroadcastInner(const std::string& room, uint64_t frame_id, uint64_t server_ts,
		const std::map<std::string, std::vector<InputSample>>& by_player, std::vector<uint8_t>& out_inner)
	{
		out_inner.clear();
		Put16(out_inner, static_cast<uint16_t>(room.size()));
		out_inner.insert(out_inner.end(), room.begin(), room.end());
		Put64(out_inner, frame_id);
		const uint8_t groups = static_cast<uint8_t>(by_player.size());
		out_inner.push_back(groups);
		Put64(out_inner, server_ts);

		std::vector<std::string> pids;
		pids.reserve(by_player.size());
		for (const auto& kv : by_player)
			pids.push_back(kv.first);
		std::sort(pids.begin(), pids.end());

		for (const std::string& pid : pids)
		{
			const auto it = by_player.find(pid);
			if (it == by_player.end())
				return false;
			Put16(out_inner, 32);
			PadPlayerId32(out_inner, pid);
			const auto& samples = it->second;
			out_inner.push_back(static_cast<uint8_t>(samples.size()));
			for (const InputSample& s : samples)
			{
				Put16(out_inner, s.index);
				out_inner.push_back(s.is_pressed);
				out_inner.push_back(s.range_value);
				out_inner.push_back(0);
				out_inner.push_back(0);
			}
		}
		return true;
	}

	bool BuildFullPacket(uint16_t msg_type, int protocol_version, const std::string& room_id,
		const std::vector<uint8_t>& inner_payload, std::vector<uint8_t>& out)
	{
		out.clear();
		Put32(out, 0x50435358u);
		Put16(out, static_cast<uint16_t>(protocol_version));
		Put16(out, msg_type);
		Put16(out, static_cast<uint16_t>(room_id.size()));
		Put16(out, 0);
		Put32(out, CalcCrc32(inner_payload.data(), inner_payload.size()));
		out.insert(out.end(), room_id.begin(), room_id.end());
		out.insert(out.end(), inner_payload.begin(), inner_payload.end());
		return true;
	}
} // namespace

NetplayLanUdpRelay::NetplayLanUdpRelay() = default;

NetplayLanUdpRelay::~NetplayLanUdpRelay()
{
	Stop();
}

bool NetplayLanUdpRelay::Start(unsigned short port, int protocol_version, std::string room_id)
{
	Stop();
	if (port == 0)
		return false;

	m_port = port;
	m_protocol_version = protocol_version;
	m_room_id = std::move(room_id);
	if (m_room_id.empty())
		m_room_id = "LAN";

	if (!NetplayUdpSocket::Init())
		return false;

	m_sock = NetplayUdpSocket::Open(m_port, true);
	if (m_sock == NP_INVALID_SOCKET)
	{
		Console.Error("NETPLAY LAN: UDP relay failed to bind port %u (in use or permission).", static_cast<unsigned>(m_port));
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(s_active_relay_mutex);
		s_active_relay = this;
	}
	m_running.store(true);
	m_thread = std::thread([this]() { ThreadMain(); });
	Console.WriteLn("NETPLAY LAN: UDP relay listening on port %u (merges inputs -> broadcast).", static_cast<unsigned>(m_port));
	return true;
}

void NetplayLanUdpRelay::Stop()
{
	m_running.store(false);
	{
		std::lock_guard<std::mutex> lock(m_injected_mutex);
		m_injected_packets.clear();
	}
	if (m_thread.joinable())
		m_thread.join();
	if (m_sock != NP_INVALID_SOCKET)
	{
		NetplayUdpSocket::Close(m_sock);
		m_sock = NP_INVALID_SOCKET;
	}
	{
		std::lock_guard<std::mutex> lock(s_active_relay_mutex);
		if (s_active_relay == this)
			s_active_relay = nullptr;
	}
}

void NetplayLanUdpRelay::InjectLoopbackPacket(const uint8_t* data, int len)
{
	if (!data || len <= 0)
		return;

	std::lock_guard<std::mutex> active_lock(s_active_relay_mutex);
	if (!s_active_relay || !s_active_relay->m_running.load())
		return;

	InjectedPacket packet;
	packet.data.assign(data, data + len);
	{
		std::lock_guard<std::mutex> lock(s_active_relay->m_injected_mutex);
		s_active_relay->m_injected_packets.push_back(std::move(packet));
	}
}

void NetplayLanUdpRelay::ThreadMain()
{
	std::vector<uint8_t> buf(65536);
	std::mutex pending_mu;
	std::map<uint64_t, std::map<std::string, std::vector<InputSample>>> pending;
	std::map<std::string, PeerAddr> peers;

	// 诊断：帮助用户快速判断 UDP 能否真的到达 Android 房主的 relay。
	// 若安卓房主 +PC 成员情形下 relay 一直收不到 PC 的 0x0001（防火墙 / 子网不同 / AP 隔离），
	// 安卓端 OnVSync 会永远卡在 WaitForFrame，画面冻结在黑屏。
	const uint64_t relay_start_ms = NowMs();
	bool any_packet_seen = false;
	bool remote_peer_seen = false;
	bool reachability_warn_emitted = false;
	bool version_mismatch_logged = false;
	uint64_t broadcast_count = 0;

	// 优化.md §B.9 跨端 FrameSync 黑屏排查诊断：
	// 当前已知 PC↔Android 双方都同时停在 OnVSync WaitForFrame(0)、relay 看见对端 IP 但
	// 永远不广播合并帧。我们必须暴露 relay 内部状态，否则只看 "saw first REMOTE peer" 这条
	// 单行日志，无法区分以下三种致命场景：
	//   (i)   远端只发了 0x0003 心跳，0x0001 输入帧根本没进 relay（NAT/防火墙/丢包/反向 socket 不可达）
	//   (ii)  远端 0x0001 进了 relay 但和本端 frame_id=0 错位（state.initial_frame_id 不对齐）
	//   (iii) 双端 0x0001 都到 pending[0] 了，但 BuildBroadcastInner / BuildFullPacket 失败
	// 三种情况调试方向完全不同，仅靠现有日志会陷入猜测。
	std::map<std::pair<std::string, uint16_t>, uint32_t> first_seen_msgtype; // (sender_ip:port, msgType) -> hits
	std::map<std::string, uint32_t> rx_msgtype_count; // "0x0001" / "0x0003" / "other" -> count
	std::map<uint64_t, bool> pending_logged; // frame_id -> 已经记录过 "missing" 警告
	uint64_t last_summary_ms = NowMs();
	uint32_t input_frames_received = 0;

	auto process_packet = [&](const uint8_t* data, int n, const std::string& sender_ip, unsigned short sender_port) {
		if (!any_packet_seen)
		{
			any_packet_seen = true;
			Console.WriteLn(
				"NETPLAY LAN: UDP relay received FIRST packet from %s:%u (%d bytes).",
				sender_ip.c_str(), static_cast<unsigned>(sender_port), n);
		}
		if (!remote_peer_seen && sender_ip != "127.0.0.1" && sender_ip != "::1")
		{
			remote_peer_seen = true;
			Console.WriteLn(
				"NETPLAY LAN: UDP relay saw first REMOTE peer %s:%u — LAN UDP path is OK.",
				sender_ip.c_str(), static_cast<unsigned>(sender_port));
		}

		uint16_t msgType = 0;
		uint16_t proto = 0;
		std::string room;
		std::string player;
		std::vector<uint8_t> inner;
		uint32_t crc = 0;
		if (!ParseOuter(data, n, msgType, proto, room, player, inner, crc))
			return;
		if (proto != static_cast<uint16_t>(m_protocol_version))
		{
			// 优化.md §B.7：不要静默丢——这是历史上「双方黑屏，relay 看似收到包却
			// 永远不广播合并帧」的标志。只在第一次发生时打印（防 spam，每秒数十包）。
			if (!version_mismatch_logged)
			{
				version_mismatch_logged = true;
				Console.Warning(
					"NETPLAY LAN: UDP relay DROPPING packet from %s:%u: peer protocol_version=0x%04X "
					"but local relay expects 0x%04X. Both sides must run matching builds; bumping only "
					"one side will cause symmetrical lockstep timeout.",
					sender_ip.c_str(), static_cast<unsigned>(sender_port),
					static_cast<unsigned>(proto), static_cast<unsigned>(m_protocol_version));
			}
			return;
		}
		if (!room.empty() && room != m_room_id)
			return;

		if (CalcCrc32(inner.data(), inner.size()) != crc)
			return;

		// 优化.md §B.9：到这里说明 magic/proto/CRC/room 全部通过。打印每对
		//   (sender_ip:port, msgType)
		// 的首包，便于看到 "PC 收到 Android 0x0001 但完全收不到自己 0x0001" 这类不对称
		// 路径问题。也按 msgType 累计计数，5 秒打一次摘要。
		const std::string sender_key = sender_ip + ":" + std::to_string(sender_port);
		auto fs_key = std::make_pair(sender_key, msgType);
		auto fs_it = first_seen_msgtype.find(fs_key);
		if (fs_it == first_seen_msgtype.end())
		{
			first_seen_msgtype[fs_key] = 1;
			Console.WriteLn(
				"NETPLAY LAN: UDP relay first-seen msgType=0x%04X from %s player='%s' room='%s' (n=%d).",
				static_cast<unsigned>(msgType), sender_key.c_str(), player.c_str(), room.c_str(), n);
		}
		else
		{
			fs_it->second++;
		}
		switch (msgType)
		{
			case 0x0001: rx_msgtype_count["0x0001"]++; break;
			case 0x0003: rx_msgtype_count["0x0003"]++; break;
			case 0x000A: rx_msgtype_count["0x000A"]++; break;
			default: rx_msgtype_count["other"]++; break;
		}

		// 优化.md §A 已废弃：联机金手指随机值同步方案不再启用，因此 relay 不再转发
		// 0x000A 类型的包。任何非 0x0001（input frame）类型在这里直接 return，与之前
		// 0x0003 心跳一样不参与合并广播。0x000A 若仍出现在网络上（旧版 client），会被
		// 当作 "other" 计数后丢弃。
		if (msgType != 0x0001)
			return;

		if (player.empty())
			return;

		uint64_t frame_id = 0;
		std::vector<InputSample> samples;
		if (!ParseInputPayload(inner, frame_id, samples))
			return;

		PeerAddr addr;
		addr.ip = sender_ip;
		addr.port = sender_port;

		std::lock_guard<std::mutex> lk(pending_mu);
		peers[player] = addr;
		pending[frame_id][player] = std::move(samples);
		++input_frames_received;

		auto& fm = pending[frame_id];
		if (fm.size() < 2)
		{
			// 优化.md §B.9：仅对前 5 帧的"只有一个玩家在 pending、对端迟到"事件打首次警告。
			// 这正是黑屏卡死的真正信号——如果 frame_id=0 一直只看到 H:xxx 而看不到 G:xxx，
			// 说明 guest 的 0x0001 根本没进 relay（即便心跳通了也不算数）。
			if (frame_id < 5 && pending_logged.find(frame_id) == pending_logged.end())
			{
				pending_logged[frame_id] = true;
				std::string have;
				for (const auto& kv : fm)
				{
					if (!have.empty())
						have += ",";
					have += kv.first;
				}
				std::string known_peers;
				for (const auto& kv : peers)
				{
					if (!known_peers.empty())
						known_peers += ",";
					known_peers += kv.first;
				}
				Console.WriteLn(
					"NETPLAY LAN: UDP relay pending[frame=%llu] only %u of >=2 inputs ready: have=[%s] known_peers=[%s] "
					"(等不到对端的 0x0001 input frame；若长时间停在这条说明对端只发心跳/输入帧丢失)。",
					static_cast<unsigned long long>(frame_id), static_cast<unsigned>(fm.size()),
					have.c_str(), known_peers.c_str());
			}
			return;
		}

		std::vector<uint8_t> inner_bc;
		if (!BuildBroadcastInner(m_room_id, frame_id, NowMs(), fm, inner_bc))
			return;

		std::vector<uint8_t> packet;
		if (!BuildFullPacket(0x0002, m_protocol_version, m_room_id, inner_bc, packet))
			return;

		if (g_NetplayFrameSync && g_NetplayFrameSync->IsHost())
		{
			NetplayFrameData fd;
			fd.frame_id = frame_id;
			fd.server_timestamp_ms = NowMs();
			for (const auto& kv : fm)
			{
				NetplayPlayerFrameInput pfi;
				pfi.player_id = kv.first;
				pfi.inputs.reserve(kv.second.size());
				for (const InputSample& s : kv.second)
					pfi.inputs.push_back(NetplayInputSample{s.index, s.is_pressed, s.range_value});
				fd.players.push_back(std::move(pfi));
			}
			g_NetplayFrameSync->AppendFrame(fd);
		}

		for (const auto& kv : fm)
		{
			const auto pit = peers.find(kv.first);
			if (pit == peers.end() || pit->second.port == 0)
				continue;
			NetplayUdpSocket::SendTo(
				m_sock, pit->second.ip.c_str(), pit->second.port, packet.data(), static_cast<int>(packet.size()));
		}

		if (broadcast_count == 0)
		{
			Console.WriteLn(
				"NETPLAY LAN: UDP relay broadcast first merged frame %llu to %u peer(s).",
				static_cast<unsigned long long>(frame_id), static_cast<unsigned>(fm.size()));
		}
		++broadcast_count;

		pending.erase(frame_id);

		while (pending.size() > 120)
			pending.erase(pending.begin());
	};

	while (m_running.load())
	{
		{
			std::vector<InjectedPacket> injected;
			{
				std::lock_guard<std::mutex> lock(m_injected_mutex);
				injected.swap(m_injected_packets);
			}
			for (const InjectedPacket& packet : injected)
				process_packet(packet.data.data(), static_cast<int>(packet.data.size()), "127.0.0.1", 0);
		}

		// 优化.md §B.9：每 5s 打一次 relay 内部状态摘要，便于跨端黑屏问题对账。
		// 关注以下三组数字：
		//   RX(0x0001) ：本端 relay 一共收到多少输入帧。两端都 ≥1 才有可能合并广播。
		//   peers      ：H:/G: 前缀的 player_id 列表，确认双方身份未冲撞。
		//   broadcast  ：合并并广播的次数；只要双端都正常发 0x0001，应该秒级递增。
		const uint64_t now_ms = NowMs();
		if (now_ms - last_summary_ms >= 5000)
		{
			last_summary_ms = now_ms;
			std::string counts;
			for (const auto& kv : rx_msgtype_count)
			{
				if (!counts.empty())
					counts += ",";
				counts += kv.first + "=" + std::to_string(kv.second);
			}
			std::string peer_list;
			{
				std::lock_guard<std::mutex> lk(pending_mu);
				for (const auto& kv : peers)
				{
					if (!peer_list.empty())
						peer_list += ",";
					peer_list += kv.first + "@" + kv.second.ip + ":" + std::to_string(kv.second.port);
				}
			}
			Console.WriteLn(
				"NETPLAY LAN: relay 5s summary: rx[%s] inputFrames=%u peers=[%s] broadcast=%llu",
				counts.c_str(), static_cast<unsigned>(input_frames_received), peer_list.c_str(),
				static_cast<unsigned long long>(broadcast_count));
		}

		std::string sender_ip;
		unsigned short sender_port = 0;
		const int n = NetplayUdpSocket::RecvFrom(m_sock, buf.data(), static_cast<int>(buf.size()), sender_ip, sender_port);
		if (n <= 0)
		{
			if (!any_packet_seen && !reachability_warn_emitted && (NowMs() - relay_start_ms) >= 15000)
			{
				Console.Warning(
					"NETPLAY LAN: UDP relay on :%u has received ZERO packets in 15s. "
					"Check that: (1) both devices are on the same LAN subnet, "
					"(2) Wi-Fi AP isolation (client isolation) is OFF on the router, "
					"(3) the guest (PC) Windows Defender Firewall is not blocking outbound UDP.",
					static_cast<unsigned>(m_port));
				reachability_warn_emitted = true;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			continue;
		}

		process_packet(buf.data(), n, sender_ip, sender_port);
	}
}
