# ============================================================================
# arm32-uwp-env.ps1  —  手动构建 ARM32 UWP 编译环境 (替代 VS2026 阉割掉的 vcvarsall)
# 背景: VS2026 vcvarsall 已移除 arm(32位)target, 但 v143 工具集 14.44 的 arm cl
#       与 lib\arm\store 实际可用。此脚本拼出 PATH/INCLUDE/LIB 直接驱动 cl(ARM)。
# 用法: . .\port\arm32-uwp-env.ps1      # 点源, 在当前会话注入环境
#       然后正常跑 cmake / ninja
# ============================================================================

$VS  = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$T   = "$VS\VC\Tools\MSVC\14.44.35207"          # 含 ARM 后端的 v143 工具集
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$VER = '10.0.19041.0'                       # SDK with ARM32 libs (22621 not installed; 26100 does not ship them)
$Ruby = if ($env:APOTHEOSIS_RUBY) { $env:APOTHEOSIS_RUBY }
        elseif (Get-Command ruby -ErrorAction SilentlyContinue) { Split-Path -Parent (Get-Command ruby).Source }
        else { (@('C:\tools\ruby34\bin', 'C:\Ruby34-x64\bin', 'C:\Ruby33-x64\bin') |
                Where-Object { Test-Path (Join-Path $_ 'ruby.exe') } | Select-Object -First 1) }
# Apotheosis: this used to be the single hardcoded path C:\Ruby33-x64\bin, which does not exist on this
# machine -- Ruby is at C:\tools\ruby34 -- so the self-check below would have failed the moment anyone
# tried an ARM build, with nothing to say about why. Ruby is a hard dependency: offlineasm generates
# JSC's LLInt from Ruby, so the engine cannot build without it. Resolution order is deliberate: an
# explicit APOTHEOSIS_RUBY wins, then whatever is on PATH, then the known install layouts. Set
# APOTHEOSIS_RUBY when several versions are installed and the wrong one is first on PATH.
$Perl = 'C:\Program Files\Git\usr\bin'                                   # create_hash_table 等
$Python = "$env:LOCALAPPDATA\Programs\Python\Python312"                  # builtins/bytecode 生成

# --- 完整性自检 ---
$must = @(
    "$T\bin\Hostx64\arm\cl.exe",
    "$T\lib\arm\store",
    "$SDK\Include\$VER\ucrt",
    "$SDK\Lib\$VER\um\arm\WindowsApp.lib",
    "$SDK\bin\$VER\x64\rc.exe",
    "$Ruby\ruby.exe"
)
$bad = $must | Where-Object { -not (Test-Path $_) }
if ($bad) { Write-Host "!! 缺失:" -ForegroundColor Red; $bad | ForEach-Object { Write-Host "   $_" }; throw "环境不完整" }

# --- INCLUDE ---
$env:INCLUDE = @(
    "$T\include"
    "$SDK\Include\$VER\ucrt"
    "$SDK\Include\$VER\shared"
    "$SDK\Include\$VER\um"
    "$SDK\Include\$VER\winrt"
    "$SDK\Include\$VER\cppwinrt"
) -join ';'

# --- LIB (UWP: store 库在前) ---
$env:LIB = @(
    "$T\lib\arm\store"
    "$T\lib\arm"
    "$SDK\Lib\$VER\ucrt\arm"
    "$SDK\Lib\$VER\um\arm"
) -join ';'

# --- PATH ---
# 前置(高优先): 目标 cl(arm) + host 工具(x64, 供 mspdbcore) + SDK bin(rc/mt) + Ruby
# 后置(低优先): Perl/Python —— 放末尾, 避免 Git 的 MSYS 工具(sort/find 等)盖掉 Windows 同名
$front = @(
    "$T\bin\Hostx64\arm"
    "$T\bin\Hostx64\x64"
    "$SDK\bin\$VER\x64"
    $Ruby
) -join ';'
$env:PATH = "$front;$env:PATH;$Perl;$Python"

# CMake 探测时认架构用
$env:VSCMD_ARG_TGT_ARCH = 'arm'

Write-Host "==> ARM32 UWP 环境已注入 (cl=$T\bin\Hostx64\arm, SDK=$VER)" -ForegroundColor Green
Write-Host "    cl 自检: " -NoNewline
& cl 2>&1 | Select-Object -First 1

# 中文 Windows: Python 代码生成脚本默认用 GBK 读 UTF-8 临时文件会炸 -> 强制 UTF-8
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"


# Git 的 MSYS2 Perl 跑 IDL 预处理时,把 /nologo /EP /TP 这三个 MSVC flag 误当 POSIX 路径转换。
# 精准排除这三个(不用 *,否则会连 .idl 文件路径的 POSIX<->Windows 往返转换一起关掉)。
$env:MSYS2_ARG_CONV_EXCL = "/nologo;/EP;/TP"

# Apotheosis: stamp the session so an x64 step run in this same shell refuses instead of quietly
# linking against Lib\10.0.19041.0\um\arm. See assert-target.ps1 for the whole contract.
$env:APOTHEOSIS_TARGET = 'arm32'
