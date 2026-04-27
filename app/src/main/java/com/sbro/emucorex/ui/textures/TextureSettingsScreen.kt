package com.sbro.emucorex.ui.textures

import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsIgnoringVisibility
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Menu
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.SaveAs
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sbro.emucorex.core.NativeApp
import com.sbro.emucorex.data.TextureFileEntry
import com.sbro.emucorex.data.TextureRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun TextureSettingsScreen(
    onMenuClick: (() -> Unit)? = null
) {
    val context = LocalContext.current
    val repository = remember(context) { TextureRepository(context) }
    val scope = rememberCoroutineScope()
    var serial by remember { mutableStateOf(runCatching { NativeApp.getGameSerial().orEmpty() }.getOrDefault("")) }
    var loadReplacements by remember { mutableStateOf(false) }
    var dumpTextures by remember { mutableStateOf(false) }
    var textureRoot by remember { mutableStateOf(repository.defaultTextureRoot().absolutePath) }
    var entries by remember { mutableStateOf<List<TextureFileEntry>>(emptyList()) }
    var showImportDialog by remember { mutableStateOf(false) }
    var pendingImportSerial by remember { mutableStateOf("") }

    fun refreshList() {
        entries = if (repository.isValidSerial(serial)) {
            repository.listReplacementTextures(serial)
        } else {
            emptyList()
        }
    }

    val textureRootPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            val ok = withContext(Dispatchers.IO) { repository.setTextureRootFromTree(uri) }
            if (ok) {
                textureRoot = repository.textureRootPath()
                refreshList()
                Toast.makeText(context, "贴图目录已更新。", Toast.LENGTH_SHORT).show()
            } else {
                Toast.makeText(context, "无法解析该目录为核心可访问路径。", Toast.LENGTH_LONG).show()
            }
        }
    }

    val importFolderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        uri ?: return@rememberLauncherForActivityResult
        val importSerial = repository.normalizeSerial(pendingImportSerial)
        scope.launch {
            val result = withContext(Dispatchers.IO) {
                repository.importTextures(importSerial, uri)
            }
            serial = importSerial
            refreshList()
            Toast.makeText(
                context,
                "已导入 ${result.copied} 个贴图，跳过 ${result.skipped} 项。",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    LaunchedEffect(repository) {
        repository.syncConfiguredSettings()
        loadReplacements = repository.loadTextureReplacements()
        dumpTextures = repository.dumpReplaceableTextures()
        textureRoot = repository.textureRootPath()
        refreshList()
    }

    val topInset = WindowInsets.statusBarsIgnoringVisibility.asPaddingValues().calculateTopPadding()
    val bottomInset = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(start = 20.dp, end = 20.dp, top = topInset + 18.dp, bottom = bottomInset + 24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        TextureHeader(onMenuClick)
        SettingsCard(title = "贴图替换") {
            TextureToggleItem(
                icon = Icons.Rounded.Settings,
                title = "启用贴图替换",
                subtitle = "对应 PCSX2 的 LoadTextureReplacements，加载 replacements 中的 PNG/DDS。",
                checked = loadReplacements,
                onCheckedChange = { enabled ->
                    loadReplacements = enabled
                    scope.launch { repository.setLoadTextureReplacements(enabled) }
                }
            )
            TextureToggleItem(
                icon = Icons.Rounded.SaveAs,
                title = "启用贴图提取",
                subtitle = "对应 DumpReplaceableTextures，将可替换贴图写入 dumps。",
                checked = dumpTextures,
                onCheckedChange = { enabled ->
                    dumpTextures = enabled
                    scope.launch { repository.setDumpReplaceableTextures(enabled) }
                }
            )
        }

        SettingsCard(title = "目录") {
            TextureActionItem(
                icon = Icons.Rounded.FolderOpen,
                label = "提取目录",
                value = textureRoot,
                onClick = { textureRootPicker.launch(null) }
            )
            TextureActionItem(
                icon = Icons.Rounded.Refresh,
                label = "恢复默认贴图目录",
                value = repository.defaultTextureRoot().absolutePath,
                onClick = {
                    scope.launch {
                        repository.resetTextureRoot()
                        textureRoot = repository.textureRootPath()
                        refreshList()
                    }
                }
            )
            Text(
                text = "目录结构与 PC 保持一致：textures/<Serial>/replacements/ 用于替换，textures/<Serial>/dumps/ 用于提取。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp)
            )
        }

        SettingsCard(title = "导入贴图") {
            OutlinedTextField(
                value = serial,
                onValueChange = { serial = repository.normalizeSerial(it) },
                label = { Text("游戏序列号 Serial") },
                placeholder = { Text("例如 SLUS-21184") },
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp)
            )
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Button(
                    onClick = {
                        pendingImportSerial = serial
                        showImportDialog = true
                    },
                    enabled = repository.isValidSerial(serial)
                ) {
                    Text("导入贴图")
                }
                TextButton(onClick = { refreshList() }) {
                    Text("刷新列表")
                }
            }
            Text(
                text = "导入时会把所选文件夹内所有 PNG / DDS 文件扁平复制到 replacements，不会复制顶层文件夹本身。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp)
            )
        }

        SettingsCard(title = "已导入的替换贴图") {
            if (!repository.isValidSerial(serial)) {
                EmptyText("请输入有效 Serial 后查看 replacements 列表。")
            } else if (entries.isEmpty()) {
                EmptyText("当前 Serial 没有已导入的 PNG / DDS。")
            } else {
                entries.forEach { entry ->
                    TextureActionItem(
                        icon = Icons.Rounded.SaveAs,
                        label = entry.fileName,
                        value = formatBytes(entry.sizeBytes),
                        onClick = {}
                    )
                }
            }
        }
    }

    if (showImportDialog) {
        ImportSerialDialog(
            serial = pendingImportSerial,
            onSerialChange = { pendingImportSerial = repository.normalizeSerial(it) },
            onDismiss = { showImportDialog = false },
            onConfirm = {
                if (!repository.isValidSerial(pendingImportSerial)) {
                    Toast.makeText(context, "请输入有效的游戏序列号。", Toast.LENGTH_SHORT).show()
                    return@ImportSerialDialog
                }
                showImportDialog = false
                importFolderPicker.launch(null)
            }
        )
    }
}

