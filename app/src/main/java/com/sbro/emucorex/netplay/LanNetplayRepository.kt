package com.sbro.emucorex.netplay

import android.content.Context
import android.util.Log
import com.sbro.emucorex.core.EmulatorBridge
import com.sbro.emucorex.data.AppPreferences
import com.sbro.emucorex.data.GameItem
import com.sbro.emucorex.data.GameLibraryCacheRepository
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * LAN 联机 Repository。
 *
 * - 持有 [MutableStateFlow<LanNetplayUiState>] 供 ViewModel 观察。
 * - 持有 [MutableSharedFlow<LanNetplayEvent>] 发射一次性事件（MinimizeLobby / LaunchGame / RequestIso）。
 * - 实现 [NativeLanCallback] 接收 C++ → JNI → Kotlin 的事件。
 * - 对外暴露 [startSession]、[endSession] 等操作方法。
 * - 单例，通过 [LanNetplayRepository.getInstance] 访问。
 */
class LanNetplayRepository private constructor(private val context: Context) : NativeLanCallback {

    companion object {
        private const val TAG = "LanNetplayRepo"

        @Volatile
        private var _instance: LanNetplayRepository? = null

        fun getInstance(context: Context): LanNetplayRepository =
            _instance ?: synchronized(this) {
                _instance ?: LanNetplayRepository(context.applicationContext).also { _instance = it }
            }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    // ── StateFlow ──────────────────────────────────────────────────────────────

    private val _uiState = MutableStateFlow<LanNetplayUiState>(LanNetplayUiState.Idle)
    val uiState: StateFlow<LanNetplayUiState> = _uiState.asStateFlow()

    // ── One-shot events ────────────────────────────────────────────────────────

    private val _events = MutableSharedFlow<LanNetplayEvent>(
        replay = 0,
        extraBufferCapacity = 16,
    )
    val events: SharedFlow<LanNetplayEvent> = _events.asSharedFlow()

    /** 最近一次成员 ISO 选择上下文；UI 侧可在 GuestSelectIso 时读取。 */
    @Volatile
    var lastCurrentSettings: LanNetplaySettings = LanNetplaySettings()
        private set

    // ── Init: 注册 JNI 回调 ────────────────────────────────────────────────────

    init {
        try {
            LanNetplayNative.registerCallback(this)
        } catch (_: Exception) { }
    }

    // ── 操作 API ───────────────────────────────────────────────────────────────

    /**
     * 用户点击 Host 或 Connect（全字段）。
     */
    fun startSession(settings: LanNetplaySettings) {
        lastCurrentSettings = settings
        _uiState.value = LanNetplayUiState.Lobby(
            statusText = when (settings.mode) {
                LanNetplayMode.HOST    -> "Waiting for players to connect..."
                LanNetplayMode.CONNECT -> "Connecting to host..."
                LanNetplayMode.OBSERVE -> "Connecting to host (observer)..."
            },
        )
        LanNetplayNative.startSession(settings)
    }

    /**
     * 结束会话（关闭大厅或游戏中断开）。
     */
    fun endSession() {
        LanNetplayNative.endSession()
        _uiState.value = LanNetplayUiState.Idle
    }

    fun sendChat(message: String) {
        // shoryu 只会把聊天消息广播给其他参会方，不会回环给发送者自己。
        // 与 PC 版 NetplayLanDialog::onSendChatClicked() 保持一致：本地先 echo 一条 "<me>"。
        addChatMessage("<me>", message)
        LanNetplayNative.sendChat(message)
    }

    fun setInputDelay(delay: Int) = LanNetplayNative.setInputDelay(delay)

    /**
     * 房主：已选好 ISO，确认启动。
     */
    fun hostConfirmStart(isoPath: String, inputDelay: Int, fairPlayNetplay: Boolean = false) {
        _uiState.update { state ->
            if (state is LanNetplayUiState.Lobby)
                state.copy(statusText = "Waiting for guest to sync...")
            else state
        }
        LanNetplayNative.hostConfirmStart(isoPath, inputDelay, fairPlayNetplay)
    }

    /**
     * 成员：ISO CRC 校验通过，写入 cheats 目录，通知原生层就绪。
     * @param cheatFilesEncoded 来自 requestGuestIsoSelection 的原始 C++ 字符串（已解码）
     *
     * 注意：C++ 端 GuestConfirmReady 只做状态标记和发送 PS2LAN_ACK，不会回调
     * RequestLaunchGame（等价 PC 版由 Controller 直接启动 VM）。因此这里必须手动
     * 发射 LaunchGame 事件，让 UI 从 GuestSelectIso / Lobby 切到 EmulationRoute，
     * 否则停在 "Starting game (guest)..." 永不前进。
     */
    fun guestConfirmReady(
        isoPath: String,
        hostHadCheats: Boolean,
        cheatFiles: List<CheatFileEntry>,
        fairPlayNetplay: Boolean = false,
    ) {
        // 优化.md §B：手动 ISO 选择路径上补 CRC 校验。
        // findMatchingLocalIso 在 GameLibrary 命中时已经过 CRC 比对，本路径只覆盖 picker
        // 兜底——用户从 SAF 里挑了一个文件后，先与当前 GuestSelectIso 状态里的
        // expectedCrcHex 做快速比对：CRC 不一致直接走 onSyncMismatch 等价路径，
        // 既弹 Toast 也把 _uiState 切到 Error，并且不会发 PS2LAN_ACK 浪费房主的等待。
        val expectedHex = (uiState.value as? LanNetplayUiState.GuestSelectIso)?.expectedCrcHex
        scope.launch(Dispatchers.IO) {
            if (!expectedHex.isNullOrBlank()) {
                val expected = normalizeCrcHex(expectedHex)
                val pickedCrc = runCatching {
                    val meta = EmulatorBridge.getGameMetadata(isoPath)
                    extractCrcFromSerialWithCrc(meta.serialWithCrc)
                }.getOrNull()
                Log.d(TAG, "guestConfirmReady CRC validate expected=$expected picked=$pickedCrc")
                if (expected != null && pickedCrc != null && expected != pickedCrc) {
                    val message = "游戏镜像 CRC 不一致，无法启动联机。\n本机：$pickedCrc\n对端：$expected"
                    _events.emit(LanNetplayEvent.Error(message))
                    _uiState.value = LanNetplayUiState.Error(message)
                    return@launch
                }
                // 注：picked 取不到（getGameMetadata 失败 / serialWithCrc 解析失败）时不
                // 拦截——因为 SAF Uri 偶尔存在权限或元数据滞后问题，强行拦下会导致用户合法
                // ISO 也进不去；此时仍由 host 侧的 PS2LAN_ACK 流程来兜底。
            }

            val vmCheatsEnabled = !fairPlayNetplay && hostHadCheats
            if (vmCheatsEnabled && cheatFiles.isNotEmpty()) {
                writeCheatFiles(cheatFiles)
            }
            _uiState.value = LanNetplayUiState.LaunchingGame(isoPath)
            // 关键：LanNetplayNative.guestConfirmReady 内部会同步阻塞最多 ~6s 等待
            // PC 房主把 PS2LAN_ACK 确认回来（避免"启动 VM 把 shoryu lobby 干掉"之前 ACK 丢失
            // 导致 PC 黑屏）。因此必须把它调度到 IO 线程，绝不能占住 UI 主线程。
            //
            // 同理，真正的 LaunchGame 事件必须在 JNI 返回之后再发射——只有握手成功了才会
            // 让 Kotlin 侧启动 VM；否则仍会启动，但产品状态已被 Native 端打印告警。
            val acked = LanNetplayNative.guestConfirmReady(isoPath, vmCheatsEnabled)
            if (acked) {
                _events.emit(LanNetplayEvent.LaunchGame(isoPath, vmCheatsEnabled))
            } else {
                _uiState.value = LanNetplayUiState.Error(
                    "PS2LAN_ACK 未被房主确认，已阻止启动游戏。请确认 PC 端已应用 ACK 修复并放行 UDP 7500/38889。"
                )
                _events.emit(LanNetplayEvent.Error("PS2LAN_ACK 未被房主确认，未启动游戏。"))
            }
        }
    }

    fun guestCancelReady() {
        LanNetplayNative.guestCancelReady()
        _uiState.update { state ->
            if (state is LanNetplayUiState.GuestSelectIso)
                LanNetplayUiState.Lobby(statusText = "Game selection cancelled.")
            else state
        }
    }

    /** VM 启动时由 Kotlin 侧（EmulatorBridge / 游戏页面）通知 */
    fun notifyVmStarted() = LanNetplayNative.onVmStarted()

    /** VM 停止时通知 */
    fun notifyVmStopped() = LanNetplayNative.onVmStopped()

    // ── NativeLanCallback 实现 ─────────────────────────────────────────────────

    override fun setStatus(status: String) {
        scope.launch {
            _uiState.update { state ->
                when (state) {
                    is LanNetplayUiState.Lobby       -> state.copy(statusText = status)
                    is LanNetplayUiState.HostWaitingForGuest -> state.copy(statusText = status)
                    else -> state
                }
            }
        }
    }

    override fun addChatMessage(username: String, message: String) {
        scope.launch {
            _uiState.update { state ->
                if (state is LanNetplayUiState.Lobby)
                    state.copy(chatMessages = state.chatMessages + ChatMessage(username, message))
                else state
            }
        }
    }

    override fun setUserlist(encodedList: String, numPlayers: Int) {
        scope.launch {
            val players = decodePlayers(encodedList)
            // 优化.md §B：双安卓场景里，「房主见聊天但成员列表空」的核心断点常在
            // 「state 此刻不是 Lobby」——如成员侧已切到 GuestSelectIso/LaunchingGame，
            // 之后再回 Lobby 时会丢失最近一次完整的 players 列表。这里把日志直接打到
            // logcat，便于和 native 侧 "JNI SetUserlist push" 行做时序对照。
            val current = _uiState.value
            if (current !is LanNetplayUiState.Lobby) {
                Log.w(TAG, "setUserlist arrived while state=${current::class.simpleName} " +
                    "size=${players.size} numPlayers=$numPlayers (kept, not pushed to UI)")
            } else {
                Log.d(TAG, "setUserlist size=${players.size} numPlayers=$numPlayers")
            }
            _uiState.update { state ->
                if (state is LanNetplayUiState.Lobby)
                    state.copy(players = players, numPlayers = numPlayers)
                else state
            }
        }
    }

    override fun onConnectionEstablished(delay: Int) {
        scope.launch {
            _uiState.update { state ->
                if (state is LanNetplayUiState.Lobby)
                    state.copy(connected = true, inputDelay = delay,
                        statusText = if (LanNetplayNative.isHost()) "Connected. Choose a game to start."
                                     else "Connected. Waiting for host to start...")
                else state
            }
        }
    }

    override fun requestLaunchGame(gamePath: String, enableCheats: Boolean) {
        scope.launch {
            _uiState.value = LanNetplayUiState.LaunchingGame(gamePath)
            _events.emit(LanNetplayEvent.LaunchGame(gamePath, enableCheats))
        }
    }

    override fun requestGuestIsoSelection(
        crcHex: String,
        serial: String,
        hostHadCheats: Boolean,
        fairPlayNetplay: Boolean,
        cheatData: String,
    ) {
        scope.launch {
            val cheatFiles = if (hostHadCheats && cheatData.isNotEmpty() && !fairPlayNetplay)
                decodeCheatFiles(cheatData) else emptyList()

            // 优化：成员收到房主游戏 (serial, CRC) 后，先尝试用本地 game library cache
            // 自动匹配一个序列号一致且 CRC 一致的 ISO；命中即直接 guestConfirmReady，免手动 pick。
            // 命中条件：
            //   1) 库里某条 cache 的 serial == host_serial（大小写不敏感）；
            //   2) 用 EmulatorBridge.getGameMetadata 实时对 path 计算 CRC，
            //      与 host 的 crcHex（8 位十六进制）一致；
            // 未命中时回退到原 GuestSelectIso UI，用户手动 pick。
            val matched = runCatching { findMatchingLocalIso(serial, crcHex) }.getOrNull()
            if (matched != null) {
                guestConfirmReady(matched, hostHadCheats, cheatFiles, fairPlayNetplay)
                return@launch
            }

            _uiState.value = LanNetplayUiState.GuestSelectIso(
                expectedCrcHex = crcHex,
                serial = serial,
                hostHadCheats = hostHadCheats,
                fairPlayNetplay = fairPlayNetplay,
                cheatFiles = cheatFiles,
            )
            _events.emit(LanNetplayEvent.RequestIso(crcHex, serial, hostHadCheats, fairPlayNetplay, cheatFiles))
        }
    }

    /**
     * 在 library cache 里查找与 (serial, crcHex) 同时命中的本地 ISO 路径。
     *
     * - `serial` 比对：大小写不敏感，与 getGameMetadata 返回的 serial 一致；
     * - `crcHex` 比对：host 端通过 `StringUtil::StdStringFromFormat("%s (%08X)", serial, crc)` 格式化，
     *   Android JNI 的 getGameMetadata 返回的第三段 `serialWithCrc` 形如 `"SLUS-21184 (B18E3F1C)"`，
     *   取其中括号内 8 位十六进制与 host 的 crcHex 对比（规范化为大写无前导 0x）。
     *
     * 整个流程走 Dispatchers.IO：文件读取 + JNI cdvdLock/PopulateEntryFromPath 是同步 I/O。
     */
    private suspend fun findMatchingLocalIso(hostSerial: String, hostCrcHex: String): String? {
        if (hostSerial.isBlank() || hostCrcHex.isBlank()) return null
        val normalizedHostCrc = normalizeCrcHex(hostCrcHex) ?: return null
        val normalizedHostSerial = hostSerial.trim().uppercase()

        return withContext(Dispatchers.IO) {
            val prefs = AppPreferences(context)
            val rootPath = prefs.gamePath.first().orEmpty()
            if (rootPath.isBlank()) return@withContext null

            val cache = GameLibraryCacheRepository(context)
            val games: List<GameItem> = cache.loadSnapshot(rootPath).games
            if (games.isEmpty()) return@withContext null

            val candidates = games.filter { item ->
                val s = item.serial?.trim()?.uppercase()
                !s.isNullOrBlank() && s == normalizedHostSerial
            }
            if (candidates.isEmpty()) return@withContext null

            for (candidate in candidates) {
                val path = candidate.path
                if (path.isBlank()) continue
                val meta = runCatching { EmulatorBridge.getGameMetadata(path) }.getOrNull() ?: continue
                val probeSerial = meta.serial?.trim()?.uppercase()
                if (probeSerial.isNullOrBlank() || probeSerial != normalizedHostSerial) continue

                val probeCrc = extractCrcFromSerialWithCrc(meta.serialWithCrc) ?: continue
                if (probeCrc == normalizedHostCrc) {
                    return@withContext path
                }
            }
            null
        }
    }

    /** 从 "SLUS-21184 (B18E3F1C)" 里抓出大写 8 位 hex，失败返回 null。 */
    private fun extractCrcFromSerialWithCrc(serialWithCrc: String?): String? {
        if (serialWithCrc.isNullOrBlank()) return null
        val open = serialWithCrc.lastIndexOf('(')
        val close = serialWithCrc.lastIndexOf(')')
        if (open < 0 || close <= open + 1) return null
        return normalizeCrcHex(serialWithCrc.substring(open + 1, close))
    }

    /** 规范化 CRC 十六进制字符串为大写、无 `0x` 前缀、左补 0 到 8 位。非法返回 null。 */
    private fun normalizeCrcHex(raw: String): String? {
        val trimmed = raw.trim().removePrefix("0x").removePrefix("0X")
        if (trimmed.isEmpty() || trimmed.length > 8) return null
        if (trimmed.any { !it.isDigit() && it.uppercaseChar() !in 'A'..'F' }) return null
        return trimmed.padStart(8, '0').uppercase()
    }

    override fun presentLobby() {
        scope.launch {
            if (_uiState.value is LanNetplayUiState.Idle) {
                _uiState.value = LanNetplayUiState.Lobby()
            }
        }
    }

    override fun minimizeLobby() {
        scope.launch { _events.emit(LanNetplayEvent.MinimizeLobby) }
    }

    override fun onSyncMismatch(reason: String, localValue: String, peerValue: String) {
        // 优化.md §B：把 BIOS / DiscID / SkipMPEG / 镜像 CRC 不匹配通过 Error 事件 + Error
        // 状态双通道暴露给 UI——
        //   - LanNetplayEvent.Error 触发 LanNetplayScreen 的 Toast.makeText（一次性提示）；
        //   - LanNetplayUiState.Error 让大厅文案稳定停留在「为什么进不去」，避免一瞬而过。
        // 同时调用 endSession() 收掉 native 侧的 shoryu 重试（join() 内 20 次重试每次 1s
        // 仍能触发新的 mismatch，会反复弹 Toast；endSession 后 _is_stopped=true，retry 循环
        // 立即跳出，UI 不再受打扰）。
        Log.w(TAG, "onSyncMismatch reason=$reason local='$localValue' peer='$peerValue'")
        scope.launch {
            val message = formatMismatchMessage(reason, localValue, peerValue)
            _events.emit(LanNetplayEvent.Error(message))
            _uiState.value = LanNetplayUiState.Error(message)
            // 让 native 侧停止重试。Kotlin endSession 会把 _uiState 切到 Idle，所以
            // 必须先 emit Error 再调用 endSession 才能看到 Error 文案——但 endSession 总会
            // 把状态覆盖为 Idle。改用直接调原生 endSession，跳过 _uiState 覆盖。
            runCatching { LanNetplayNative.endSession() }
        }
    }

    private fun formatMismatchMessage(reason: String, local: String, peer: String): String =
        when (reason) {
            "bios" -> "BIOS 版本不一致，无法加入房间。\n本机：${local.ifBlank { "未知" }}\n对端：${peer.ifBlank { "未知" }}"
            "disc_id" -> "游戏序列号不一致，无法启动联机。\n本机：${local.ifBlank { "未知" }}\n对端：${peer.ifBlank { "未知" }}"
            "skip_mpeg" -> "金手指 / SkipMPEG 设置不一致。\n本机：${local}\n对端：${peer}"
            "game_crc" -> "游戏镜像 CRC 不一致，无法启动联机。\n本机：${local.ifBlank { "未知" }}\n对端：${peer.ifBlank { "未知" }}"
            else -> "联机配置不一致 ($reason)：本机=${local} / 对端=${peer}"
        }

    // ── 私有工具 ───────────────────────────────────────────────────────────────

    /** 解析 "name|ping|side,name|ping|side,..." */
    private fun decodePlayers(encoded: String): List<LanPlayerInfo> {
        if (encoded.isBlank()) return emptyList()
        return encoded.split(',').mapNotNull { part ->
            val tokens = part.split('|')
            if (tokens.size >= 3) LanPlayerInfo(
                name = tokens[0],
                ping = tokens[1],
                side = tokens[2].toIntOrNull() ?: 0,
            ) else null
        }
    }

    /**
     * 解析 cheat 文件编码串（RS = 0x1E 分隔文件，US = 0x1F 分隔 name 和 data）。
     */
    private fun decodeCheatFiles(encoded: String): List<CheatFileEntry> {
        return encoded.split('\u001E').mapNotNull { entry ->
            val us = entry.indexOf('\u001F')
            if (us < 0) return@mapNotNull null
            val name = entry.substring(0, us)
            val data = entry.substring(us + 1).toByteArray(Charsets.ISO_8859_1)
            CheatFileEntry(name, data)
        }
    }

    /**
     * 把 host 发来的 cheat 文件写入 Android cheats 目录。
     * 路径与 EmulatorBridge.applyRuntimeConfig 中保持一致。
     */
    private fun writeCheatFiles(files: List<CheatFileEntry>) {
        val cheatsDir = File(
            context.getExternalFilesDir(null) ?: context.filesDir,
            "cheats"
        ).apply { mkdirs() }

        for (entry in files) {
            val name = entry.name.replace(Regex("[/\\\\]"), "_") // 安全检查
            if (name.isBlank() || name.contains("..")) continue
            try {
                File(cheatsDir, name).writeBytes(entry.data)
            } catch (_: Exception) { }
        }
    }
}
