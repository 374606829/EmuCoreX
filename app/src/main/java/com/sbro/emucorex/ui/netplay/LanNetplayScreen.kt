package com.sbro.emucorex.ui.netplay

import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsIgnoringVisibility
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Chat
import androidx.compose.material.icons.rounded.Menu
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.PowerSettingsNew
import androidx.compose.material.icons.rounded.SportsEsports
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sbro.emucorex.netplay.CheatFileEntry
import com.sbro.emucorex.netplay.LanNetplayEvent
import com.sbro.emucorex.netplay.LanNetplayMode
import com.sbro.emucorex.netplay.LanNetplayRepository
import com.sbro.emucorex.netplay.LanNetplaySettings
import com.sbro.emucorex.netplay.LanNetplayUiState

private enum class NetplayPanel {
    Settings,
    Lobby
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun LanNetplayScreen(
    onMenuClick: (() -> Unit)? = null,
    onLaunchGame: (String) -> Unit,
) {
    val context = LocalContext.current
    val repository = remember(context) { LanNetplayRepository.getInstance(context) }
    val uiState by repository.uiState.collectAsState()
    var panel by rememberSaveable { mutableStateOf(NetplayPanel.Settings) }
    var selectedTab by rememberSaveable { mutableStateOf(0) }
    var username by rememberSaveable { mutableStateOf("Player") }
    var hostAddress by rememberSaveable { mutableStateOf("") }
    var hostPortText by rememberSaveable { mutableStateOf("7500") }
    var listenPortText by rememberSaveable { mutableStateOf("7500") }
    var inputDelayText by rememberSaveable { mutableStateOf("1") }
    var saveReplay by rememberSaveable { mutableStateOf(false) }
    var clientOnlyDelay by rememberSaveable { mutableStateOf(true) }
    var memcardSync by rememberSaveable { mutableStateOf(true) }
    var readonlyMemcard by rememberSaveable { mutableStateOf(false) }
    var chatText by rememberSaveable { mutableStateOf("") }
    var pendingGuestCheats by remember { mutableStateOf<List<CheatFileEntry>>(emptyList()) }
    var pendingGuestEnableCheats by remember { mutableStateOf(false) }
    var pendingGuestFairPlay by remember { mutableStateOf(false) }
    var fairPlayLan by rememberSaveable { mutableStateOf(false) }

    fun persistReadPermission(uri: Uri) {
        runCatching {
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        }
    }

    val hostIsoPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        persistReadPermission(uri)
        repository.hostConfirmStart(
            isoPath = uri.toString(),
            inputDelay = inputDelayText.toIntOrNull()?.coerceAtLeast(1) ?: 1,
            fairPlayNetplay = fairPlayLan,
        )
    }

