<#
    Link test for the static capstone.lib.

    Builds test_link.c against lib\capstone.lib into an x86 /MT executable and
    runs it. It must NOT need capstone.dll - that is the whole point of the
    static build.

    Exit code 0 = passed.

    NOTE: English comments on purpose - PowerShell 5.1 reads a BOM-less .ps1 as
    ANSI, so UTF-8 Chinese comments turn into mojibake that can break the parser.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$IncDir    = Join-Path $ScriptDir 'include'
$Lib       = Join-Path $ScriptDir 'lib\capstone.lib'
$Src       = Join-Path $ScriptDir 'test_link.c'
$OutDir    = Join-Path $ScriptDir 'obj'
$OutExe    = Join-Path $OutDir 'test_link.exe'

function Write-Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok  ([string]$m) { Write-Host "    $m" -ForegroundColor Green }

if (-not (Test-Path $Lib)) { throw "missing $Lib - run build.ps1 first" }

# ---------------------------------------------------------------------------
# Locate MSVC
# ---------------------------------------------------------------------------
function Get-NewestVersionDir([string]$root) {
    if (-not (Test-Path $root)) { return $null }
    $dirs = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue
    if (-not $dirs) { return $null }
    return ($dirs | Sort-Object {
        try { [version]($_.Name -replace '^.*?(\d+\.\d+\.\d+(\.\d+)?).*$', '$1') }
        catch { [version]'0.0.0' }
    } -Descending)[0].FullName
}

Write-Step 'Locating toolchain'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsDir = $null
if (Test-Path $vswhere) {
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($p) { $vsDir = $p.Trim() }
}
if (-not $vsDir) { throw 'Visual Studio not found' }

$msvcDir = Get-NewestVersionDir (Join-Path $vsDir 'VC\Tools\MSVC')
if (-not $msvcDir) { throw 'MSVC toolset not found' }
$sdkRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (-not $sdkRoot -or -not (Test-Path $sdkRoot)) {
    foreach ($c in @('C:\Program Files (x86)\Windows Kits\10', 'D:\Windows Kits\10')) {
        if (Test-Path $c) { $sdkRoot = $c; break }
    }
}
$sdkIncDir = Get-NewestVersionDir (Join-Path $sdkRoot 'Include')
$sdkLibDir = Get-NewestVersionDir (Join-Path $sdkRoot 'Lib')

$cl = Join-Path $msvcDir 'bin\Hostx86\x86\cl.exe'
if (-not (Test-Path $cl)) { $cl = Join-Path $msvcDir 'bin\Hostx64\x86\cl.exe' }
if (-not (Test-Path $cl)) { throw 'cl.exe (x86) not found' }
Write-Ok "MSVC : $msvcDir"

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

# ---------------------------------------------------------------------------
# Build + run
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Step 'Building test_link.exe'
# The library goes after /link: with /TC in effect, anything on the compile
# side is treated as a C source file (cl tried to compile capstone.lib).
$flags = @('/nologo', '/MT', '/O2', '/W3', '/TC',
           '/D_CRT_SECURE_NO_WARNINGS', "/I$IncDir")
$out = & $cl $flags "/Fe$OutExe" "/Fo$OutDir\test_link.obj" $Src '/link' $Lib 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    throw 'compile/link failed'
}
Write-Ok "$OutExe  ($((Get-Item $OutExe).Length) bytes)"

# A capstone.dll sitting next to the exe would mask a bad static link.
$dllBeside = Join-Path $OutDir 'capstone.dll'
if (Test-Path $dllBeside) {
    Write-Host "    WARNING: $dllBeside exists - the test would not prove static linkage" -ForegroundColor Yellow
}

Write-Step 'Running'
& $OutExe
$rc = $LASTEXITCODE
Write-Host ''
if ($rc -ne 0) { Write-Host "FAILED (rc=$rc)" -ForegroundColor Red; exit 1 }
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