@Composable
private fun TextureHeader(onMenuClick: (() -> Unit)?) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Surface(shape = RoundedCornerShape(18.dp), color = MaterialTheme.colorScheme.primaryContainer) {
            Icon(
                imageVector = Icons.Rounded.FolderOpen,
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
            Text("贴图", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
            Text(
                "Texture replacements",
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

@Composable
private fun SettingsCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(
            modifier = Modifier.padding(vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(horizontal = 16.dp)
            )
            content()
        }
    }
}

@Composable
private fun TextureToggleItem(
    icon: ImageVector,
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    val interactionSource = remember { MutableInteractionSource() }
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp)
            .combinedClickable(
                interactionSource = interactionSource,
                indication = null,
                onClick = { onCheckedChange(!checked) }
            ),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.22f)
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconBox(icon)
            Spacer(Modifier.width(14.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.bodyLarge)
                Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Switch(checked = checked, onCheckedChange = onCheckedChange)
        }
    }
}

@Composable
private fun TextureActionItem(
    icon: ImageVector,
    label: String,
    value: String,
    onClick: () -> Unit
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.18f),
        onClick = onClick
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconBox(icon)
            Spacer(Modifier.width(14.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(label, style = MaterialTheme.typography.bodyLarge)
                Text(
                    value,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
private fun IconBox(icon: ImageVector) {
    Box(
        modifier = Modifier
            .size(38.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.1f)),
        contentAlignment = Alignment.Center
    ) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(20.dp))
    }
}

@Composable
private fun EmptyText(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp)
    )
}

@Composable
private fun ImportSerialDialog(
    serial: String,
    onSerialChange: (String) -> Unit,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("导入贴图") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("输入游戏序列号后选择包含 PNG / DDS 的文件夹。")
                OutlinedTextField(
                    value = serial,
                    onValueChange = onSerialChange,
                    label = { Text("Serial") },
                    singleLine = true
                )
            }
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text("选择文件夹") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("取消") } }
    )
}

private fun formatBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024) return "%.1f KB".format(kb)
    return "%.1f MB".format(kb / 1024.0)
}
