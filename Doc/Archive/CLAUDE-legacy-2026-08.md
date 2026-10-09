# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这是什么

**Project Apotheosis / EdgeHTML Reborn** —— 把现代 WebKit/WebCore（webkitgtk-2.52.4）移植到 **Windows 10 Mobile / Lumia 950（ARM32, UWP, App Container）**，让被微软放弃的 Windows Phone 跑真实现代网页，带 JIT 与 GPU 合成。这是"复活 Win10M 生态"大计划的浏览器引擎组件。当前开发线在 **x64-uwp 构建机**（`build-x64-gpu` = GPU+JIT 线）上迭代，目标是稳住 x64 后回归 ARM32。

**2026-08 现状**：appx v0.1.8.51 在 Win11 上启动、真文本（含 CJK）渲染正常，CJK 卡死已根因定位并修复（`FontDataCacheKeyTraits::emptyValue` hash 空哨兵无限循环 → `FontPlatformData.cpp:182` 的 `if (size)` 守卫），8/8 追踪测试全过。**GPU 整页拉伸已修复（16.08）**：`WebCoreGpuResize` 在引擎线程重建 GLContext/surface（新 `EGLRenderSurfaceSizeProperty`）+ TextureMapper 并 `finishInteractionPaint`；验证 `contents=1024x694`、多次改窗口 rc=0、200×200 方块保持正方形（详见 AGENTS.md "Rendering distortion"）。**ARM32↔x64 命名同步已完成（16.08）**：构建目录 `build-clang-*`→`build-arm32-*`（对齐 `build-x64-gpu` 的 `build-<arch>-<variant>`）、obj 后缀统一（GPU `.arm32.obj` / JIT `.arm32-jit.obj` / 软渲染 `.arm32-soft.obj`）、`recompile-stubs-arm32.ps1` 更名为 `recompile-stubs-x64.ps1`（实为 x64 脚本）；两架构共用同一 `Package.appxmanifest` → **appx 版本号天然一致**。**下一步：重建 ARM32 引擎**（reconfigure + 多小时 `ninja -j1`）验证真机。**计划策略（2026-08）**：GitHub/CI 相关任务一律 OPTIONAL；所有 ARM32 开发/构建任务**完全本地**（本机构建 `build-arm32-*`/`build-x64-gpu`，真机手动验证）。

真机（Lumia 950, Win10M 15254）已验证真实跑通：WTF+JSC CLoop → WebCore+Cairo 软渲染（Bing/GitHub/Apple 等真实站点）→ 真实事件交互 → JSC JIT → ANGLE(D3D11 FL9_3)+TextureMapper GPU 合成直呈现到 SwapChainPanel → 平滑滚动 + 捏合缩放。UI 正演进为 Safari/Edge 形态。

## 关键约束（先读，违反必踩坑）

- **只能用 ASCII 路径**：构建链（Ruby 代码生成器 / meson）对非 ASCII 路径敏感 → 仓库必须在无空格 ASCII 路径（当前 `C:\Users\media\source\repos\!Vibe\Apotheosis`），vcpkg 在 `C:\vcpkg`。所有脚本经 `$env:APOTHEOSIS_ROOT`（由 `Src\setenv.ps1` 设置）取相对路径，**不要硬编码绝对路径**。原中文路径炸过生成器。
- **三套工具链并存，别混**：
  - 引擎 WTF/JSC/WebCore = **clang-cl**（LLVM 22.1.8；ARM32 用 `--target=thumbv7-unknown-windows-msvc`，x64 用 `x86_64-unknown-windows-msvc`；WebKit 已弃纯 MSVC）。
  - 移植驱动 `Src\port\*.cpp` = clang-cl，链 **lld-link**。
  - harness（C++/CX UWP）= **MSVC v143（VS 2022 Community，工具集 14.44.35207）ARM32**。arm32 用 `arm32-uwp-env.ps1` 手搓环境；本机 x64 构建用 VS2022 全功能环境。
- **C++ 异常必须关**：clang 的 thumbv7-windows-msvc 后端无法 lower `cleanupret`（Windows 异常展开）→ `_HAS_EXCEPTIONS=0` + `/EHs-c-`。
- **所有上游 WebKit 改动用 `#if defined(WK_WINUWP)` 守卫 + `Apotheosis:` 注释**，只影响本 port，不污染上游语义。
- **软件渲染是通用底座，GPU 运行时切换**：合成开关严格 gate 在 `g_gpuActive`（默认 false，仅 `WebCoreGpuInit` 成功后置 true）。GPU 未起时回 Cairo 软渲染 + EmptyChromeClient（零回归）。曾经无条件开合成导致真机静默闪退（`__fastfail`，无 dump）。

## 仓库布局 / 什么被跟踪

仓库**只跟踪移植层与宿主**，不含 GB 级上游与可重下二进制：

