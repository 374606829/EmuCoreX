# EmuCoreX

[![License: GPL v3+](https://img.shields.io/badge/License-GPLv3%2B-blue.svg)](LICENSE)
[![Support the project](https://img.shields.io/badge/Donate-Support%20EmuCoreX-ff5f45.svg)](https://send.monobank.ua/jar/9ZocYsprhJ)

EmuCoreX is a PlayStation 2 library and launcher for Android. It pairs a custom Android interface with a PCSX2-based emulation core adapted by EmuCoreX for Android.

Official website: https://emucorex.web.app/

![Status](https://img.shields.io/badge/Status-Early%20Development%20%2F%20Unstable-red)

## 近期更新 · Recent updates

本仓库在持续迭代中，**近期重点包括**：

- **联机（Netplay / LAN）**：面向多人同步与局域网联机场景的核心与桥接工作（含 Android 端相关 UI 与 `Netplay` 模块），在兼容性与网络稳定性上仍在完善。
- **ReShade 风格着色器**：在 Vulkan 渲染路径中集成 ReShadeFX / 相关着色与预设管线（见 `ReShade` 与 `reshadefx` 等目录及游戏内 ReShade 面板），用于后处理与画质增强；具体效果因设备、驱动与游戏而异。

*English: This tree emphasizes **netplay/online-oriented integration** and **ReShade-style post-processing** on the Vulkan path; both areas are under active development.*

> [!WARNING]
> EmuCoreX is currently in the early stages of development. Expect instability, visual issues, performance drops, random slowdowns, and occasional crashes depending on the game, device, renderer, and driver stack.
>
> The current Android focus is mid-range and high-end phones. Budget devices are not optimized yet.
>
> At this stage, optimization work is mainly focused on Snapdragon devices. MediaTek optimization is still incomplete and may improve later.
>
> If you are using a MediaTek device, try the OpenGL renderer first. If that is still unstable or too slow for a specific game, try Software rendering as a fallback.
>
> Current rough minimum chipset recommendations as of April 2026:
> - Snapdragon: Snapdragon 8 Gen 2 or Snapdragon 7+ Gen 3 class devices
> - MediaTek: Dimensity 9300 or Dimensity 8400 class devices
>
> These are practical starting points, not guarantees. Cooling, GPU drivers, RAM bandwidth, renderer choice, and the game itself still matter a lot.
>
> Not all games work correctly yet. Compatibility, fixes, and performance optimization are still in active development.

## Highlights

- **Netplay / 联机**：联机与局域网联机相关能力在持续合入与打磨（以当前分支中的 `Netplay` 与 Android 联机 UI 为准）。
- **ReShade-style shaders / 着色器增强**：Vulkan 侧 ReShade 风格后处理与预设管理，便于画质调节与效果实验。
- PCSX2-based emulation core adapted by EmuCoreX for Android
- Home screen with cover art, game metadata, recent games, and search
- BIOS and game folder setup, with recovery when folders become invalid
- In-game overlay for renderer, aspect ratio, resolution, speedhacks, cheats, FPS, and quick actions
- Save state manager, BIOS boot, and library navigation from the side drawer
- RetroAchievements integration and a dedicated achievements screen
- Cheat management with `.pnach` import, editing, and per-game activation in overlay
- Advanced graphics and GS hack controls, including device-safe defaults for MediaTek
- Physical gamepad remapping and gamepad-aware UI flows

## What This Repository Contains

This repository contains the Android app, UI, settings, bridge code, and bundled native core sources used by EmuCoreX.

## Tech Stack

- Kotlin + Jetpack Compose
- Android DataStore
- JNI bridge to native C++
- Emulation core derived from PCSX2 and integrated into EmuCoreX's native Android stack
- Firebase services used by the Android app

## Current App Scope

EmuCoreX currently targets Android with:

- `minSdk 29`
- `targetSdk 36`
- package id `com.sbro.emucorex`
- version `0.1.1`

## Building Locally

### Requirements

- Android Studio with Android SDK and NDK configured
- JDK compatible with the Gradle setup in this project
- A device or emulator for Android testing

### Debug Build

```powershell
.\gradlew :app:assembleDebug
```

### Release Build

```powershell
.\gradlew :app:assembleRelease
```

## Project Structure

- `app/` Android application module
- `app/src/main/java/com/sbro/emucorex` Kotlin app code
- `app/src/main/cpp` Native bridge and core sources
- `app/src/main/res` Android resources and translations

## Notes

- BIOS files and game images are not distributed with this project.
- You must use your own legally obtained BIOS files and game dumps.
- Compatibility, performance, and graphics behavior vary by device and renderer.

## Credits

EmuCoreX builds on the open-source PCSX2 project together with its own Android interface, library system, runtime controls, and handheld-focused UX. The Android bridge has also been rewritten in Kotlin, and the core has been adapted by EmuCoreX for Android. Further work on stability, integration, and core improvements is planned.

- PCSX2: https://github.com/PCSX2/pcsx2

## Support

If you want to support ongoing development:

- Website: https://emucorex.web.app/
- Donate: https://send.monobank.ua/jar/9ZocYsprhJ
- More apps by the author: https://play.google.com/store/apps/dev?id=7136622298887775989

## License

This project includes and derives from GPL-licensed PCSX2 code, so the repository is distributed under the GNU General Public License v3.0 or later.

See [LICENSE](LICENSE) for details.