    val guestIsoPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        persistReadPermission(uri)
        repository.guestConfirmReady(
            isoPath = uri.toString(),
            hostHadCheats = pendingGuestEnableCheats,
            cheatFiles = pendingGuestCheats,
            fairPlayNetplay = pendingGuestFairPlay,
        )
    }

    LaunchedEffect(repository) {
        repository.events.collect { event ->
            when (event) {
                is LanNetplayEvent.LaunchGame -> onLaunchGame(event.isoPath)
                is LanNetplayEvent.RequestIso -> {
                    panel = NetplayPanel.Lobby
                    pendingGuestEnableCheats = event.hostHadCheats
                    pendingGuestCheats = event.cheatFiles
                    pendingGuestFairPlay = event.fairPlayNetplay
                    guestIsoPicker.launch(arrayOf("*/*"))
                }
                is LanNetplayEvent.Error -> Toast.makeText(context, event.message, Toast.LENGTH_LONG).show()
                LanNetplayEvent.MinimizeLobby -> Toast.makeText(context, "联机大厅已最小化。", Toast.LENGTH_SHORT).show()
            }
        }
    }

    val topInset = WindowInsets.statusBarsIgnoringVisibility.asPaddingValues().calculateTopPadding()
    val bottomInset = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(start = 20.dp, end = 20.dp, top = topInset + 18.dp, bottom = bottomInset + 24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        NetplayHeader(onMenuClick)
        when (panel) {
            NetplayPanel.Settings -> SettingsPanel(
                selectedTab = selectedTab,
                onSelectedTabChange = { selectedTab = it },
                username = username,
                onUsernameChange = { username = it },
                hostAddress = hostAddress,
                onHostAddressChange = { hostAddress = it },
                hostPortText = hostPortText,
                onHostPortChange = { hostPortText = it },
                listenPortText = listenPortText,
                onListenPortChange = { listenPortText = it },
                saveReplay = saveReplay,
                onSaveReplayChange = { saveReplay = it },
                clientOnlyDelay = clientOnlyDelay,
                onClientOnlyDelayChange = { clientOnlyDelay = it },
                memcardSync = memcardSync,
                onMemcardSyncChange = { memcardSync = it },
                readonlyMemcard = readonlyMemcard,
                onReadonlyMemcardChange = { readonlyMemcard = it },
                onCancel = repository::endSession,
                onSubmit = {
                    val mode = if (selectedTab == 0) LanNetplayMode.CONNECT else LanNetplayMode.HOST
                    repository.startSession(
                        LanNetplaySettings(
                            username = username.ifBlank { "Player" },
                            mode = mode,
                            hostAddress = hostAddress.trim(),
                            hostPort = hostPortText.toIntOrNull() ?: 7500,
                            listenPort = listenPortText.toIntOrNull() ?: 7500,
                            saveReplay = saveReplay,
                            memcardSync = memcardSync,
                            clientOnlyDelay = clientOnlyDelay,
                            readonlyMemcard = readonlyMemcard,
                            numPlayers = 2
                        )
                    )
                    panel = NetplayPanel.Lobby
                }
            )

            NetplayPanel.Lobby -> LobbyPanel(
                state = uiState,
                isHostSettingsTab = selectedTab == 1,
                fairPlayLan = fairPlayLan,
                onFairPlayLanChange = { fairPlayLan = it },
                inputDelayText = inputDelayText,
                onInputDelayChange = { inputDelayText = it },
                chatText = chatText,
                onChatTextChange = { chatText = it },
                onSendChat = {
                    val message = chatText.trim()
                    if (message.isNotEmpty()) {
                        repository.sendChat(message)
                        chatText = ""
                    }
                },
                onStart = { hostIsoPicker.launch(arrayOf("*/*")) },
                onGuestPickIso = { guestIsoPicker.launch(arrayOf("*/*")) },
                onGuestCancelIso = repository::guestCancelReady,
                onBackToSettings = {
                    fairPlayLan = false
                    repository.endSession()
                    panel = NetplayPanel.Settings
                }
            )
        }
    }
}

