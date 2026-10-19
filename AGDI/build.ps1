<#
    ORBAGDI - AGDI layer build script
    ----------------------------------------------------------------------------
    Mirrors the ORBMDK build paradigm:
        - MSVC x86 (Keil uVision is a 32-bit process)
        - static CRT (/MT)  -> avoids crashing on Keil's bundled old MSVCP140.dll
        - /O2, /std:c++17, /utf-8, no MFC
        - hand written compile + link (no .sln/.vcxproj)

    Output:
        bin\ORB_AGDI.dll      (the AGDI driver)

    The driver is deployed into Keil's ARM\BIN as ORB_AGDI.dll and picked up
    from there; it does NOT replace Keil's own CMSIS_AGDI.dll. At run time the
    driver loads the RDDI layer (CMSIS_DAP.dll, provided by ORBMDK).

    Usage:
        .\build.ps1                 # build (incremental)
        .\build.ps1 -Force          # rebuild every source regardless
        .\build.ps1 -Clean          # delete obj\ and bin\, then exit
        .\build.ps1 -Clean -Deploy  # full rebuild from scratch, then deploy
        .\build.ps1 -Deploy         # build then deploy to Keil ARM\BIN
        .\build.ps1 -DeployOnly     # deploy existing bin\ORB_AGDI.dll
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Force,
    [switch]$Deploy,
    [switch]$DeployOnly,
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$SrcDir    = Join-Path $ScriptDir 'src'
$IncDir    = Join-Path $ScriptDir 'include'
# 第三方移植代码：orbuculum 的 trace 解码器（BSD-3-Clause，见 traceDec/ 与
# COMPAT_ANALYSIS.md §10.21）。单独一个目录，与 DbgCM\ 同样的处理方式 ——
# 来源清楚、便于与上游对照、不会和自研代码混在一起。
$DecDir    = Join-Path $ScriptDir 'traceDec'
# 第三方：capstone 反汇编器（BSD-3-Clause）。源码不在这里 —— 静态库由
# capstone\build.ps1 从 PyPI 的 sdist 编出来（为什么必须自己编：见该脚本与
# Todo.md 的 2026-10-14 那一节）。缺库时下面会直接报错，不会静默少编。
$CapDir    = Join-Path $ScriptDir 'capstone'
$CapInc    = Join-Path $CapDir    'include'
$CapLib    = Join-Path $CapDir    'lib\capstone.lib'
$ObjDir    = Join-Path $ScriptDir 'obj'
$BinDir    = Join-Path $ScriptDir 'bin'
$OutDll    = Join-Path $BinDir   'ORB_AGDI.dll'
$OutPdb    = Join-Path $BinDir   'ORB_AGDI.pdb'
$OutLib    = Join-Path $BinDir   'ORB_AGDI.lib'

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok  ([string]$msg) { Write-Host "    $msg" -ForegroundColor Green }

# 诊断输出（错误 / 警告 / 编译器原话）**只能进成功流** —— 不能走 Write-Host。
#
# 2026-10-14 两次实测，都写在这里免得后人再踩：
#
#   ① `Write-Host` 写的是宿主控制台，**不进管道** —— `| Select-String` 抓不到，
#      PS 5.1 下 `*>&1` 也抓不到（信息流不参与那个重定向）。于是编译失败时调用方
#      只看到最后一句 `compile failed: X.cpp`，真正的 `error C2039: ...` 全被吞掉
#      （当时为了看一眼真报错，只能手搓一遍 cl.exe 命令行）。
#
#   ② 光把 Write-Host 换成 `Write-Output` **还不够**：脚本里一句未捕获的 `throw`
#      会让调用方**整条语句的输出被丢掉** —— 实测 `Write-Output` / `Write-Error` /
#      `Write-Host` 三种全没，只剩 host 报的那句 throw。
#      ⇒ 所以本脚本的失败路径一律走 `Fail()`，它用 **`exit 1`** 而不是 `throw`：
#        输出照常提交，`$LASTEXITCODE` 照样是 1。
#        `test\run-tests.ps1:93` 正是靠 `$LASTEXITCODE` 判构建成败的 ⇒ 兼容。
#
# 分工：**进度** -> Write-Host（要颜色，不必进管道）；**诊断** -> Write-Diag（进管道）。
#
# `$color` 参数保留只为不改调用点，**不再用来上色** —— 上色就得同时写 host，
# 屏幕上每行会出现两遍。诊断行靠内容自己认（编译器那几行本来就带 `error Cxxxx`）。
function Write-Diag([string]$msg, [string]$color = 'Gray') {
    Write-Output $msg
}

