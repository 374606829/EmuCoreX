# 最小 MVP：ReShade 风格预设 + Vulkan 出画

## 1. 文档目的与源码位置

| 项目 | 路径 |
|------|------|
| **本模拟器 / 安卓端工程** | `D:\PS2\EmuCoreX-official-latest` |
| **ReShade 上游源码（FX 编译器、SPIR-V 生成等）** | `D:\PS2\reshade-main` |
| **MVP 验收用的 PC 版预设（内容基准）** | `D:\laohei\pcsx2 1.7\1.默认·适应大部分.ini` |

本文件用于**指导开发团队落地**「最小可演示」版本：在仅支持 **Vulkan** 的前提下，**加载上述 PC 版 ReShade 预设 INI**（及同目录下的 `*.fx` 与依赖资源），**解析成功并完成正常画面输出**（无崩溃、画面正确经链式后处理到屏幕）。

---

## 2. 当前进度与验收差距（2026-04 更新）

### 2.1 已完成（可认为「前半段 MVP」已落地）

| 领域 | 说明 | 主要源码位置 |
|------|------|----------------|
| **安卓 UI** | 预设列表、`Techniques` 展示、从 `.fx` 解析 uniform 元数据（含中文 UI 标签）、滑条编辑、`保存` 写回 INI、主开关与镜像路径 | `app/src/main/java/com/sbro/emucorex/ui/emulation/ReShadePane.kt`、`ReShadePresetParser.kt`、`ReShadeStorage.kt`、`ReShadeUiPreferences.kt` |
| **预设 INI 解析（Native）** | `Techniques=` 链、各 `[*.fx]` 参数节 | `app/src/main/cpp/pcsx2/GS/Renderers/Vulkan/ReShade/ReShadePresetINI.cpp` |
| **FX → SPIR-V 编译** | 嵌入 `reshadefx`，按 include 路径编译 `.fx` | `ReShadeFXCompiler.cpp`、`app/src/main/cpp/3rdparty/reshadefx/` |
| **GS 挂钩** | 首帧懒加载链、`BeginPresent` 在 swapchain pass 前调用 `Apply()`；交换链缩放时 `NotifyBackbufferResized`；预设目录约定为 `<DataRoot>/shaders/reshade/`（`preset.ini` 或中文文件名回退） | `GS/Renderers/Vulkan/GSDeviceVK.cpp`、`ReShade/ReShadeChainVK.cpp` |

### 2.2 尚未达到文档「DoD」的部分（后半段 MVP）

1. **Vulkan 全屏 Pass 执行未接线**  
   `ReShadeChainVK.cpp` 中 `ClassifyEffect()` 在校验通过后仍**强制** `runnable = false`，原因为 `Pass execution not yet wired into Vulkan runtime (MVP)`。因此 `IsActive()` 恒为假，`Apply()` 不执行任何后处理，画面为 **passthrough**。  
   `LoadPreset()` 以 **`runnable > 0`** 为成功条件，当前会打出 **`LoadPreset reported no runnable effects`**。

2. **验收预设中的 `CMAA_2.fx` 被 MVP 能力集拒绝**  
   运行时会归类为：`Multi-render-target passes are not supported in this MVP runtime`。在**未实现 MRT** 前，该效果**不会进入可执行链**；即使完成 Pass 执行器，也需**单独排期**「多 RT Pass」或**改用**单 RT 的 AA 预设做首版验收。

3. **UI 滑条与画面**  
   滑条主要更新内存/写 INI；**无**每帧 JNI 推送 uniform 到 Native。在 **`Apply()` 未跑通前**，拖滑条**不应期望**画面变化；写回 INI 后，也需在执行层**读 preset 参数并填 UBO** 后才能看到效果。

### 2.3 典型 logcat（说明「编译通了、但没出画」）

当出现下列日志时，表示 **解析 + 编译成功，但执行链未启用**，与当前代码一致：

