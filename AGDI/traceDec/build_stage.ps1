<#
    traceDec - orbuculum trace 解码子系统移植的**独立编译验证**脚本。

    它不属于正式构建：`build.ps1` 只 glob `src\*.cpp`（非递归），所以这个目录
    里的文件不会进 ORB_AGDI.dll。本脚本存在的唯一目的，是在把文件搬进 `src\`
    之前，用与正式构建**完全相同的编译选项**先编一遍，让编译器把移植问题
    （C-only 写法、POSIX 依赖、变参宏…）一次性全逼出来。

    编译选项刻意与 `..\build.ps1` 保持一致：MSVC x86、/MT 静态 CRT、/O2、
    /std:c++17、/utf-8。C++ 而非 C 是必须的 —— 这些源码里到处都是 `0b...`
    二进制字面量，MSVC 的 C 模式不支持。

    用法：
        .\build_stage.ps1
        .\build_stage.ps1 -Clean
#>
[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Dir    = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ObjDir = Join-Path $Dir 'obj'

if ($Clean -and (Test-Path $ObjDir)) {
    Remove-Item -Recurse -Force $ObjDir
    Write-Host "removed $ObjDir"
}

# ---------------------------------------------------------------------------
# 工具链（与 ..\build.ps1 同一套发现逻辑）
# ---------------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsDir = $null
if (Test-Path $vswhere) {
    $p = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
    if ($p) { $vsDir = $p.Trim() }
}
if (-not $vsDir) { throw 'Visual Studio with C++ toolset not found.' }

$msvcRoot = Join-Path $vsDir 'VC\Tools\MSVC'
$msvcDir  = (Get-ChildItem $msvcRoot -Directory |
             Sort-Object { try { [version]($_.Name -replace '^.*?(\d+\.\d+\.\d+(\.\d+)?).*$','$1') } catch { [version]'0.0.0' } } -Descending |
             Select-Object -First 1).FullName

$sdkRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' `
            -ErrorAction SilentlyContinue).KitsRoot10
if (-not $sdkRoot -or -not (Test-Path $sdkRoot)) { throw 'Windows SDK not found.' }
$sdkIncDir = (Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
              Sort-Object { try { [version]$_.Name } catch { [version]'0.0.0' } } -Descending |
              Select-Object -First 1).FullName
$sdkLibDir = (Get-ChildItem (Join-Path $sdkRoot 'Lib') -Directory |
              Sort-Object { try { [version]$_.Name } catch { [version]'0.0.0' } } -Descending |
              Select-Object -First 1).FullName

$cl = Join-Path $msvcDir 'bin\Hostx86\x86\cl.exe'
if (-not (Test-Path $cl)) { $cl = Join-Path $msvcDir 'bin\Hostx64\x86\cl.exe' }
if (-not (Test-Path $cl)) { throw "cl.exe not found under $msvcDir" }

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

$vsCommon = Join-Path $vsDir 'Common7\IDE'
if (Test-Path $vsCommon) { $env:PATH = "$vsCommon;$env:PATH" }

New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# ---------------------------------------------------------------------------
# 编译
# ---------------------------------------------------------------------------
$CompilerFlags = @(
    '/c', '/nologo',
    '/O2', '/MT', '/W3', '/EHsc',
    '/std:c++17', '/utf-8',
    '/D_CRT_SECURE_NO_WARNINGS',
    '/DWIN32', '/D_WINDOWS',
    "/I$Dir"
)

$sources = Get-ChildItem -Path $Dir -Filter *.cpp -File | Sort-Object Name
if (-not $sources) { throw "no sources found in $Dir" }

$bad = 0
foreach ($s in $sources) {
    $obj = Join-Path $ObjDir ($s.BaseName + '.obj')
    Write-Host "    cl $($s.Name)"
    $out = & $cl @CompilerFlags "/Fo$obj" $s.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
        ++$bad
    } elseif ($out) {
        $out | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkYellow }
    }
}

Write-Host ''
if ($bad) {
    Write-Host "$bad file(s) failed to compile" -ForegroundColor Red
    exit 1
}
Write-Host "all $($sources.Count) file(s) compiled" -ForegroundColor Green
