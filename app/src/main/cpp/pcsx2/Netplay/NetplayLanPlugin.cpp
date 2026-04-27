// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Android LAN 联机插件实现。
// 去掉所有 Qt 依赖：QString → std::string，QMetaObject::invokeMethod → 直接调 callback。
// 状态机与协议与 PC 版完全一致。

#include "PrecompiledHeader.h"

#include "Netplay/NetplayLanPlugin.h"
#include "Netplay/NetplayLanAndroidController.h"
#include "Netplay/NetplayLanSettingsAndroid.h"
#include "Netplay/NetplayRoomStateAndroid.h"
#include "Netplay/NetplayFrameSyncClient.h"
#include "Netplay/NetplayFrameSyncInputHooks.h"
#include "Netplay/NetplayLanUdpRelay.h"

#include "shoryu/session.h"
#include "Netplay/NetplayLanMessage.h"

#include "pcsx2/Config.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/NetplayHook.h"
#include "common/Console.h"
#include "common/Path.h"
#include "platform/host/Host.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <optional>
#include <chrono>
#include <atomic>

// ======================== IOPHook globals ========================
LanIOPHook* g_LanIOPHook = nullptr;

void HookLanIOP(LanIOPHook* hook)
{
	g_LanIOPHook = hook;
}

void UnhookLanIOP()
{
	g_LanIOPHook = nullptr;
}

namespace
{
	std::optional<EmulatorSyncState> BuildLanSyncState()
	{
		if (!VMManager::HasValidVM())
			return std::nullopt;

		EmulatorSyncState s{};
		const std::string disc = VMManager::GetDiscSerial();
		if (disc.empty())
			return std::nullopt;

		memset(s.discId, 0, sizeof(s.discId));
		memcpy(s.discId, disc.data(), std::min(disc.size(), sizeof(s.discId)));

		const std::string biosPath = EmuConfig.FullpathToBios();
		const std::string biosTag(Path::GetFileName(biosPath));
		memset(s.biosVersion, 0, sizeof(s.biosVersion));
		memcpy(s.biosVersion, biosTag.c_str(), std::min(biosTag.size(), sizeof(s.biosVersion) - 1));

		s.skipMpeg = EmuConfig.Gamefixes.SkipMPEGHack;
		return s;
	}

	EmulatorSyncState BuildLanLobbyPlaceholderState()
	{
		EmulatorSyncState s{};
		const std::string biosPath = EmuConfig.FullpathToBios();
		const std::string biosTag(Path::GetFileName(biosPath));
		memset(s.biosVersion, 0, sizeof(s.biosVersion));
		memcpy(s.biosVersion, biosTag.c_str(), std::min(biosTag.size(), sizeof(s.biosVersion) - 1));

		memset(s.discId, 0, sizeof(s.discId));
		const char placeholder[] = "LAN_LOBBY____";
		static_assert(sizeof(placeholder) - 1 <= sizeof(s.discId));
		memcpy(s.discId, placeholder, sizeof(placeholder) - 1);

		s.skipMpeg = EmuConfig.Gamefixes.SkipMPEGHack;
		return s;
	}

	std::optional<EmulatorSyncState> GetSessionSyncState()
	{
		if (g_LanNetplaySettings.LobbyPhaseOnly)
			return BuildLanLobbyPlaceholderState();
		return BuildLanSyncState();
	}
} // namespace

// ======================== NetplayPlugin Implementation ========================
class NetplayLanPluginImpl : public ILanNetplayPlugin
{
	typedef shoryu::session<NetplayLanMessage, EmulatorSyncState> session_type;
	std::shared_ptr<session_type> _session;
	std::shared_ptr<std::thread> _connect_thread;
	std::unique_ptr<NetplayFrameSyncClient> _fsync;
	std::unique_ptr<NetplayLanUdpRelay> _lan_udp_relay;
	uint64_t _expected_frame = 0;
	uint64_t _last_sync_error_report_ms = 0;
	std::unique_ptr<std::thread> _fault_thread;
	int _calib_skip_mod = 0;
	int _calib_mod_counter = 0;
	int _calib_frames_left = 0;

public:
	NetplayLanPluginImpl()
		: _is_initialized(false)
		, _is_stopped(false)
		, _callback(nullptr)
		, _boot_fsync_hooks_active(false)
		, _handshake_gate_open(false)
	{
	}

