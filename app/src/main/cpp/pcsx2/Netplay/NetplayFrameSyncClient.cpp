// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// 实现与 pcsx2-online 的 NetplayFrameSync.cpp 同源逻辑；若协议或步进有争议，以该文件为准再同步到本树。

#include "Netplay/NetplayFrameSyncClient.h"

#include "Netplay/NetplayLanUdpRelay.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/NetplayHook.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"

#include "common/Console.h"

#include <algorithm>
#include <chrono>

NetplayFrameSyncClient* g_NetplayFrameSync = nullptr;

namespace
{
	/// 严格 lockstep 模式：不使用短超时触发"本地单边继续"。
	/// 用户产品要求：宁愿同步卡顿，也不允许画面不同步。
	/// 若远端长时间不推进，OnVSync 会一直在 WaitForFrame 里旋转（持 _mutex 的条件等待），
	/// 模拟器 VSync 线程被阻塞 == 画面冻结（"卡顿"），直到远端帧到达。
	/// 只有当 _running 被外部置 false（EndSession / Stop）时才退出等待；此时 OnVSync 不会
	/// 应用任何本地输入，彻底消除"单边继续推帧 → 双端画面漂移"的可能。
	///
	/// 兜底：为避免一端真崩溃后另一端永远冻死，设一个"非常长但有限"的上限（5 分钟）。
	/// 正常联机绝无可能达到；达到时视为连接永久丢失，发 disconnected 让 UI 兜底退出。
	constexpr uint64_t NETPLAY_LOCKSTEP_MAX_WAIT_MS = 5ull * 60ull * 1000ull;
} // namespace

static uint64_t NowMs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

NetplayFrameSyncClient::NetplayFrameSyncClient() = default;

NetplayFrameSyncClient::~NetplayFrameSyncClient()
{
	Stop();
}

bool NetplayFrameSyncClient::Start(const NetplayRoomState& state)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_room_id = state.room_id;
	_player_id = state.player_id;
	_server_addr = state.udp_server_addr;
	_server_port = state.udp_server_port;
	_is_host = state.is_host;
	_is_verifier = state.is_verifier;
	_host_player_id = state.host_player_id;

	// UDP relay 用 player_id 作为内部 map key 区分两端转发队列。若房主和成员都叫 "Player"
	// （默认未改昵称的常见场景），两条流会撞到同一个 key，表现为：成员本地输入被房主的队列
	// 覆盖 / 房主反向收到自己输入的回环，俗称"一个人在玩单机"。统一加 H:/G: 前缀硬隔离。
	if (!_is_verifier)
	{
		const std::string prefix = _is_host ? "H:" : "G:";
		if (_player_id.rfind("H:", 0) != 0 && _player_id.rfind("G:", 0) != 0)
			_player_id = prefix + _player_id;
		if (!_host_player_id.empty() && _host_player_id.rfind("H:", 0) != 0 && _host_player_id.rfind("G:", 0) != 0)
			_host_player_id = std::string("H:") + _host_player_id;
	}
	_initial_frame_id = state.initial_frame_id;
	_frame_interval_us = state.frame_interval_us;
	_frame_cache_size = state.frame_cache_size;
	_protocol_version = state.protocol_version;
	_sync_delay = state.sync_delay;

	_next_send_frame = _initial_frame_id;
	_last_broadcast_frame = 0;
	_last_observed_frame = _initial_frame_id;

	_target_frame_interval_us.store(16667);
	_current_frame_interval_us.store(16667);
	_transition_total_frames = 0;
	_transition_done_frames = 0;
	_transition_start_interval = 0;
	_last_frame_end_us = 0;

	// 用户要求：无须额外防作弊校验，直接跳过 Warmup 与 Whitelist，加速联机起步速度（节省300帧/5秒）。
	_warmup_done = true;
	_whitelist_ready = true;

	if (!NetplayUdpSocket::Init())
		return false;

	_sock = NetplayUdpSocket::Open(0, true);
	if (_sock == NP_INVALID_SOCKET)
	{
		Console.Error("NETPLAY: Failed to open UDP socket, retrying with fallback port...");
		for (unsigned short p = 40000; p < 40010; p++)
		{
			_sock = NetplayUdpSocket::Open(p, true);
			if (_sock != NP_INVALID_SOCKET)
				break;
		}
		if (_sock == NP_INVALID_SOCKET)
		{
			NetplayUdpSocket::Shutdown();
			return false;
		}
	}

	// 打开 Pad 物理输入路由：从现在起所有上层 SetControllerState 调用都会被导向
	// NetplayHook::SetPhysicalInput 中间缓冲区，OnVSync 每帧从那里读 local 并独占写 pad。
	// ClearPhysicalInputs 清除上一场会话可能残留的粘键状态。
	NetplayHook::ClearPhysicalInputs();
	NetplayHook::SetPadOverride(true);

	_running.store(true);
	_recv_thread = std::thread([this]() { ReceiverLoop(); });
	_send_thread = std::thread([this]() { SenderLoop(); });
	SendHeartbeat();

	Console.WriteLn("NETPLAY: FrameSync started (UDP %s:%u) - %s mode (sync_delay=%d)",
		_server_addr.c_str(), _server_port,
		_sync_delay > 0 ? "BUFFERED" : "LOCKSTEP", _sync_delay);
	return true;
}

