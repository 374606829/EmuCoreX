# PC 端金手指逻辑移植说明（安卓 / EmuCoreX）

## 1. 文档目的与范围

- **目标**：在安卓端复现 **PC 版 PCSX2 从磁盘加载并解析、生效的 pnach 金手指/补丁** 的语义。玩家在 PC 上能稳定识别、生效的**同一条** `patch=...` 文本，在安卓端也应有**一致**的解析与行为（在同等模拟器状态与文件路径下）。
- **范围（本文约定）**：
  - **包含**：`cheats` / `patches` 目录下的 `.pnach` 文件、`patches.zip` 资源包中的同名逻辑，以及 `LoadPatchesFromString` 能解析的**同套文本语法**；`GameIndex.yaml` 等内嵌的 `[patches]` 文本在 PC 上若走同一套解析器，语义上**等同磁盘字符串**，安卓亦应对齐。
  - **不包含**（与「仅磁盘」无关的 PC 专有路径）：从启动器/平台**内存注入**的 pnach（`LoadAndApplyPatchesFromMemory`、`LoadPlatformPatchesForMod`、多 mod 合并、`s_platform_patch_start_index` 双空间隔离等）。若安卓未实现这些 API，**不影响**与「把 PC 上用户放进 `cheats` 文件夹的 pnach」对齐。

- **参考实现（PC，上游）**：以仓库内 `pcsx2/pcsx2/Patch.cpp` / `Patch.h` 为权威行为定义（下简称 **PC Patch**）。

- **安卓实现（本项目）**：`app/src/main/cpp/pcsx2/Patch.cpp`、 `app/src/main/cpp/pcsx2/Patch.h`（下简称 **安卓 Patch**）。

---

## 2. 磁盘金手指的加载与启用（与 PC 一致的部分）

- **文件名**：`{Serial}_{CRC8}.pnach`（可带通配/无 Serial 等变体，与 PC 的 `GetPnachTemplate` / 枚举逻辑对齐）。
- **编码形态**：`[组名]` 分组；组内为 `key=value` 行；`//` 行注释与行尾注释；`patch=` / `dpatch=` / `gsaspectratio` / `gsinterlacemode` 等指令与 PC 的 `s_patch_commands` 一致时，**解析入口一致**则语义应对齐。

---

## 3. `patch=` 行语法（PC 与安卓应对齐的完整能力）

单条格式（逗号分隔；若**写值**里再含逗号，会拆成 6 段，解析时需拼回，见下）：

```text
patch=<place>,<EE|IOP>,<地址或指针表达式>,<操作数类型>,<写值或指针或随机>
```

- **place（应用时机）**：`0` 仅启动、`1` 每帧（vsync 路径）、`2` 组合、`3` 启动或 GUI 启用时（与 `patch_place_type` 一致）。
- **CPU**：`EE` / `IOP`。
- **第三段 地址**：
  - 无小括号：静态十六进制地址（**不带** `0x` 前缀）。
  - 含 `(...)`：指针链表达式，见第 4 节。
- **第四段 操作数类型**（名称表须与 PC 的 `s_type_to_string` **完全一致**）：

| 名称 | 含义（写入宽度等） |
|------|-------------------|
| `byte` / `short` / `word` / `double` / `extended` | 与 PC 相同 |
| `beshort` / `beword` / `bedouble` | 大端，与 PC 相同 |
| `bytes` | 原始十六进制字节串 |
| `swap` | 交换两 32 位地址（可配合 4 码循环） |
| `float` | 32 位 IEEE-754 写入 |

- **第五段 写值**（及第六段，见下）：
  - 若值中含 **一个** 额外逗号（如 `randint(1,10)`、`randfloat(1,10)`），整行用逗号 `Split` 会得到 **6 段**，实现上应把**第 5、6 段用逗号拼回**为完整 value 再解析（PC 与安卓均已如此处理 `randint`）。

---

## 4. 指针链（与 PC 一致时的行为约定）

- **形式**：`[prefix](base_hex[+off_hex+...])[suffix]`
- **地址链**：`prefix` 为**恰好一个**十进制数字字符 `0`–`9`（如 `2(3d4bdc+10)`）→ 解析结果为**最终写地址**（末段 offset 不再次解引用）。
- **值链**：`prefix` 为空，或**不是**「单个数字」→ 为 **§12.1 值链**，解析结果为**最终地址处读出的 32 位值**，用作 `data`。
- 走链时若规则约定「空指针则跳过行」，与 PC 相同（地址链 `nullopt` 则该行不写）。

**移植注意**：任一字符串若**不是** `randint` / `randfloat` 却以 `(` 出现，会进入指针链解析。PC 上新增的 **`randfloat(...)`** 若安卓未实现，整段会误进指针链，**与 PC 行为不一致**（见第 6 节）。

---

## 5. 随机与浮点写值

### 5.1 `randint(lo,hi)`

