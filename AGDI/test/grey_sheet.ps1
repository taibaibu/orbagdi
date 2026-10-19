<#
    Grey-state matrix of the Trace page.

    The sheet's contract is "a greyed control can never change anything", and that
    only holds if the greying matches what the driver actually reads.  It did not:
    the whole target-side half (ITM / DWT, Trace Events, Timestamps) was greyed for
    the **orbtrace** source, while that path calls ItmProgramTarget() and reads
    every one of those fields.  Nothing caught it - no check looked at enablement,
    and a grey label next to a grey edit box is not something a screenshot review
    reliably catches either.

    So this walks the matrix.  For each source (and with the master switch off) it
    asserts, control by control, whether the user may operate it:

      control                        orbtrace  ETB   SWO pin  Trace Enable off
      -----------------------------  --------  ----  -------  ----------------
      Trace Port (+ its group)         live    grey   grey        grey
      SWO Encoding                     grey    grey   live        grey
      ITM / DWT, Trace Events,
        Timestamps (+ their groups)    live    live   live        grey
      ETM Trace Enable, Unlimited
        Trace, Use Core Clock,
        ETM Alt (combo)                live    live   live        grey
      Trace Enable (the master)        live    live   live        live

    The "all three sources" row is the one that was wrong.  All three program the
    target's ITM/DWT, just from different places:

      0 SWO pin   -> ItmStart() -> ItmProgramTarget() -> ProgramItmDwt()
      1 ETB       -> ItmArmForEtb() (once per run)    -> ProgramItmDwt()
      2 orbtrace  -> ItmProgramTarget()               -> ProgramItmDwt()

    and ProgramItmDwt() reads itmPorts / itmPriv / pcPrescaler / pcPeriodic /
    pcDataRw / tsEnable / tsPrescaler / traceEvents.  Every control in that row
    feeds one of those fields, so greying any of them for a live source is a lie.

    Enablement is read with IsWindowEnabled, never from a picture.

    Usage:  .\test\grey_sheet.ps1
            .\test\grey_sheet.ps1 -Dump     # print the whole matrix, pass or fail
    Exit code: 0 when the matrix holds.
#>
param(
    [string]$Exe,
    [int]$Wait = 5,
    [switch]$Dump
)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\SettingsPreview.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first" }

$script:failures = 0
function Write-Pass([string]$m) { Write-Host "    PASS  $m" -ForegroundColor Green }
function Write-Fail([string]$m) { Write-Host "    FAIL  $m" -ForegroundColor Red
                                  $script:failures++ }

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class G {
  public delegate bool EP(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
}
'@

$DialogTitle = 'ORBTRACE Debugger Settings'
$WM_COMMAND  = 0x0111
$IDCANCEL    = 2
$GW_CHILD    = 5
$GW_HWNDNEXT = 2

# Every control on the Trace page, by id.  The ids are positional in
# src\ORBAGDI_Settings.cpp's enum, so this table has to be kept in step with it -
# a control that moves gets caught as MISSING rather than silently skipped.
# A plain hashtable, **not** `[ordered]`: an OrderedDictionary's `this[int]` indexer
# is by *position*, so `$Ids[1202]` would fetch entry number 1202 (i.e. nothing)
# instead of the control with id 1202.
$Ids = @{
    1201 = 'Source combo'
    1202 = 'SWO Baud'
    1203 = 'Core Clock'
    1204 = 'Trace Port'
    1205 = 'SWO Prescaler'
    1206 = 'SWO Encoding'
    1207 = 'ITM Ports'
    1208 = 'Privilege'
    1209 = 'PC Sample Tap'
    1210 = 'Periodic'
    1211 = 'on Data R/W'
    1212 = 'Timestamps'
    1213 = 'Show the time column'
    1214 = 'grp Trace Source'
    1215 = 'grp ITM / DWT'
    1216 = 'grp Trace Port'
    1217 = 'grp Timestamps'
    1218 = 'Trace Enable (master)'
    1219 = 'Trace Clock'
    1220 = 'Timestamps Prescaler'
    1221 = 'evt CPI'
    1222 = 'evt EXC'
    1223 = 'evt SLEEP'
    1224 = 'evt LSU'
    1225 = 'evt FOLD'
    1226 = 'evt EXCTRC'
    1227 = 'grp Trace Events'
    1228 = 'ETM Trace Enable'
    1229 = 'Unlimited Trace'
    1230 = 'Use Core Clock'
    1231 = 'ETM Alt (combo)'
}
$AllIds = @($Ids.Keys | Sort-Object)

# The probe's own port format - only the orbtrace source has a probe to configure.
$ProbeOnly  = @(1204, 1216)
# The SWO pin's encoding - the other two sources never drive that pin.
$SwoPinOnly = @(1206)
# Never greyed: the master switch is what turns the rest off.
$NeverGrey  = @(1214, 1218)

# Per case: the ids that must be **greyed**.  Every other id must be live.  The
# target-side half (ITM / DWT, Trace Events, Timestamps) is in no case's grey list
# except the last one - that is the whole point.
$Cases = @(
    @{ Name = 'orbtrace source';  Opts = 'orbtrace=1 clock=1000'
       Grey = @($SwoPinOnly) },
    @{ Name = 'ETB source';       Opts = 'etb=1 clock=1000'
       Grey = @($ProbeOnly) + @($SwoPinOnly) },
    @{ Name = 'SWO pin source';   Opts = 'swo=1000000 clock=1000'
       Grey = @($ProbeOnly) },
    @{ Name = 'Trace Enable off'; Opts = 'orbtrace=1 tren=0'
       Grey = @($AllIds | Where-Object { $NeverGrey -notcontains $_ }) }
)

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][G]::GetClassName($h, $s, 256)
    return $s.ToString()
}

