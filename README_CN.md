# Project Apotheosis — EdgeHTML Reborn

> 将现代 **WebKit/WebCore**（webkitgtk-2.52.4）移植到 **Windows 10 Mobile（ARM32, UWP）**。
> 为 Lumia 950 带来 JIT 加速、GPU 合成的浏览器引擎。

## 状态

### 真机 (Lumia 950, Win10M 15254)

| 功能 | 状态 |
|------|------|
| WTF + JavaScriptCore CLoop | ✅ |
| WebCore + Cairo 软件渲染 | ✅ |
| 实时交互会话（点击、表单、滚动、键盘） | ✅ |
| JSC JIT (~5-50× 加速) | ✅ |
| GPU 合成 (ANGLE D3D11 FL9.3 + TextureMapper) | ✅ |
| 平滑滚动 / 双指缩放 | ✅ |
| 浏览器界面（标签页、地址栏、设置） | ✅ |
| 多语言 UI (中/英/俄) | ✅ |

### x64 PC 调试构建（进行中）

| 组件 | 状态 |
|------|------|
| 依赖项（vcpkg 16包、ICU、SQLite、ANGLE） | ✅ 已安装 |
| WebKit CMake 配置 | ✅ 首次成功 (6月29日) |
| WTF + bmalloc 编译 | ✅ 完成（12+ 个 WK_WINUWP 补丁） |
| PAL 头文件 | ✅ 已生成 |
| GNU 驱动（clang++）处理 AT&T 汇编文件 | ✅ 两个文件（LowLevelInterpreter + MacroAssemblerX86_64）均通过 |
| JavaScriptCore → `bin/JavaScriptCore.dll` | 🔄 编译中 ~8/111，仅警告 |
| CMake 4.0 缺失规则修复 | ✅ `patch-build-ninja-gnu.ps1` 自动扫描补充 |
| WebCore → `bin/WebCore.dll` | ❌ 等待 JSC |
| 驱动层 → `WebCoreDriver-x64.dll` | ❌ |
| Harness.appx | ❌ |

## 架构

```
Harness (UWP C++/CX 应用)
   · SwapChainPanel ← GPU | WriteableBitmap ← 软件回退
        │  C ABI (WebCoreDriver.h)
WebCoreDriver (移植层)
   · 页面/框架管理、事件分发
   · Cairo 软件渲染 | TextureMapper GPU 渲染
        │
WebKit / WebCore / JSC / WTF
   · WK_WINUWP 补丁（ARM32 UWP App Container）
```

## 仓库

本仓库只跟踪**移植层和宿主**，不包含 GB 级的上游 WebKit 源码。

```
Src/
├── port/        ← WebCore 驱动、stub、构建脚本、工具链
├── harness/     ← UWP 宿主应用（C++/CX, XAML）
├── tools/       ← WDP 部署和诊断脚本
├── angle/include/ ← ANGLE 头文件
└── setenv.ps1   ← 环境设置
Doc/             ← 文档（计划、总结、多语言 Wiki）
```

## 构建

```powershell
. .\Src\setenv.ps1
pwsh -File Src/port/link-driver-gpu.ps1       # ARM32
pwsh -File Src/port/build-harness.ps1          # Appx
pwsh -File Src/tools/deploy-launch.ps1 -Ip ... # 部署到 Lumia
```

用于 x64 调试：
```powershell
. .\Src\setenv.ps1
Set-Item -Path env:APOTHEOSIS_ARCH -Value x64
ninja -C build-x64-gpu JavaScriptCore WebCore
pwsh -File Src/port/link-driver-gpu-x64.ps1
```

## 鸣谢

- [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis) — 原始项目
- [Reddit: 将 WebKitGTK 2.52.4 移植到 Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- WebKitGTK 团队 — 上游引擎

## 许可

MIT（移植层）；LGPL-2.1/BSD（上游 WebKit 和依赖项）。

---

*按原样提供。无技术支持。仅供研究。*