@Composable
private fun NetplayHeader(onMenuClick: (() -> Unit)?) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Surface(
            shape = RoundedCornerShape(18.dp),
            color = MaterialTheme.colorScheme.primaryContainer
        ) {
            Icon(
                imageVector = Icons.Rounded.SportsEsports,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.padding(12.dp)
            )
        }
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(start = 14.dp)
        ) {
            Text("局域网联机", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
            Text(
                "Connect / Host 设置后进入大厅，流程对齐 PCSX2-Qt LAN 对话框。",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        if (onMenuClick != null) {
            IconButton(onClick = onMenuClick) {
                Icon(Icons.Rounded.Menu, contentDescription = null)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SettingsPanel(
    selectedTab: Int,
    onSelectedTabChange: (Int) -> Unit,
    username: String,
    onUsernameChange: (String) -> Unit,
    hostAddress: String,
    onHostAddressChange: (String) -> Unit,
    hostPortText: String,
    onHostPortChange: (String) -> Unit,
    listenPortText: String,
    onListenPortChange: (String) -> Unit,
    saveReplay: Boolean,
    onSaveReplayChange: (Boolean) -> Unit,
    clientOnlyDelay: Boolean,
    onClientOnlyDelayChange: (Boolean) -> Unit,
    memcardSync: Boolean,
    onMemcardSyncChange: (Boolean) -> Unit,
    readonlyMemcard: Boolean,
    onReadonlyMemcardChange: (Boolean) -> Unit,
    onCancel: () -> Unit,
    onSubmit: () -> Unit,
) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(
            modifier = Modifier
                .verticalScroll(rememberScrollState())
                .padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Text("设置", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
            OutlinedTextField(
                value = username,
                onValueChange = onUsernameChange,
                label = { Text("Username") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth()
            )
            CheckRow("Save Replay", saveReplay, onSaveReplayChange)
            PrimaryTabRow(selectedTabIndex = selectedTab) {
                Tab(selected = selectedTab == 0, onClick = { onSelectedTabChange(0) }, text = { Text("Connect") })
                Tab(selected = selectedTab == 1, onClick = { onSelectedTabChange(1) }, text = { Text("Host") })
            }
            if (selectedTab == 0) {
                ConnectForm(
                    hostAddress = hostAddress,
                    onHostAddressChange = onHostAddressChange,
                    hostPortText = hostPortText,
                    onHostPortChange = onHostPortChange
                )
            } else {
                HostForm(
                    listenPortText = listenPortText,
                    onListenPortChange = onListenPortChange,
                    clientOnlyDelay = clientOnlyDelay,
                    onClientOnlyDelayChange = onClientOnlyDelayChange,
                    memcardSync = memcardSync,
                    onMemcardSyncChange = onMemcardSyncChange,
                    readonlyMemcard = readonlyMemcard,
                    onReadonlyMemcardChange = onReadonlyMemcardChange
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(onClick = onSubmit) {
                    Icon(Icons.Rounded.PowerSettingsNew, contentDescription = null)
                    Text(if (selectedTab == 0) "Connect" else "Host", modifier = Modifier.padding(start = 8.dp))
                }
                TextButton(onClick = onCancel) {
                    Text("Cancel")
                }
            }
        }
    }
}

@Composable
private fun ConnectForm(
    hostAddress: String,
    onHostAddressChange: (String) -> Unit,
    hostPortText: String,
    onHostPortChange: (String) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        OutlinedTextField(
            value = hostAddress,
            onValueChange = onHostAddressChange,
            label = { Text("Host Address") },
            placeholder = { Text("192.168.1.4") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )
        OutlinedTextField(
            value = hostPortText,
            onValueChange = onHostPortChange,
            label = { Text("Host Port") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )
    }
}

@Composable
private fun HostForm(
    listenPortText: String,
    onListenPortChange: (String) -> Unit,
    clientOnlyDelay: Boolean,
    onClientOnlyDelayChange: (Boolean) -> Unit,
    memcardSync: Boolean,
    onMemcardSyncChange: (Boolean) -> Unit,
    readonlyMemcard: Boolean,
    onReadonlyMemcardChange: (Boolean) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        OutlinedTextField(
            value = listenPortText,
            onValueChange = onListenPortChange,
            label = { Text("Listen Port") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )
        Text(
            "成员连接时 Host Port 必须与房主 Listen Port 一致。Players 固定为 2。",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        ReadOnlyLine("Players", "2")
        CheckRow("Client-Only Delay", clientOnlyDelay, onClientOnlyDelayChange)
        CheckRow("Memory Card Sync", memcardSync, onMemcardSyncChange)
        CheckRow("Read-Only Memory Card", readonlyMemcard, onReadonlyMemcardChange)
    }
}

@Composable
private fun LobbyPanel(
    state: LanNetplayUiState,
    isHostSettingsTab: Boolean,
    fairPlayLan: Boolean,
    onFairPlayLanChange: (Boolean) -> Unit,
    inputDelayText: String,
    onInputDelayChange: (String) -> Unit,
    chatText: String,
    onChatTextChange: (String) -> Unit,
    onSendChat: () -> Unit,
    onStart: () -> Unit,
    onGuestPickIso: () -> Unit,
    onGuestCancelIso: () -> Unit,
    onBackToSettings: () -> Unit,
) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Text("大厅", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
            LobbyStatus(state)
            HorizontalDivider()
            PlayerTable(state)
            HorizontalDivider()
            ChatArea(
                state = state,
                chatText = chatText,
                onChatTextChange = onChatTextChange,
                onSendChat = onSendChat
            )
            HorizontalDivider()
            if (isHostSettingsTab) {
                val canStart = (state as? LanNetplayUiState.Lobby)?.let {
                    it.connected || it.players.size >= 2
                } == true
                OutlinedTextField(
                    value = inputDelayText,
                    onValueChange = onInputDelayChange,
                    label = { Text("Input Delay") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                CheckRow("公平联机（本场禁用磁盘金手指，不对成员同步）", fairPlayLan, onFairPlayLanChange)
                Button(onClick = onStart, enabled = canStart) {
                    Icon(Icons.Rounded.PlayArrow, contentDescription = null)
                    Text("Start", modifier = Modifier.padding(start = 8.dp))
                }
            } else {
                Text(
                    "成员端等待房主 Start；收到 PS2LAN_V3 后选择本机相同 ISO。",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (state is LanNetplayUiState.GuestSelectIso) {
                    Text("请求游戏：${state.serial} (${state.expectedCrcHex})", maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Button(onClick = onGuestPickIso) { Text("选择 ISO") }
                        TextButton(onClick = onGuestCancelIso) { Text("取消") }
                    }
                }
            }
            TextButton(onClick = onBackToSettings) {
                Text("返回设置 / 断开")
            }
        }
    }
}

@Composable
private fun LobbyStatus(state: LanNetplayUiState) {
    val text = when (state) {
        LanNetplayUiState.Idle -> "未连接。"
        is LanNetplayUiState.Lobby -> state.statusText.ifBlank { "已进入大厅。" }
        is LanNetplayUiState.HostWaitingForGuest -> state.statusText
        is LanNetplayUiState.GuestSelectIso -> "房主已发起游戏同步，请选择 ISO。"
        is LanNetplayUiState.LaunchingGame -> "正在启动：${state.isoPath}"
        is LanNetplayUiState.Error -> state.message
    }
    Text(
        text = text,
        style = MaterialTheme.typography.bodyLarge,
        color = if (state is LanNetplayUiState.Error) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
    )
}

@Composable
private fun PlayerTable(state: LanNetplayUiState) {
    val players = (state as? LanNetplayUiState.Lobby)?.players.orEmpty()
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("#", modifier = Modifier.weight(0.18f), fontWeight = FontWeight.SemiBold)
            Text("Name", modifier = Modifier.weight(0.56f), fontWeight = FontWeight.SemiBold)
            Text("Ping", modifier = Modifier.weight(0.26f), fontWeight = FontWeight.SemiBold)
        }
        if (players.isEmpty()) {
            Text("等待玩家列表...", color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            players.forEach { player ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text((player.side + 1).toString(), modifier = Modifier.weight(0.18f))
                    Text(player.name, modifier = Modifier.weight(0.56f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(player.ping, modifier = Modifier.weight(0.26f), maxLines = 1)
                }
            }
        }
    }
}

@Composable
private fun ChatArea(
    state: LanNetplayUiState,
    chatText: String,
    onChatTextChange: (String) -> Unit,
    onSendChat: () -> Unit,
) {
    val messages = (state as? LanNetplayUiState.Lobby)?.chatMessages.orEmpty()
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.AutoMirrored.Rounded.Chat, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Text("聊天", modifier = Modifier.padding(start = 8.dp), style = MaterialTheme.typography.titleMedium)
        }
        LazyColumn(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 88.dp, max = 180.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            if (messages.isEmpty()) {
                item { Text("暂无聊天消息。", color = MaterialTheme.colorScheme.onSurfaceVariant) }
            } else {
                items(messages.takeLast(30)) { message ->
                    Text("${message.username}: ${message.text}", style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = chatText,
                onValueChange = onChatTextChange,
                label = { Text("消息") },
                singleLine = true,
                modifier = Modifier.weight(1f)
            )
            Button(onClick = onSendChat) { Text("发送") }
        }
    }
}

@Composable
private fun CheckRow(label: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Checkbox(checked = checked, onCheckedChange = onCheckedChange)
        Text(label, modifier = Modifier.padding(start = 8.dp))
    }
}

@Composable
private fun ReadOnlyLine(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, fontWeight = FontWeight.SemiBold)
    }
}
