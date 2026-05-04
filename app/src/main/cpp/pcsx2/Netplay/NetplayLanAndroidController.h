// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Android LAN 联机控制器：等价于 PC 版 NetplayLanController，
// 去掉 Qt 信号槽，改用 std::function 回调 + std::mutex。
// 所有回调均在内部线程调用，调用方负责切换至 Kotlin 主线程（通过 JNI）。

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <atomic>

/// 玩家信息（用于大厅玩家列表）
struct LanUserInfo
{
    std::string name;
    std::string ping;
    int side = 0;
};

/// 联机状态通知（由原生层回调到 Kotlin）
struct ILanNetplayDialogCallback
{
    virtual ~ILanNetplayDialogCallback() = default;

    /// 更新大厅状态文字
    virtual void SetStatus(const std::string& status) = 0;

    /// 收到聊天消息
    virtual void AddChatMessage(const std::string& username, const std::string& message) = 0;

    /// 更新玩家列表
    virtual void SetUserlist(const std::vector<LanUserInfo>& users, int numPlayers) = 0;

    /// 连接建立（delay 为帧延迟）
    virtual void OnConnectionEstablished(int delay) = 0;

    /// 房主：游戏已选定，需要启动 VM（gamePath 为 ISO 路径）
    virtual void RequestLaunchGame(const std::string& gamePath, bool enableCheats) = 0;

    /// 成员：已收到 V3 会话数据，需要弹出 ISO 选择（验证 CRC 后调 ConfirmGuestReady）
    virtual void RequestGuestIsoSelection(uint32_t expectedCrc, const std::string& serial,
                                          bool hostHadCheats,
                                          bool fairPlayNetplay,
                                          const std::vector<std::pair<std::string,std::string>>& cheatFiles) = 0;

    /// 已进入大厅（点击 Host/Connect 后立即切页）
    virtual void PresentLobby() = 0;

    /// 最小化大厅窗口（房主发 PS2LAN_MINIMIZE_LOBBY）
    virtual void MinimizeLobby() = 0;

    /// 联机握手 / 启动期检测到双端 EmulatorSyncState 不一致。
    /// reason 取值（见 NetplayLanPlugin::CheckSyncStates 与 LanNetplayRepository.formatMismatchMessage）：
    ///   "bios"      → BIOS 文件名不一致；
    ///   "disc_id"   → 游戏序列号不一致；
    ///   "skip_mpeg" → SkipMPEGHack 设置不一致；
    ///   "game_crc"  → 镜像 CRC32 不一致（仅由 Kotlin 侧手动 ISO 选择路径触发）。
    /// localValue / peerValue 为对应字段的人类可读值，已在 native 侧 trim 末尾 \0；
    /// Kotlin 侧直接用作 Toast / Error 文案。
    virtual void OnSyncMismatch(const std::string& reason,
                                const std::string& localValue,
                                const std::string& peerValue) = 0;
};

class NetplayLanAndroidController
{
public:
    static NetplayLanAndroidController& GetInstance();

    /// 设置回调（通常由 JNI 层实现并注册）
    void SetCallback(ILanNetplayDialogCallback* callback);

    /// 用户点击 Host 或 Connect 时调用（旧签名，等价于 StartSessionEx 的默认值版）
    void StartSession(
        const std::string& username,
        bool isHost,
        const std::string& hostAddress,    // Connect 时填房主 IP，Host 时留空
        unsigned int hostPortOrListenPort  // Connect: host port；Host: listen port
    );

    /// 全字段联机入口（对齐 Qt NetplayLanSettings）。
    void StartSessionEx(
        const std::string& username,
        bool isHost,
        const std::string& hostAddress,
        unsigned int hostPort,
        unsigned int listenPort,
        bool observe,
        bool saveReplay,
        bool memcardSync,
        bool clientOnlyDelay,
        bool readonlyMemcard
    );

    /// 用户取消 / 关闭大厅
    void EndSession();

    /// VM 已启动（等价于 Qt 版 onVMStarted 槽）
    void OnVMStarted();

    /// VM 已停止（等价于 Qt 版 onVMStopped 槽）
    void OnVMStopped();

    /// 发送聊天消息
    void SendChat(const std::string& message);

    /// 房主：点击 Start，选好 ISO 后调此接口（fairPlayNetplay 仅房主有效）
    void HostConfirmStart(const std::string& isoPath, int inputDelay, bool fairPlayNetplay);

    /// 成员：ISO 选好并 CRC 验证通过后调此接口（发 PS2LAN_ACK）
    void GuestConfirmReady(const std::string& isoPath, bool enableCheats);

    /// 成员放弃（取消 ISO 选择）
    void GuestCancelReady();

    /// 设置输入延迟（房主在大厅可调）
    void SetInputDelay(int delay);

    /// 查询当前是否 Host 模式
    bool IsHost() const;
    bool IsEnabled() const;

    /// 运行中：将会话内当前游戏的 pnach 正文广播给对端（公平联机或未启用会话时不发送）。
    void BroadcastRuntimeCheatPnachUtf8(const std::string& utf8);

    // ---- 协议消息处理（由 Plugin 聊天回调调用）----
    void OnLanProtocolMessage(const std::string& username, const std::string& message);

    // ---- 供 Plugin 使用的大厅接口 ----
    int WaitForConfirmation();    // 房主blocking等待 Start 确认，返回 inputDelay
    int GetInputDelay() const;
    std::vector<LanUserInfo> GetCachedUserlistSnapshot() const;

    /// 写回最新的成员列表快照到 m_cachedUserlist。
    /// 由 NetplayLanPlugin::HandleUsernames() 在每次推送给 UI 之前调用，
    /// 用于在 ping_clients 周期偶发推送 partial userlist 时，让下次合并能用上一帧补齐。
    /// 双安卓场景下若 Info 包到达顺序异常或部分包丢失，缓存可避免 UI 抖回到「空列表」。
    void SetCachedUserlist(const std::vector<LanUserInfo>& users);

    void hostLaunchGameAfterLobby();  // 移至 public：Plugin 需直接调用

private:
    NetplayLanAndroidController() = default;
    ~NetplayLanAndroidController() = default;

    void onConnectionSettingsReady();
    void guestProcessV3Line(const std::string& message);
    void guestApplySessionPayload(const std::string& jsonStr);
    void processRuntimeCheatChunkLine(const std::string& message);

    ILanNetplayDialogCallback* m_callback = nullptr;
    mutable std::mutex m_mutex;

    // ---- Host start 确认同步 ----
    std::mutex m_confirmMutex;
    std::condition_variable m_confirmCond;
    bool m_confirmReady = false;
    bool m_confirmCancelled = false;
    int m_inputDelay = 1;

    // ---- Host 等待 PS2LAN_ACK（与 Qt m_hostBootWait 语义一致，独立于 m_confirmCond）----
    std::mutex m_ackMutex;
    std::condition_variable m_ackCond;
    int m_pendingAcks = 0;
    bool m_ackReceived = false;

    // ---- 成员 V3 分片 ----
    std::vector<std::string> m_v3Chunks;
    // ---- 运行中金手指 pnach 同步（PS2LAN_CH）----
    std::mutex m_rtChMutex;
    std::vector<std::string> m_rtChChunks;
    bool m_guestLanBootHandled = false;
    std::string m_preselectedIsoPath;

    // ---- 状态 ----
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_isHost{false};

    // ---- 用户列表缓存 ----
    std::vector<LanUserInfo> m_cachedUserlist;

    static NetplayLanAndroidController* s_instance;
};
