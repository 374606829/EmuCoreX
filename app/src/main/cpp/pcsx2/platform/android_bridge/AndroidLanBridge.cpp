// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// LAN 联机 JNI 桥接层
// 独立文件，追加编译到 emucore 库（与 AndroidBridge.cpp 同模块）。
// 所有方法均以 Java_com_sbro_emucorex_core_NativeApp_lan 为前缀。

#include "PrecompiledHeader.h"

#include "Netplay/NetplayLanAndroidController.h"
#include "Netplay/NetplayLanPlugin.h"
#include "Netplay/NetplayLanSettingsAndroid.h"
#include "Netplay/NetplayRoomStateAndroid.h"

#include "common/Console.h"

#include <jni.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <chrono>

// ─────────────────────────────────────────────────
// JNI 工具：从 jstring 取 std::string
// ─────────────────────────────────────────────────
static std::string JStringToStdString(JNIEnv* env, jstring jstr)
{
    if (!jstr) return {};
    const char* raw = env->GetStringUTFChars(jstr, nullptr);
    std::string result(raw ? raw : "");
    env->ReleaseStringUTFChars(jstr, raw);
    return result;
}

static jstring StdStringToJString(JNIEnv* env, const std::string& s)
{
    return env->NewStringUTF(s.c_str());
}

// ─────────────────────────────────────────────────
// Kotlin 回调 ViewModel 的 Java 方法 IDs（缓存）
// ─────────────────────────────────────────────────
static std::mutex g_jniCallbackMutex;
static JavaVM* g_jvm = nullptr;
static jobject g_lanCallbackObj = nullptr;  // 全局引用，指向 Kotlin LanNetplayCallbackTarget

// 方法 ID 缓存（setStatus, addChatMessage, setUserlist, onConnectionEstablished, …）
struct LanCallbackMethodIds
{
    jmethodID setStatus = nullptr;
    jmethodID addChatMessage = nullptr;
    jmethodID setUserlist = nullptr;
    jmethodID onConnectionEstablished = nullptr;
    jmethodID requestLaunchGame = nullptr;
    jmethodID requestGuestIsoSelection = nullptr;
    jmethodID presentLobby = nullptr;
    jmethodID minimizeLobby = nullptr;
} g_lanMids;

static bool g_lanMidsReady = false;

// ─────────────────────────────────────────────────
// Callback impl：把 C++ 事件转发到 Kotlin
// ─────────────────────────────────────────────────
class JniLanCallbackImpl : public ILanNetplayDialogCallback
{
    JNIEnv* GetEnv() const
    {
        JNIEnv* env = nullptr;
        if (g_jvm)
            g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        return env;
    }

    JNIEnv* AttachOrGetEnv(bool& didAttach) const
    {
        didAttach = false;
        if (!g_jvm) return nullptr;
        JNIEnv* env = nullptr;
        jint res = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (res == JNI_EDETACHED)
        {
            JavaVMAttachArgs args{JNI_VERSION_1_6, "LanNetplay", nullptr};
            if (g_jvm->AttachCurrentThread(&env, &args) == JNI_OK)
                didAttach = true;
            else
                return nullptr;
        }
        return env;
    }