	void HandleUsernames(const std::vector<shoryu::userinfo>& usernames)
	{
		int num_players = _session ? _session->num_players() : 0;
		if (!_callback)
			return;

		std::vector<LanUserInfo> lanUsers;
		for (const auto& u : usernames)
		{
			LanUserInfo lu;
			lu.name = u.name;
			lu.ping = u.ping;
			lu.side = u.side;
			lanUsers.push_back(lu);
		}

		// 房主单独在大厅时保证本地 Username 显示在 side 0
		if (g_LanNetplaySettings.Mode == HostMode)
		{
			bool has_side0 = false;
			for (const LanUserInfo& lu : lanUsers)
			{
				if (lu.side == 0) { has_side0 = true; break; }
			}
			if (!has_side0)
			{
				LanUserInfo h;
				h.side = 0;
				h.name = g_LanNetplaySettings.Username;
				h.ping = "-";
				lanUsers.insert(lanUsers.begin(), h);
			}
			if (num_players <= 0)
				num_players = (g_LanNetplaySettings.NumPlayers > 0) ? g_LanNetplaySettings.NumPlayers : 2;
		}

		// 底层 userlist 不完整时保留缓存中的已知玩家
		const std::vector<LanUserInfo> prev = NetplayLanAndroidController::GetInstance().GetCachedUserlistSnapshot();
		if (num_players >= 2 && (g_LanNetplaySettings.Mode == HostMode || g_LanNetplaySettings.Mode == ConnectMode))
		{
			for (int s = 0; s < num_players; ++s)
			{
				bool have = false;
				for (const LanUserInfo& u : lanUsers)
					if (u.side == s) { have = true; break; }
				if (have) continue;
				for (const LanUserInfo& u : prev)
					if (u.side == s && !u.name.empty()) { lanUsers.push_back(u); break; }
			}
		}

		_callback->SetUserlist(lanUsers, num_players);
	}

	void HandleChatMessage(const std::string& username, const std::string& message)
	{
		if (message.rfind("PS2LAN_", 0) == 0)
		{
			NetplayLanAndroidController::GetInstance().OnLanProtocolMessage(username, message);
			if (message.rfind("PS2LAN_V3", 0) == 0 || message.rfind("PS2LAN_ACK", 0) == 0)
				return;
			if (message.rfind("PS2LAN_MINIMIZE_LOBBY", 0) == 0)
				return;
		}
		if (_callback)
			_callback->AddChatMessage(username, message);
	}

	// ---- ILanNetplayPlugin ----

	void SetCallback(ILanNetplayDialogCallback* cb) override
	{
		_callback = cb;
	}

	void BeginLanLobbySession(void* /*dlg*/) override
	{
		// 用户反复进/退大厅或在上一次会话未彻底关闭时再次点击"进入大厅"会导致
		// _session 仍指向旧 shoryu::session 实例——它的 async_transport 里 peer map 可能
		// 还在被 ping_thread / send_thread 异步访问；Open() 里 `_session.reset(new session_type())`
		// 会在 Kotlin DefaultDispatch 线程上同步析构旧 session → 触发 peer shared_ptr 的
		// __release_shared，与工作线程竞争导致 SIGABRT（见用户堆栈：async_transport::stop() →
		// unordered_map::clear → ~shared_ptr<peer> → abort）。
		// 解决：在创建新会话前，强制先走 EndSession() 把旧会话的所有线程 join 干净。
		if (_session || _is_initialized)
			EndSession();
		g_LanNetplaySettings.LobbyPhaseOnly = true;
		Init();
		Open();
	}

