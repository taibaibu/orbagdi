<#
    Builds test\bin\SettingsPreview.exe - a standalone viewer for the ORBAGDI
    settings sheet.

    It compiles the REAL dialog sources (ORBAGDI_Settings.cpp plus the three
    files it needs) together with test\ORBAGDI_SettingsPreview.cpp, so what it
    shows is what uVision shows.  Nothing here touches the driver build; the
    build.ps1 one folder up is unaffected.

    Usage:
        .\test\build.ps1            # build
        .\test\build.ps1 -Clean     # wipe obj\ and bin\ first
        .\test\build.ps1 -Run       # build, then start the preview
#>
[CmdletBinding()]
param([switch]$Clean, [switch]$Run)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Root    = Split-Path -Parent $TestDir
$ObjDir  = Join-Path $TestDir 'obj'
$BinDir  = Join-Path $TestDir 'bin'
$OutExe  = Join-Path $BinDir  'SettingsPreview.exe'

function Write-Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok  ([string]$m) { Write-Host "    $m" -ForegroundColor Green }

if ($Clean) {
    foreach ($d in @($ObjDir, $BinDir)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d; Write-Ok "removed $d" }
    }
}

# ---------------------------------------------------------------------------
# Toolchain - same discovery the parent build.ps1 uses.
# ---------------------------------------------------------------------------
function Find-VSDir {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath 2>$null
        if ($p) { return $p.Trim() }
    }
    foreach ($base in @(${env:ProgramFiles}, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        $r = Join-Path $base 'Microsoft Visual Studio'
        if (-not (Test-Path $r)) { continue }
        foreach ($y in Get-ChildItem $r -Directory -ErrorAction SilentlyContinue) {
            foreach ($e in Get-ChildItem $y.FullName -Directory -ErrorAction SilentlyContinue) {
                if (Test-Path (Join-Path $e.FullName 'VC\Tools\MSVC')) { return $e.FullName }
            }
        }
    }
    throw 'Visual Studio with the C++ toolset was not found.'
}

function Newest-Dir([string]$root) {
    if (-not (Test-Path $root)) { return $null }
    $d = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue
    if (-not $d) { return $null }
    return ($d | Sort-Object Name -Descending)[0].FullName
}

Write-Step 'Locating toolchain'
$vsDir = Find-VSDir
$msvc  = Newest-Dir (Join-Path $vsDir 'VC\Tools\MSVC')
if (-not $msvc) { throw "MSVC toolset not found under $vsDir" }

$sdkRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' `
            -ErrorAction SilentlyContinue).KitsRoot10
if (-not $sdkRoot -or -not (Test-Path $sdkRoot)) {
    foreach ($c in @('C:\Program Files (x86)\Windows Kits\10', 'D:\Windows Kits\10')) {
        if (Test-Path $c) { $sdkRoot = $c; break }
    }
}
if (-not $sdkRoot) { throw 'Windows SDK not found.' }
$sdkInc = Newest-Dir (Join-Path $sdkRoot 'Include')
$sdkLib = Newest-Dir (Join-Path $sdkRoot 'Lib')

$cl   = Join-Path $msvc 'bin\Hostx86\x86\cl.exe'
$link = Join-Path $msvc 'bin\Hostx86\x86\link.exe'
if (-not (Test-Path $cl))   { $cl   = Join-Path $msvc 'bin\Hostx64\x86\cl.exe' }
if (-not (Test-Path $link)) { $link = Join-Path $msvc 'bin\Hostx64\x86\link.exe' }
if (-not (Test-Path $cl))   { throw "cl.exe not found under $msvc" }
Write-Ok "MSVC : $(Split-Path -Leaf $msvc)"

$env:INCLUDE = @(
    (Join-Path $msvc   'include'),
    (Join-Path $sdkInc 'ucrt'),
    (Join-Path $sdkInc 'shared'),
    (Join-Path $sdkInc 'um')
) -join ';'
$env:LIB = @(
    (Join-Path $msvc   'lib\x86'),
    (Join-Path $sdkLib 'ucrt\x86'),
    (Join-Path $sdkLib 'um\x86')
) -join ';'
$vsCommon = Join-Path $vsDir 'Common7\IDE'
if (Test-Path $vsCommon) { $env:PATH = "$vsCommon;$env:PATH" }