- `port/` —— WebCore 驱动 + Port 层客户端 + 各 stub + 构建/链接脚本。⚠️ 真源码混在**大量一次性调试残留**里（`repro_*.cpp`、`mangle-repro*`、`*.obj/*.lib/*.dll`、`*.log`、`undef-*.txt`、`_*.bat`）——这些是趟编译墙时的实验件，可忽略。核心源码见下。
- `harness/` —— UWP 宿主 App（C++/CX、XAML、`Package.appxmanifest`、签名证书 `.cer`/`.pfx`）。
- `tools/` —— Device Portal（WDP）远程部署 / 抓崩溃 dump / 自动诊断脚本。
- `angle/include` —— ANGLE 头（跟踪）；`angle/arm`、`angle-windowsstore` 二进制 gitignore（可重下）。
- **不在仓库**：`WebKit/`（sparse webkitgtk-2.52.4，GB 级，gitignore；上游 ARM32/App-Container 补丁清单记在**项目记忆**而非仓库）、`build-arm32-*/` `build-release/` `deps-build/`（构建输出）、字体、`*.pfx`、`*.log`。

**核心移植层源码**（在 `port/` 一堆实验件中）：

- `WebCoreDriver.cpp` / `WebCoreDriver.h` —— 引擎对外的 C ABI + 常驻 Page 会话（导航、真实事件派发、链接提取、软/硬呈现）。
- `PortChromeClient.{h,cpp}` —— 非 final 的 `ChromeClient` 子类（EmptyChromeClient 合成钩子是 `final` 不能覆写），开 GPU 合成、捕获根 `GraphicsLayer`、`triggerRenderingUpdate` 置 needsPresent。
- `LoadingFrameLoaderClient.{h,cpp}` —— 真策略回调的 `FrameLoaderClient`（非 Empty，`PolicyAction::Use`）。
- `PortPlatformStrategies` / `PortNetworkStorageSession` —— 装 LoaderStrategy、网络存储会话。
- `stubs-*.cpp` —— 平台未实现符号 stub（crypto/network/pasteboard/ax/loader/other）。
- `Toolchain-ARM32-UWP-clang.cmake` / `arm32-uwp-env.ps1` / `clang-cl-arm-shim.h` —— 工具链与环境。

## 架构（大图，要读多文件才看得清）

三层经 C ABI 解耦：

```
Harness (C++/CX UWP, MSVC v143)         harness/
  · MainPage: 地址栏/工具栏 + 触摸手势 → 引擎滚动/点击/缩放/选择
  · GpuPanel (SwapChainPanel) ← GPU 直呈现 | RenderImage (WriteableBitmap) ← 软件回退
        │  C ABI = WebCoreDriver.h  (extern "C")
WebCoreDriver (port/, clang-cl → WebCoreDriver-gpu.lib)
  · 常驻 Page/Frame/FrameView 会话、真实事件派发、链接提取
  · 两条呈现路：Cairo paintToRGBA（软件） | TextureMapper→ANGLE swapchain（GPU）
  · PortChromeClient / LoadingFrameLoaderClient / Port*Strategies
        │
WebCore / JavaScriptCore / WTF (clang-cl, thumbv7-windows-msvc, App Container)
  · ARM32 / App Container 补丁全部 WK_WINUWP 守卫；上游源不在本仓库
```

- **线程铁律**：present **只在引擎线程**；UI 线程**绝不同步 wait 引擎**（ANGLE 把 surface create/resize marshal 回 panel dispatcher，互等 = 死锁，`RunOnUIThread` 超时会 `std::terminate`）。所有 C ABI 调用在**单一引擎线程**串行化。
- **C ABI 有两份副本**：`port/WebCoreDriver.h` 与 `harness/WebCoreDriver.h`。加/改导出时**两份必须同步**，否则 ABI 不一致。
- **GPU 合成 recipe**（`WebCoreComposite`，镜像 WebKit 的 `WCScene::update`）：`flushCompositingStateIncludingSubframes` → `updateBackingStoreIncludingSubLayers` → `applyAnimationsRecursively` → `beginPainting`/`paint`/`endPainting` → `eglSwapBuffers`（直呈现）或 `glReadPixels`（离屏 readback 验证）。根层 = `PortChromeClient::rootLayer()`，实为同步 `GraphicsLayerTextureMapper`（`USE_COORDINATED_GRAPHICS=0`）。

引擎构建配置（x64 最新）：

| 构建目录 | 配置 | 用途 |
|---|---|---|
| `build-arm32-webcore` | Cairo 软渲染 | Phase 1b 基线（ARM32） |
| `build-arm32-jit` | + JSC JIT | JIT 线（ARM32） |
| `build-arm32-gpu` | + GPU（TextureMapper+ANGLE）+ JIT | ARM32 GPU 线 |
| `build-x64-gpu` | x64-uwp：GPU + JIT（含 FTL_JIT） | **x64 当前开发线** |

