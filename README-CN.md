# Project Apotheosis — EdgeHTML Reborn (中文)

> **请先阅读：**[当前证据 — 2026-09-17](Doc/STATUS-2026-09-17.md)。
> 下方八月的介绍和状态表是历史记录，不代表 .93 已通过验证。


> 将现代 **WebKit/WebCore**（webkitgtk-2.52.4）移植到 **Windows 10 Mobile (ARM32, UWP)**，
> 为 Lumia 950 带回 JIT 加速、GPU 合成的网页浏览体验。
> **[2026年8月 — x64-uwp 线：appx v0.1.8.51 在 Win11 上启动；真实文本（含 CJK）渲染正常；CJK 卡死已根因修复（hash 表空哨兵 bug）；8/8 追踪测试全部通过；GPU 整页拉伸已通过 `WebCoreGpuResize` 修复（表面跟随 ContentArea — 验证 `contents=1024x694`，200×200 方块保持正方形）。GitHub/CI 工作为可选；所有 ARM32 开发/构建均在本机进行。下一步：ARM32↔x64 最大化同步（相同文件树/脚本命名/共享 appx 版本），然后重建 ARM32 引擎验证 Lumia 950。](**)**

> **2026年8月25日 -- 轻量模式在手机上运行。** ya.ru 快速加载且稳定运行（在设置中禁用 JS）。
> example.com 和 news.ycombinator.com 完整渲染。
> 通过二分法确定了即时静默死亡的原因：三个叠加缺陷——无线电中断时引擎线程上的同步 curl、
> 每次导航时的 CA 存储重新解析，以及 AppContainer 内的 JIT 执行。三者均已修复。

## 当前状态 — 2026-09-17

> **2026年9月17日 — x64 状态更新。** `0.1.9.93`（Release x64）已构建并**原地安装**（通过
> `Add-AppxPackage` 原地更新现有包，LocalState 已备份；非全新重建）。最初的简单加载返回 rc=0
> —— **但这不是视觉证明**。GPU 回读显示 `makeCurrent=0`（被驱动忽略）、`GL_VERSION` 为 NULL、
> `fbo=0 tex=0`，却报告“成功”；GPU 渲染**未经验证**。随后发生 **0xc0000005 崩溃
> （WebCore+0x204901f）**，符号化（仅地址，非调用栈）指向 **CSS StyleCalculationTree 的
> std::variant 访问** —— 这**不是**已证实的 JSC/EGL 原因。**原始运行中没有崩溃转储。**
> 计划：GPU 错误 fail-closed 处理、实时崩溃转储收集、独立的渲染/JS 测试；ARM32 稍后处理；
> Git 为可选项。⚠️ 当前 `x64-cycle.ps1` 会删除已安装的包（与其头部说明不符）—— 请勿用于
> 保留数据的更新。详见 [STATUS-2026-09-17.md](Doc/STATUS-2026-09-17.md)。

当前配置：vcpkg ICU 78；harness 使用 SDK 19041，x64 引擎使用 SDK 26100；MSVC 14.44。
今天没有构建 Debug harness，也没有全新重建。没有任何 .94 修复通过验证；
父会话的 CDB 调查仍在进行，其结果尚未纳入本文。

## 历史记录 — 2026年8月及以前

旧说法保留但未针对 .93 重新验证。此前的 JSC/JIT 诊断和 GPU 勾选项
不能证明此次崩溃的原因，也不能证明当前图像显示正确。



---

## 状态

### 真机（Lumia 950, Win10M 15254）— 此前已验证

| 功能 | 状态 |
|------|------|
| WTF + JavaScriptCore CLoop | ✅ |
| WebCore + Cairo 软件渲染 | ✅ |
| 实时交互会话（点击、表单、滚动、键盘） | ✅ |
| JSC JIT（约 5-50× 加速） | ✅ |
| GPU 合成（ANGLE D3D11 FL9.3 + TextureMapper） | ✅ |
| 平滑滚动 / 双指缩放 | ✅ |
| 浏览器外壳（标签页、地址栏、设置） | ✅ |
| 多语言 UI（en/ru/zh） | ✅ |

### x64-uwp 调试构建（2026年8月）