    void DetachIfNeeded(bool didAttach) const
    {
        if (didAttach && g_jvm)
            g_jvm->DetachCurrentThread();
    }

public:
    void SetStatus(const std::string& status) override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        jstring js = StdStringToJString(env, status);
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.setStatus, js);
        env->DeleteLocalRef(js);
        DetachIfNeeded(didAttach);
    }

    void AddChatMessage(const std::string& username, const std::string& message) override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        jstring ju = StdStringToJString(env, username);
        jstring jm = StdStringToJString(env, message);
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.addChatMessage, ju, jm);
        env->DeleteLocalRef(ju);
        env->DeleteLocalRef(jm);
        DetachIfNeeded(didAttach);
    }

    void SetUserlist(const std::vector<LanUserInfo>& users, int numPlayers) override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        // 编码为 "name|ping|side,name|ping|side,..." 字符串，Kotlin 侧解析
        std::string encoded;
        for (const auto& u : users)
        {
            if (!encoded.empty()) encoded += ',';
            encoded += u.name + '|' + u.ping + '|' + std::to_string(u.side);
        }
        jstring jenc = StdStringToJString(env, encoded);
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.setUserlist, jenc, (jint)numPlayers);
        env->DeleteLocalRef(jenc);
        DetachIfNeeded(didAttach);
    }

    void OnConnectionEstablished(int delay) override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.onConnectionEstablished, (jint)delay);
        DetachIfNeeded(didAttach);
    }

    void RequestLaunchGame(const std::string& gamePath, bool enableCheats) override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        jstring jp = StdStringToJString(env, gamePath);
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.requestLaunchGame, jp, (jboolean)enableCheats);
        env->DeleteLocalRef(jp);
        DetachIfNeeded(didAttach);
    }

    void RequestGuestIsoSelection(uint32_t expectedCrc, const std::string& serial,
                                  bool hostHadCheats,
                                  const std::vector<std::pair<std::string,std::string>>& cheatFiles) override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        // 序列化 cheatFiles 为 "name\001data_hex,name\001data_hex,..."
        // Kotlin 侧解析后写入 cheats 目录
        std::string cheatEncoded;
        for (const auto& [name, data] : cheatFiles)
        {
            if (!cheatEncoded.empty()) cheatEncoded += '\x1E'; // RS separator
            // encode data as base64 for safe transfer
            // (简单起见：此处直接传 name，大小限制在 JNI 可接受范围)
            cheatEncoded += name + '\x1F' + data; // US separator
        }
        char crcBuf[16];
        snprintf(crcBuf, sizeof(crcBuf), "%08X", expectedCrc);
        jstring jcrc    = StdStringToJString(env, crcBuf);
        jstring jserial = StdStringToJString(env, serial);
        jstring jcheats = StdStringToJString(env, cheatEncoded);
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.requestGuestIsoSelection,
                            jcrc, jserial, (jboolean)hostHadCheats, jcheats);
        env->DeleteLocalRef(jcrc);
        env->DeleteLocalRef(jserial);
        env->DeleteLocalRef(jcheats);
        DetachIfNeeded(didAttach);
    }

    void PresentLobby() override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.presentLobby);
        DetachIfNeeded(didAttach);
    }

    void MinimizeLobby() override
    {
        std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
        if (!g_lanCallbackObj || !g_lanMidsReady) return;
        bool didAttach;
        JNIEnv* env = AttachOrGetEnv(didAttach);
        if (!env) return;
        env->CallVoidMethod(g_lanCallbackObj, g_lanMids.minimizeLobby);
        DetachIfNeeded(didAttach);
    }
};

static JniLanCallbackImpl g_jniLanCallback;

// ─────────────────────────────────────────────────
// JNI 注册回调对象
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanRegisterCallback(
    JNIEnv* env, jclass /*clazz*/, jobject callbackObj)
{
    env->GetJavaVM(&g_jvm);
    std::lock_guard<std::mutex> lk(g_jniCallbackMutex);
    if (g_lanCallbackObj)
    {
        env->DeleteGlobalRef(g_lanCallbackObj);
        g_lanCallbackObj = nullptr;
    }
    if (!callbackObj)
    {
        g_lanMidsReady = false;
        NetplayLanAndroidController::GetInstance().SetCallback(nullptr);
        ILanNetplayPlugin::GetInstance().SetCallback(nullptr);
        return;
    }

    g_lanCallbackObj = env->NewGlobalRef(callbackObj);
    jclass cls = env->GetObjectClass(g_lanCallbackObj);

    g_lanMids.setStatus              = env->GetMethodID(cls, "setStatus",              "(Ljava/lang/String;)V");
    g_lanMids.addChatMessage         = env->GetMethodID(cls, "addChatMessage",         "(Ljava/lang/String;Ljava/lang/String;)V");
    g_lanMids.setUserlist            = env->GetMethodID(cls, "setUserlist",            "(Ljava/lang/String;I)V");
    g_lanMids.onConnectionEstablished= env->GetMethodID(cls, "onConnectionEstablished","(I)V");
    g_lanMids.requestLaunchGame      = env->GetMethodID(cls, "requestLaunchGame",      "(Ljava/lang/String;Z)V");
    g_lanMids.requestGuestIsoSelection=env->GetMethodID(cls, "requestGuestIsoSelection","(Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V");
    g_lanMids.presentLobby           = env->GetMethodID(cls, "presentLobby",           "()V");
    g_lanMids.minimizeLobby          = env->GetMethodID(cls, "minimizeLobby",          "()V");
    env->DeleteLocalRef(cls);

    g_lanMidsReady = (g_lanMids.setStatus != nullptr);

    NetplayLanAndroidController::GetInstance().SetCallback(&g_jniLanCallback);
    ILanNetplayPlugin::GetInstance().SetCallback(&g_jniLanCallback);
}

// ─────────────────────────────────────────────────
// 开始联机会话
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanStartSession(
    JNIEnv* env, jclass /*clazz*/,
    jstring username, jboolean isHost,
    jstring hostAddress, jint port)
{
    // 旧签名：字段不全 → 降级到 Ex 版，其余取 Qt 默认
    const unsigned int p = static_cast<unsigned int>(port);
    NetplayLanAndroidController::GetInstance().StartSessionEx(
        JStringToStdString(env, username),
        static_cast<bool>(isHost),
        JStringToStdString(env, hostAddress),
        /*hostPort*/ p,
        /*listenPort*/ p,
        /*observe*/ false,
        /*saveReplay*/ false,
        /*memcardSync*/ true,
        /*clientOnlyDelay*/ true,
        /*readonlyMemcard*/ false);
}

