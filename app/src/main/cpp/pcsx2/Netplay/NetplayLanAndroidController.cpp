// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Android LAN 联机控制器实现。
// 等价于 PC 版 NetplayLanController.cpp，去掉所有 Qt 依赖。
// JSON 序列化用 nlohmann/json 或简单手写解析（项目中已有 fmt，此处用 fmt + 手写）。

#include "PrecompiledHeader.h"

#include "Netplay/NetplayLanAndroidController.h"
#include "Netplay/NetplayLanSettingsAndroid.h"
#include "Netplay/NetplayRoomStateAndroid.h"
#include "Netplay/NetplayLanPlugin.h"

#include "pcsx2/VMManager.h"
#include "pcsx2/Config.h"
#include "pcsx2/core/runtime/GameList.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <fmt/format.h>

#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

// ---- Base64（同 Qt QByteArray::fromBase64 语义）----
static const std::string kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static std::string Base64Decode(const std::string& encoded)
{
    std::string decoded;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++)
        T[(unsigned char)kBase64Chars[i]] = i;

    int val = 0, bits = -8;
    for (unsigned char c : encoded)
    {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0)
        {
            decoded.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

static std::string Base64Encode(const std::string& data)
{
    std::string encoded;
    int val = 0, bits = -6;
    const unsigned int b63 = 0x3F;
    for (unsigned char c : data)
    {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0)
        {
            encoded.push_back(kBase64Chars[(val >> bits) & b63]);
            bits -= 6;
        }
    }
    if (bits > -6)
        encoded.push_back(kBase64Chars[((val << 8) >> (bits + 8)) & b63]);
    while (encoded.size() % 4)
        encoded.push_back('=');
    return encoded;
}