| 组件 | 状态 |
|------|------|
| WebKit CMake configure（x64-uwp，GPU + JIT + FTL_JIT） | ✅ |
| WTF + bmalloc + PAL + JavaScriptCore + WebCore | ✅ |
| 移植层驱动（`WebCoreDriver-gpu.lib`） | ✅ |
| **Harness appx**（manifest `0.1.8.51`） | ✅ **可在 Win11 AppContainer 构建、注册、启动**；可加载真实网页 — `example.com` → `rc=0`, `title=Example Domain`（见日志） |
| **XAML 激活** | ✅ **已修复：绕过 XBF 加载，纯代码 UI 兜底** |
| **引擎 DLL 加载 + JSC 初始化 + `Page::create` + 首次样式解析** | ✅ |
| **x64 文本渲染** | ✅ `about:home` 绘制抗锯齿字形（Tier-3 字体崩溃 8.10 修复，复杂文本路径路由 8.11 修复） |
| **CJK 文本** | ✅ **卡死已根因定位并修复（8.16）** — `FontDataCacheKeyTraits::emptyValue` hash 空哨兵无限循环；simhei 系统回退通过强制捆绑字体；8/8 追踪测试通过 |
| **真实站点** | ✅ `example.com` 加载成功（`rc=0`）；`file://` → `http(s)://` → 大站 阶梯验证进行中 |
| **GPU 合成（ANGLE D3D11 FL9.3）** | ✅ **渲染无失真** — 整页拉伸已通过 `WebCoreGpuResize` 修复（8.16）：在引擎线程按 ContentArea 重建 surface；验证 `contents=1024x694`、多次调整窗口 `rc=0`、无崩溃。真机（ARM32）验证待进行 |
| **ARM32 工具链** | 🔓 已解锁 — 安装 `Microsoft.VisualStudio.Component.VC.14.38.17.8.ARM`（目录）；所有 ARM32 构建/开发**完全在本机**进行 |

> **工具链恢复（2026.8.14）**：Windows 重装后，整条 x64-uwp 线从零重建（LLVM 22.1.8 / CMake 4.0.1 / Ninja / Python 3.12 / vcpkg 13 端口 / ICU75 导入库 / harfbuzz / Ruby 3.4.10 + MSYS2 / perl / ccache）。引擎 DLL 由 `ninja -C build-x64-gpu WebCore -j1` 重建（计划任务 `ApotheosisNinja`）。详见 `Doc/Summary.md`。

### 关键突破（2026年7-8月）

1. **BCryptGenRandom NTSTATUS 修复** — 逻辑反转导致 JSC 初始化成功时崩溃。已在 `RandomDevice.cpp` 修复。
2. **XAML 激活绕过** — 跳过 `Frame::Navigate`/`LoadComponent`（AppContainer 中类型提供者为 null）；直接创建页面。
3. **WebCoreRenderHtml 软件管线（7月）** — HTML→布局→Cairo→位图→屏幕 已在 x64 验证。
4. **工具链恢复（8月）** — Windows 重装后恢复 x64-uwp 线（`reinstall-env.ps1`、ICU75 导入库、harfbuzz 导入库、移除 fontconfig — 0 引用）。完整构建通过。
5. **appx v0.1.8.25 启动成功** — 引擎初始化通过 XAML/JSC/`Page::create`；崩溃被定位到字体解析。
6. **CJK 卡死已根因定位并修复（8.16）** — “字体高度异常 / CJK 豆腐块”实为 `FontDataCacheKeyTraits::emptyValue()` 中的死循环：WK_WINUWP 的 `FontPlatformData` 构造函数给 size-0 的 hash 空哨兵分配了活 `m_scaledFont`，导致 memset-0 槽永远不相等、线性探测死转。修复：size 为 0 时 `m_scaledFont` 保持 null（`if (size)` 守卫，`FontPlatformData.cpp:182`）。CJK 现在通过强制捆绑 simhei 字体解析（`stubs-font-uwp.cpp` + `apotheosisSetForcedBundledFontName`）。完整 8 测试追踪套件通过（single/mixed/wan-meta/kana-n/br-kana/mixed-e/br-euro/cjk-b — 全部 ALIVE，`gdc-> c=U+4E00 g=1078 ok=1`）。
7. **GPU 整页拉伸（已修复 8.16）** — `WebCoreGpuInit` 只在 720×1080 创建一次 ANGLE swapchain，GPU 模式从不调整；`SwapChainPanel` 将其拉伸到 ContentArea，任何宽高比不匹配都会让整页内容变宽。修复：`WebCoreGpuResize`（`WebCoreDriver.cpp`）— harness 投递 resize 任务（新 `PropertySet` + `EGLRenderSurfaceSizeProperty`），在引擎线程重建 GLContext/surface + TextureMapper，然后 `finishInteractionPaint`。x64 验证：resize 后 `contents=1024x694`，多次实时调整窗口 rc=0，200×200 测试方块保持正方形。字体始终是正确的。

---

## 架构

```
Harness (UWP C++/CX App)
   · SwapChainPanel ← GPU | WriteableBitmap ← SW 兜底
        │  C ABI (WebCoreDriver.h)
WebCoreDriver（移植层）
   · Page/frame 管理、事件分发
   · Cairo（软件）| TextureMapper（GPU）渲染
        │
WebKit / WebCore / JSC / WTF
   · 面向 ARM32 UWP App Container 的 WK_WINUWP 补丁
```