New-Item -ItemType Directory -Force -Path $ObjDir, $BinDir | Out-Null

# ---------------------------------------------------------------------------
# Sources - the real dialog plus its dependencies, plus the preview main.
# ---------------------------------------------------------------------------
$Sources = @(
    (Join-Path $Root    'src\app\ORBAGDI_Settings.cpp'),
    (Join-Path $Root    'src\app\ORBAGDI_Config.cpp'),
    (Join-Path $Root    'src\app\ORBAGDI_Log.cpp'),
    (Join-Path $Root    'src\coresight\ORBAGDI_Rddi.cpp'),
    # The Power page has to be able to actually send (that is its whole point), so
    # this one comes along.  It only uses the two OrbTrace_SetVoltage* exports and
    # depends on nothing else - see include\ORBAGDI_Power.h.
    (Join-Path $Root    'src\target\ORBAGDI_Power.cpp'),
    # Settings.cpp 里"取消 Trace Enable 当场收源"要调 orb::agdi::StopTraceSources()，
    # 实现在 src\app\ORBAGDI_Agdi.cpp（会话层：DAP / USB / 状态机）。预览没有会话，
    # 所以用 stub\AGDIStub.cpp 里同签名的替身，src 再长也往那个文件里加。
    (Join-Path $TestDir 'stub\AGDIStub.cpp'),
    (Join-Path $TestDir 'ORBAGDI_SettingsPreview.cpp')
)
foreach ($s in $Sources) {
    if (-not (Test-Path $s)) { throw "missing source: $s" }
}

$Flags = @(
    '/c', '/nologo', '/O2', '/MT', '/W3', '/EHsc', '/std:c++17', '/utf-8',
    '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32', '/D_WINDOWS',
    "/I$Root\src", "/I$Root\include", "/I$Root",
    # Keil 官方 AGDI SDK 头（include\sdk）：这些源里写的是裸名字 "BOM.H" / "AGDI.H"。
    "/I$Root\include\sdk"
)

Write-Step 'Compiling'
$objs = @()
foreach ($s in $Sources) {
    $obj = Join-Path $ObjDir ((Split-Path -Leaf $s) -replace '\.cpp$', '.obj')
    $objs += $obj
    Write-Host "    cl $(Split-Path -Leaf $s)"
    $out = & $cl @Flags "/Fo$obj" $s 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host $_ }
        throw "compile failed: $s"
    }
}

Write-Step 'Linking'
$LinkFlags = @(
    '/nologo', '/INCREMENTAL:NO', "/OUT:$OutExe", '/SUBSYSTEM:CONSOLE',
    'kernel32.lib', 'user32.lib', 'advapi32.lib', 'comdlg32.lib', 'comctl32.lib',
    'gdi32.lib'      # SetBkColor / SetTextColor, for the Info page's text box
)
$out = & $link @LinkFlags @objs 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    throw 'link failed'
}

Write-Step 'Result'
$info = Get-Item $OutExe
Write-Ok ("{0}  ({1:N0} bytes)" -f $info.Name, $info.Length)

# ---------------------------------------------------------------------------
# The AGDI probe - a console host that loads bin\ORB_AGDI.dll and drives the AGDI
# entry points the way uVision does.  It deliberately links no driver source:
# the point is to exercise the built DLL, not a copy of it.
# ---------------------------------------------------------------------------
Write-Step 'Compiling the AGDI probe'
$ProbeOut = Join-Path $BinDir 'ORBAGDI_AgdiProbe.exe'
$ProbeObj = Join-Path $ObjDir 'ORBAGDI_AgdiProbe.obj'
$ProbeSrc = Join-Path $TestDir 'ORBAGDI_AgdiProbe.cpp'

$out = & $cl @Flags "/Fo$ProbeObj" $ProbeSrc 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    throw "compile failed: ORBAGDI_AgdiProbe.cpp"
}