# Opens the sheet with $Opts and returns the Trace page's window handle (plus the
# process, so the caller can close it).
function Start-Sheet([string]$Opts) {
    $out = Join-Path $env:TEMP 'grey_sheet.out'
    $err = Join-Path $env:TEMP 'grey_sheet.err'
    if (Test-Path $out) { Remove-Item $out -Force }
    if (Test-Path $err) { Remove-Item $err -Force }

    $p = Start-Process -FilePath $Exe -ArgumentList ('"' + $Opts + '"') `
         -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    Start-Sleep -Seconds $Wait
    if ($p.HasExited) {
        Write-Fail "the preview exited before the sheet appeared (0x$('{0:X8}' -f $p.ExitCode))"
        Get-Content $err -ErrorAction SilentlyContinue | ForEach-Object { "      $_" }
        return $null
    }

    $script:target = [IntPtr]::Zero
    $script:pid2   = [uint32]$p.Id
    $cb = [G+EP]{
        param($h, $l)
        $owner = 0
        [void][G]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -ne $script:pid2) { return $true }
        $t = New-Object System.Text.StringBuilder 256
        [void][G]::GetWindowText($h, $t, 256)
        if ($t.ToString() -eq $DialogTitle) { $script:target = $h; return $false }
        return $true
    }
    [void][G]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:target -eq [IntPtr]::Zero) {
        Write-Fail 'the settings dialog was not found'
        $p.Kill(); return $null
    }

    # Wait for WM_INITDIALOG to finish before reading any enable state.  The pages
    # are created without WS_VISIBLE and only the initial one is shown at the very
    # end of it (ShowPage), so "some page is visible" is the ready signal - the
    # same one e2e_sheet.ps1 and inspect_sheet.ps1 use.  The Trace page is then
    # found by its source combo (1201), which is **not** required to be visible:
    # the sheet opens on Debug, and the Trace page still exists behind it.
    $markers = @(1101, 1201, 1301, 1401, 1701, 1801, 1500)
    $page = [IntPtr]::Zero
    $deadline = (Get-Date).AddMilliseconds(25000)
    while ((Get-Date) -lt $deadline) {
        $c = [G]::GetWindow($script:target, $GW_CHILD)
        while ($c -ne [IntPtr]::Zero) {
            if ((Get-ClassName $c) -eq '#32770') {
                if ($page -eq [IntPtr]::Zero -and [G]::GetDlgItem($c, 1201) -ne [IntPtr]::Zero) {
                    $page = $c
                }
                foreach ($m in $markers) {
                    if ([G]::GetDlgItem($c, $m) -ne [IntPtr]::Zero) {
                        if ([G]::IsWindowVisible($c) -and $page -ne [IntPtr]::Zero) {
                            return [pscustomobject]@{ Proc = $p; Dlg = $script:target; Page = $page }
                        }
                        break
                    }
                }
            }
            $c = [G]::GetWindow($c, $GW_HWNDNEXT)
        }
        Start-Sleep -Milliseconds 150
    }

    Write-Fail 'the sheet never finished coming up'
    $p.Kill(); return $null
}

function Close-Sheet($sheet) {
    [void][G]::PostMessage($sheet.Dlg, $WM_COMMAND, [IntPtr]$IDCANCEL, [IntPtr]::Zero)
    if (-not $sheet.Proc.WaitForExit(5000)) { $sheet.Proc.Kill() }
}

# ===========================================================================
Write-Host '==> Trace page: grey state matches what the driver reads' -ForegroundColor Cyan

foreach ($c in $Cases) {
    Write-Host "  -- $($c.Name)  [$($c.Opts)]" -ForegroundColor Cyan
    $sheet = Start-Sheet $c.Opts
    if (-not $sheet) { continue }

    $bad = @()
    foreach ($id in $AllIds) {
        $h = [G]::GetDlgItem($sheet.Page, $id)
        if ($h -eq [IntPtr]::Zero) { $bad += "$id ($($Ids[$id])) MISSING"; continue }
        $live = [G]::IsWindowEnabled($h)
        $want = ($c.Grey -notcontains $id)
        if ($Dump) {
            Write-Host ("      {0,-5} {1,-24} {2}" -f $id, $Ids[$id],
                        $(if ($live) { 'live' } else { 'grey' })) -ForegroundColor DarkGray
        }
        if ($live -ne $want) {
            $bad += ("{0} ({1}) is {2}, expected {3}" -f $id, $Ids[$id],
                     $(if ($live) { 'live' } else { 'grey' }),
                     $(if ($want) { 'live' } else { 'grey' }))
        }
    }
    Close-Sheet $sheet

    if ($bad.Count -eq 0) {
        Write-Pass "$($c.Name): every one of $($AllIds.Count) controls is as expected"
    } else {
        Write-Fail "$($c.Name): $($bad.Count) control(s) wrong"
        $bad | ForEach-Object { "      $_" }
    }
}

Write-Host ''
if ($script:failures -gt 0) {
    Write-Host "FAILED  ($($script:failures) problem(s))" -ForegroundColor Red
    exit 1
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