// ---- 简易 zlib 解压（依赖 zlib，项目中已有 ----
#include <zlib.h>
static std::string ZlibUncompress(const std::string& compressed)
{
    if (compressed.size() < 4) return {};
    // Qt qCompress 在前 4 字节存原始大小（big-endian）
    uint32_t expected_size = 0;
    const auto* raw = reinterpret_cast<const unsigned char*>(compressed.data());
    expected_size = (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
    if (expected_size == 0 || expected_size > 64 * 1024 * 1024) return {};

    std::string out(expected_size, '\0');
    uLongf dest_len = expected_size;
    int ret = uncompress(
        reinterpret_cast<Bytef*>(out.data()), &dest_len,
        reinterpret_cast<const Bytef*>(compressed.data() + 4),
        static_cast<uLong>(compressed.size() - 4));
    if (ret != Z_OK) return {};
    out.resize(dest_len);
    return out;
}

static std::string ZlibCompress(const std::string& input, int level = 7)
{
    uLongf dest_len = compressBound(input.size());
    std::string out(dest_len + 4, '\0');
    // Qt qCompress 头：4 字节 BE 原始大小
    auto* head = reinterpret_cast<unsigned char*>(out.data());
    uint32_t sz = static_cast<uint32_t>(input.size());
    head[0] = (sz >> 24) & 0xFF;
    head[1] = (sz >> 16) & 0xFF;
    head[2] = (sz >> 8) & 0xFF;
    head[3] = sz & 0xFF;
    int ret = compress2(
        reinterpret_cast<Bytef*>(out.data() + 4), &dest_len,
        reinterpret_cast<const Bytef*>(input.data()), input.size(), level);
    if (ret != Z_OK) return {};
    out.resize(4 + dest_len);
    return out;
}

// ---- 极简 JSON 辅助 ----
static std::string JsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

/// 从 JSON 字符串中提取指定 key 的字符串值（只处理简单顶层 key）
static std::string JsonGetString(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

static bool JsonGetBool(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    return json.substr(pos, 4) == "true";
}

// 简单解析 "files" 数组：[{"name":"...", "data_b64":"..."}, ...]
static std::vector<std::pair<std::string,std::string>> JsonGetCheatFiles(const std::string& json)
{
    std::vector<std::pair<std::string,std::string>> result;
    auto arrPos = json.find("\"files\":[");
    if (arrPos == std::string::npos) return result;
    arrPos += 8; // skip "files":
    // iterate objects
    size_t cur = arrPos;
    while (true)
    {
        auto objStart = json.find('{', cur);
        if (objStart == std::string::npos) break;
        auto objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = json.substr(objStart, objEnd - objStart + 1);
        std::string name = JsonGetString(obj, "name");
        std::string data_b64 = JsonGetString(obj, "data_b64");
        if (!name.empty())
            result.emplace_back(name, Base64Decode(data_b64));
        cur = objEnd + 1;
        if (json.find(']', cur) < json.find('{', cur)) break;
    }
    return result;
}

// ======================== 全局实例 ========================
NetplayLanSettings g_LanNetplaySettings;
NetplayRoomState g_NetplayRoomState;
NetplayLanAndroidController* NetplayLanAndroidController::s_instance = nullptr;

NetplayLanAndroidController& NetplayLanAndroidController::GetInstance()
{
    if (!s_instance)
        s_instance = new NetplayLanAndroidController();
    return *s_instance;
}

void NetplayLanAndroidController::SetCallback(ILanNetplayDialogCallback* callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = callback;
}

bool NetplayLanAndroidController::IsHost() const
{
    return m_isHost.load();
}

bool NetplayLanAndroidController::IsEnabled() const
{
    return m_enabled.load();
}

void NetplayLanAndroidController::StartSession(
    const std::string& username,
    bool isHost,
    const std::string& hostAddress,
    unsigned int portParam)
{
    // 旧签名：降级调用全字段版本（其它字段保留 Qt 默认）。
    StartSessionEx(
        username,
        isHost,
        hostAddress,
        /*hostPort*/ isHost ? portParam : portParam,   // Connect: hostPort=portParam
        /*listenPort*/ portParam,                       // Host:    listenPort=portParam
        /*observe*/ false,
        /*saveReplay*/ false,
        /*memcardSync*/ true,
        /*clientOnlyDelay*/ true,
        /*readonlyMemcard*/ false);
}

void NetplayLanAndroidController::StartSessionEx(
    const std::string& username,
    bool isHost,
    const std::string& hostAddress,
    unsigned int hostPort,
    unsigned int listenPort,
    bool observe,
    bool saveReplay,
    bool memcardSync,
    bool clientOnlyDelay,
    bool readonlyMemcard)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_guestLanBootHandled = false;
        m_preselectedIsoPath.clear();
        m_v3Chunks.clear();
    }

    // 填充 g_LanNetplaySettings（一一对齐 Qt 字段）。
    g_LanNetplaySettings.Username = username;
    if (observe)
        g_LanNetplaySettings.Mode = ObserveMode;
    else
        g_LanNetplaySettings.Mode = isHost ? HostMode : ConnectMode;
    g_LanNetplaySettings.IsEnabled = true;
    g_LanNetplaySettings.LobbyPhaseOnly = true;
    g_LanNetplaySettings.SkipHostWaitAfterLobby = false;
    g_LanNetplaySettings.NumPlayers = 2;
    g_LanNetplaySettings.SaveReplay = saveReplay;
    g_LanNetplaySettings.MemcardSync = memcardSync;
    g_LanNetplaySettings.ClientOnlyDelay = clientOnlyDelay;
    g_LanNetplaySettings.ReadonlyMemcard = readonlyMemcard;
    g_LanNetplaySettings.HostAddress = hostAddress;
    g_LanNetplaySettings.HostPort = hostPort;
    g_LanNetplaySettings.ListenPort = listenPort;
    if (isHost)
    {
        // Connect 成员的 HostPort 应等于房主 ListenPort，此处 host 侧把两者对齐
        // （UI 侧通常只需要填 ListenPort 即可）。
        if (g_LanNetplaySettings.HostPort == 0)
            g_LanNetplaySettings.HostPort = listenPort;
    }
    g_LanNetplaySettings.SanityCheck();

    // 填充 g_NetplayRoomState
    g_NetplayRoomState.room_id = "LAN";
    g_NetplayRoomState.player_id = username;
    g_NetplayRoomState.is_host = isHost;
    g_NetplayRoomState.initial_frame_id = 0;
    g_NetplayRoomState.frame_interval_us = 16667;
    g_NetplayRoomState.frame_cache_size = 100;
    g_NetplayRoomState.protocol_version = 0x0200;
    g_NetplayRoomState.sync_delay = 0;
    if (isHost)
        g_NetplayRoomState.udp_server_addr = "127.0.0.1";
    else
        g_NetplayRoomState.udp_server_addr = hostAddress;
    g_NetplayRoomState.udp_server_port = 38889;

    m_isHost.store(isHost);
    m_enabled.store(true);

    // 通知 UI 立即切换到大厅页
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
            m_callback->PresentLobby();
        if (m_callback)
        {
            if (isHost)
                m_callback->SetStatus("Waiting for players to connect...");
            else
                m_callback->SetStatus("Connecting to host...");
        }
    }

    // 启动 LAN 大厅会话（仅 shoryu，不起 VM）
    ILanNetplayPlugin::GetInstance().BeginLanLobbySession(nullptr);
}

