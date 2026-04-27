// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Android 适配版 NetplayLanPlugin。
// 去掉所有 Qt 依赖：QString → std::string，QMetaObject::invokeMethod → std::thread / callback。
// 协议/状态机与 PC 版完全一致。

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include <string>
#include <optional>

#include "Netplay/NetplayLanAndroidController.h"

// Forward declarations
class NetplayFrameSyncClient;
class NetplayLanUdpRelay;

//
// IOPHook interface — Android 版与 PC 版相同
//
class LanIOPHook
{
public:
    virtual ~LanIOPHook() = default;
    virtual uint8_t HandleIO(int side, int index, uint8_t value) = 0;
    virtual void NextFrame() = 0;
    virtual void AcceptInput(int side) = 0;
    virtual int RemapVibrate(int pad) = 0;
};

//
// INetplayPlugin interface
//
class ILanNetplayPlugin : public LanIOPHook
{
protected:
    static ILanNetplayPlugin* instance;

public:
    static ILanNetplayPlugin& GetInstance();

    virtual void Open() = 0;
    virtual void Init() = 0;
    virtual bool IsInit() = 0;
    virtual void EndSession() = 0;
    virtual void Close() = 0;

    /// 大厅阶段（无 VM）
    virtual void BeginLanLobbySession(void* dlg) = 0;
    /// VM 已运行后完整联机
    virtual void BeginLanBootSession(void* dlg) = 0;
    virtual void ShutdownLanBootSession() = 0;

    /// 发送聊天消息
    virtual void SendChatMessage(const std::string& message) = 0;

    /// 主动触发一次 shoryu session 的 send() —— 用于对"已经入队但还没被对端 ack"的消息
    /// 进行显式重传。shoryu 成员侧没有自动 ping 线程（ping_thread 仅房主启动），
    /// 对于关键一次性消息（如 PS2LAN_ACK）需要显式多次 flush 以抵御局域网 UDP 丢包，
    /// 否则单包丢失会导致 PC 房主 hostBootWait 超时并一直卡黑屏不启动。
    virtual void FlushSend() = 0;

    /// 异步启动一个"ACK 重传守护线程"：在指定 duration 内每 ~interval 毫秒调用一次 FlushSend()，
    /// 直到：① 到时；② shoryu session 被 Close() 置空；③ 新一轮 BeginAckRetransmissionBurst 覆盖。
    /// 用于成员端在发完 PS2LAN_ACK 后立即返回，由后台线程继续兜底重传，避免"成员 VM 已开始启动、
    /// shoryu 成员侧又没有自动 ping"窗口内 ACK 丢包导致 PC 房主永远卡在 hostBootWait。
    virtual void BeginAckRetransmissionBurst(uint32_t duration_ms, uint32_t interval_ms) = 0;

    /// 同步阻塞当前线程，周期性调用 _session->send()（会触发 shoryu 的未 ack 消息重传 +
    /// 对端的 ack 头回带）直到 peer msg_queue 为空，或超时。
    ///
    /// 返回：
    ///   true  — 在超时前 msg_queue 已清空，说明最后一条关键消息（如 PS2LAN_ACK）已被对端确认；
    ///           此时调用方可以安全地进入下一阶段（例如启动 VM / 拆掉 shoryu lobby）。
    ///   false — 到时未清空，可能 peer 没回应；调用方应兜底继续（产品行为上仍启动 VM），
    ///           或上报断线。
    ///
    /// 使用场景（关键）：成员端发送 PS2LAN_ACK 之后，必须等到房主真的确认这一条才能
    /// 调用 RequestLaunchGame；否则 Kotlin 随即启动 VM → OnVMStarted → ShutdownLanBootSession
    /// 会把 shoryu lobby 直接干掉，PS2LAN_ACK 如果恰好处在"已入队未 ack"状态就再也发不出去，
    /// 表现为"安卓直接进游戏、PC 一直黑屏"。
    virtual bool WaitForSendQueueEmpty(uint32_t timeout_ms, uint32_t interval_ms) = 0;

    /// 向 JNI 回调注册用户列表/聊天处理
    virtual void SetCallback(ILanNetplayDialogCallback* cb) = 0;
};

// Hook/Unhook the IOP for LAN netplay
void HookLanIOP(LanIOPHook* hook);
void UnhookLanIOP();

// The global IOP hook pointer (for PAD callback interception)
extern LanIOPHook* g_LanIOPHook;