$out = & $link '/nologo' '/INCREMENTAL:NO' "/OUT:$ProbeOut" '/SUBSYSTEM:CONSOLE' `
       $ProbeObj 'kernel32.lib' 'user32.lib' 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    throw 'link failed: ORBAGDI_AgdiProbe.exe'
}
Write-Ok ("{0}  ({1:N0} bytes)" -f 'ORBAGDI_AgdiProbe.exe', (Get-Item $ProbeOut).Length)

# ---------------------------------------------------------------------------
# The orbtrace scan probe - mirrors the RDDI host's ScanForOrbTrace step by step.
# Links nothing from the driver: it is a plain WinUSB enumeration test.
# ---------------------------------------------------------------------------
Write-Step 'Compiling the orbtrace scan probe'
$OrbOut = Join-Path $BinDir 'ORBAGDI_OrbTraceProbe.exe'
$OrbObj = Join-Path $ObjDir 'ORBAGDI_OrbTraceProbe.obj'

$out = & $cl @Flags "/Fo$OrbObj" (Join-Path $TestDir 'ORBAGDI_OrbTraceProbe.cpp') 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    throw 'compile failed: ORBAGDI_OrbTraceProbe.cpp'
}

$out = & $link '/nologo' '/INCREMENTAL:NO' "/OUT:$OrbOut" '/SUBSYSTEM:CONSOLE' `
       $OrbObj 'setupapi.lib' 'winusb.lib' 'kernel32.lib' 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    throw 'link failed: ORBAGDI_OrbTraceProbe.exe'
}
Write-Ok ("{0}  ({1:N0} bytes)" -f 'ORBAGDI_OrbTraceProbe.exe', (Get-Item $OrbOut).Length)