```text
[ReShade] 'ArcaneBloom.fx' compiled but is not runnable ... Pass execution not yet wired into Vulkan runtime (MVP)
[ReShade] 'CMAA_2.fx' compiled but is not runnable ... Multi-render-target passes are not supported ...
[ReShade] 'PD80_03_Filmic_Adaptation.fx' compiled but is not runnable ... Pass execution not yet wired ...
[ReShade] Preset loaded: 3 effects parsed, 3 compiled, 0 runnable (rest fall back to passthrough)
[ReShade] LoadPreset reported no runnable effects; chain stays in passthrough
```

**验收目标日志（待实现后）**：`runnable` 计数 **> 0**，且不再出现上述「0 runnable」总结行；或至少对**可执行子集**出现 `Compiled 'xxx.fx' OK` 且 **`runnable` 被置为 true**。

---

## 3. 下一步落地任务（开发团队优先级）

下列顺序兼顾依赖关系与可演示性，可按迭代切开。

### P0 — Vulkan 执行器（打通「有画面差异」）

1. **在 `ReShadeChainVK.cpp`（`ChainImpl`）内实现**  
   - 为每个 **已编译且通过 `ClassifyEffect` 图形约束** 的 effect 创建/缓存：`VkShaderModule`（来自已有 SPIR-V）、`VkPipeline`、`VkPipelineLayout`、descriptor set layout（与 `effect_codegen_spirv` 约定的 **set0 UBO + set1 纹理采样器** 对齐）。  
   - **Ping-pong 中间 RT**（与 swapchain/backbuffer 同尺寸或规则缩放），在 `Apply()` 内按 technique 的 pass 顺序：绑输入（上一 pass / `m_current`）、写 UBO、全屏绘制、交换输出纹理。  
   - 最后一 pass 输出仍须落在 **`GSTextureVK*`，并回写 `current_io`**，以便现有 `BeginPresent` 后续 present 路径**无需大改**。

2. **将「仅因占位而设的」`runnable = false` 改为**：执行器资源创建**全部成功**后对该 effect **`runnable = true`**；失败则记录原因并保持 passthrough。

3. **从 `Preset` / INI 节读取 uniform 初值**，在 `LoadPreset` 或每帧 `Apply` 填入 UBO（与 PC 预设键名一致）。

### P1 — 验收预设与 CMAA2

- **方案 A**：为 MVP 扩展 Vulkan 路径支持 **单 pass 多 RT（MRT）**（工作量较大，需与 `GSDeviceVK` 的 render pass 能力对齐）。  
- **方案 B（推荐用于首版闭环）**：另备一份验收 INI，将 `CMAA_2@CMAA_2.fx` **替换为**仅含 **单 RT / 纯 PS** 的 AA（如 `FXAA.fx`），先证明 **链式 + 出屏**；CMAA2 作为后续里程碑。

### P2 — 预设热更新与 UI 联动

- 用户点击「保存」后，若镜像路径即 `<DataRoot>/shaders/reshade/preset.ini`，Native 侧需 **重新 `LoadPreset()`**（或提供 JNI：`Reset` + `LoadPreset`），避免必须重启进程才生效。  
- 可选：滑条拖动时 debounce 写 INI + 通知 GS 线程轻量重载（注意线程安全）。

### P3 — 性能与稳定性

- SPIR-V / `VkPipeline` **磁盘或内存缓存**（preset + fx 内容哈希）。  
- `OUT_OF_DATE`、resize 时释放中间 RT 并在下一帧按新尺寸重建（`NotifyBackbufferResized` 已预留钩子）。

---

## 4. 最小成功标准（Definition of Done）

1. 运行时能指定**一个预设文件路径**（默认或配置项均可），指向与 PC 包中**同名同内容**的 `1.默认·适应大部分.ini`（可放在应用私有目录，开发阶段可从 `D:\laohei\pcsx2 1.7\` 复制整套文件）。  
   **说明**：若坚持 CMAA2 仍不可用，可暂时以 **P1 方案 B** 的替代 INI 作为「最小 DoD」，待 MRT 完成后再换回完整三枚链。
2. 与该 INI **同目录** 提供 ReShade 标准目录结构中的必备文件，至少包括：
   - 预设第一行 `Techniques=` 中列出的全部 `*.fx`；
   - 各 `.fx` 中 `#include` 所依赖的公共头文件（如 `ReShade.fxh` 等，与 PC 端 `reshade-shaders` 包一致即可）。