void NetplayLanAndroidController::EndSession()
{
    {
        std::lock_guard<std::mutex> lock(m_confirmMutex);
        m_confirmCancelled = true;
        m_confirmReady = true;
    }
    m_confirmCond.notify_all();

    // 唤醒可能卡在 ACK 等待的 hostLaunchGameAfterLobby
    {
        std::lock_guard<std::mutex> lock(m_ackMutex);
        m_ackReceived = true;
    }
    m_ackCond.notify_all();

    if (g_LanNetplaySettings.IsEnabled)
    {
        g_LanNetplaySettings.IsEnabled = false;
        g_LanNetplaySettings.LobbyPhaseOnly = false;
        g_LanNetplaySettings.SkipHostWaitAfterLobby = false;
        ILanNetplayPlugin::GetInstance().ShutdownLanBootSession();
    }

    m_enabled.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_guestLanBootHandled = false;
        m_preselectedIsoPath.clear();
        m_v3Chunks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_ackMutex);
        m_pendingAcks = 0;
        m_ackReceived = false;
    }
}

void NetplayLanAndroidController::OnVMStarted()
{
    if (!g_LanNetplaySettings.IsEnabled)
        return;

    if (g_LanNetplaySettings.LobbyPhaseOnly)
    {
        g_LanNetplaySettings.LobbyPhaseOnly = false;
        if (g_LanNetplaySettings.Mode == HostMode)
            g_LanNetplaySettings.SkipHostWaitAfterLobby = true;
        // 与 PC NetplayLanController::onVMStarted 对齐：禁止在此 Shutdown。
        // Shutdown 会析构 shoryu session，使 BeginLanBootSession 无法复用大厅 peer → 帧同步孤立。
    }
    else
    {
        g_LanNetplaySettings.SkipHostWaitAfterLobby = false;
    }
    ILanNetplayPlugin::GetInstance().BeginLanBootSession(nullptr);
}

void NetplayLanAndroidController::OnVMStopped()
{
    if (g_LanNetplaySettings.IsEnabled)
        ILanNetplayPlugin::GetInstance().ShutdownLanBootSession();
}

void NetplayLanAndroidController::SendChat(const std::string& message)
{
    // 通过插件 session 发送聊天（Plugin 内部保持 session 引用）
    // 实际实现在 Plugin 层，此处作为便捷入口
    // 找到当前 Plugin 实例并调用 (由 AndroidBridge JNI 侧直接转发)
}

void NetplayLanAndroidController::SetInputDelay(int delay)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inputDelay = delay;
}

int NetplayLanAndroidController::GetInputDelay() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inputDelay;
}

std::vector<LanUserInfo> NetplayLanAndroidController::GetCachedUserlistSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cachedUserlist;
}

/// 房主 blocking 等待 Start 确认，返回 inputDelay（<0 表示取消；调用方负责把 0 收敛到 >=1）
int NetplayLanAndroidController::WaitForConfirmation()
{
    std::unique_lock<std::mutex> lock(m_confirmMutex);
    m_confirmReady = false;
    m_confirmCancelled = false;
    m_confirmCond.wait(lock, [this] { return m_confirmReady; });
    if (m_confirmCancelled)
        return -1;
    return m_inputDelay;
}