- **lo / hi 为十六进制无符号整型**（与 PC 相同），`apply` 时在闭区间内均匀取**整数**，再按本行 `word`/`float` 等按位写入（`float`+`randint` 在 PC 上仍是**整数位型**写 32 位，不是 IEEE 浮点随机，见 PC Patch 说明）。

### 5.2 `randfloat(lo,hi)`（PC Patch 已支持）

- **仅**与第四段 `float` 同用；`lo`/`hi` 为**十进制**浮点字面量；在「十分位」上均匀随机，结果保留**一位小数**，再写成 IEEE-754 的 32 位。

### 5.3 固定 `float` 字面量

- 应为**十进制**（如 `1.5`）；PC 使用 `StringUtil::FromChars<float>` 以兼容各平台；安卓若仅使用 `std::from_chars` 解析 `float`，在部分 C++ 标准库上可能与 PC 不完全一致，**建议与 PC 对齐为 `StringUtil::FromChars<float>`**。

### 5.4 负十六进制写值

- 非 `float`/`bytes` 时支持以 `-` 前缀表示负值，与 PC 相同。

---

## 6. 扩展行（`extended`）与 D/E 条件、F-gate、SkipCount

- **Extended 子类型**（`handle_extended_t` 体系）：8/16/32 位写、批量写、多指针链（`0x6…` 等）、D/E 条件、**F-gate 三行块** 等，应以 **PC `Patch.cpp` 中同一组掩码与顺序** 为基准做安卓对齐。
- **D/E 的 `skip_count`（跳过后续若干「行」）**：
  - **PC**（`ApplyPatch` 入口）：在解析指针链之前，**只要** `SkipCount > 0`，**任意类型**的补丁行都先递减并 return（用于消费「应跳过的行」），见 PC Patch 中注释「unified check for **ALL** types」。
  - **安卓**若仅在 `p->type == EXTENDED_T` 时于 `ApplyPatch` 中递减 skip，且非 extended 的 `patch` 行不消费 skip，则与 PC **不等价**，混排 D 码与常规 `patch` 行时会出现**漏跳/多跳**。
  - **建议**：`ApplyPatch` 入口的 skip 逻辑与 **PC 完全一致**；`handle_extended_t` 内部的 skip 处理是否需二次对齐，以 PC 现实现为准对拍。

- **F-gate**：三行 `extended` 组成 open / close / guarded；第三行在指针链解析**之前**捕获，与 PC 相同；`ApplyFGateUpdates` 每帧更新、门开时 `ApplyPatch` 守护行。安卓使用 `EEMemoryInterface` 等时，F-gate 内读显存/条件比较路径须指向真实 EE 内存，与 PC 的 `memRead16` 等一致。

---

## 7. 其它行类型

- **`dpatch=`**：版本号 + 模式/替换表，与 PC `PatchFunc::dpatch` 一致（当前仅 type `0`）。
- **`gsaspectratio` / `gsinterlacemode`**：与 PC 一致时，对 GS/配置的覆盖语义一致；安卓 `Patch.h` 中若多 `override_aspect_ratio_mode` 等扩展，属 **UI/配置** 层差异，不改变 `patch` 行内存写入语义。

---

## 8. 与 PC「磁盘金手指」对齐的检核清单（给维护者）

以下每一项若与 PC 行为不一致，即视为**未完全满足**「PC 能识别生效的语法在安卓都生效」：

| 项目 | 说明 |
|------|------|
| `patch` 五段/六段与 `s_type_to_string` | 与 PC 表一致，含 `swap`/`float` |
| 指针链地址链 / 值链 | 与 PC `PointerChainExpr` 规则一致 |
| `randint` | 十六进制 `lo,hi`；`apply` 时整数均匀随机 |
| `randfloat` | 与 PC 同语义；且**不得**被指针链误解析 |
| 固定 `float` | 与 PC 同解析路径（建议 `StringUtil::FromChars<float>`） |
| D/E + `skip_count` | `ApplyPatch` 入口对**所有**类型行的 skip 消费与 PC 一致 |
| `extended` 全套子码 + F-gate | 与 PC `handle_extended_t` / `FGate` 对拍 |
| 热重载 / 调试日志 | 非金手指**语义**必须项；可选 |

---

## 9. 参考与同步方式

- 以 **PC `pcsx2/pcsx2/Patch.cpp`** 为功能基准；在 PC 上合并上游或本地修改后，用本清单对 **安卓 `Patch.cpp/.h`** 做 diff 回归。
- 项目内若另有《金手指优化》等设计文档，与 **PC `Patch.cpp` 中 § 注释** 冲突时，以 **PC 现源码行为** 为准，并更新本文件对应小节。

---

## 10. 版本说明

- 本文档随 EmuCoreX 仓库维护；**PC 参考路径** 以开发者本地 `D:\PS2\pcsx2\pcsx2\Patch.cpp`（或上游 PCSX2）为准。
- 若需记录「某次已对齐的 PC 提交 / 文件哈希」，可在此处追加子节。