3. 模拟器在 **Vulkan GS 出画路径** 上，在**最终 Present 前**将「当前帧作为输入」经预设声明的**有序效果链**处理，再输出到 **swapchain**；用户可见为**可玩的正常游戏画面**（允许与无滤镜时在观感上有差异，但不得黑屏/花屏/仅灰屏）。  
   **当前状态**：本条 **未满足**，待 **§3 P0** 完成。

**MVP 不要求**：GLES、与 PC 完全一致的 ReShade 注入/热重载/完整 UI、全社区随机 `.fx` 兼容、深度依赖类效果的精度与 PC 逐像素一致。

---

## 5. 验收基准：预设文件格式（首行与参数块）

以 `1.默认·适应大部分.ini` 为**唯一**验收用例时，**必须成功解析并执行**的链由首行决定：

- `Techniques=ArcaneBloom@ArcaneBloom.fx,CMAA_2@CMAA_2.fx,prod80_03_FilmicTonemap@PD80_03_Filmic_Adaptation.fx`
  - 即顺序执行三个效果：`ArcaneBloom.fx` → `CMAA_2.fx` → `PD80_03_Filmic_Adaptation.fx`（文件名以首行为准，与 PC 目录中实际文件名一致即可）。
- 同一文件内为各 `*.fx` 提供了多节 `[某文件名]`（如 `[ArcaneBloom.fx]`）及键值，用于 **uniform / 用户可调参数** 的落盘，与 PC 上 ReShade 预设**语义一致**；MVP 需落实现参绑定，使画面与「默认参数下 PC」大致同族。

> 说明：同文件中的 `TechniqueSorting=` 行极长，MVP 可**不依赖**其排序逻辑，**以 `Techniques=` 的逗号顺序为唯一效果链顺序**即可。

---

## 6. 技术路线（与 ReShade 仓库的关系）

1. **不要**在安卓端以「整包 Windows ReShade DLL + Hook」方式集成；`reshade-main` 的 `CMakeLists.txt` 以 Windows / 注入架构为主，不适合直接当 APK 主依赖。
2. **应当**复用或裁剪 `D:\PS2\reshade-main` 中的 **ReShade FX 独立编译管线**（README 所指的 `source/effect_*` 等，及 `tools/fxc.cpp` 的用法思想）：将 `.fx` 编译为 **SPIR-V**，在模拟器内用 **已有 Vulkan 设备** 执行多 Pass 全屏效果。
3. 安卓工程已使用 Vulkan 与 `shaderc`（见 `app/src/main/cpp/pcsx2/CMakeLists.txt` 中 `USE_VULKAN` 与 `shaderc_static` 配置），MVP 的 SPIR-V 与管线创建应与 **现有 `GSDeviceVK` 及纹理/命令缓冲生命周期** 一致，避免与 swapchain 的 acquire / present 信号量冲突。

---

## 7. 模拟器侧建议集成点（仅 Vulkan）