void NetplayFrameSyncClient::Stop()
{
	// 关闭 Pad 物理输入路由，让上层 SetControllerState 恢复直写 pad（单机/退出联机后正常玩的必要路径）。
	// 同时清空中间缓冲区，防止下次 Start 读到上一场残留。
	NetplayHook::SetPadOverride(false);
	NetplayHook::ClearPhysicalInputs();

	_running.store(false);
	_cv.notify_all();
	if (_recv_thread.joinable())
		_recv_thread.join();
	if (_send_thread.joinable())
	{
		_send_cv.notify_all();
		_send_thread.join();
	}
	if (_sock != NP_INVALID_SOCKET)
	{
		NetplayUdpSocket::Close(_sock);
		_sock = NP_INVALID_SOCKET;
	}
	NetplayUdpSocket::Shutdown();
	std::lock_guard<std::mutex> lock(_mutex);
	_frames.clear();

	_target_frame_interval_us.store(16667);
	_current_frame_interval_us.store(16667);
	_transition_total_frames = 0;
	_transition_done_frames = 0;
	_last_frame_end_us = 0;

	Console.Warning("NETPLAY: FrameSync stopped");
}

void NetplayFrameSyncClient::TickSync()
{
	uint64_t now_ms = NowMs();
	if (_last_heartbeat_ms == 0 || now_ms - _last_heartbeat_ms >= 1000)
	{
		SendHeartbeat();
		_last_heartbeat_ms = now_ms;
	}
}

uint64_t NetplayFrameSyncClient::GetCurrentFrameId() const
{
	return _last_broadcast_frame ? _last_broadcast_frame : _last_observed_frame;
}

void NetplayFrameSyncClient::SetObservedFrame(uint64_t frame_id)
{
	_last_observed_frame = frame_id;
}

void NetplayFrameSyncClient::SubmitLocalInputs(const std::vector<NetplayInputSample>& inputs)
{
	SendInputFrame(inputs, _next_send_frame);
	_next_send_frame++;
}

bool NetplayFrameSyncClient::GetFrame(uint64_t frame_id, NetplayFrameData& out_frame)
{
	std::lock_guard<std::mutex> lock(_mutex);
	auto it = _frames.find(frame_id);
	if (it == _frames.end())
		return false;
	out_frame = it->second;
	return true;
}

bool NetplayFrameSyncClient::WaitForFrame(uint64_t frame_id, uint64_t timeout_ms)
{
	auto deadline = NowMs() + timeout_ms;
	std::unique_lock<std::mutex> lock(_mutex);
	while (true)
	{
		auto it = _frames.find(frame_id);
		if (it != _frames.end())
		{
			bool has_remote = false;
			for (const auto& pfi : it->second.players)
			{
				if (pfi.player_id != _player_id)
				{
					has_remote = true;
					break;
				}
			}
			if (has_remote)
				return true;
		}
		if (!_running.load())
			return false;
		uint64_t now = NowMs();
		if (now >= deadline)
			return false;
		auto remaining = std::min<uint64_t>(deadline - now, 16);
		_cv.wait_for(lock, std::chrono::milliseconds(remaining));
	}
}

void NetplayFrameSyncClient::AppendFrame(const NetplayFrameData& fd)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_frames[fd.frame_id] = fd;
	while ((int)_frames.size() > _frame_cache_size)
		_frames.erase(_frames.begin());
	_cv.notify_all();
}

// ---------------------------------------------------------------------------