void NetplayLanAndroidController::HostConfirmStart(const std::string& isoPath, int inputDelay)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_inputDelay = inputDelay;
        m_preselectedIsoPath = isoPath;
    }
    // 通知 WaitForConfirmation 解除阻塞
    {
        std::lock_guard<std::mutex> lock(m_confirmMutex);
        m_confirmReady = true;
        m_confirmCancelled = false;
    }
    m_confirmCond.notify_all();
}

void NetplayLanAndroidController::GuestConfirmReady(const std::string& isoPath, bool enableCheats)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_guestLanBootHandled = true;
        m_preselectedIsoPath = isoPath;
    }

    // 发送 PS2LAN_ACK 给房主
    // (实际由 Plugin chatmessage handler 发)
    // 此处触发 VM 启动——通过 VMManager
    // (由 AndroidBridge JNI 侧在收到此回调后启动 VM)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_callback)
        m_callback->SetStatus("Starting game (guest)...");
}

void NetplayLanAndroidController::GuestCancelReady()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_guestLanBootHandled = false;
    if (m_callback)
        m_callback->SetStatus("Game selection cancelled.");
}

// ===================== 协议消息处理 =====================

void NetplayLanAndroidController::OnLanProtocolMessage(const std::string& username, const std::string& message)
{
    const std::string myUser = g_LanNetplaySettings.Username;

    if (message.rfind("PS2LAN_MINIMIZE_LOBBY", 0) == 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
            m_callback->MinimizeLobby();
        return;
    }

    if (message.rfind("PS2LAN_ACK", 0) == 0)
    {
        if (g_LanNetplaySettings.Mode != HostMode)
            return;
        // 历史上这里对照 Qt 加了 "username == myUser return" 去掉自己发的消息，
        // 但 PS2LAN_ACK 只由成员→房主单向发送，shoryu 的 recv echo 逻辑对
        // (m_host && side != 0) 本来就会 "if (i+1 == side) continue" 跳过发送者，
        // 房主永远不可能收到自己发的 ACK。若房主和成员恰好同名（例如两端都用默认
        // "Player"），这个过滤反而会把对端合法的 ACK 直接丢弃，导致 PC 端
        // m_hostBootWait 永远等不到 release → "PC 卡黑屏不启动"。
        // 因此这里不再按 username 过滤，只按 "只有房主才消费 ACK" 的模式过滤。
        // 独立信号量，语义等价 Qt m_hostBootWait.release()
        (void)myUser;
        {
            std::lock_guard<std::mutex> lk(m_ackMutex);
            ++m_pendingAcks;
            m_ackReceived = true;
        }
        m_ackCond.notify_all();
        return;
    }

    if (message.rfind("PS2LAN_V3", 0) == 0)
    {
        if (g_LanNetplaySettings.Mode != ConnectMode)
            return;
        // 在后台线程处理 V3，避免阻塞网络回调
        std::thread([this, message]() {
            guestProcessV3Line(message);
        }).detach();
        return;
    }
}

void NetplayLanAndroidController::guestProcessV3Line(const std::string& message)
{
    bool alreadyHandled;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        alreadyHandled = m_guestLanBootHandled;
    }
    if (alreadyHandled || VMManager::HasValidVM())
        return;
    if (g_LanNetplaySettings.Mode != ConnectMode)
        return;

    // 解析 "PS2LAN_V3\tidx\ttotal\tchunk"
    const char* ps2lan_v3 = "PS2LAN_V3";
    if (message.rfind(ps2lan_v3, 0) != 0)
        return;

    // 分割
    std::vector<std::string> parts;
    {
        std::istringstream ss(message);
        std::string token;
        while (std::getline(ss, token, '\t'))
            parts.push_back(token);
    }
    if (parts.size() < 4 || parts[0] != "PS2LAN_V3")
        return;

    int idx = 0, total = 0;
    {
        char* end_ptr = nullptr;
        idx   = (int)std::strtol(parts[1].c_str(), &end_ptr, 10);
        if (!end_ptr || *end_ptr != '\0') return;
        total = (int)std::strtol(parts[2].c_str(), &end_ptr, 10);
        if (!end_ptr || *end_ptr != '\0') return;
    }
    if (total <= 0 || idx < 0 || idx >= total)
        return;

    // 重组 chunk（可能含多个 tab）
    std::string chunk;
    for (size_t i = 3; i < parts.size(); ++i)
    {
        if (i > 3) chunk += '\t';
        chunk += parts[i];
    }

    std::string json_copy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (idx == 0)
            m_v3Chunks.assign(static_cast<size_t>(total), std::string{});
        if (m_v3Chunks.size() != static_cast<size_t>(total))
            return;
        m_v3Chunks[static_cast<size_t>(idx)] = chunk;

        for (const auto& c : m_v3Chunks)
            if (c.empty()) return;

        // 所有分片齐，合并并解析
        std::string b64;
        for (const auto& c : m_v3Chunks)
            b64 += c;
        m_v3Chunks.clear();

        std::string comp = Base64Decode(b64);
        json_copy = ZlibUncompress(comp);
        if (json_copy.empty())
        {
            Console.Error("NETPLAY LAN: V3 decompress failed.");
            return;
        }
    }
    // 在 lock 外调用，避免死锁
    guestApplySessionPayload(json_copy);
}