> **ARM32↔x64 命名同步（已完成 2026-08-16）**：ARM32 目录统一为 `build-arm32-*`（与 `build-x64-gpu` 同为 `build-<arch>-<variant>`）；obj 后缀统一：GPU `.arm32.obj` / JIT `.arm32-jit.obj` / 软渲染 `.arm32-soft.obj`，x64 `.x64.obj`。

分支：`gpu-path1` = GPU 开发线；`master` = 纯 JIT 封存线。x64 开发对应 `build-x64-gpu`。

## 常用命令（PowerShell 7 / pwsh）

先 `. .\Src\setenv.ps1`（设置 `$env:APOTHEOSIS_ROOT`）。

改 `Src/port/*.cpp` 驱动后，重编 + 重链 GPU 驱动（产出 `WebCoreDriver-gpu.lib`）：

```powershell
# x64 开发线（本机）：
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\link-driver-gpu-x64.ps1
# ARM32 线（真机）：
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\link-driver-gpu-arm32.ps1
```

单文件编译验证（快，定位编译错）：

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\compile-driver-gpu-x64.ps1 $env:APOTHEOSIS_ROOT\Src\port\WebCoreDriver.cpp $env:APOTHEOSIS_ROOT\Src\port\WebCoreDriver.x64.obj
```

> ⚠️ `link-driver-gpu-arm32.ps1` / `compile-driver-gpu-arm32.ps1` **仅限 ARM32**（内部经 `arm32-uwp-env.ps1`，在 x64 本机硬失败）。x64 开发一律用 `*-x64.ps1` 变体。

改了上游 WebCore 源（WK_WINUWP 补丁）后，增量重编引擎，再重链驱动：

```powershell
. .\Src\setenv.ps1
& "C:\Program Files\CMake\bin\ninja.exe" -C $env:APOTHEOSIS_ROOT\build-x64-gpu WebCore -j1
pwsh -File $env:APOTHEOSIS_ROOT\Src\port\link-driver-gpu-arm32.ps1
```

构建 harness appx（x64，单次 msbuild）：

```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /p:MinimalTest=true
```

部署到真机并启动（交互测，不轮询）：

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\tools\deploy-launch.ps1 -Ip <设备IP> -Ver <版本号>
```

全自动诊断回路（卸→装→启→轮询拉 `LocalState` 的 dump/BMP 截图；仅当设备里有 `autodiag.txt` 时触发）：

```powershell
pwsh -File $env:APOTHEOSIS_ROOT\Src\tools\auto-diag2.ps1
```

- **x64 开发线**（本机，非 ARM）：`build-x64-gpu` 目录；改 `src/port/*.cpp` 后重链 `WebCoreDriver-gpu.lib`，再 msbuild harness。ARM32 线用 `port\configure-gpu*.ps1` / `link-driver-jit-arm32.ps1` 等。
- 升版本号改 `Src\harness\Package.appxmanifest`，deploy 脚本 `-Ver` 要对上。
- 量 appx 大小用 PowerShell `.Length`（**别用 `ls -la`**，Windows 属主名带空格会把列读偏）。
- 本机（x64）**ARM32 appx 跑不了**——x64 验证用 Win11 x64 的 AppContainer（"x64 模拟器" = dev 机），ARM32 引擎验证唯一靠真机。设备常因省电掉 WiFi，部署易传一半断，用 `tools\Deploy-Robust.ps1` 容错重试；远程时只产出 appx 交用户部署。

## 真机部署前置

设备：开机、同一 WiFi、设置→面向开发人员→开 **Device Portal**。appx 依赖 `Microsoft.VCLibs.140.00 (ARM)`（设备多半已由其他 -Reborn 应用装上）。**HTTPS 在 App Container 无系统证书库** → 打包 `cacert.pem`，启动时 `WebCoreSetCACertPath` 注入（curl/OpenSSL 自带 TLS 1.3，不靠 OS 的只到 1.2 的 Schannel）。

## 项目记忆（深层背景在这）

每个里程碑的**根因 / 试错 / 真机数据点**、以及**上游 WebKit 补丁清单**都在 Claude Code 项目记忆（`MEMORY.md` 索引 + 各 `.md`），不在仓库里。动手前先扫 `MEMORY.md`。仓库内还有 `HANDOFF.md`（Phase 0，偏早）、`M2-HANDOFF.md`（GPU 呈现细节）、`README.md`。

---

## AI 助手 (AI agents)

- **opencode** — https://opencode.ai · 模型 `deepseek-v4-flash-free` (opencode/deepseek-v4-flash-free) · 2026年8月
- **Claude Opus 5** —（稍后添加）
