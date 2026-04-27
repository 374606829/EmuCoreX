// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// 帧同步：协议、UDP 包布局（0x0001 输入 / 0x0002 广播等）、OnVSync 锁步与缓冲语义以 pcsx2-online 为唯一参照：
//   <仓库>/pcsx2-online/pcsx2/Netplay/NetplayFrameSync.{h,cpp}
// 本文件为移植到 Qt/pcsx2-qt 的实现，集成点（Hook、线程、类型）可与上游不同，但**对外行为应与上述参照一致**。

#pragma once

#include <vector>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <limits>

#include "Netplay/NetplayRoomStateAndroid.h"
#include "Netplay/NetplayUdpSocket.h"

struct NetplayInputSample
{
	uint16_t index;
	uint8_t is_pressed;
	uint8_t range_value;
};

struct NetplayPlayerFrameInput
{
	std::string player_id;
	std::vector<NetplayInputSample> inputs;
};

struct NetplayFrameData
{
	uint64_t frame_id = 0;
	uint64_t server_timestamp_ms = 0;
	std::vector<NetplayPlayerFrameInput> players;
};

class NetplayFrameSyncClient
{
public:
	NetplayFrameSyncClient();
	~NetplayFrameSyncClient();

	bool Start(const NetplayRoomState& state);
	void Stop();

	void SubmitLocalInputs(const std::vector<NetplayInputSample>& inputs);
	bool GetFrame(uint64_t frame_id, NetplayFrameData& out_frame);
	bool WaitForFrame(uint64_t frame_id, uint64_t timeout_ms);
	void AppendFrame(const NetplayFrameData& fd);
	void TickSync();

	// Called once per emulator vsync - lockstep: submit inputs, wait for broadcast, apply
	void OnVSync();

	uint64_t GetCurrentFrameId() const;
	void SetObservedFrame(uint64_t frame_id);
	uint64_t LastRxMs() const { return _last_rx_ms; }
	bool IsHost() const { return _is_host; }
	const std::string& GetLocalPlayerId() const { return _player_id; }
	bool IsDisconnected() const { return _disconnected.load(); }

	// Anti-cheat / desync detection
	bool IsDesyncDetected() const { return _desync_detected.load(); }
	void ClearDesyncFlag() { _desync_detected.store(false); }

	static constexpr int MEMORY_CRC_INTERVAL = 60;
	static constexpr uint32_t CRC_MEM_SIZE = 0x02000000; // 32MB EE Main RAM
	static constexpr int PAGE_SIZE = 4096;
	static constexpr int TOTAL_PAGES = CRC_MEM_SIZE / PAGE_SIZE; // 8192
	static constexpr int WARMUP_FRAMES = 300; // ~5 seconds
	static constexpr int WHITELIST_SIZE = 50;

	enum class PageClass : uint8_t { UNKNOWN = 0, NOISE, STATIC, LOGIC };

	bool IsWarmupDone() const { return _warmup_done; }
	bool IsVerifier() const { return _is_verifier; }

private:
	void OnVSyncVerifier();
	void ReceiverLoop();
	void SenderLoop();
	void DetectAndRequestMissing(uint64_t latest_frame);
	void SendHeartbeat();
	void SendInputFrame(const std::vector<NetplayInputSample>& inputs, uint64_t frame_id);
	void SendMissingFrameRequest(uint64_t missing_frame_id);
	void SendMemoryCrc(uint64_t epoch, uint32_t crc);
	void SendWhitelist();
	void WarmupTick();
	void BuildWhitelist();
	uint32_t ComputeWhitelistCrc();

	struct OutMsg
	{
		uint16_t type;
		std::vector<uint8_t> payload;
		int priority;
		uint64_t due;
		int attempts;
		uint64_t frame_id = std::numeric_limits<uint64_t>::max();
	};
	void Enqueue(uint16_t msgType, const std::vector<uint8_t>& payload, int priority, uint64_t delay_ms, int attempts,
		uint64_t frame_id = std::numeric_limits<uint64_t>::max());

	bool BuildAndSendPacket(uint16_t msgType, const std::vector<uint8_t>& payload);
	bool ParseBroadcastPacket(const uint8_t* data, int len);

	static uint32_t CalcCrc32(const uint8_t* data, size_t len);

	mutable std::mutex _mutex;
	std::condition_variable _cv;
	std::map<uint64_t, NetplayFrameData> _frames;
	std::atomic<bool> _running{false};
	std::thread _recv_thread;
	std::thread _send_thread;
	std::mutex _send_mutex;
	std::condition_variable _send_cv;
	std::deque<OutMsg> _hq;
	std::deque<OutMsg> _lq;

	np_socket_t _sock = NP_INVALID_SOCKET;

	std::string _room_id;
	std::string _player_id;
	std::string _server_addr;
	unsigned short _server_port = 0;
	bool _is_host = false;
	bool _is_verifier = false;
	std::string _host_player_id;

	uint64_t _initial_frame_id = 0;
	uint64_t _frame_interval_us = 16667;
	int _frame_cache_size = 100;
	int _protocol_version = 0x0200;
	int _sync_delay = 0;

	uint64_t _next_send_frame = 0;
	uint64_t _last_broadcast_frame = 0;
	uint64_t _last_observed_frame = 0;
	uint64_t _last_rx_ms = 0;
	uint64_t _last_heartbeat_ms = 0;
	std::atomic<bool> _disconnected{false};

	// Anti-cheat / desync detection
	std::atomic<bool> _desync_detected{false};
	uint64_t _crc_check_counter = 0;

	// Warmup phase: track per-page change frequency
	bool _warmup_done = false;
	uint64_t _warmup_frame_count = 0;
	std::vector<uint32_t> _page_prev_crc;    // CRC of each page at previous sample
	std::vector<uint16_t> _page_change_count; // how many times each page changed

	// Whitelist: selected logic pages for CRC verification
	bool _whitelist_ready = false;
	std::vector<uint16_t> _whitelist_pages; // page indices

	// Adaptive frame pacing (共同降帧)
	std::atomic<uint32_t> _target_frame_interval_us{16667};
	std::atomic<uint32_t> _current_frame_interval_us{16667};
	uint32_t _transition_total_frames = 0;
	uint32_t _transition_done_frames = 0;
	uint32_t _transition_start_interval = 0;
	uint64_t _last_frame_end_us = 0;
};

// Global pointer to the active frame sync client, set when netplay is active
extern NetplayFrameSyncClient* g_NetplayFrameSync;