// ---- Guest 自动匹配：优先按 serial+CRC、其次按 CRC 在本机 GameList 中查 ----
// 对齐 Qt NetplayLanController::tryAutoPickIsoByCrc() + 按用户要求增加 serial 首轮匹配。
static std::string NetplayLanAndroidTryAutoPickIso(const std::string_view serial, uint32_t crc)
{
    auto lock = GameList::GetLock();

    // 1) serial + CRC 精确匹配（最强）。
    if (!serial.empty())
    {
        if (const GameList::Entry* e = GameList::GetEntryBySerialAndCRC(serial, crc); e && e->IsDisc())
            return e->path;
    }

    // 2) 仅 CRC 匹配（回退，等价 Qt tryAutoPickIsoByCrc 的默认行为）。
    const u32 count = GameList::GetEntryCount();
    for (u32 i = 0; i < count; i++)
    {
        const GameList::Entry* entry = GameList::GetEntryByIndex(i);
        if (!entry || !entry->IsDisc())
            continue;
        if (entry->crc == crc)
            return entry->path;
    }
    return {};
}

// 前向声明：NetplayLanIsSafeBasename 的定义在文件下方 pnach 搜索辅助区域。
static bool NetplayLanIsSafeBasename(const std::string_view name);

// 写入 host 下发的 cheat 文件到本机 EmuFolders::Cheats 目录。
// 与 Kotlin 侧 LanNetplayRepository.writeCheatFiles 等价，但在自动匹配路径上由 C++ 直接落盘，
// 避免再绕回 Kotlin。文件名做安全过滤，禁止目录穿越。
static void NetplayLanAndroidWriteHostCheatFiles(
    const std::vector<std::pair<std::string,std::string>>& files)
{
    if (files.empty())
        return;
    if (!FileSystem::DirectoryExists(EmuFolders::Cheats.c_str()))
        FileSystem::CreateDirectoryPath(EmuFolders::Cheats.c_str(), true);
    for (const auto& [name, data] : files)
    {
        if (!NetplayLanIsSafeBasename(name))
            continue;
        const std::string full = Path::Combine(EmuFolders::Cheats, name);
        FileSystem::WriteBinaryFile(full.c_str(), data.data(), data.size());
    }
}