# 失败即退出。**不要用 `throw`** —— 理由见上面 ②。
function Fail([string]$msg) {
    Write-Diag "*** $msg"
    exit 1
}

function Write-Err ([string]$msg) { Write-Diag "*** $msg" }

# ---------------------------------------------------------------------------
# Clean
#
# `-Clean` 删掉 obj\ 与 bin\（obj\dep\ 的依赖索引一并消失，所以下一次一定是
# 全量编译），并且**在一切其它步骤之前**执行，因此可以和 -Force / -Deploy
# 叠用（"推倒重建再部署"）。没有要求构建时直接退出，保持原有语义。
#
# 为什么不能只写一行 Remove-Item：部署过之后 bin\ORB_AGDI.dll 仍被 uVision
# 映射着，Remove-Item 抛 .NET IOException，而 $ErrorActionPreference='Stop'
# 会**绕过 Fail()** 直接终止脚本 —— 用户只看到一行宿主抛的原始异常，连前面
# 删掉哪些目录都看不到（同文件头注释 ②）。所以这里捕获异常、逐个文件探测
# 占用，把"该关掉谁"说清楚。
# ---------------------------------------------------------------------------
if ($Clean) {
    Write-Step 'Cleaning'

    # 守卫：只许删本工程的 obj\ 与 bin\。**部署到 Keil 的那份不受影响** ——
    # $Deploy 走的是 Copy-Item，ARM\BIN\ORB_AGDI.dll 是一份独立副本，clean
    # 从头到尾都不碰 Keil 的安装目录。
    #
    # 把这条约束写成代码，防的是两种手滑：
    #   ① 有人把 $BinDir 直接改成 ARM\BIN，想省掉复制这一步；
    #   ② 有人把 bin\ 做成指向 ARM\BIN 的 junction —— PS 5.1 的
    #      Remove-Item -Recurse 在 reparse point 上删除目标内容的记录是有过的，
    #      一旦删穿，整个 Keil 安装目录就没了。
    $projRoot = [System.IO.Path]::GetFullPath($ScriptDir).TrimEnd('\') + '\'
    foreach ($d in @($ObjDir, $BinDir)) {
        $full = [System.IO.Path]::GetFullPath($d)
        if (-not $full.StartsWith($projRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Fail "refusing to clean '$full' - outside the project folder ($projRoot)"
        }
        $info = Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
        if ($info -and ($info.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            Fail "refusing to clean '$full' - it is a junction/symlink; remove it by hand"
        }
    }

    foreach ($d in @($ObjDir, $BinDir)) {
        if (-not (Test-Path -LiteralPath $d)) { Write-Ok "$d  (absent)"; continue }
        try {
            Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction Stop
            Write-Ok "removed $d"
        } catch {
            $why = $_.Exception.Message

            # Remove-Item 会删掉能删的、留下不能删的，所以走一遍残留文件，
            # 用独占打开探测谁还被占着。
            $held = @()
            Get-ChildItem -LiteralPath $d -Recurse -File -ErrorAction SilentlyContinue |
                ForEach-Object {
                    try {
                        [System.IO.File]::Open($_.FullName, 'Open', 'ReadWrite', 'None').Dispose()
                    } catch {
                        $held += $_.FullName
                    }
                }
            if ($held.Count -gt 0) {
                Write-Err "cannot remove $d - still in use:"
                $held | ForEach-Object { Write-Diag "    $_" }
                Write-Diag '    Close uVision (it keeps ORB_AGDI.dll loaded) and retry.'
            } else {
                Write-Err "cannot remove ${d}: $why"
            }
            Fail 'clean failed'
        }
    }
    if (-not ($Deploy -or $DeployOnly)) { Write-Host 'Done.'; exit 0 }
}

if ($DeployOnly) { $Deploy = $true }

# ---------------------------------------------------------------------------
# Deploy only
# ---------------------------------------------------------------------------
function Find-KeilArmBin {
    $cands = @()
    if ($env:KEIL_ARM_BIN) { $cands += $env:KEIL_ARM_BIN }
    foreach ($root in @('C:\', 'D:\', 'E:\')) {
        # An absent drive makes Join-Path throw; with $ErrorActionPreference =
        # 'Stop' that aborts the scan (and the deploy) before the real
        # candidate is ever reached, so probe the root first.
        if (-not (Test-Path $root)) { continue }
        foreach ($k in @('Keil_v5', 'MDK5', 'Keil', 'Keil_v4', 'MDK')) {
            $cands += (Join-Path $root (Join-Path $k 'ARM\BIN'))
        }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c 'CMSIS_AGDI.dll'))) { return $c }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

if ($DeployOnly) {
    Write-Step 'Deploy only'
    if (-not (Test-Path $OutDll)) { Write-Err "missing $OutDll"; exit 1 }
    $armBin = Find-KeilArmBin
    if (-not $armBin) {
        Write-Err 'Keil ARM\BIN not found. Set $env:KEIL_ARM_BIN to that folder.'
        exit 1
    }
    $target = Join-Path $armBin 'ORB_AGDI.dll'
    Copy-Item $OutDll $target -Force
    Write-Ok "deployed -> $target"
    Write-Host 'Done.'
    exit 0
}

# ---------------------------------------------------------------------------
# Locate the toolchain
# ---------------------------------------------------------------------------
function Find-VSDir {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath 2>$null
        if ($p) { return $p.Trim() }
    }
    $roots = @()
    foreach ($base in @(${env:ProgramFiles}, ${env:ProgramFiles(x86)}, 'D:\Program Files', 'D:\Program Files (x86)')) {
        if ($base) { $roots += (Join-Path $base 'Microsoft Visual Studio') }
    }
    $best = $null; $bestVer = -1
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        foreach ($y in (Get-ChildItem $r -Directory -ErrorAction SilentlyContinue)) {
            foreach ($e in (Get-ChildItem $y.FullName -Directory -ErrorAction SilentlyContinue)) {
                $vc = Join-Path $e.FullName 'VC\Tools\MSVC'
                if (Test-Path $vc) {
                    $v = 0
                    if ($y.Name -match '(\d{4})') { $v = [int]$Matches[1] }
                    if ($v -gt $bestVer) { $bestVer = $v; $best = $e.FullName }
                }
            }
        }
    }
    if ($best) { return $best }
    Fail 'Visual Studio with C++ toolset not found.'
}

function Get-NewestVersionDir([string]$root) {
    if (-not (Test-Path $root)) { return $null }
    $dirs = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue
    if (-not $dirs) { return $null }
    $sorted = $dirs | Sort-Object { try { [version]($_.Name -replace '^.*?(\d+\.\d+\.\d+(\.\d+)?).*$','$1') } catch { [version]'0.0.0' } } -Descending
    return $sorted[0].FullName
}

Write-Step 'Locating toolchain'
$vsDir = Find-VSDir
Write-Ok "VS          : $vsDir"

$msvcRoot = Join-Path $vsDir 'VC\Tools\MSVC'
$msvcDir  = Get-NewestVersionDir $msvcRoot
if (-not $msvcDir) { Fail "MSVC toolset not found under $msvcRoot" }
Write-Ok "MSVC        : $(Split-Path -Leaf $msvcDir)"

$sdkRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' `
            -ErrorAction SilentlyContinue).KitsRoot10
