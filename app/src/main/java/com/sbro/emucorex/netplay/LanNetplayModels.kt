package com.sbro.emucorex.netplay

// ─────────────────────────────────────────────────────────────────────────────
// LAN 联机 Kotlin 数据模型
// ─────────────────────────────────────────────────────────────────────────────

/** 联机模式（与 C++ NetplayLanMode 对齐：CONNECT=0, HOST=1, OBSERVE=2）。 */
enum class LanNetplayMode { CONNECT, HOST, OBSERVE }

/**
 * 联机会话设置。字段完全对齐 pcsx2-qt 的 `NetplayLanSettings`：
 *  - [hostAddress] / [hostPort]    Connect 模式用；
 *  - [listenPort]                  Host 模式用；
 *  - [observe]                     Observe 模式（仅连接，不占用手柄槽，不参与帧同步）；
 *  - [saveReplay] / [readonlyMemcard] / [clientOnlyDelay] / [memcardSync]  与 Qt 对话框一一对应；
 *  - [numPlayers]                  目前固定 2（房主 1P / 成员 2P）。
 *
 *  [port] 保留作为兼容老签名的入口（host 时等于 listenPort，connect 时等于 hostPort）。
 */
data class LanNetplaySettings(
    val username: String = "",
    val mode: LanNetplayMode = LanNetplayMode.CONNECT,
    val hostAddress: String = "",
    val hostPort: Int = 7500,
    val listenPort: Int = 7500,
    val observe: Boolean = false,
    val saveReplay: Boolean = false,
    val memcardSync: Boolean = true,
    val clientOnlyDelay: Boolean = true,
    val readonlyMemcard: Boolean = false,
    val numPlayers: Int = 2,
) {
    /** 兼容字段：老代码还有地方用 [port]，等价于 mode==HOST?listenPort:hostPort。 */
    val port: Int get() = if (mode == LanNetplayMode.HOST) listenPort else hostPort
}

/** 大厅玩家信息 */
data class LanPlayerInfo(
    val name: String,
    val ping: String,
    val side: Int,
)

/** 大厅 UI 状态 */
sealed class LanNetplayUiState {
    /** 初始/未连接 */
    object Idle : LanNetplayUiState()

    /** 已进入大厅，等待玩家 */
    data class Lobby(
        val statusText: String = "",
        val players: List<LanPlayerInfo> = emptyList(),
        val numPlayers: Int = 2,
        val connected: Boolean = false,
        val inputDelay: Int = 1,
        val chatMessages: List<ChatMessage> = emptyList(),
    ) : LanNetplayUiState()

    /** 房主：等待成员选 ISO + ACK */
    data class HostWaitingForGuest(val statusText: String) : LanNetplayUiState()

    /** 成员：需要选择 ISO（CRC 校验） */
    data class GuestSelectIso(
        val expectedCrcHex: String,
        val serial: String,
        val hostHadCheats: Boolean,
        val fairPlayNetplay: Boolean,
        val cheatFiles: List<CheatFileEntry>,
    ) : LanNetplayUiState()

    /** 游戏启动中 */
    data class LaunchingGame(val isoPath: String) : LanNetplayUiState()

    /** 错误 */
    data class Error(val message: String) : LanNetplayUiState()
}

data class ChatMessage(
    val username: String,
    val text: String,
    val isSystem: Boolean = false,
)

/**
 * 一次性事件（由 [LanNetplayRepository] 通过 `SharedFlow` 发射）。
 * UI 层收到事件后再做导航/最小化等副作用，避免与 [LanNetplayUiState] 冗余。
 */
sealed class LanNetplayEvent {
    /** 房主/成员都会收到：把联机页最小化（Qt 对应 NetplayLanDialog::MinimizeLobby）。 */
    object MinimizeLobby : LanNetplayEvent()
    /** 请成员弹出 ISO 选择（仅成员）。 */
    data class RequestIso(
        val expectedCrcHex: String,
        val serial: String,
        val hostHadCheats: Boolean,
        val fairPlayNetplay: Boolean,
        val cheatFiles: List<CheatFileEntry>,
    ) : LanNetplayEvent()
    /** 帧同步通道就绪，可跳转 EmulationRoute 启动 VM。 */
    data class LaunchGame(val isoPath: String, val enableCheats: Boolean) : LanNetplayEvent()
    /** 错误提示（也会同步到 UiState.Error，但 UI 可另行 snackbar）。 */
    data class Error(val message: String) : LanNetplayEvent()
}

data class CheatFileEntry(
    val name: String,
    val data: ByteArray,
) {
    override fun equals(other: Any?): Boolean =
        other is CheatFileEntry && name == other.name && data.contentEquals(other.data)
    override fun hashCode(): Int = 31 * name.hashCode() + data.contentHashCode()
}