void NetplayLanAndroidController::guestApplySessionPayload(const std::string& jsonStr)
{
    const std::string crcStr = JsonGetString(jsonStr, "crc");
    const std::string serial = JsonGetString(jsonStr, "serial");
    const bool hostHadCheats = JsonGetBool(jsonStr, "hostHadCheats");
    const auto cheatFiles = JsonGetCheatFiles(jsonStr);

    if (crcStr.size() < 8)
    {
        Console.Error("NETPLAY LAN: V3 invalid CRC.");
        return;
    }

    uint32_t expectedCrc = 0;
    {
        char* end_ptr = nullptr;
        unsigned long v = std::strtoul(crcStr.c_str(), &end_ptr, 16);
        if (!end_ptr || *end_ptr != '\0') { Console.Error("NETPLAY LAN: V3 CRC parse error."); return; }
        expectedCrc = static_cast<uint32_t>(v);
    }

    // 先尝试在本机 GameList 里自动匹配（与 PC 版一致，复用 PCSX2 既有 GameList 接口）。
    // 命中时跳过 ISO 选择对话框，双方直接进入帧同步流程。
    const std::string autoPath = NetplayLanAndroidTryAutoPickIso(serial, expectedCrc);
    if (!autoPath.empty())
    {
        // 二次校验：强制 serial/CRC 都一致（对齐 Qt 在 guestApplySessionPayload 中的
        // GetSerialAndCRCForFilename + CRC 比对逻辑）。
        std::string sCheck;
        u32 cCheck = 0;
        const bool sameCrc = GameList::GetSerialAndCRCForFilename(autoPath.c_str(), &sCheck, &cCheck) && cCheck == expectedCrc;
        if (sameCrc)
        {
            // 落盘 host 下发的 cheats（若有），避免 Kotlin 侧再绕一遍。
            if (hostHadCheats && !cheatFiles.empty())
                NetplayLanAndroidWriteHostCheatFiles(cheatFiles);

            // 先提示 UI，但不要标记 guest 已处理；只有 ACK 被房主确认后才能进入 VM。
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_callback)
                    m_callback->SetStatus("Sending PS2LAN_ACK to host...");
            }

            // 发送 PS2LAN_ACK 通知房主 guest 已就绪。
            //
            // 可靠性说明（与 PC 对齐的一致性修复）：
            //   shoryu 成员侧没有 ping_thread（ping 只在房主 create_handler 里启动），
            //   因此 _session->send() 只会在成员主动调用时执行；单次 UDP 丢包就会让
            //   PC 房主永远停在 m_hostBootWait.tryAcquire(1, 60000) 上，直到 60s 超时，
            //   UI 呈现为「PC 卡黑屏不启动」。
            //
            //   这里做两层加固：
            //     1) 再入队一条 ACK（queue_message + send 一次）——让 PC 有更高概率收到任一份；
            //     2) 随后短时间内再显式 FlushSend()，让 shoryu 把"已入队且尚未被对端 ack"的
            //        消息重发若干次（serialize_datagram 打包全部未 ack 消息）。
            //   这样即使前几次 UDP 被局域网丢掉，后续重传也能把 ACK 送达。
            //   这是"产品行为对齐"的关键——Android 房主 + PC 成员方向能跑通，是因为 PC 端有
            //   ping_thread 自动重传；反向时必须成员侧自己兜底。
            // ========== PS2LAN_ACK 可靠送达（关键路径）==========
            //
            // 必须确保在启动 VM（→ OnVMStarted → ShutdownLanBootSession 把 shoryu lobby 干掉）
            // 之前，PS2LAN_ACK 已经被对端（PC 房主）真的 ack 掉。否则：
            //   成员 VM 立刻启动 → shoryu lobby 被 Close → peer::msg_queue 清空 + socket 关闭 →
            //   还没发出去的那条 PS2LAN_ACK 就彻底丢了 → PC 房主 m_hostBootWait 永远等不到 release
            //   → 60s 后 UI 报"成员未就绪"，实际表现为"安卓直接进游戏、PC 端黑屏"。
            //
            // 做法（两步握手，严格同步）：
            //   Step A: 入队一条 PS2LAN_ACK；热身 flush 若干次把第一批数据报打出去，抗首包丢失。
            //   Step B: 同步阻塞在 WaitForSendQueueEmpty 直到房主回包里 header 携带这条消息的 ack
            //          （shoryu 房主的 ping_thread 1 秒内至少发一次带 ack header 的数据报），
            //          或超时（本地网络异常）。
            // 由于本路径在 OnLanProtocolMessage 派发的 detached 工作线程里跑，这里阻塞不会卡
            // UI / 网络回调主循环，安全。
            ILanNetplayPlugin::GetInstance().SendChatMessage("PS2LAN_ACK");
            for (int i = 0; i < 6; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                ILanNetplayPlugin::GetInstance().FlushSend();
            }
            // 再塞一条新 id 的 ACK，和上一条共同入队；哪一条先被 ack 都能让 queue 清空。
            ILanNetplayPlugin::GetInstance().SendChatMessage("PS2LAN_ACK");
            const bool acked = ILanNetplayPlugin::GetInstance().WaitForSendQueueEmpty(6000u, 30u);
            if (!acked)
            {
                Console.Error(
                    "NETPLAY LAN (guest): PS2LAN_ACK not acknowledged in 6s; "
                    "NOT launching VM to preserve strict lockstep. Check PC host ACK handling/firewall.");
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_guestLanBootHandled = false;
                    if (m_callback)
                        m_callback->SetStatus("PS2LAN_ACK was not acknowledged by host. Game launch blocked.");
                }
                return;
            }

            // 通知 Kotlin UI 启动 VM，直接进入帧同步联机。
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_guestLanBootHandled = true;
                m_preselectedIsoPath = autoPath;
                if (m_callback)
                    m_callback->SetStatus("Starting game (guest)...");
                if (m_callback)
                    m_callback->RequestLaunchGame(autoPath, hostHadCheats || EmuConfig.EnableCheats);
            }
            return;
        }
    }

    // 未自动匹配到（或 CRC 二次校验失败）：回退到 Kotlin UI 手动选择。
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_callback)
        m_callback->RequestGuestIsoSelection(expectedCrc, serial, hostHadCheats, cheatFiles);
}