// 全字段联机入口（与 Qt NetplayLanSettings 字段对齐）。
extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanStartSessionEx(
    JNIEnv* env, jclass /*clazz*/,
    jstring username, jboolean isHost, jstring hostAddress,
    jint hostPort, jint listenPort,
    jboolean observe, jboolean saveReplay,
    jboolean memcardSync, jboolean clientOnlyDelay, jboolean readonlyMemcard)
{
    NetplayLanAndroidController::GetInstance().StartSessionEx(
        JStringToStdString(env, username),
        static_cast<bool>(isHost),
        JStringToStdString(env, hostAddress),
        static_cast<unsigned int>(hostPort),
        static_cast<unsigned int>(listenPort),
        static_cast<bool>(observe),
        static_cast<bool>(saveReplay),
        static_cast<bool>(memcardSync),
        static_cast<bool>(clientOnlyDelay),
        static_cast<bool>(readonlyMemcard));
}

// ─────────────────────────────────────────────────
// 结束联机会话
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanEndSession(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    NetplayLanAndroidController::GetInstance().EndSession();
}

// ─────────────────────────────────────────────────
// 发送聊天
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanSendChat(
    JNIEnv* env, jclass /*clazz*/, jstring message)
{
    ILanNetplayPlugin::GetInstance().SendChatMessage(JStringToStdString(env, message));
}

// ─────────────────────────────────────────────────
// VM 生命周期通知
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanOnVmStarted(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    NetplayLanAndroidController::GetInstance().OnVMStarted();
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanOnVmStopped(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    NetplayLanAndroidController::GetInstance().OnVMStopped();
}

// ─────────────────────────────────────────────────
// 房主：确认 Start（已选好 ISO）
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanHostConfirmStart(
    JNIEnv* env, jclass /*clazz*/, jstring isoPath, jint inputDelay)
{
    NetplayLanAndroidController::GetInstance().HostConfirmStart(
        JStringToStdString(env, isoPath),
        static_cast<int>(inputDelay));
}

// ─────────────────────────────────────────────────
// 成员：确认就绪（CRC 校验通过后）
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanGuestConfirmReady(
    JNIEnv* env, jclass /*clazz*/, jstring isoPath, jboolean enableCheats)
{
    // 成员确认就绪后发 PS2LAN_ACK，并同步阻塞在 WaitForSendQueueEmpty 上，
    // 直到 PC 房主在 header 里 ack 了这条关键消息才返回。
    //
    // 必须同步阻塞的原因参考 NetplayLanAndroidController::guestApplySessionPayload：
    // Kotlin 侧本函数返回后会立刻发射 LaunchGame → 启动 VM → OnVMStarted →
    // ShutdownLanBootSession 把 shoryu lobby Close 掉；若 PS2LAN_ACK 此时还没被 ack，
    // 消息就会随 session 一起丢失，PC 房主永远卡在 hostBootWait → PC 黑屏。
    //
    // 注意：Kotlin 侧需把 LanNetplayNative.guestConfirmReady 调度到 Dispatchers.IO，
    // 否则主线程会被阻塞最多 6s（只是"产品要求同步握手"的代价，不是 ANR 级的卡死，
    // 但仍不应占住 UI 线程）。
    ILanNetplayPlugin::GetInstance().SendChatMessage("PS2LAN_ACK");
    for (int i = 0; i < 6; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        ILanNetplayPlugin::GetInstance().FlushSend();
    }
    ILanNetplayPlugin::GetInstance().SendChatMessage("PS2LAN_ACK");
    const bool acked = ILanNetplayPlugin::GetInstance().WaitForSendQueueEmpty(6000u, 30u);
    if (!acked)
    {
        Console.Error(
            "NETPLAY LAN (guest, manual pick): PS2LAN_ACK not acknowledged in 6s; "
            "NOT launching VM to preserve strict lockstep. Check PC host ACK handling/firewall.");
        return JNI_FALSE;
    }

    NetplayLanAndroidController::GetInstance().GuestConfirmReady(
        JStringToStdString(env, isoPath),
        static_cast<bool>(enableCheats));
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanGuestCancelReady(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    NetplayLanAndroidController::GetInstance().GuestCancelReady();
}

// ─────────────────────────────────────────────────
// 查询状态
// ─────────────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanIsEnabled(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    return static_cast<jboolean>(NetplayLanAndroidController::GetInstance().IsEnabled());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanIsHost(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    return static_cast<jboolean>(NetplayLanAndroidController::GetInstance().IsHost());
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorex_core_NativeApp_lanSetInputDelay(
    JNIEnv* /*env*/, jclass /*clazz*/, jint delay)
{
    NetplayLanAndroidController::GetInstance().SetInputDelay(static_cast<int>(delay));
}