	void BeginLanBootSession(void* /*dlg*/) override
	{
		// **关键**：lobby → boot 过渡时**不能**走 EndSession() 拆掉 shoryu 会话。
		// 旧实现无条件 EndSession() + Open() 里的 `_session.reset(new session_type())` + `bind()`
		// 会把大厅里已经跟 PC 房主握好手的 session 整个拆毁，然后再 Join() 一次——与 PC 两边
		// 的 reset/rejoin 竞态几乎必然错过，表现为 PC 那侧玩家数 2 → 1、本地 FrameSyncClient 起来了
		// 却没有任何人跟它对等，WaitForFrame(0) 永久黑屏。
		// 正确做法：若上一步 BeginLanLobbySession 已经建立并握手成功（_session 存在且已初始化），
		// 就**原地复用**这条会话，只把 LobbyPhaseOnly 翻到 false 后把 FrameSync/UDP relay hooks
		// 叠加上去；Open() 内部识别到复用路径会跳过 session reset / bind / Host+Join 线程重起。
		// 注意：NetplayLanAndroidController::OnVMStarted 会**先于**本函数把 LobbyPhaseOnly 置为 false；
		// 若用 LobbyPhaseOnly 参与复用判据，会永远为假 → 又走 EndSession() 拆掉大厅。只能看 _session。
		const bool reuse_lobby_session = (_session != nullptr) && _is_initialized;
		if (!reuse_lobby_session)
		{
			if (_session || _is_initialized)
				EndSession();
		}
		g_LanNetplaySettings.LobbyPhaseOnly = false;
		Init();
		Open();
	}

	void ShutdownLanBootSession() override
	{
		UnhookLanIOP();
		Close();
	}

	void SendChatMessage(const std::string& message) override
	{
		recursive_lock lock(_mutex);
		if (_session)
			_session->send_chatmessage(message);
	}

	// 等价 Qt 没实现但 shoryu 原生支持：把当前 peer msg_queue 里尚未被对端 ack 的消息
	// 再发一遍（serialize_datagram 会打包全部未 ack 的消息）。在 PS2LAN_ACK 这类
	// 一次性关键消息之后显式调用，可以在不新增消息 id 的前提下完成局域网重传，抗 UDP 丢包。
	void FlushSend() override
	{
		recursive_lock lock(_mutex);
		if (_session)
			_session->send();
	}