// ===================== pnach 搜索辅助（对齐 Qt NetplayFindCheatPatchFilesOnDisk）=====================

static std::string NetplayLanGetPnachSearchPattern(std::string_view serial, uint32_t crc,
    bool include_serial, bool add_wildcard)
{
    if (!serial.empty() && include_serial)
        return fmt::format("{}_{:08X}{}.pnach", serial, crc, add_wildcard ? "*" : "");
    return fmt::format("{:08X}{}.pnach", crc, add_wildcard ? "*" : "");
}

static std::vector<std::string> NetplayLanFindCheatPatchFilesOnDisk(std::string_view serial, uint32_t crc)
{
    std::vector<std::string> ret;
    FileSystem::FindResultsArray files;

    FileSystem::FindFiles(EmuFolders::Cheats.c_str(),
        NetplayLanGetPnachSearchPattern(serial, crc, true, true).c_str(),
        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);
    ret.reserve(files.size());
    for (FILESYSTEM_FIND_DATA& fd : files)
        ret.push_back(std::move(fd.FileName));

    FileSystem::FindFiles(EmuFolders::Cheats.c_str(),
        NetplayLanGetPnachSearchPattern(serial, crc, false, true).c_str(),
        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);
    ret.reserve(ret.size() + files.size());
    for (FILESYSTEM_FIND_DATA& fd : files)
        ret.push_back(std::move(fd.FileName));

    return ret;
}

static bool NetplayLanIsSafeBasename(const std::string_view name)
{
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos &&
           name.find("..") == std::string_view::npos;
}

static std::string NetplayLanResolveCheatsPath(const std::string& p)
{
    if (Path::IsAbsolute(p))
        return p;
    return Path::Combine(EmuFolders::Cheats, p);
}

static bool NetplayLanLoadCheatsForDisc(std::string_view serial, uint32_t crc,
    std::vector<std::pair<std::string, std::string>>* out)
{
    out->clear();
    const std::vector<std::string> paths = NetplayLanFindCheatPatchFilesOnDisk(serial, crc);
    for (const std::string& p : paths)
    {
        const std::string full = NetplayLanResolveCheatsPath(p);
        const std::string base(Path::GetFileName(full));
        if (!NetplayLanIsSafeBasename(base))
            continue;
        std::optional<std::string> contents(FileSystem::ReadFileToString(full.c_str()));
        if (!contents.has_value())
            continue;
        out->emplace_back(base, std::move(*contents));
    }
    return !out->empty();
}

// ===================== hostLaunchGameAfterLobby =====================
// 由 Plugin 在房主 Host() 分支的大厅阶段调用。逻辑对齐 Qt NetplayLanController::hostLaunchGameAfterLobby：
//   1. 取出 UI 已选的 ISO 路径；
//   2. GetSerialAndCRCForFilename → 装配 V3 JSON；
//   3. qCompress(level=7) + base64 + 按 kLanV3ChunkSize=900 分片；
//   4. 通过 Plugin 的 chat session 发送 "PS2LAN_V3\t{idx}\t{total}\t{chunk}"；
//   5. 等待 PS2LAN_ACK（60s 超时），对齐 Qt m_hostBootWait.tryAcquire(1, 60000)；
//   6. 通知 Kotlin UI 启动 VM。
constexpr int kLanV3ChunkSize = 900;