if (-not $sdkRoot -or -not (Test-Path $sdkRoot)) {
    foreach ($c in @('C:\Program Files (x86)\Windows Kits\10', 'D:\Windows Kits\10', 'C:\Program Files\Windows Kits\10')) {
        if (Test-Path $c) { $sdkRoot = $c; break }
    }
}
if (-not $sdkRoot) { Fail 'Windows SDK not found.' }
$sdkIncDir = Get-NewestVersionDir (Join-Path $sdkRoot 'Include')
$sdkLibDir = Get-NewestVersionDir (Join-Path $sdkRoot 'Lib')
$sdkVer    = Split-Path -Leaf $sdkIncDir
Write-Ok "Windows SDK : $sdkVer  ($sdkRoot)"

# Prefer the native x86 cross compiler (Hostx86\x86) to avoid mspdbcore.dll
# dependency issues that plague the 64-bit host cross-compilers.
$cl    = Join-Path $msvcDir 'bin\Hostx86\x86\cl.exe'
$link  = Join-Path $msvcDir 'bin\Hostx86\x86\link.exe'
if (-not (Test-Path $cl))   { $cl   = Join-Path $msvcDir 'bin\Hostx64\x86\cl.exe' }
if (-not (Test-Path $link)) { $link = Join-Path $msvcDir 'bin\Hostx64\x86\link.exe' }
if (-not (Test-Path $cl))   { Fail "cl.exe not found under $msvcDir" }

