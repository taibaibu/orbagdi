<#
    capstone -> static library (x86 / MSVC / static CRT)

    Why build it ourselves
    ----------------------
    This driver is 32-bit (uVision is a 32-bit process). The `capstone.dll`
    inside PyPI's `capstone-*-win32.whl` is **actually x64** (measured:
    machine=0x8664), so "just pip install and use it" is a dead end.
    Upstream's own build wants cmake/meson, which this machine does not have.
    => Compile the C sources shipped in the sdist with cl.exe, archive with lib.exe.

    Where the inputs come from (PyPI is reachable)
    ---------------------------------------------
        pip download capstone --no-binary :all: --no-deps -d <dir>
          -> capstone-<ver>.tar.gz, which carries src/ (79 .c files plus every
             generated .inc table).
        Headers are NOT in the sdist; take them from any wheel:
          capstone/include/capstone/*.h

    NOTE ON COMMENTS: this file is deliberately in English. PowerShell 5.1 reads
    a BOM-less .ps1 as ANSI, so UTF-8 Chinese comments decode to mojibake that can
    contain stray quote characters and break the parser. The project's own .ps1
    files are fine because they carry a BOM; write_to_file does not add one.

    NOTE ON THE CRT: the driver links /MT, so capstone must too. Mixing two CRTs
    in one process is not survivable.

    Usage:
        .\build.ps1
        .\build.ps1 -Src <extracted sdist root> -Inc <wheel include dir>
#>
[CmdletBinding()]
param(
    [string]$Src = "$env:TEMP\capstone_sdist\x\capstone-5.0.9",
    # Headers must come from the SAME version as the sources. The sdist carries
    # them at src\include\capstone\*.h - do NOT take them from a wheel, whose
    # version can differ (that mismatch silently breaks the ARM_INS_* enums).
    [string]$Inc = ''
)

if (-not $Inc) { $Inc = Join-Path $Src 'src\include' }

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$LibDir    = Join-Path $ScriptDir 'lib'
$IncOut    = Join-Path $ScriptDir 'include'
$ObjDir    = Join-Path $ScriptDir 'obj'
$OutLib    = Join-Path $LibDir 'capstone.lib'

function Write-Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok  ([string]$m) { Write-Host "    $m" -ForegroundColor Green }

# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------
Write-Step 'Checking inputs'
if (-not (Test-Path (Join-Path $Src 'src\cs.c'))) {
    throw "capstone source not found at $Src (expected src\cs.c). Use -Src."
}
if (-not (Test-Path (Join-Path $Inc 'capstone\capstone.h'))) {
    throw "capstone headers not found at $Inc (expected capstone\capstone.h). Use -Inc."
}
Write-Ok "src = $Src"
Write-Ok "inc = $Inc"

# ---------------------------------------------------------------------------
# Locate MSVC (same approach as test\run_probe_bom.ps1)
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

$cl  = Join-Path $msvcDir 'bin\Hostx86\x86\cl.exe'
if (-not (Test-Path $cl)) { $cl = Join-Path $msvcDir 'bin\Hostx64\x86\cl.exe' }
$lib = Join-Path $msvcDir 'bin\Hostx86\x86\lib.exe'
if (-not (Test-Path $lib)) { $lib = Join-Path $msvcDir 'bin\Hostx64\x86\lib.exe' }
if (-not (Test-Path $cl))  { throw 'cl.exe (x86) not found' }
if (-not (Test-Path $lib)) { throw 'lib.exe (x86) not found' }
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
# Sources. cs.c includes all 18 architecture modules under #ifndef CAPSTONE_DIET
# and all_arch lists them all, so this is NOT an ARM-only build - compile them all.
# ---------------------------------------------------------------------------
$SrcRoot = Join-Path $Src 'src'
$files = @(Get-ChildItem -Path $SrcRoot -Filter *.c -Recurse -File |
           Sort-Object FullName | Select-Object -ExpandProperty FullName)
Write-Step "Compiling $($files.Count) C files (x86 / /MT)"
if ($files.Count -lt 50) { throw "expected about 79 sources, got $($files.Count)" }

# One switch per architecture, matching the all_arch table in cs.c. 全部编进去：
# 驱动现在只用 ARM/Thumb，但多出来的架构是"以后接别的核只改 cs_open()"的余地，
# 体积换余地，划算（实测全 18 个架构的 DLL 是 6.7 MB）。
$archNames = @('ARM', 'ARM64', 'MIPS', 'X86', 'PPC', 'SPARC', 'SYSZ', 'XCORE',
               'M68K', 'TMS320C64X', 'M680X', 'EVM', 'MOS65XX', 'WASM', 'BPF',
               'RISCV', 'SH', 'TRICORE')
$defines = @()
foreach ($a in $archNames) { $defines += "/DCAPSTONE_HAS_$a" }

# /w silences third-party warnings; /MT matches the driver's static CRT;
# /TC forces C compilation.
$baseFlags = @('/nologo', '/c', '/O2', '/MT', '/w', '/TC')
$baseFlags += '/D_CRT_SECURE_NO_WARNINGS'
$baseFlags += '/D_CRT_NONSTDC_NO_WARNINGS'
$baseFlags += '/DWIN32'
$baseFlags += '/D_WINDOWS'
# Without this, cs.c leaves cs_mem_malloc/calloc/realloc/free NULL and every
# cs_open() fails with CS_ERR_MEMSETUP. It is upstream's default (config.mk).
$baseFlags += '/DCAPSTONE_USE_SYS_DYN_MEM'

if (Test-Path $ObjDir) { Remove-Item $ObjDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $ObjDir, $LibDir | Out-Null

$failed = @()
$n = 0
foreach ($f in $files) {
    $n++
    # Source file names collide across architectures (XxxMapping.c etc.), so the
    # object name is the flattened relative path.
    $rel = $f.Substring($SrcRoot.Length + 1) -replace '[\\/]', '_'
    $obj = Join-Path $ObjDir ([IO.Path]::ChangeExtension($rel, '.obj'))
    # Two include roots are needed: files that say "capstone/platform.h" want
    # $Inc, while a few (TriCoreInstPrinter.c) say "platform.h" and want the
    # capstone subdirectory itself.
    $out = & $cl $baseFlags $defines "/I$Inc" "/I$Inc\capstone" "/I$SrcRoot" "/Fo$obj" $f 2>&1
    if ($LASTEXITCODE -ne 0) {
        $failed += $rel
        Write-Host "    FAIL  $rel" -ForegroundColor Red
        $out | Select-Object -First 8 | ForEach-Object { Write-Host "          $_" }
    }
    if ($n % 20 -eq 0) { Write-Host "    ... $n / $($files.Count)" -ForegroundColor DarkGray }
}
if ($failed.Count) { throw "$($failed.Count) source(s) failed to compile" }
Write-Ok "$($files.Count) objects"

# ---------------------------------------------------------------------------
# Archive
# ---------------------------------------------------------------------------
Write-Step 'Archiving capstone.lib'
$objs = @(Get-ChildItem -Path $ObjDir -Filter *.obj -File | Select-Object -ExpandProperty FullName)
$out = & $lib '/nologo' "/out:$OutLib" $objs 2>&1
if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; throw 'lib.exe failed' }
Write-Ok "$OutLib  ($((Get-Item $OutLib).Length) bytes)"

# ---------------------------------------------------------------------------
# Vendor the headers next to it, so the driver build only needs one /I
# ---------------------------------------------------------------------------
Write-Step 'Vendoring headers'
$IncDst = Join-Path $IncOut 'capstone'
New-Item -ItemType Directory -Force -Path $IncDst | Out-Null
Copy-Item -Path (Join-Path $Inc 'capstone\*.h') -Destination $IncDst -Force
$nh = (Get-ChildItem $IncDst -Filter *.h | Measure-Object).Count
Write-Ok "$nh headers -> $IncDst"

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