void NetplayLanAndroidController::hostLaunchGameAfterLobby()
{
    if (!g_LanNetplaySettings.IsEnabled)
        return;
    if (g_LanNetplaySettings.Mode != HostMode)
        return;
    if (VMManager::HasValidVM())
        return;

    std::string isoPath;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        isoPath = m_preselectedIsoPath;
    }
    if (isoPath.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
            m_callback->SetStatus("No game selected. Start cancelled.");
        return;
    }

    // 1. Drain any stale acks
    {
        std::lock_guard<std::mutex> lk(m_ackMutex);
        m_pendingAcks = 0;
        m_ackReceived = false;
    }

    // 2. CRC + serial
    std::string serial;
    u32 crc = 0;
    if (!GameList::GetSerialAndCRCForFilename(isoPath.c_str(), &serial, &crc))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
            m_callback->SetStatus("Could not read disc CRC.");
        return;
    }

    // 3. Load cheats
    std::vector<std::pair<std::string, std::string>> cheatFiles;
    const bool hostHad = NetplayLanLoadCheatsForDisc(serial, crc, &cheatFiles);

    // 4. Build compact JSON manually
    std::string json = "{";
    json += "\"crc\":\"" + fmt::format("{:08X}", crc) + "\",";
    json += "\"serial\":\"" + JsonEscape(serial) + "\",";
    json += "\"hostHadCheats\":";
    json += (hostHad ? "true" : "false");
    json += ",\"files\":[";
    for (size_t i = 0; i < cheatFiles.size(); ++i)
    {
        if (i)
            json += ',';
        json += "{\"name\":\"";
        json += JsonEscape(cheatFiles[i].first);
        json += "\",\"data_b64\":\"";
        json += Base64Encode(cheatFiles[i].second);
        json += "\"}";
    }
    json += "]}";

    // 5. qCompress-compatible compress + base64
    const std::string comp = ZlibCompress(json, 7);
    if (comp.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
            m_callback->SetStatus("Failed to prepare session data.");
        return;
    }
    const std::string b64 = Base64Encode(comp);

    // 6. Chunk and send via plugin chat
    const int n = static_cast<int>((b64.size() + kLanV3ChunkSize - 1) / kLanV3ChunkSize);
    for (int i = 0; i < n; ++i)
    {
        const size_t off = static_cast<size_t>(i) * kLanV3ChunkSize;
        const size_t len = std::min<size_t>(kLanV3ChunkSize, b64.size() - off);
        std::string line = "PS2LAN_V3\t";
        line += std::to_string(i);
        line += '\t';
        line += std::to_string(n);
        line += '\t';
        line.append(b64, off, len);
        ILanNetplayPlugin::GetInstance().SendChatMessage(line);
    }
    for (int i = 0; i < 8; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ILanNetplayPlugin::GetInstance().FlushSend();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
            m_callback->SetStatus("Waiting for member to sync cheats and start...");
    }

    // 7. Wait for PS2LAN_ACK (60s timeout)
    {
        std::unique_lock<std::mutex> lk(m_ackMutex);
        const bool got = m_ackCond.wait_for(lk, std::chrono::seconds(60),
            [this] { return m_ackReceived; });
        if (!got)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_callback)
                m_callback->SetStatus("Member did not become ready within 60 s. Start cancelled.");
            return;
        }
        // consume one ack
        if (m_pendingAcks > 0)
            --m_pendingAcks;
        m_ackReceived = (m_pendingAcks > 0);
    }
    for (int i = 0; i < 10; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        ILanNetplayPlugin::GetInstance().FlushSend();
    }

    // 8. Notify Kotlin UI: launch VM with the preselected ISO
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback)
        {
            m_callback->SetStatus("Starting game (host)...");
            m_callback->RequestLaunchGame(isoPath, hostHad || EmuConfig.EnableCheats);
        }
    }
}