$env:INCLUDE = @(
    (Join-Path $msvcDir   'include'),
    (Join-Path $sdkIncDir 'ucrt'),
    (Join-Path $sdkIncDir 'shared'),
    (Join-Path $sdkIncDir 'um'),
    (Join-Path $sdkIncDir 'winrt')
) -join ';'
$env:LIB = @(
    (Join-Path $msvcDir   'lib\x86'),
    (Join-Path $sdkLibDir 'ucrt\x86'),
    (Join-Path $sdkLibDir 'um\x86')
) -join ';'

# mspdbcore.dll lives in Common7\IDE but the compiler looks for it in its own
# bin folder; add it to the PATH so CL can find the PDB server.
$vsCommon = Join-Path $vsDir 'Common7\IDE'
if (Test-Path $vsCommon) {
    $env:PATH = "$vsCommon;$env:PATH"
}

New-Item -ItemType Directory -Force -Path $ObjDir, $BinDir | Out-Null

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
Write-Step 'Compiling'

$CompilerFlags = @(
    '/c', '/nologo',
    '/O2'              # optimise for speed
    '/MT'              # static CRT
    '/W3', '/EHsc',
    '/std:c++17',
    '/utf-8',
    '/D_CRT_SECURE_NO_WARNINGS',
    '/DWIN32', '/D_WINDOWS',
    "/I$IncDir", "/I$DecDir", "/I$ScriptDir",
    "/I$CapInc"                 # capstone（P1-0 的指令解码器）
)

if (-not (Test-Path $CapLib)) {
    Fail "capstone.lib not found at $CapLib - run capstone\build.ps1 first (see Todo.md 2026-10-14)"
}

$sources  = @()
$sources += Get-ChildItem -Path $SrcDir -Filter *.cpp -File
$sources += Get-ChildItem -Path $DecDir -Filter *.cpp -File
$sources  = $sources | Sort-Object Name
if (-not $sources) { Fail "no sources found in $SrcDir or $DecDir" }

# ---------------------------------------------------------------------------
# Incremental gate — real dependency tracking
#
# cl is invoked with /showIncludes, so it prints every header it opened; those
# lines are captured (and kept off the console) and written next to the object
# as a `.d` list.  The next build walks that list, so **editing a header only
# rebuilds the objects that actually include it**.
#
# The first version of this gate compared against *every* header in the trees,
# which is coarse enough to be useless: touching one header rebuilt all 29
# translation units.  There is no /sourceDependencies here (that is VS2019
# 16.7+; this toolset is VS2017 14.16), so /showIncludes is the way.
#
# build.ps1 itself counts as a dependency of everything: the flags live in it.
# ---------------------------------------------------------------------------
$DepDir = Join-Path $ObjDir 'dep'
New-Item -ItemType Directory -Force -Path $DepDir | Out-Null

# The include notes look like "<localized prefix>: <absolute path>" - English
# "Note: including file:" / Chinese "注意: 包含文件:".  The prefix is localized,
# so the test is on the **shape** (nothing but a prefix and an absolute path on
# the line) plus an existence check, which is what keeps cl's real messages out.
#
# Do NOT filter on a ".h" suffix: the C++ standard library headers are included
# as "string", "istream", ... with no extension at all, and those lines would
# leak into the console.
$IncludeLine = [regex]'^\s*\S[^\r\n]*?:\s+([A-Za-z]:\\[^\r\n]*?)\s*$'

$objs = @()
$compiled = 0
$skipped  = 0
$buildPs1 = Get-Item $PSCommandPath

