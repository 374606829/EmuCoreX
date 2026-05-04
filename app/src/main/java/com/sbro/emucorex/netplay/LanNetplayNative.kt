package com.sbro.emucorex.netplay

import com.sbro.emucorex.core.NativeApp
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.File

// ─────────────────────────────────────────────────────────────────────────────
// JNI 方法声明（对应 AndroidLanBridge.cpp）
// 由 NativeApp object 持有，确保 emucore 库已加载
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin 侧 JNI 包装。直接用 NativeApp 前缀统一调用，与现有 AndroidBridge 模式一致。
 * 所有 external fun 声明在 NativeApp.kt 中（或扩展此处）；此处通过 NativeApp 代理。
 */
object LanNetplayNative {

    fun registerCallback(cb: NativeLanCallback?) {
        NativeApp.lanRegisterCallback(cb)
    }

    /** 旧签名兼容入口（保留，便于过渡期 / 外部调用者）。内部会降级到 Ex 版。 */
    fun startSession(username: String, isHost: Boolean, hostAddress: String, port: Int) {
        NativeApp.lanStartSession(username, isHost, hostAddress, port)
    }

    /** 全字段入口：与 Qt NetplayLanSettings 一致。统一建议调用此版本。 */
    fun startSession(settings: LanNetplaySettings) {
        NativeApp.lanStartSessionEx(
            settings.username,
            settings.mode == LanNetplayMode.HOST,
            settings.hostAddress,
            settings.hostPort,
            settings.listenPort,
            settings.observe || settings.mode == LanNetplayMode.OBSERVE,
            settings.saveReplay,
            settings.memcardSync,
            settings.clientOnlyDelay,
            settings.readonlyMemcard,
        )
    }

    fun endSession() = NativeApp.lanEndSession()

    fun sendChat(message: String) = NativeApp.lanSendChat(message)

    fun onVmStarted() = NativeApp.lanOnVmStarted()

    fun onVmStopped() = NativeApp.lanOnVmStopped()

    fun hostConfirmStart(isoPath: String, inputDelay: Int, fairPlayNetplay: Boolean = false) =
        NativeApp.lanHostConfirmStart(isoPath, inputDelay, fairPlayNetplay)

    fun guestConfirmReady(isoPath: String, enableCheats: Boolean): Boolean =
        NativeApp.lanGuestConfirmReady(isoPath, enableCheats)

    fun guestCancelReady() = NativeApp.lanGuestCancelReady()

    fun isEnabled(): Boolean = NativeApp.lanIsEnabled()

    /** 公平联机勾选时为 true；未勾选时即时存档/解除帧限制等不被会话强行禁用。 */
    fun fairPlayNetplay(): Boolean = NativeApp.lanFairPlayNetplay()

    fun isHost(): Boolean = NativeApp.lanIsHost()
    fun setInputDelay(delay: Int) = NativeApp.lanSetInputDelay(delay)

    /** 运行中将会话内当前游戏的 pnach 正文广播给对端（非公平联机且会话启用时）。 */
    fun broadcastRuntimeCheatPnach(text: String) = NativeApp.lanBroadcastRuntimeCheatPnach(text)
}

// ─────────────────────────────────────────────────────────────────────────────
// 从 C++ 接收回调的接口，由 AndroidLanBridge.cpp JNI 层调用
// ─────────────────────────────────────────────────────────────────────────────

/**
 * 传给 lanRegisterCallback 的回调对象。
 * JNI 层通过反射找到这些方法（方法名/签名必须与 AndroidLanBridge.cpp 中一致）。
 */
interface NativeLanCallback {
    fun setStatus(status: String)
    fun addChatMessage(username: String, message: String)
    /** encodedList = "name|ping|side,name|ping|side,..." */
    fun setUserlist(encodedList: String, numPlayers: Int)
    fun onConnectionEstablished(delay: Int)
    fun requestLaunchGame(gamePath: String, enableCheats: Boolean)
    fun requestGuestIsoSelection(crcHex: String, serial: String, hostHadCheats: Boolean, fairPlayNetplay: Boolean, cheatData: String)
    fun presentLobby()
    fun minimizeLobby()

    /**
     * 联机握手 / 启动期检测到双端 EmulatorSyncState 不一致。
     * 由 NetplayLanPlugin::CheckSyncStates 通过 JNI bridge 上报；
     * Kotlin 侧应弹 Toast 并把 UI 切到 [LanNetplayUiState.Error]，让用户明确为什么进不去房间。
     *
     * @param reason     "bios" / "disc_id" / "skip_mpeg" / "game_crc"
     * @param localValue 本机字段值（例如 "scph10000.bin" / "SLUS-21184" / "B18E3F1C" / "on"）
     * @param peerValue  对端字段值，含义同上
     */
    fun onSyncMismatch(reason: String, localValue: String, peerValue: String)
}