## 仓库结构

```
Src/
├── port/         ← WebCore 驱动、stubs、构建脚本、工具链
├── harness/      ← UWP 宿主应用（C++/CX, XAML）
├── tools/        ← WDP 部署 + 诊断
├── angle/include/← ANGLE 头文件
├── Apotheosis/   ← 备用构建（C# 实验）
├── setenv.ps1    ← 环境设置
Doc/              ← 架构、计划、交接说明
```

## 快速构建（x64-gpu，改代码后）

```powershell
. .\Src\setenv.ps1
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /t:Build /v:minimal
```

如需重建引擎 DLL（改 WebKit 源码）：

```powershell
. .\Src\setenv.ps1
ninja -C build-x64-gpu WebCore -j1
# 然后重新构建 harness（见上）
```

### 部署与运行

```powershell
# 安装 AppX
PowerShell -ExecutionPolicy Bypass -File "Src\harness\AppPackages\Harness\Harness_0.1.8.51_x64_Test\Add-AppDevPackage.ps1"
# 启动
cmd /c start shell:AppsFolder\EdgeHTMLReborn.Harness_edmb40rfkwsbg!App
# 查看日志
Get-Content "$env:LOCALAPPDATA\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState\log.txt"
```

> **注意：** 4 GB 内存 — 大型构建前请重启。只使用 `ninja -j1`。内存不足时 `WebCoreDriver.cpp` 可能报 `fatal error C1060`。

---

## 加入我们！

**Apotheosis 正在让 Windows 10 Mobile 重获新生。** 如果你有 C++/CX、WebKit、DirectX、ARM32 交叉编译的经验，或者只是充满热情——我们需要你！

- **我们需要**: C++开发者、测试人员（特别是手上有Lumia 950/950 XL的）、UI/UX设计师、WebKit专家
- **如何贡献**: fork仓库、提交PR、开issues、在Reddit (r/windowsphone)上讨论

---

## 路线图

> **策略（2026年8月）**：所有与 GitHub 相关的任务（CI、GitHub Actions、自动发布、PR 流水线、托管 runner）
> 均为**可选 / 非阻塞**。所有 ARM32 开发/构建任务**完全在本机**进行（无云端 runner）。顺序：本机 x64 验证 → 本机 ARM32 构建产物 → 真机手动验证 →（可选）任何 CI/GitHub 自动化。

- **ARM32↔x64 同步**（已完成 8.16）：ARM32 目录已重命名 `build-clang-*` → `build-arm32-*`（匹配 `build-x64-gpu` = `build-<arch>-<variant>`）；obj 后缀已统一（GPU `.arm32.obj` / JIT `.arm32-jit.obj` / 软渲染 `.arm32-soft.obj`，x64 `.x64.obj`）；`recompile-stubs-arm32.ps1` → `recompile-stubs-x64.ps1`。两架构从同一 `Package.appxmanifest` 构建 → **x64 与 ARM32 的 appx 版本号一致**。
- **本机 x64**（当前）：GPU 拉伸已修复（`WebCoreGpuResize`）。继续站点阶梯（Bing/GitHub/Apple）、Enter/SPA/docs 交互；精简 harness 诊断。
- **本机 ARM32**：完整重建（重新 configure `build-arm32-gpu` + 数小时 `ninja -j1` — 旧 ARM32 树已随系统重装被清除）→ 驱动链接 → ARM32 appx（同版本）。Lumia 屏幕宽度差异无关紧要：surface 始终匹配 ContentArea，任意宽高比均无失真。
- **真机（手动）**：通过 `Deploy-Robust.ps1` + WDP 将 ARM32 appx 部署到 Lumia 950；验证同样的站点阶梯 + GPU。
- **（可选）GitHub CI** — `windows-latest`（7 GB RAM）比 4 GB 开发机构建 x64 更快（`ninja -j8` 每次引擎构建约 40 分钟）；自动发布；fork→PR→自动构建。仅在本机线路通过后考虑。

---

## 鸣谢（Credits）

- [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis) — 原始项目
- [Reddit: Porting WebKitGTK 2.52.4 to Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- WebKitGTK 团队 — 上游引擎

## 许可证

MIT（移植层）；LGPL-2.1/BSD（上游 WebKit + 依赖）。

---

*As is. No support. RnD only. DIY.*
[中文] 2026年8月16日

---

### AI 助手

- **opencode** — https://opencode.ai · 模型 `deepseek-v4-flash-free` (opencode/deepseek-v4-flash-free) · 2026年8月
- **Claude Opus 5** —（稍后添加）
