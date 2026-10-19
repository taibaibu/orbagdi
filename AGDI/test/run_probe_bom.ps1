<#
    编译并运行 probe_bom_layout.cpp，打印 struct bom 的关键字段偏移。
    用与 build.ps1 相同的工具链与包含路径。
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$AgdiDir   = Split-Path -Parent $ScriptDir
$IncDir    = Join-Path $AgdiDir 'include'
$OutExe    = Join-Path $ScriptDir 'probe_bom_layout.exe'

function Get-NewestVersionDir([string]$root) {
    if (-not (Test-Path $root)) { return $null }
    $dirs = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue
    if (-not $dirs) { return $null }
    return ($dirs | Sort-Object { try { [version]($_.Name -replace '^.*?(\d+\.\d+\.\d+(\.\d+)?).*$','$1') } catch { [version]'0.0.0' } } -Descending)[0].FullName
}

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

$flags = @('/nologo','/O2','/MT','/W3','/EHsc','/std:c++17','/utf-8',
           '/D_CRT_SECURE_NO_WARNINGS','/DWIN32','/D_WINDOWS',
           "/I$IncDir","/I$AgdiDir")

$out = & $cl @flags "/Fe$OutExe" (Join-Path $ScriptDir 'probe_bom_layout.cpp') 2>&1
if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; throw 'compile failed' }

& $OutExe
