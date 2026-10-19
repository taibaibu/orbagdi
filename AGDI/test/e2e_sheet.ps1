<#
    End-to-end check of the settings sheet, driven entirely from the outside.

    It starts SettingsPreview.exe with a known option string, finds the dialog by
    its window title, presses OK (or Cancel) with PostMessage, and compares what
    the sheet wrote back with what went in.  Nothing in src\ knows this script
    exists - the dialog is driven exactly the way a user drives it.

    Two things this catches that nothing else does:

      * a key that Format() writes but Parse() does not consume, or the other way
        round - the round trip through the real dialog is what proves a setting
        actually arrives;
      * a page that silently normalises a value, e.g. the Trace page dropping
        swo= because the source combo is not "SWO pin".

    Pressing OK too early is a race, not a bug: the dialog is enumerable before
    WM_INITDIALOG has finished filling it.  Wait-SheetReady closes that window,
    so a busy machine cannot produce a failure the sheet did not cause.

    The trace-source cases are the interesting ones: "Trace Enable" (the master
    switch, `tren=`) plus the source combo are authoritative, so each of the
    three sources has to produce its own set of keys and drop the ones that do
    not belong to it - and switching the master switch off has to drop all of
    them while still echoing `tren=0` back.

    Usage:  .\test\e2e_sheet.ps1
            .\test\e2e_sheet.ps1 -Exe <path> -Wait 6

    Exit code: 0 when every case passed.
#>
param(
    [string]$Exe,
    [int]$Wait = 5
)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\SettingsPreview.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first" }

$script:failures = 0
function Write-Pass([string]$m) { Write-Host "    PASS  $m" -ForegroundColor Green }
function Write-Fail([string]$m) { Write-Host "    FAIL  $m" -ForegroundColor Red
                                  $script:failures++ }

# ---------------------------------------------------------------------------
# Just enough Win32 to find the dialog and press a button in it.
# ---------------------------------------------------------------------------
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class W {
  public delegate bool EP(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

$DialogTitle = 'ORBTRACE Debugger Settings'
$WM_COMMAND  = 0x0111
$IDOK        = 1
$IDCANCEL    = 2
$GW_CHILD    = 5
$GW_HWNDNEXT = 2

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][W]::GetClassName($h, $s, 256)
    return $s.ToString()
}

# The dialog window exists from the moment it is created, but WM_INITDIALOG is
# still running: Parse, then the seven pages, then the RDDI probe (which loads a
# DLL and enumerates interfaces, and is the slow part), then the controls on
# every page.  Pressing OK inside that window reads half-filled controls and
# fails on keys that the sheet would have written correctly a moment later - a
# false failure that showed up once on a machine that had just finished a build.
#
# The pages are created without WS_VISIBLE and the first one is only shown at
# the very end of WM_INITDIALOG (ShowPage), so "some page is visible" is the
# signal that the sheet is ready to be driven.  Any page counts, not just Debug,
# because the initial page is not fixed.
$script:pageMarkers = @(1101, 1201, 1301, 1401, 1801, 1500)

function Wait-SheetReady([IntPtr]$dlg, [int]$timeoutMs = 25000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $c = [W]::GetWindow($dlg, $GW_CHILD)
        while ($c -ne [IntPtr]::Zero) {
            if ((Get-ClassName $c) -eq '#32770') {
                foreach ($m in $script:pageMarkers) {
                    if ([W]::GetDlgItem($c, $m) -ne [IntPtr]::Zero) {
                        if ([W]::IsWindowVisible($c)) { return $true }
                        break
                    }
                }
            }
            $c = [W]::GetWindow($c, $GW_HWNDNEXT)
        }
        Start-Sleep -Milliseconds 150
    }
    return $false
}

# A file that exists, so rddi= has something real to keep: the sheet only fills
# the path in when the project leaves it empty, and a path that is not on disk
# would add a warning to the Info page of every case.
$RealFile = $Exe

<#
    Runs one case: open the sheet with $Options, press $Button, and check the
    written string against $Want (a '|' separated list of regexes, ALL of which
    must match) and $Absent (a single regex that must NOT match).