void NetplayFrameSyncClient::ReceiverLoop()
{
	std::vector<uint8_t> buf(4096);
	while (_running.load())
	{
		std::string sender_ip;
		unsigned short sender_port;
		int n = NetplayUdpSocket::RecvFrom(_sock, buf.data(), (int)buf.size(), sender_ip, sender_port);
		if (n > 0)
		{
			ParseBroadcastPacket(buf.data(), n);
			continue; // immediately try reading again without sleeping
		}
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
}

void NetplayFrameSyncClient::DetectAndRequestMissing(uint64_t latest_frame)
{
	if (_last_broadcast_frame == 0)
	{
		_last_broadcast_frame = latest_frame;
		return;
	}
	if (latest_frame > _last_broadcast_frame + 1)
	{
		for (uint64_t f = _last_broadcast_frame + 1; f < latest_frame; f++)
			SendMissingFrameRequest(f);
	}
	_last_broadcast_frame = latest_frame;
}

void NetplayFrameSyncClient::SendHeartbeat()
{
	std::vector<uint8_t> payload;
	Enqueue(0x0003, payload, 0, 0, 0);
}

void NetplayFrameSyncClient::SendInputFrame(const std::vector<NetplayInputSample>& inputs, uint64_t frame_id)
{
	std::vector<uint8_t> payload;
	payload.reserve(16 + inputs.size() * 6);

	auto push64 = [&payload](uint64_t v) {
		for (int i = 7; i >= 0; i--)
			payload.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
	};

	push64(frame_id);
	payload.push_back((uint8_t)inputs.size());
	for (auto& s : inputs)
	{
		payload.push_back((uint8_t)((s.index >> 8) & 0xFF));
		payload.push_back((uint8_t)(s.index & 0xFF));
		payload.push_back(s.is_pressed ? 1 : 0);
		payload.push_back(s.range_value);
		payload.push_back(0);
		payload.push_back(0);
	}
	push64(NowMs());

	Enqueue(0x0001, payload, 1, 0, 0);

	// 关键：首帧（frame_id == _initial_frame_id）发出时，对端 UDP relay 很可能还没 bind 上来（
	// 对端 VM 起得更慢 / BeginLanBootSession 还没执行到 _lan_udp_relay->Start）。
	// 若只重传 3 次 ~350ms 就放弃，首帧会彻底丢，双端永远卡在 lockstep WaitForFrame。
	//
	// 策略：
	//   · 前几个 input frame（刚起步，对端可能还没 ready）—— 长时间重传（最多 ~8 秒，
	//     每 200ms 一次），直到 relay 回广播同一帧（证明对端已在线）或 _running=false。
	//   · 后续帧 —— 保持原来的 3 次短重传（350ms），适配稳态低延迟联机。
	// 这对带宽影响极小（每次 < 200B），但彻底消除首帧丢失导致的"双端卡黑屏"问题。
	const uint64_t initialFid = _initial_frame_id;
	const bool is_startup_frame = (frame_id < initialFid + 30ull);
	if (is_startup_frame)
	{
		int attempts = 0;
		for (uint64_t delay_ms = 200; delay_ms <= 8000; delay_ms += 200)
			Enqueue(0x0001, payload, 1, delay_ms, ++attempts, frame_id);
	}
	else
	{
		const uint64_t delays[] = {50, 150, 350};
		for (int tries = 0; tries < 3; tries++)
			Enqueue(0x0001, payload, 1, delays[tries], tries + 1, frame_id);
	}
}

void NetplayFrameSyncClient::SendMissingFrameRequest(uint64_t missing_frame_id)
{
	std::vector<uint8_t> payload;
	for (int i = 7; i >= 0; i--)
		payload.push_back((uint8_t)((missing_frame_id >> (i * 8)) & 0xFF));
	Enqueue(0x0004, payload, 1, 0, 0);
}

void NetplayFrameSyncClient::SendMemoryCrc(uint64_t epoch, uint32_t crc)
{
	std::vector<uint8_t> payload;
	payload.reserve(12);
	// epoch (8 bytes)
	for (int i = 7; i >= 0; i--)
		payload.push_back((uint8_t)((epoch >> (i * 8)) & 0xFF));
	// crc32 (4 bytes)
	payload.push_back((uint8_t)((crc >> 24) & 0xFF));
	payload.push_back((uint8_t)((crc >> 16) & 0xFF));
	payload.push_back((uint8_t)((crc >> 8) & 0xFF));
	payload.push_back((uint8_t)(crc & 0xFF));
	Enqueue(0x0005, payload, 0, 0, 0);
}

bool NetplayFrameSyncClient::BuildAndSendPacket(uint16_t msgType, const std::vector<uint8_t>& payload)
{
	if (_sock == NP_INVALID_SOCKET)
		return false;

	std::vector<uint8_t> buf;
	buf.reserve(16 + _room_id.size() + _player_id.size() + payload.size());

	auto put16 = [&buf](uint16_t v) {
		buf.push_back((uint8_t)((v >> 8) & 0xFF));
		buf.push_back((uint8_t)(v & 0xFF));
	};
	auto put32 = [&buf](uint32_t v) {
		buf.push_back((uint8_t)((v >> 24) & 0xFF));
		buf.push_back((uint8_t)((v >> 16) & 0xFF));
		buf.push_back((uint8_t)((v >> 8) & 0xFF));
		buf.push_back((uint8_t)(v & 0xFF));
	};

	put32(0x50435358u);
	put16((uint16_t)_protocol_version);
	put16(msgType);
	put16((uint16_t)_room_id.size());
	put16((uint16_t)_player_id.size());
	put32(CalcCrc32(payload.data(), payload.size()));

	buf.insert(buf.end(), _room_id.begin(), _room_id.end());
	buf.insert(buf.end(), _player_id.begin(), _player_id.end());
	buf.insert(buf.end(), payload.begin(), payload.end());

	if (_is_host && msgType == 0x0001 &&
		(_server_addr == "127.0.0.1" || _server_addr == "localhost"))
	{
		NetplayLanUdpRelay::InjectLoopbackPacket(buf.data(), static_cast<int>(buf.size()));
	}

	return NetplayUdpSocket::SendTo(_sock, _server_addr.c_str(), _server_port, buf.data(), (int)buf.size()) > 0;
}

bool NetplayFrameSyncClient::ParseBroadcastPacket(const uint8_t* data, int len)
{
	if (len < 16)
		return false;

	int pos = 0;
	auto get16 = [&](uint16_t& v) { v = (uint16_t)((data[pos] << 8) | data[pos + 1]); pos += 2; };
	auto get32 = [&](uint32_t& v) {
		v = (uint32_t)((data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3]);
		pos += 4;
	};

	uint32_t magic;
	get32(magic);
	if (magic != 0x50435358u)
		return false;

	uint16_t ver;
	get16(ver);
	if (ver != (uint16_t)_protocol_version)
		return false;

	uint16_t msgType;
	get16(msgType);
	uint16_t roomIdLen;
	get16(roomIdLen);
	uint16_t playerIdLen;
	get16(playerIdLen);
	uint32_t crc;
	get32(crc);

	if (pos + roomIdLen + playerIdLen > len)
		return false;
	pos += roomIdLen;
	if (playerIdLen)
		pos += playerIdLen;

	if (msgType == 0x0006)
	{
		// Desync alert from server
		_desync_detected.store(true);
		Console.Warning("NETPLAY: Desync alert received from server!");
		return true;
	}

	if (msgType == 0x0008)
	{
		Console.Error("NETPLAY: Kicked by server - anti-cheat CRC mismatch detected!");
		_running.store(false);
		_disconnected.store(true);
		_cv.notify_all();
		return true;
	}

	if (msgType == 0x0009)
	{
		const uint8_t* p = data + pos;
		int plen = len - pos;
		if (plen >= 12)
		{
			uint32_t targetInterval =
				((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
				((uint32_t)p[2] << 8) | (uint32_t)p[3];
			uint16_t transFrames = ((uint16_t)p[4] << 8) | p[5];
			uint8_t reason = p[6];

			if (targetInterval >= 10000 && targetInterval <= 50000 && transFrames > 0)
			{
				_transition_start_interval = _current_frame_interval_us.load();
				_target_frame_interval_us.store(targetInterval);
				_transition_total_frames = transFrames;
				_transition_done_frames = 0;

				const char* reasonStr = "unknown";
				switch (reason) {
					case 0x00: reasonStr = "recovery"; break;
					case 0x01: reasonStr = "high RTT"; break;
					case 0x02: reasonStr = "packet loss"; break;
					case 0x03: reasonStr = "high jitter"; break;
				}
				Console.WriteLn("NETPLAY: Frame rate change -> %u us (%u fps), reason=%s, transition=%u frames",
					targetInterval, 1000000 / targetInterval, reasonStr, transFrames);
			}
		}
		return true;
	}

	if (msgType == 0x0007 && (_is_verifier || !_is_host))
	{
		// Whitelist from host (relayed by server)
		const uint8_t* wl = data + pos;
		int wlen = len - pos;
		if (wlen >= 2)
		{
			uint16_t count = (uint16_t)((wl[0] << 8) | wl[1]);
			if (wlen >= 2 + count * 2)
			{
				_whitelist_pages.clear();
				for (int i = 0; i < count; i++)
				{
					int off = 2 + i * 2;
					uint16_t idx = (uint16_t)((wl[off] << 8) | wl[off + 1]);
					_whitelist_pages.push_back(idx);
				}
				_whitelist_ready = true;
				Console.WriteLn("NETPLAY: Received host whitelist (%d pages)", count);
			}
		}
		return true;
	}

	if (msgType != 0x0002)
		return false;
	if (pos >= len)
		return false;

	const uint8_t* payload = data + pos;
	size_t plen = len - pos;
	if (CalcCrc32(payload, plen) != crc)
		return false;

	size_t p = 0;
	auto has = [&](size_t n) { return n <= plen && p <= plen - n; };
	auto p_get16 = [&](uint16_t& v) {
		if (!has(2))
			return false;
		v = (uint16_t)((payload[p] << 8) | payload[p + 1]);
		p += 2;
		return true;
	};
	auto p_get64 = [&](uint64_t& v) {
		if (!has(8))
			return false;
		v = 0;
		for (int i = 0; i < 8; i++)
			v = (v << 8) | payload[p + i];
		p += 8;
		return true;
	};

	uint16_t ridLen;
	if (!p_get16(ridLen) || !has(ridLen))
		return false;
	p += ridLen;

	uint64_t frameId;
	if (!p_get64(frameId) || !has(1))
		return false;
	uint8_t groups = payload[p];
	p += 1;
	uint64_t serverTs;
	if (!p_get64(serverTs))
		return false;

	NetplayFrameData fd;
	fd.frame_id = frameId;
	fd.server_timestamp_ms = serverTs;

	for (uint8_t gi = 0; gi < groups; gi++)
	{
		uint16_t pidLen;
		if (!p_get16(pidLen) || !has(pidLen))
			return false;
		std::string pid((const char*)(payload + p), pidLen);
		p += pidLen;

		// Server pads player_id to fixed length (32 bytes) with null bytes.
		// Trim trailing nulls so comparison with our _player_id works correctly.
		while (!pid.empty() && pid.back() == '\0')
			pid.pop_back();
		if (!has(1))
			return false;
		uint8_t inputsCount = payload[p];
		p += 1;

		NetplayPlayerFrameInput pfi;
		pfi.player_id = pid;

		for (uint8_t k = 0; k < inputsCount; k++)
		{
			uint16_t idx;
			if (!p_get16(idx) || !has(4))
				return false;
			uint8_t isPressed = payload[p];
			p += 1;
			uint8_t rangeVal = payload[p];
			p += 1;
			p += 2;

			NetplayInputSample s;
			s.index = idx;
			s.is_pressed = isPressed ? 1 : 0;
			s.range_value = rangeVal;
			pfi.inputs.push_back(s);
		}
		fd.players.push_back(pfi);
	}

	{
		std::lock_guard<std::mutex> lock(_mutex);
		_frames[frameId] = fd;
		while ((int)_frames.size() > _frame_cache_size)
			_frames.erase(_frames.begin());
	}
	DetectAndRequestMissing(frameId);
	_last_rx_ms = NowMs();

	// Wake up any thread waiting in WaitForFrame
	_cv.notify_all();

	return true;
}

void NetplayFrameSyncClient::Enqueue(uint16_t msgType, const std::vector<uint8_t>& payload, int priority, uint64_t delay_ms, int attempts,
	uint64_t frame_id)
{
	OutMsg m;
	m.type = msgType;
	m.payload = payload;
	m.priority = priority;
	m.due = NowMs() + delay_ms;
	m.attempts = attempts;
	m.frame_id = frame_id;
	{
		std::lock_guard<std::mutex> lk(_send_mutex);
		if (priority > 0)
			_hq.push_back(std::move(m));
		else
			_lq.push_back(std::move(m));
	}
	_send_cv.notify_one();
}

void NetplayFrameSyncClient::SenderLoop()
{
	while (_running.load())
	{
		uint64_t now_ms = NowMs();
		OutMsg m;
		bool has = false;
		{
			std::unique_lock<std::mutex> lk(_send_mutex);
			auto take_due = [now_ms](std::deque<OutMsg>& q, OutMsg& out) {
				for (auto it = q.begin(); it != q.end(); ++it)
				{
					if (it->due <= now_ms)
					{
						out = std::move(*it);
						q.erase(it);
						return true;
					}
				}
				return false;
			};

			if (take_due(_hq, m))
			{
				has = true;
			}
			else if (take_due(_lq, m))
			{
				has = true;
			}
			else
			{
				uint64_t next_due = UINT64_MAX;
				for (const OutMsg& queued : _hq)
					next_due = std::min(next_due, queued.due);
				for (const OutMsg& queued : _lq)
					next_due = std::min(next_due, queued.due);
				uint64_t wait_ms = 1000;
				if (next_due != UINT64_MAX && next_due > now_ms)
					wait_ms = std::min<uint64_t>(next_due - now_ms, 1000);
				_send_cv.wait_for(lk, std::chrono::milliseconds(wait_ms));
				continue;
			}
		}
		if (has)
		{
			if (m.frame_id != UINT64_MAX)
			{
				NetplayFrameData tmp;
				if (GetFrame(m.frame_id, tmp))
					continue;
			}
			bool ok = BuildAndSendPacket(m.type, m.payload);
			if (!ok && m.attempts < 3)
			{
				uint64_t backoff = (m.attempts == 0 ? 50 : (m.attempts == 1 ? 100 : 200));
				Enqueue(m.type, m.payload, m.priority, backoff, m.attempts + 1, m.frame_id);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Lockstep OnVSync - modeled after pcsx2-online Boot Netplay HandleIO
// ---------------------------------------------------------------------------

static void ApplyInputSamplesToPad(u32 padIndex, const std::vector<NetplayInputSample>& inputs)
{
	for (const auto& s : inputs)
	{
		float value;
		if (PadDualshock2::IsAnalogKey((int)s.index))
			value = (float)s.range_value / 255.0f;
		else
			value = s.is_pressed ? 1.0f : 0.0f;
		Pad::SetControllerState(padIndex, (u32)s.index, value);
	}
}

void NetplayFrameSyncClient::OnVSync()
{
	if (!_running.load())
		return;

	TickSync();

	if (_is_verifier)
	{
		OnVSyncVerifier();
		return;
	}

	// 关闭 Pad::SetControllerState 在本线程的联机路由：下面 Clear 两侧 pad 以及把 local/remote
	// 样本写入对应 pad 都是**合法且必须**的写入，不能被 NetplayHook 重定向回物理缓冲区。
	// RAII 的生存期覆盖整个 OnVSync 函数体，栈帧退出时自动恢复路由。
	Pad::NetplayRedirectBypass bypass;

	// --- Step 1: Read local physical inputs from NetplayHook intermediate buffer ---
	// 对标 ARMSX2-online：本机所有上层输入通道（JNI / InputManager / UI 宏）
	// 都会在 SetControllerState 入口被路由到 NetplayHook::SetPhysicalInput 缓冲区；
	// 我们只从这里读一次，pad 0/1 在两次 VSync 之间永不被上层触碰，远端样本不会被污染。
	std::vector<NetplayInputSample> localInputs;
	localInputs.reserve(PadDualshock2::Inputs::LENGTH);
	for (u32 i = 0; i < PadDualshock2::Inputs::LENGTH; i++)
	{
		NetplayInputSample s;
		s.index = (uint16_t)i;
		u8 raw = NetplayHook::GetPhysicalInput(i);
		if (PadDualshock2::IsAnalogKey((int)i))
			s.is_pressed = (raw != 0x7f && raw != 0x80) ? 1 : 0;
		else
			s.is_pressed = (raw > 0) ? 1 : 0;
		s.range_value = raw;
		localInputs.push_back(s);
	}

	// --- Step 2: Clear both pads ---
	for (u32 i = 0; i < PadDualshock2::Inputs::LENGTH; i++)
	{
		Pad::SetControllerState(0, i, 0.0f);
		Pad::SetControllerState(1, i, 0.0f);
	}

	// --- Step 3: Submit local inputs to the server as frame N ---
	uint64_t sendFrame = _next_send_frame;
	if (sendFrame == _initial_frame_id)
	{
		Console.WriteLn(
			"NETPLAY: OnVSync submitting first frame %llu (is_host=%d, dest=%s:%u).",
			static_cast<unsigned long long>(sendFrame), _is_host ? 1 : 0,
			_server_addr.c_str(), static_cast<unsigned>(_server_port));
	}
	SubmitLocalInputs(localInputs);

	// --- Step 4: Determine render frame ---
	// With sync_delay > 0: render lags behind send by N frames (buffered mode).
	// With sync_delay == 0: render == send (strict lockstep).
	int64_t renderFrame64 = (int64_t)sendFrame - (int64_t)_sync_delay;
	bool bufferingPhase = renderFrame64 < (int64_t)_initial_frame_id;

	if (bufferingPhase)
	{
		u32 localPad = _is_host ? 0 : 1;
		ApplyInputSamplesToPad(localPad, localInputs);
		return;
	}

	uint64_t renderFrame = (uint64_t)renderFrame64;

	// --- Step 5: Wait for broadcast of the render frame ---
	// 严格 lockstep：必须等到远端把 frame N 推过来（UDP relay 只有在两端都提交 N 之后才会
	// 广播 N），任何"假装远端没按→本地独自前进"都会立刻产生画面不同步。
	static thread_local bool s_first_wait_logged = false;
	if (!s_first_wait_logged && renderFrame == _initial_frame_id)
	{
		s_first_wait_logged = true;
		Console.WriteLn(
			"NETPLAY: OnVSync entering lockstep WaitForFrame(%llu) — waiting for UDP relay broadcast.",
			static_cast<unsigned long long>(renderFrame));
	}

	bool gotFrame = WaitForFrame(renderFrame, NETPLAY_LOCKSTEP_MAX_WAIT_MS);

	static thread_local bool s_first_ok_logged = false;
	if (gotFrame && !s_first_ok_logged && renderFrame == _initial_frame_id)
	{
		s_first_ok_logged = true;
		Console.WriteLn(
			"NETPLAY: OnVSync received first merged broadcast for frame %llu — lockstep UNBLOCKED.",
			static_cast<unsigned long long>(renderFrame));
	}

	if (!gotFrame)
	{
		// 两种情况：
		//   (a) _running == false：会话已被外部停止（EndSession / Stop），正常退出，
		//       此时**绝不**应用 localInputs，直接返回——VM 停止时本来就不会再渲染，不会产生漂移。
		//   (b) 5 分钟都没等到远端帧：视为连接永久丢失。记日志并 disconnected，让 UI 走断线兜底。
		//       同样不应用 localInputs，保持两端画面对齐（对端也卡住）。
		if (_running.load())
		{
			_disconnected.store(true);
			Console.Error("NETPLAY: Hard timeout (>5 min) on render frame %llu (send=%llu). Connection lost.",
				(unsigned long long)renderFrame, (unsigned long long)sendFrame);
		}
		return;
	}

	if (_disconnected.load())
	{
		_disconnected.store(false);
		Console.WriteLn("NETPLAY: Connection restored at render frame %llu", (unsigned long long)renderFrame);
	}

	// --- Step 6: Apply inputs ---
	u32 localPad = _is_host ? 0 : 1;
	u32 remotePad = _is_host ? 1 : 0;

	if (_sync_delay > 0)
	{
		// BUFFERED: apply BOTH players' inputs from the broadcast.
		NetplayFrameData fd;
		if (GetFrame(renderFrame, fd))
		{
			for (const auto& pfi : fd.players)
			{
				if (pfi.player_id == _player_id)
					ApplyInputSamplesToPad(localPad, pfi.inputs);
				else
					ApplyInputSamplesToPad(remotePad, pfi.inputs);
			}
		}
	}
	else
	{
		// STRICT LOCKSTEP: local from physical read, remote from broadcast.
		ApplyInputSamplesToPad(localPad, localInputs);

		NetplayFrameData fd;
		if (GetFrame(renderFrame, fd))
		{
			for (const auto& pfi : fd.players)
			{
				if (pfi.player_id == _player_id)
					continue;
				ApplyInputSamplesToPad(remotePad, pfi.inputs);
			}
		}
	}

	SetObservedFrame(renderFrame);

	// --- Step 7: Adaptive frame pacing (共同降帧) ---
	{
		if (_transition_total_frames > 0 && _transition_done_frames < _transition_total_frames)
		{
			_transition_done_frames++;
			float t = (float)_transition_done_frames / (float)_transition_total_frames;
			uint32_t target = _target_frame_interval_us.load();
			uint32_t interp = (uint32_t)(
				(float)_transition_start_interval * (1.0f - t) + (float)target * t);
			_current_frame_interval_us.store(interp);

			if (_transition_done_frames >= _transition_total_frames)
			{
				_current_frame_interval_us.store(target);
				Console.WriteLn("NETPLAY: Frame rate transition complete -> %u us (%u fps)",
					target, 1000000 / target);
			}
		}

		uint32_t intervalUs = _current_frame_interval_us.load();
		if (intervalUs > 16667)
		{
			uint64_t nowUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();

			if (_last_frame_end_us > 0)
			{
				uint64_t elapsed = nowUs - _last_frame_end_us;
				if (elapsed < intervalUs)
				{
					uint64_t sleepUs = intervalUs - elapsed;
					if (sleepUs > 40000) sleepUs = 40000;
					std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
				}
			}
			_last_frame_end_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		else
		{
			_last_frame_end_us = 0;
		}
	}

	// --- Anti-cheat ---
	if (!_warmup_done)
	{
		WarmupTick();
	}
	else if (_whitelist_ready)
	{
		_crc_check_counter++;
		if (_crc_check_counter >= MEMORY_CRC_INTERVAL)
		{
			_crc_check_counter = 0;
			uint32_t crc = ComputeWhitelistCrc();
			uint64_t epoch = renderFrame / MEMORY_CRC_INTERVAL;
			SendMemoryCrc(epoch, crc);
		}
	}
	else if (!_is_host && _warmup_done && !_whitelist_ready)
	{
		_crc_check_counter++;
		if (_crc_check_counter >= WARMUP_FRAMES)
		{
			Console.WriteLn("NETPLAY: Host whitelist not received, using local whitelist as fallback");
			_whitelist_ready = true;
		}
	}
}

// ---------------------------------------------------------------------------
// Verifier-specific VSync: no local input, apply all broadcast inputs, report CRC
// ---------------------------------------------------------------------------

void NetplayFrameSyncClient::OnVSyncVerifier()
{
	// 同 OnVSync：Verifier 要写入两侧 pad，需要关闭联机重定向。
	Pad::NetplayRedirectBypass bypass;

	for (u32 i = 0; i < PadDualshock2::Inputs::LENGTH; i++)
	{
		Pad::SetControllerState(0, i, 0.0f);
		Pad::SetControllerState(1, i, 0.0f);
	}

	uint64_t thisFrame = _next_send_frame;
	_next_send_frame++;

	bool gotFrame = WaitForFrame(thisFrame, 15000);

	if (!gotFrame)
	{
		if (_running.load())
		{
			_disconnected.store(true);
			Console.Error("NETPLAY VERIFIER: Timeout waiting for frame %llu", (unsigned long long)thisFrame);
		}
		return;
	}

	if (_disconnected.load())
	{
		_disconnected.store(false);
		Console.WriteLn("NETPLAY VERIFIER: Connection restored at frame %llu", (unsigned long long)thisFrame);
	}

	NetplayFrameData fd;
	if (GetFrame(thisFrame, fd))
	{
		for (const auto& pfi : fd.players)
		{
			u32 pad;
			if (!_host_player_id.empty() && pfi.player_id == _host_player_id)
				pad = 0; // host -> Pad 0 (1P)
			else
				pad = 1; // member -> Pad 1 (2P)
			ApplyInputSamplesToPad(pad, pfi.inputs);
		}
	}

	SetObservedFrame(thisFrame);

	// Anti-cheat CRC: verifier uses the same warmup+whitelist logic
	if (!_warmup_done)
	{
		WarmupTick();
	}
	else if (_whitelist_ready)
	{
		_crc_check_counter++;
		if (_crc_check_counter >= MEMORY_CRC_INTERVAL)
		{
			_crc_check_counter = 0;
			uint32_t crc = ComputeWhitelistCrc();
			uint64_t epoch = thisFrame / MEMORY_CRC_INTERVAL;
			SendMemoryCrc(epoch, crc);
		}
	}
	else if (_warmup_done && !_whitelist_ready)
	{
		_crc_check_counter++;
		if (_crc_check_counter >= WARMUP_FRAMES)
		{
			Console.WriteLn("NETPLAY VERIFIER: Using local whitelist (no host whitelist received)");
			_whitelist_ready = true;
		}
	}
}

// ---------------------------------------------------------------------------
// Anti-cheat: Warmup + Whitelist
// ---------------------------------------------------------------------------

void NetplayFrameSyncClient::WarmupTick()
{
	if (!eeMem)
		return;

	if (_page_prev_crc.empty())
	{
		// First tick: initialize tracking arrays and snapshot all pages
		_page_prev_crc.resize(TOTAL_PAGES);
		_page_change_count.resize(TOTAL_PAGES, 0);
		for (int i = 0; i < TOTAL_PAGES; i++)
			_page_prev_crc[i] = CalcCrc32(eeMem->Main + (i * PAGE_SIZE), PAGE_SIZE);
		_warmup_frame_count = 1;
		Console.WriteLn("NETPLAY: Anti-cheat warmup started (%d frames, %d pages)", WARMUP_FRAMES, TOTAL_PAGES);
		return;
	}

	// Sample every 10 frames during warmup to reduce overhead (30 samples total)
	_warmup_frame_count++;
	if (_warmup_frame_count % 10 != 0)
	{
		if (_warmup_frame_count >= WARMUP_FRAMES)
		{
			_warmup_done = true;
			BuildWhitelist();
		}
		return;
	}

	for (int i = 0; i < TOTAL_PAGES; i++)
	{
		uint32_t crc = CalcCrc32(eeMem->Main + (i * PAGE_SIZE), PAGE_SIZE);
		if (crc != _page_prev_crc[i])
		{
			_page_change_count[i]++;
			_page_prev_crc[i] = crc;
		}
	}

	if (_warmup_frame_count >= WARMUP_FRAMES)
	{
		_warmup_done = true;
		BuildWhitelist();
	}
}

void NetplayFrameSyncClient::BuildWhitelist()
{
	// Classify pages: 30 samples during warmup
	int totalSamples = WARMUP_FRAMES / 10;
	int noiseThreshold = totalSamples * 80 / 100; // > 80% change rate = noise

	struct PageScore
	{
		uint16_t index;
		uint16_t changes;
	};
	std::vector<PageScore> candidates;

	int noiseCount = 0, staticCount = 0;
	for (int i = 0; i < TOTAL_PAGES; i++)
	{
		uint16_t c = _page_change_count[i];
		if (c >= noiseThreshold)
		{
			noiseCount++;
			continue; // NOISE: skip
		}
		if (c == 0)
		{
			staticCount++;
			continue; // STATIC: skip
		}
		// LOGIC: changed sometimes but not constantly
		candidates.push_back({(uint16_t)i, c});
	}

	// Sort by change count ascending - pages that change least often (but > 0)
	// are typically key game state (HP, score, inventory)
	std::sort(candidates.begin(), candidates.end(),
		[](const PageScore& a, const PageScore& b) { return a.changes < b.changes; });

	_whitelist_pages.clear();
	int count = std::min((int)candidates.size(), WHITELIST_SIZE);
	for (int i = 0; i < count; i++)
		_whitelist_pages.push_back(candidates[i].index);

	// Sort by index for deterministic ordering
	std::sort(_whitelist_pages.begin(), _whitelist_pages.end());

	Console.WriteLn("NETPLAY: Whitelist built - %d logic pages selected (noise=%d, static=%d, logic_candidates=%d)",
		(int)_whitelist_pages.size(), noiseCount, staticCount, (int)candidates.size());

	// Free warmup memory
	_page_prev_crc.clear();
	_page_prev_crc.shrink_to_fit();
	_page_change_count.clear();
	_page_change_count.shrink_to_fit();

	if (_is_host && !_whitelist_pages.empty())
	{
		SendWhitelist();
		_whitelist_ready = true;
	}
	else if (!_is_host)
	{
		// Member waits for host's whitelist via 0x0007;
		// if none arrives within a few seconds, use own whitelist as fallback
		_whitelist_ready = false;
		Console.WriteLn("NETPLAY: Member waiting for host whitelist...");
	}
}

uint32_t NetplayFrameSyncClient::ComputeWhitelistCrc()
{
	if (!eeMem || _whitelist_pages.empty())
		return 0;
	uint32_t combined = 0;
	for (uint16_t pageIdx : _whitelist_pages)
	{
		const uint8_t* page = eeMem->Main + ((uint32_t)pageIdx * PAGE_SIZE);
		combined ^= CalcCrc32(page, PAGE_SIZE);
	}
	return combined;
}

void NetplayFrameSyncClient::SendWhitelist()
{
	std::vector<uint8_t> payload;
	uint16_t count = (uint16_t)_whitelist_pages.size();
	payload.push_back((uint8_t)(count >> 8));
	payload.push_back((uint8_t)(count & 0xFF));
	for (uint16_t idx : _whitelist_pages)
	{
		payload.push_back((uint8_t)(idx >> 8));
		payload.push_back((uint8_t)(idx & 0xFF));
	}
	Enqueue(0x0007, payload, 1, 0, 0);
	Console.WriteLn("NETPLAY: Host sent whitelist (%d pages) to server", count);
}

// ---------------------------------------------------------------------------

uint32_t NetplayFrameSyncClient::CalcCrc32(const uint8_t* data, size_t len)
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