# ---------------------------------------------------------------------------
# The OFLOW file reader - built from orbuculum's OWN cobs.c (not a line changed),
# so it is an independent check of the files the driver dumps (P2-1).  The
# sibling checkout is ../../orbuculum; when it is absent this step is skipped.
# ---------------------------------------------------------------------------
Write-Step 'Compiling the OFLOW file reader'
# 旁边那份 orbuculum checkout：先找 AGDI 的上一级，再找上两级（两份布局都见过）。
# 目录名不写死——从发行包解出来的那几份带版本号（orbuculum-2.2.0），
# 所以要按 orbuculum* 匹配，再用 Src\cobs.c 确认是不是源码树。
$Orbuculum = $null
foreach ($base in @((Split-Path -Parent $Root), (Split-Path -Parent (Split-Path -Parent $Root)))) {
    if (-not (Test-Path $base)) { continue }
    foreach ($c in (Get-ChildItem $base -Directory -Filter 'orbuculum*' -ErrorAction SilentlyContinue)) {
        if (Test-Path (Join-Path $c.FullName 'Src\cobs.c')) { $Orbuculum = $c.FullName; break }
    }
    if ($Orbuculum) { break }
}
$CobsSrc = if ($Orbuculum) { Join-Path $Orbuculum 'Src\cobs.c' } else { '' }
if (-not $CobsSrc -or -not (Test-Path $CobsSrc)) {
    Write-Host '    skipped - no orbuculum checkout found next to this tree' -ForegroundColor DarkGray
} else {
    # /I<stub> must come first: cobs.h includes <sys/time.h>, which MSVC has no
    # header for (the stub is empty; cobs.c uses nothing from it).
    $FlagsC = @(
        '/c', '/nologo', '/O2', '/MT', '/W3', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS',
        "/I$(Join-Path $TestDir 'stub')", "/I$(Join-Path $Orbuculum 'Inc')", "/I$TestDir"
    )
    $CobsObj = Join-Path $ObjDir 'orbuculum_cobs.obj'
    $ReadObj = Join-Path $ObjDir 'ofl_read.obj'

    $out = & $cl @FlagsC "/Fo$CobsObj" $CobsSrc 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; throw 'compile failed: orbuculum cobs.c' }
    $out = & $cl @FlagsC "/Fo$ReadObj" (Join-Path $TestDir 'ofl_read.c') 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; throw 'compile failed: ofl_read.c' }

    $ReadOut = Join-Path $BinDir 'ofl_read.exe'
    $out = & $link '/nologo' '/INCREMENTAL:NO' "/OUT:$ReadOut" '/SUBSYSTEM:CONSOLE' `
           $ReadObj $CobsObj 'kernel32.lib' 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; throw 'link failed: ofl_read.exe' }
    Write-Ok ("{0}  ({1:N0} bytes)" -f 'ofl_read.exe', (Get-Item $ReadOut).Length)
}

# ---------------------------------------------------------------------------
# The ETM replay tool - decodes an OFLOW capture OFFLINE, with the SAME decoder
# the driver uses (traceDec/, orbuculum's ETMv3.5 engine).  No Keil, no probe,
# no target: "run it in uVision" is far too slow to iterate on (the user's
# words), and a decode bug only needs a byte stream - which we already dump.
#
# It can also A/B the branch-address encoding (--alt 0|1) on one file, which is
# the fastest way to tell "wrong encoding" from "the stream itself is broken".
#
# Needs the orbuculum checkout (for cobs.c) and traceDec\*.cpp - both optional,
# so this step is skipped when either is missing.
# ---------------------------------------------------------------------------
Write-Step 'Compiling the ETM replay tool'
$DecDir = Join-Path $Root 'src\coresight\traceDec'
if (-not $CobsSrc -or -not (Test-Path $CobsSrc)) {
    Write-Host '    skipped - needs the orbuculum checkout (for cobs.c)' -ForegroundColor DarkGray
} elseif (-not (Test-Path (Join-Path $DecDir 'traceDecoder.cpp'))) {
    Write-Host "    skipped - no decoder sources in $DecDir" -ForegroundColor DarkGray
} else {
    # Order matters: traceDec MUST come before orbuculum\Inc.  Both carry a
    # traceDecoder.h - the driver uses the one in traceDec\ (patched so it is
    # valid C++), while the upstream copy in Inc\ is C-flavoured and fails to
    # compile as C++ (traceDecoder.h(169): missing ')' before 'using').
    #
    # Do NOT put comments inside the @( ... ) literal below.  A quote inside such
    # a comment swallowed the following /I... arguments, and cl then failed with
    #   cobs.h(14): fatal error C1083: cannot open "sys/time.h"
    # - the stub was right there, the /I<stub> just never reached the command line.
    $FlagsD = @(
        '/c', '/nologo', '/O2', '/MT', '/W3', '/EHsc', '/std:c++17', '/utf-8',
        '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32', '/D_WINDOWS',
        "/I$(Join-Path $TestDir 'stub')", "/I$DecDir",
        "/I$(Join-Path $Orbuculum 'Inc')", "/I$TestDir"
    )

    $DecObjs = @()
    foreach ($s in (Get-ChildItem -Path $DecDir -Filter *.cpp -File)) {
        $o = Join-Path $ObjDir ('dec_' + ($s.BaseName + '.obj'))
        $DecObjs += $o
        Write-Host "    cl $($s.Name)"
        $out = & $cl @FlagsD "/Fo$o" $s.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            $out | ForEach-Object { Write-Host $_ }
            throw "compile failed: $($s.Name)"
        }
    }

    $ReplayObj = Join-Path $ObjDir 'etm_replay.obj'
    Write-Host '    cl etm_replay.cpp'
    $out = & $cl @FlagsD "/Fo$ReplayObj" (Join-Path $TestDir 'etm_replay.cpp') 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host $_ }
        throw 'compile failed: etm_replay.cpp'
    }

    $ReplayOut = Join-Path $BinDir 'etm_replay.exe'
    $out = & $link '/nologo' '/INCREMENTAL:NO' "/OUT:$ReplayOut" '/SUBSYSTEM:CONSOLE' `
           $ReplayObj $CobsObj @DecObjs 'kernel32.lib' 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host $_ }
        throw 'link failed: etm_replay.exe'
    }
    Write-Ok ("{0}  ({1:N0} bytes)" -f 'etm_replay.exe', (Get-Item $ReplayOut).Length)
}

if ($Run) {
    Write-Step 'Running'
    & $OutExe --ui
}

Write-Host 'Done.' -ForegroundColor Green