#>
function Invoke-Case {
    param(
        [string]$Name,
        [string]$Options,
        [string]$Want,
        [string]$Absent,
        [int]$Button = $IDOK
    )

    Write-Host "  -- $Name" -ForegroundColor Cyan

    $out = Join-Path $env:TEMP 'e2e_sheet.out'
    $err = Join-Path $env:TEMP 'e2e_sheet.err'
    Remove-Item $out, $err -ErrorAction SilentlyContinue

    $p = Start-Process -FilePath $Exe -ArgumentList ('"' + $Options + '"') `
         -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    Start-Sleep -Seconds $Wait

    if ($p.HasExited) {
        Write-Fail "the process exited before the dialog appeared (0x$('{0:X8}' -f $p.ExitCode))"
        Get-Content $err -ErrorAction SilentlyContinue | ForEach-Object { "      $_" }
        return
    }

    $script:target = [IntPtr]::Zero
    $script:pid2   = [uint32]$p.Id
    $cb = [W+EP]{
        param($h, $l)
        $owner = 0
        [void][W]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -ne $script:pid2) { return $true }
        $t = New-Object System.Text.StringBuilder 256
        [void][W]::GetWindowText($h, $t, 256)
        if ($t.ToString() -eq $DialogTitle) { $script:target = $h; return $false }
        return $true
    }
    [void][W]::EnumWindows($cb, [IntPtr]::Zero)

    if ($script:target -eq [IntPtr]::Zero) {
        Write-Fail 'the settings dialog was not found'
        $p.Kill(); return
    }

    if (-not (Wait-SheetReady $script:target)) {
        Write-Fail 'the sheet never finished coming up'
        $p.Kill(); return
    }

    [void][W]::PostMessage($script:target, $WM_COMMAND, [IntPtr]$Button, [IntPtr]0)
    if (-not $p.WaitForExit(8000)) {
        Write-Fail 'the sheet did not close'
        $p.Kill(); return
    }

    $lines = @(Get-Content $out -ErrorAction SilentlyContinue)
    $result  = (($lines | Where-Object { $_ -like 'result*'  }) -replace '^result\s*:\s*', '')
    $written = (($lines | Where-Object { $_ -like 'options:*' }) -replace '^options:\s*', '')

    if ($Button -eq $IDCANCEL) {
        if ($result -notlike 'Cancel*') { Write-Fail "expected Cancel, got '$result'"; return }
        if ($written -ne $Options) {
            Write-Fail 'Cancel did not restore the incoming option string'
            Write-Host "      in : $Options"
            Write-Host "      out: $written"
            return
        }
        Write-Pass 'Cancel left the option string alone'
        return
    }

    if ($result -ne 'OK (accepted)') { Write-Fail "the sheet did not accept (result='$result')"; return }

    # Every alternative must match, so this is an AND - a single -match on the
    # whole alternation would pass as soon as one key was found.
    $missing = @()
    foreach ($k in ($Want -split '\|')) {
        if ($k -and ($written -notmatch $k)) { $missing += $k }
    }
    if ($missing.Count -gt 0) {
        Write-Fail "keys missing or wrong in the written string: $($missing -join ', ')"
        Write-Host "      out: $written"
        return
    }
    Write-Pass 'every expected key survived the sheet (open -> OK)'

    if ($Absent) {
        if ($written -match $Absent) {
            Write-Fail "keys that should have been dropped are present: $Absent"
            Write-Host "      out: $written"
        } else {
            Write-Pass "dropped as expected: $Absent"
        }
    }
}

# ---------------------------------------------------------------------------
# Cases.  The base string sets a non-default value for every page; each trace
# case then changes only the trace source and its own keys.
# ---------------------------------------------------------------------------
$base = "rddi=$RealFile clock=1000 cpu=48000 port=1 swj=0 ap=2 " +
        "connect=1 reset=2 rstcon=0 stoprst=1 logacc=1 " +
        "cachecode=0 cachemem=0 dlflash=1 " +
        "etmen=1 trunlim=1 tclkc=1 etmalt=0 " +
        "pcpresc=3 ports=FFFFFFF0 priv=0 events=0x30 " +
        "trchunk=1 trmem=2 trkeep=2 trfile=$RealFile trrec=$RealFile " +
        "ram=20000000,2000 " +
        "flm=$RealFile erase=2 fdverify=1 resetrun=1"

# Format() writes a key only when it differs from the default, so this list is
# exactly the keys the base string moves off their defaults.  Two of them need
# to be read together with ORBAGDI_Config.cpp's kExtraKeys table, because the
# default is also the value that is *not* written:
#
#   program=1   absent on purpose: 1 *is* the default, so a correct sheet does
#               not write it
#   rstcon=0    the base string deliberately uses 0 here.  The key's default is
#               1 ("Default ON", COMPAT_ANALYSIS.md §10.12), so feeding
#               rstcon=1 would be testing "a value equal to the default is not
#               echoed back" - true, but not what this case is about.  0 moves
#               it off the default, so the checkbox is actually exercised.
#
# The Uint keys come back as 0x-prefixed hex.
$common = 'clock=1000|cpu=48000|port=1|swj=0|ap=2|' +
          'connect=1|reset=2|rstcon=0|stoprst=1|logacc=1|' +
          'cachecode=0|cachemem=0|dlflash=1|' +
          'etmen=1|trunlim=1|tclkc=1|etmalt=0|' +
          'pcpresc=3|ports=0xFFFFFFF0|priv=0x0\b|events=0x30\b|' +
          'trchunk=1|trmem=2|trkeep=2|trfile=".*exe"|trrec=".*exe"|' +
          'ram=20000000,2000\b|erase=2|fdverify=1|resetrun=1'

Invoke-Case -Name 'orbtrace probe is the trace source' `
    -Options "$base orbtrace=1 trwidth=2 trswo=2 trbaud=2000000 troflow=0" `
    -Want    "$common|orbtrace=1|trwidth=2|trswo=2|trbaud=2000000|troflow=0" `
    -Absent  '\bswo=[1-9]'

Invoke-Case -Name 'SWO pin is the trace source' `
    -Options "$base swo=115200" `
    -Want    "$common|swo=115200" `
    -Absent  '\borbtrace=1|\betb=1'

Invoke-Case -Name 'on-chip ETB is the trace source' `
    -Options "$base etb=1" `
    -Want    "$common|etb=1" `
    -Absent  '\borbtrace=1|\bswo=[1-9]'

Invoke-Case -Name 'trace off' `
    -Options "$base" `
    -Want    $common `
    -Absent  '\borbtrace=1|\betb=1|\bswo=[1-9]'

Invoke-Case -Name 'Trace Enable off (master switch)' `
    -Options "$base orbtrace=1 tren=0" `
    -Want    "$common|tren=0" `
    -Absent  '\borbtrace=1|\betb=1|\bswo=[1-9]'

Invoke-Case -Name 'Cancel restores the incoming string' `
    -Options "$base orbtrace=1" `
    -Want    '' `
    -Absent  '' `
    -Button  $IDCANCEL

# ---------------------------------------------------------------------------
Write-Host ''
if ($script:failures -gt 0) {
    Write-Host "FAILED  ($script:failures problem(s))" -ForegroundColor Red
    exit 1
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