	bool WaitForSendQueueEmpty(uint32_t timeout_ms, uint32_t interval_ms) override
	{
		// 周期性调用 _session->send()（= async_transport::send → peer::serialize_datagram）。
		// send_n == peer::msg_queue.size()，所以 send_n == 0 代表对端已 ack 了所有入队消息。
		// peer::msg_queue 的元素在 deserialize_datagram 里随对端回包的 header.is_acknoledged 被删掉；
		// 因此只要对端继续往这边发任何数据（含 ping_thread 每 1s 的 ping 数据报），
		// 这边再次 send() 就会看到 queue 变空。
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeout_ms);
		const auto iv = std::chrono::milliseconds(interval_ms == 0 ? 30u : interval_ms);
		while (true)
		{
			int remaining = -1;
			{
				recursive_lock lock(_mutex);
				if (!_session)
					return true; // 会话已不存在，认为"不再有需要等待的队列"
				remaining = _session->send();
			}
			if (remaining == 0)
				return true;
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(iv);
		}
	}

	void BeginAckRetransmissionBurst(uint32_t duration_ms, uint32_t interval_ms) override
	{
		StopAckRetransmissionBurst();
		_ack_burst_stop.store(false);
		const uint32_t dur = (duration_ms == 0) ? 2000u : duration_ms;
		const uint32_t iv = (interval_ms == 0) ? 30u : interval_ms;
		_ack_burst_thread = std::thread([this, dur, iv]() {
			const auto start = std::chrono::steady_clock::now();
			const auto deadline = start + std::chrono::milliseconds(dur);
			while (!_ack_burst_stop.load())
			{
				{
					recursive_lock lock(_mutex);
					if (!_session) break;
					// send() 会序列化所有未 ack 的入队消息（包含 PS2LAN_ACK），
					// 对端 ack 后 session 会自动从 msg_queue 移除，不会无限累积。
					_session->send();
				}
				std::unique_lock<std::mutex> lk(_ack_burst_mutex);
				const auto now = std::chrono::steady_clock::now();
				if (now >= deadline) break;
				const auto wait = std::min<std::chrono::milliseconds>(
					std::chrono::milliseconds(iv),
					std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
				_ack_burst_cv.wait_for(lk, wait, [this]() { return _ack_burst_stop.load(); });
			}
		});
	}

	void Open() override
	{
		_is_stopped = false;
		NetplayLanSettings& settings = g_LanNetplaySettings;

		if (settings.HostPort <= 0 || settings.HostPort > 65535)
		{
			Stop(); Console.Error("NETPLAY LAN: Invalid host port: %u.", settings.HostPort); return;
		}
		if (settings.ListenPort <= 0 || settings.ListenPort > 65535)
		{
			Stop(); Console.Error("NETPLAY LAN: Invalid listen port: %u.", settings.ListenPort); return;
		}
		if (settings.Mode == ConnectMode && settings.HostAddress.empty())
		{
			Stop(); Console.Error("NETPLAY LAN: Invalid hostname."); return;
		}

		// 复用路径检测：lobby → boot 过渡（_session 已经存在且非 LobbyPhaseOnly）
		// → 不重建 shoryu session，不 bind 新端口，不重起 Host/Join 线程，
		// 只把 FrameSync / UDP relay / input hooks 叠加上去。必须与 PC 端对齐，
		// 任何一端若重建 session 都会导致对端 peer 丢失 → OnVSync 永远等不到第 0 帧。
		const bool reuse_existing_session = !g_LanNetplaySettings.LobbyPhaseOnly && _session != nullptr;

		if (!reuse_existing_session)
		{
			_handshake_gate_open = false;

			{
				recursive_lock lock(_mutex);
				shoryu::prepare_io_service();
				_session.reset(new session_type());
				_session->userlist_handler([this](std::vector<shoryu::userinfo> usernames) { HandleUsernames(usernames); });
				_session->set_chatmessage_handler([this](std::string username, std::string message) { HandleChatMessage(username, message); });

				const int localPort = (settings.Mode == HostMode) ? static_cast<int>(settings.ListenPort) : 0;
				if (!_session->bind(localPort))
				{
					lock.unlock();
					Stop();
					Console.Error("NETPLAY LAN: Unable to bind port %u.", localPort);
					return;
				}

				if (settings.Mode == HostMode)
					Console.WriteLn("NETPLAY LAN: Listening on port %u (lobby).", static_cast<unsigned>(localPort));
				else
					Console.WriteLn("NETPLAY LAN: Client socket ready (lobby). Connecting to host...");

				_state = SSNone;
				_session->username(settings.Username);
				_game_name.clear();

				// 注册聊天发送回调给 Controller
				// (由 JNI 侧通过 SendChatMessage 直接调用)
			}
		}
		else
		{
			Console.WriteLn(
				"NETPLAY LAN: Boot transition — REUSING existing lobby shoryu session "
				"(keeping peer connected). Layering FrameSync/UDP relay on top.");
		}

		// Boot 帧同步分支
		const bool bootUsesFsync = !g_LanNetplaySettings.LobbyPhaseOnly &&
			(settings.Mode == HostMode || settings.Mode == ConnectMode);
		bool boot_fsync_started = false;

		if (bootUsesFsync)
		{
			if (settings.Mode == HostMode)
			{
				_lan_udp_relay = std::make_unique<NetplayLanUdpRelay>();
				if (!_lan_udp_relay->Start(static_cast<unsigned short>(g_NetplayRoomState.udp_server_port),
						g_NetplayRoomState.protocol_version, g_NetplayRoomState.room_id))
				{
					Console.Error("NETPLAY LAN: UDP relay failed to bind port %u.",
						static_cast<unsigned>(g_NetplayRoomState.udp_server_port));
				}
			}
			_fsync.reset(new NetplayFrameSyncClient());
			if (!_fsync->Start(g_NetplayRoomState))
			{
				Stop();
				Console.Error("NETPLAY LAN: Unable to start frame sync client.");
				return;
			}
			_expected_frame = g_NetplayRoomState.initial_frame_id;
			_last_sync_error_report_ms = 0;
			_calib_skip_mod = 0;
			_calib_mod_counter = 0;
			_calib_frames_left = 0;
			{
				recursive_lock lk(_mutex);
				_state = SSReady;
			}
			if (_callback)
				_callback->OnConnectionEstablished(1);

			g_NetplayFrameSync = _fsync.get();
			// 强约束：房主只能驱动 1P (slot 0)，成员只能驱动 2P (slot 1)。
			// 产品契约与 Qt 对齐：两端游戏内对 1P/2P 的语义是"1P=房主, 2P=成员"。
			// PollInputOnCPUThread 会每帧把另一侧 pad 清零，彻底封掉"本端意外写对端 pad"通路，
			// 和 OnVSync 里的 `readPad = _is_host ? 0 : 1` 严格一致。
			const std::optional<uint32_t> exclusiveLocalSlot =
				(settings.Mode == HostMode) ? std::optional<uint32_t>(0u)
											: std::optional<uint32_t>(1u);
			NetplayFrameSyncRegisterQuickJoinStyleInputHooks(exclusiveLocalSlot);
			_boot_fsync_hooks_active = true;
			boot_fsync_started = true;

			Console.WriteLn("NETPLAY LAN: Frame sync client started (Boot Netplay).");

			{
				std::lock_guard<std::mutex> lk(_connection_mutex);
				_handshake_gate_open = true;
			}
			{
				recursive_lock lk2(_mutex);
				if (_state == SSReady)
					_state = SSRunning;
			}
			_ready_to_connect_cond.notify_all();
		}

		// 大厅阶段握手门
		if (g_LanNetplaySettings.LobbyPhaseOnly)
		{
			std::lock_guard<std::mutex> lk(_connection_mutex);
			_handshake_gate_open = true;
			Console.WriteLn("NETPLAY LAN: Lobby handshake gate open.");
		}

		// 复用老 session 的情况下已经有一条活着的 Host/Join 线程在跑，不再起新的——否则
		// 新旧线程会同时访问 _session（ping_thread / recv_thread / serialize_datagram），
		// 必然 SIGABRT；而且 boot 阶段本来就不需要再握一次手。
		if (!reuse_existing_session)
		{
			_connect_thread.reset(new std::thread([this]() {
			/* -fno-exceptions: no try/catch; Host()/Join() handle errors internally */
			if (g_LanNetplaySettings.Mode == HostMode)
				Host();
			else if (g_LanNetplaySettings.Mode == ConnectMode)
				Join(g_LanNetplaySettings.HostAddress,
					static_cast<unsigned short>(g_LanNetplaySettings.HostPort), 120000);
		}));
		}

		_ready_to_connect_cond.notify_all();

		if (!boot_fsync_started)
		{
			recursive_lock lock2(_mutex);
			_state = SSReady;
		}
	}

	bool IsInit() override { return _is_initialized; }

	void Init() override
	{
		_is_initialized = true;
		_is_stopped = false;
	}

	void Close() override
	{
		_is_initialized = false;
		EndSession();
	}

	void EndSession() override
	{
		{
			std::lock_guard<std::mutex> lk(_connection_mutex);
			_is_stopped = true;
			_handshake_gate_open = true;
		}
		_ready_to_connect_cond.notify_all();

		// 先终止 ACK 重传守护线程，避免它在 _session.reset() 之后仍持 _mutex 调用 _session->send()
		// 造成 use-after-free。内部会 notify + join，join 完成即保证线程不会再触达 _session。
		StopAckRetransmissionBurst();

		if (_boot_fsync_hooks_active)
		{
			if (g_NetplayFrameSync == _fsync.get())
				g_NetplayFrameSync = nullptr;
			NetplayFrameSyncUnregisterQuickJoinStyleInputHooks();
			_boot_fsync_hooks_active = false;
		}
		if (_lan_udp_relay)
		{
			_lan_udp_relay->Stop();
			_lan_udp_relay.reset();
		}
		if (_fsync)
		{
			_fsync->Stop();
			_fsync.reset();
		}

		recursive_lock lock(_mutex);

		if (_session)
		{
			if (_session->state() == shoryu::MessageType::Ready)
			{
				_session->send_end_session_request();
				int try_count = _session->delay() * 4;
				while (_session->send())
				{
					shoryu::sleep(17);
					if (try_count-- == 0) break;
				}
			}
			_session->shutdown();
			_session->unbind();
		}

		if (_connect_thread)
		{
			if (_connect_thread->joinable())
				_connect_thread->join();
			_connect_thread.reset();
		}

		_session.reset();

		if (_fault_thread)
		{
			_is_stopped = true;
			if (_fault_thread->joinable())
				_fault_thread->join();
			_fault_thread.reset();
		}
	}

	void Stop()
	{
		_is_stopped = true;
		EndSession();
		/* Android: Host::RequestVMShutdown is implemented in AndroidBridge.cpp */
		if (VMManager::HasValidVM())
			Host::RequestVMShutdown(false, false, false);
	}

	// ---- Frame handling (from IOPHook interface) ----

	void NextFrame() override
	{
		if (_is_stopped || !_session) return;
		_my_frame = NetplayLanMessage();
		if (_state == SSReady)
			_state = SSRunning;
		_expected_frame++;
	}

	void AcceptInput(int /*side*/) override { }

	int RemapVibrate(int pad) override
	{
		if (_is_stopped || !_session) return pad;
		if (pad == 0) return _session->side();
		return -1;
	}

	uint8_t HandleIO(int side, int index, uint8_t value) override
	{
		if (_is_stopped) return value;
		if (!_session) return value;

		// Boot + 帧同步：手柄在 NetplayFrameSyncClient::OnVSync 中采样，此处不重复读
		if (_boot_fsync_hooks_active)
			return value;

		NetplayLanMessage frame;
		if (side >= 2) return frame.input[index];
		if (side == _session->side())
			_my_frame.input[index] = value;

		auto timeout = shoryu::time_ms() + 10000;
		/* -fno-exceptions: no try/catch; session::get() returns bool */
		while (true)
		{
			if (_session->state() == shoryu::MessageType::None) { Stop(); break; }
			if (_session->get(side, frame, 5000)) break;
			if (timeout <= shoryu::time_ms())
			{
				Stop();
				Console.Error("NETPLAY LAN: Timeout on frame data.");
				break;
			}
		}
		return frame.input[index];
	}

	bool Join(const std::string& ip, unsigned short port, int timeout)
	{
		{
			std::unique_lock<std::mutex> connection_lock(_connection_mutex);
			_ready_to_connect_cond.wait(connection_lock, [&] {
				return _handshake_gate_open || _is_stopped;
			});
		}
		if (_is_stopped) return false;

		const std::optional<EmulatorSyncState> built = GetSessionSyncState();
		if (!built)
		{
			Console.Error("NETPLAY LAN: Join aborted — could not build sync state.");
			return false;
		}
		EmulatorSyncState state = *built;

		zed_net_address_t ep;
		zed_net_get_address(&ep, ip.c_str(), port);
		Console.WriteLn("NETPLAY LAN: Join → %s:%u", ip.c_str(), static_cast<unsigned>(port));

		bool joined = false;
		for (int r = 0; r < 20; r++)
		{
			if (_is_stopped) break;
			if (_session && _session->join(ep, state,
					[&](const EmulatorSyncState& s1, const EmulatorSyncState& s2) -> bool {
						return CheckSyncStates(s1, s2);
					},
					timeout))
			{
				joined = true;
				break;
			}
			if (_is_stopped) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		if (!joined)
		{
			Console.Error("NETPLAY LAN: join() failed (timeout, rejected, or sync state mismatch).");
			return false;
		}

		{
			recursive_lock lock(_mutex);
			if (!_session || _session->state() != shoryu::MessageType::Ready)
				return false;
			if (_callback)
				_callback->OnConnectionEstablished(_session->delay());
		}

		if (g_LanNetplaySettings.LobbyPhaseOnly)
			return true;

		if (_session)
			_session->wait_for_start(ep);

		if (_callback)
			_callback->SetStatus("Game started.");

		return true;
	}

	bool Host()
	{
		{
			std::unique_lock<std::mutex> connection_lock(_connection_mutex);
			_ready_to_connect_cond.wait(connection_lock, [&] {
				return _handshake_gate_open || _is_stopped;
			});
		}
		if (_is_stopped) return false;

		const std::optional<EmulatorSyncState> built = GetSessionSyncState();
		if (!built)
		{
			Console.Error("NETPLAY LAN: Host aborted — could not build sync state.");
			return false;
		}
		EmulatorSyncState state = *built;

		if (_callback)
			_callback->OnConnectionEstablished(1);

		const unsigned int numPlayers = 2;
		Console.WriteLn("NETPLAY LAN: Host create() — Listen Port=%u.",
			static_cast<unsigned>(g_LanNetplaySettings.ListenPort));

		if (!_session || !_session->create(numPlayers, state,
				[&](const EmulatorSyncState& s1, const EmulatorSyncState& s2) -> bool {
					return CheckSyncStates(s1, s2);
				}))
		{
			Console.Error("NETPLAY LAN: create() failed.");
			return false;
		}

		{
			recursive_lock lock(_mutex);
			if (!_session || _session->state() != shoryu::MessageType::Ready)
				return false;
		}

		// 等待 Kotlin 侧用户点击 Start（blocking）
		int delay = 0;
		if (g_LanNetplaySettings.SkipHostWaitAfterLobby)
		{
			g_LanNetplaySettings.SkipHostWaitAfterLobby = false;
			delay = NetplayLanAndroidController::GetInstance().GetInputDelay();
		}
		else
		{
			delay = NetplayLanAndroidController::GetInstance().WaitForConfirmation();
		}
		// WaitForConfirmation 仅在 Cancel 时返回 -1；UI 允许 inputDelay=0 时不应误判为取消。
		// shoryu session 的 input delay 必须 >= 1（与 PC PCSX2 UI 强制最小值 1 对齐），
		// 这里把 0 收敛到 1，避免房主点 Start 后 Host() 直接返回导致游戏进不去。
		if (delay < 0)
			return false;
		if (delay < 1)
			delay = 1;

		if (delay != _session->delay())
			_session->delay(delay);
		_session->reannounce_delay();

		if (g_LanNetplaySettings.LobbyPhaseOnly)
		{
			// 大厅阶段：通知 Controller 去触发游戏启动（通过 Kotlin 回调）
			NetplayLanAndroidController::GetInstance().hostLaunchGameAfterLobby();
			return true;
		}

		zed_net_address_t w{};
		if (_session)
			_session->wait_for_start(w);

		if (_callback)
			_callback->SetStatus("Game started.");

		return true;
	}

protected:
	bool CheckSyncStates(const EmulatorSyncState& s1, const EmulatorSyncState& s2)
	{
		if (memcmp(s1.biosVersion, s2.biosVersion, sizeof(s1.biosVersion)))
		{
			Console.Error("NETPLAY LAN: Bios version mismatch.");
			return false;
		}
		if (memcmp(s1.discId, s2.discId, sizeof(s1.discId)))
		{
			const char placeholder[] = "LAN_LOBBY____";
			if (memcmp(s2.discId, placeholder, sizeof(placeholder) - 1) == 0 ||
			    memcmp(s1.discId, placeholder, sizeof(placeholder) - 1) == 0)
			{
				// Soft rejection: lobby vs game race condition, will retry
			}
			else
			{
				Console.Error("NETPLAY LAN: Disc ID mismatch.");
			}
			return false;
		}
		if (s1.skipMpeg != s2.skipMpeg)
		{
			Console.Error("NETPLAY LAN: SkipMpegHack settings mismatch.");
			return false;
		}
		return true;
	}

	void StopAckRetransmissionBurst()
	{
		_ack_burst_stop.store(true);
		_ack_burst_cv.notify_all();
		if (_ack_burst_thread.joinable())
			_ack_burst_thread.join();
	}

	enum SessionState
	{
		SSNone,
		SSCancelled,
		SSReady,
		SSRunning
	} _state = SSNone;

	bool _is_initialized;
	bool _is_stopped;
	std::condition_variable _ready_to_connect_cond;
	std::mutex _connection_mutex;
	std::string _game_name;
	NetplayLanMessage _my_frame;
	ILanNetplayDialogCallback* _callback;
	std::recursive_mutex _mutex;
	typedef std::unique_lock<std::recursive_mutex> recursive_lock;
	bool _boot_fsync_hooks_active;
	bool _handshake_gate_open;

	// ACK 重传守护线程：在 BeginAckRetransmissionBurst 后以 interval_ms 周期调用 FlushSend()，
	// 最长运行 duration_ms 毫秒。EndSession 进入时会先把 _ack_burst_stop 置 true + notify_all，
	// 再 join 线程，保证不会在 _session 已被 reset 后仍访问。
	std::thread _ack_burst_thread;
	std::mutex _ack_burst_mutex;
	std::condition_variable _ack_burst_cv;
	std::atomic<bool> _ack_burst_stop{false};
};

// ======================== Singleton ========================
ILanNetplayPlugin* ILanNetplayPlugin::instance = nullptr;

ILanNetplayPlugin& ILanNetplayPlugin::GetInstance()
{
	if (!instance)
		instance = new NetplayLanPluginImpl();
	return *instance;
}