foreach ($s in $sources) {
    $obj = Join-Path $ObjDir ($s.BaseName + '.obj')
    $dep = Join-Path $DepDir ($s.BaseName + '.d')
    $objs += $obj

    $needBuild = $true
    if (-not $Force -and (Test-Path $obj) -and (Test-Path $dep)) {
        $o = Get-Item $obj
        if ($buildPs1.LastWriteTime -gt $o.LastWriteTime) {
            $needBuild = $true                      # flags may have changed
        } else {
            $newest = $s.LastWriteTime
            $needBuild = $false
            foreach ($line in [System.IO.File]::ReadAllLines($dep)) {
                if (-not $line) { continue }
                if (-not (Test-Path -LiteralPath $line)) { $needBuild = $true; break }
                $h = (Get-Item -LiteralPath $line).LastWriteTime
                if ($h -gt $newest) { $newest = $h }
            }
            if (-not $needBuild -and $o.LastWriteTime -le $newest) { $needBuild = $true }
        }
    }

    if (-not $needBuild) {
        Write-Host "    --  $($s.Name)  (up to date)" -ForegroundColor DarkGray
        ++$skipped
        continue
    }

    Write-Host "    cl  $($s.Name)"
    $out = & $cl @CompilerFlags '/showIncludes' "/Fo$obj" $s.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        # 全量落一份日志：下面那个 include 过滤是给控制台看的（一行 /showIncludes
        # 一个头文件，几千行），排错时要的是**全量**。
        $errLog = Join-Path $ObjDir ($s.BaseName + '.build.log')
        $out | ForEach-Object { "$_" } | Set-Content -LiteralPath $errLog -Encoding UTF8

        Write-Diag "    --- compiler output ($($s.Name)) ---" 'Red'
        foreach ($line in $out) {
            $t = "$line"
            $m = $IncludeLine.Match($t)
            if ($m.Success -and (Test-Path -LiteralPath $m.Groups[1].Value)) { continue }
            Write-Diag "    $t" 'Red'
        }
        Write-Diag "    --- full output: $errLog ---"
        Fail "compile failed: $($s.Name)"
    }

    $headers = New-Object System.Collections.Generic.List[string]
    foreach ($line in $out) {
        $t = "$line"
        $m = $IncludeLine.Match($t)
        if ($m.Success -and (Test-Path -LiteralPath $m.Groups[1].Value)) {
            $headers.Add($m.Groups[1].Value)
            continue
        }
        if ($t.Trim() -eq $s.Name) { continue }   # cl echoes the input file name
        # 警告也走 Write-Diag：本项目的目标是 **0 warning**，所以它们必须能被
        # `| Select-String 'warning'` 抓到，不能只留在控制台上。
        Write-Diag "        $t" 'Yellow'
    }
    [System.IO.File]::WriteAllLines($dep, $headers)
    ++$compiled
}
Write-Ok ("{0} compiled, {1} up to date" -f $compiled, $skipped)