以下路径均在 `D:\PS2\EmuCoreX-official-latest\app\src\main\cpp\pcsx2\` 下：

| 区域 | 文件 | 说明 |
|------|------|------|
| Vulkan 设备与出屏 | `GS/Renderers/Vulkan/GSDeviceVK.cpp` / `GSDeviceVK.h` | 核心设备、`BeginPresent` / `EndPresent`、swapchain 材质与 `vkQueuePresentKHR` 前命令缓冲提交流；**ReShade `Apply()` 已在 `BeginPresent` 内调用**。 |
| ReShade 链 | `GS/Renderers/Vulkan/ReShade/ReShadeChainVK.cpp` / `.h` | **下一阶段主战场**：Pass 执行、UBO、管线、中间 RT；当前为编译 + 分类 + passthrough。 |
| 交换链 | `GS/Renderers/Vulkan/VKSwapChain.cpp` / `VKSwapChain.h` | 交换链 image、Present 相关状态。 |
| 着色与缓存 | `GS/Renderers/Vulkan/VKShaderCache.cpp` 等 | 可类比现有管线缓存；ReShade 生成的 SPIR-V 建议**单独缓存**（含 preset + fx 内容哈希），避免每帧全量编译。 |

**实现提示（概念层）**：

- `BeginPresent` 中在将 `m_current` 准备为 `ShaderReadOnly` 之后、向 swapchain 做最终拉伸/Present 之前，已由 `ChainVK::Apply()` 预留插入点；**需补全 `Apply()` 内部实现**（§3 P0）。
- 若首版不导出 GS 深度，链上若某 `fx` **强依赖** ReShade 的 depth 纹理，需提前用占位或从文档中**限定**「本包效果不依赖 depth」；若某枚实际依赖 depth，则必须在对应里程碑**补**深度导出或**更换**测试预设。

---

## 8. 功能拆分（供排期 — 与 §2/§3 对照）

| 模块 | 内容 | 状态（2026-04） |
|------|------|------------------|
| A. 配置与路径 | 读入预设 INI；`<DataRoot>/shaders/reshade/`；错误时日志可辨 | Native + Storage 已基本对齐 |
| B. 解析 | 解析 `Techniques=`；解析各 `[filename.fx]` 节 | **已完成** |
| C. 编译 | `.fx` → SPIR-V；`#include` 搜索路径 | **已完成** |
| D. 运行时 | `VkPipeline`、描述符、中间 RT、UBO、与 GS 提交同步 | **未完成（P0）** |
| E. 验收 | 预设 + 完整 `fx`/`include` 真机跑通、录屏对比 | **待 D + P1** |

---

## 9. 资源与部署检查清单（测试前必做）

- [ ] 从 `D:\laohei\pcsx2 1.7\` 复制 `1.默认·适应大部分.ini` 到目标目录（或应用约定的 `shaders/reshade/`）。
- [ ] 同目录（或项目约定的 `Shaders` 根）包含 `ArcaneBloom.fx`、`CMAA_2.fx`、`PD80_03_Filmic_Adaptation.fx` 及**全部**其依赖的 `.fxh`/贴图（若有）。
- [ ] 若 PC 上依赖 `ReShade.ini` 中的 `EffectSearchPaths` / `TextureSearchPaths`，MVP 可在代码里写死**等价根路径**，与复制后的目录树一致即可；Native 会尝试 `reshade-shaders/Shaders` 子目录作为额外 include 根。

---

## 10. 风险与回退

| 风险 | 缓释 |
|------|------|
| 某 `.fx` 使用了 SPIR-V 后端尚未覆盖的语法/内建 | 首版以验收链为**白名单**；日志打印编译器错误。 |
| `CMAA_2` 依赖 MRT | §3 P1：扩展 MRT 或替换为单 RT AA 预设。 |
| 首帧/换预设卡顿 | 异步编译或磁盘缓存已编译 SPIR-V / pipeline 键（§3 P3）。 |
| 与 swapchain resize、OUT_OF_DATE | 在 `AcquireNextImage` 失败或 `ResizeWindow` 后**重建** ReShade 链与中间 RT 尺寸。 |

回退策略：解析失败或编译失败时**跳过滤镜链**，回退为现有无 ReShade 的 Present 路径，**不得**因此崩溃。

---

## 11. 许可与合规

- `D:\PS2\reshade-main` 以 **BSD 3-Clause** 为主（见该仓库 `LICENSE.md`），集成衍生代码时应在产品「关于/第三方许可」中按条款保留版权声明。
- 分发给用户的 `*.fx` / 贴图包若含第三方内容，应单独核对其许可证。

---

## 12. 参考命令（仅开发用）

将 PC 侧整包拷入安卓可读目录**无固定命令**；以 Android Studio / `adb push` 或应用内首次解压资产为准，**以第 9 节清单对齐为准**。

---

*文档版本：MVP v2（含进度与后续任务）*  
*目标：以 `1.默认·适应大部分.ini`（或 P1 替代链）为硬验收，完成「解析 INI + FX 编译 + **Vulkan 执行链** + 出屏」闭环。*