# ---------------------------------------------------------------------------
# Version resource
#
# ORBAGDI.rc carries the VERSIONINFO Explorer shows, and it takes its numbers
# from include\ORBAGDI_Version.h - the same header ORBAGDI.h uses for the session
# banner.  rc.exe lives next to the SDK's Include\ folder (bin\<ver>\x86); when
# it is missing the build continues without it and says so, rather than
# silently shipping a DLL with no version at all.
# ---------------------------------------------------------------------------
Write-Step 'Version resource'
$RcFile  = Join-Path $ScriptDir 'ORBAGDI.rc'
$VerHdr  = Join-Path $IncDir   'ORBAGDI_Version.h'
$ResFile = Join-Path $ObjDir   'ORBAGDI.res'
$RcExe   = Join-Path $sdkRoot  ("bin\{0}\x86\rc.exe" -f $sdkVer)
if (Test-Path $RcFile) {
    $needRc = $true
    if ((Test-Path $ResFile) -and -not $Force) {
        $resTime = (Get-Item $ResFile).LastWriteTime
        if ((Get-Item $RcFile).LastWriteTime -le $resTime -and
            (-not (Test-Path $VerHdr) -or (Get-Item $VerHdr).LastWriteTime -le $resTime)) {
            $needRc = $false
        }
    }
    if (-not (Test-Path $RcExe)) {
        Write-Diag "*** rc.exe not found at $RcExe - the DLL will carry no version resource"
    } elseif ($needRc) {
        $out = & $RcExe /nologo "/fo$ResFile" "/I$IncDir" $RcFile 2>&1
        if ($LASTEXITCODE -ne 0) {
            $out | ForEach-Object { Write-Diag "    $_" }
            Fail 'resource compile failed (ORBAGDI.rc)'
        }
        Write-Ok "    ORBAGDI.res"
    } else {
        Write-Ok '    ORBAGDI.res  (up to date)'
    }
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
Write-Step 'Linking'
$LinkFlags = @(
    '/nologo', '/DLL', '/INCREMENTAL:NO',
    "/OUT:$OutDll", "/PDB:$OutPdb", "/IMPLIB:$OutLib",
    'kernel32.lib', 'user32.lib', 'advapi32.lib', 'setupapi.lib',
    'comdlg32.lib',  # GetOpenFileNameW, used by the Settings dialog
    'comctl32.lib',  # the tab control on the Settings property sheet
    'gdi32.lib',     # SetBkColor / SetTextColor, for the Info page's text box
    $CapLib          # capstone，静态链（P1-0 的指令解码器）
)
# The .res is an input to the link, not an object: it must not enter $objs
# (that list is what the compile gate walks).
if (Test-Path $ResFile) { $LinkFlags += $ResFile }
# A .def file keeps the AGDI export names undecorated (x86 __stdcall would
# otherwise decorate them). We use /EXPORT per symbol instead, so the exports
# are exactly the AGDI entry points.
$DefFile = Join-Path $ScriptDir 'ORBAGDI.def'
if (Test-Path $DefFile) { $LinkFlags += "/DEF:$DefFile" }

# Link only when something moved: a fresh object, a missing DLL, an object newer
# than the DLL, or a changed .def (it decides the export list).
$needLink = -not (Test-Path $OutDll)
if (-not $needLink) {
    $dllTime = (Get-Item $OutDll).LastWriteTime
    foreach ($o in $objs) {
        if ((Get-Item $o).LastWriteTime -gt $dllTime) { $needLink = $true; break }
    }
    if (-not $needLink -and (Test-Path $DefFile) -and
        (Get-Item $DefFile).LastWriteTime -gt $dllTime) {
        $needLink = $true
    }
    # 换过 capstone.lib（重编了它）也要重链。
    if (-not $needLink -and (Test-Path $CapLib) -and
        (Get-Item $CapLib).LastWriteTime -gt $dllTime) {
        $needLink = $true
    }
    # Bumping the version only touches ORBAGDI.rc / ORBAGDI_Version.h, so the
    # resource is what decides a relink here - otherwise "released 0.2.0" would
    # keep shipping a DLL whose properties still say the old number.
    if (-not $needLink -and (Test-Path $ResFile) -and
        (Get-Item $ResFile).LastWriteTime -gt $dllTime) {
        $needLink = $true
    }
}

if ($needLink) {
    $out = & $link @LinkFlags @objs 2>&1
    if ($LASTEXITCODE -ne 0) {
        $errLog = Join-Path $ObjDir 'link.build.log'
        $out | ForEach-Object { "$_" } | Set-Content -LiteralPath $errLog -Encoding UTF8
        Write-Diag '    --- linker output ---' 'Red'
        $out | ForEach-Object { Write-Diag "    $_" 'Red' }
        Write-Diag "    --- full output: $errLog ---"
        Fail 'link failed'
    }
    # 链接成功时也可能带 LNK4xxx 警告 —— 同样要走 Write-Diag 才抓得到。
    $out | ForEach-Object { Write-Diag "    $_" 'Yellow' }
} else {
    Write-Ok 'nothing changed - link skipped'
}

Write-Step 'Result'
$info = Get-Item $OutDll
Write-Ok ("{0}  ({1:N0} bytes)" -f $info.Name, $info.Length)

# ---------------------------------------------------------------------------
# Deploy (optional)
# ---------------------------------------------------------------------------
if ($Deploy) {
    Write-Step 'Deploy'
    $armBin = Find-KeilArmBin
    if (-not $armBin) {
        Write-Err 'Keil ARM\BIN not found. Set $env:KEIL_ARM_BIN then re-run -DeployOnly.'
        exit 1
    }
    $target = Join-Path $armBin 'ORB_AGDI.dll'
    Copy-Item $OutDll $target -Force
    Write-Ok "deployed -> $target"
}

Write-Host 'Done.' -ForegroundColor Green
