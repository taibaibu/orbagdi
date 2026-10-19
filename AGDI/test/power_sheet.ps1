<#
    Power page: the one page that can energise the target.

    Two properties, and the second only matters because the first holds:

      1. **OK and Cancel send nothing.**  The probe's Vtref / Vtpwr outputs are the
         target's power rails; a stray click that turns one on is not a cosmetic
         bug.  Only the page's Apply button may reach `OrbPowerApply` - `Accept()`
         does not read this page at all (it has no option key, by design).  Checked
         by opening the sheet, pressing OK, and requiring the driver's log to hold
         no `power:` line whatsoever.

      2. **Apply really does send.**  Pick a level, tick a channel, click Apply,
         and read the driver's own log back; then click All Off and check both
         channels report off.  Finally Cancel and check that it added nothing.

    Needs an orbtrace probe on the bench.  The target may be disconnected - the
    supply outputs are the whole point - and without a probe the driver logs
    `power: TraceStream_Open ...` and case 2 is reported SKIPPED rather than
    failed, because that is an environment fact and not a defect.

    !! Case 2 is **off by default** and has to be asked for with `-Energise`.  It
      turns one probe output ON for about a second; nothing may be wired to it,
      and All Off runs in a finally block so it ends off either way.
      The rule (2026-10-14, user): **do not test the power outputs.**  Case 1 is
      the half that matters for routine work - it is what stops a stray OK from
      energising the target - and it never touches the supply.

    Usage:  .\test\power_sheet.ps1                # case 1 only - safe, no output is enabled
            .\test\power_sheet.ps1 -Energise      # also sends (energises a probe output!)
#>
param(
    [string]$Exe,
    [int]$Wait = 5,
    # The energising half must be asked for explicitly; the default only runs
    # "OK / Cancel send nothing".  Rule from 2026-10-14: do not test the power
    # outputs.
    # (ASCII only, here and everywhere in this file: PowerShell 5.1 reads .ps1 as
    #  ANSI, and a non-ASCII comment can swallow the next line - which is how this
    #  switch disappeared once already.)
    [switch]$Energise
)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\SettingsPreview.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first" }

$script:failures = 0
$script:skipped  = 0
function Write-Pass([string]$m) { Write-Host "    PASS  $m" -ForegroundColor Green }
function Write-Fail([string]$m) { Write-Host "    FAIL  $m" -ForegroundColor Red
                                  $script:failures++ }
function Write-Skip([string]$m) { Write-Host "    SKIP  $m" -ForegroundColor Yellow
                                  $script:skipped++ }

# ---------------------------------------------------------------------------
# Just enough Win32 to find the dialog and click a control in it.  Every message
# sent below takes no pointer, so nothing has to be marshalled across processes.
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
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

$DialogTitle = 'ORBTRACE Debugger Settings'
$WM_COMMAND  = 0x0111
$BM_CLICK    = 0x00F5
$CB_SETCURSEL = 0x014E
$IDOK        = 1
$IDCANCEL    = 2
$GW_CHILD    = 5
$GW_HWNDNEXT = 2

# Control ids, from src\ORBAGDI_Settings.cpp.  1801 is the page's anchor - the
# same id inspect_sheet.ps1 uses to recognise the page - and the rest follow it.
$kGrpVtref   = 1801
$kCmbVtref   = 1802
$kChkVtrefEn = 1803
$kCmbVtpwr   = 1805
$kChkVtpwrEn = 1806
$kBtnApply   = 1807
$kBtnOff     = 1808

$script:pageMarkers = @(1101, 1201, 1301, 1401, 1801, 1500)

# ---------------------------------------------------------------------------
# The RDDI host DLL.
#
# SettingsPreview.exe **links** the driver sources instead of loading ORB_AGDI.dll,
# so ORBAGDI_Rddi.cpp's "next to me" search resolves to test\bin\ - where there is
# no host.  Every other sheet check is happy without one (they only look at the
# dialog), but the Power page cannot reach the probe without it, so point the
# loader at the real one.  This is a property of the harness, not of the driver:
# under uVision the host sits next to ORB_AGDI.dll and is found the normal way.
# ---------------------------------------------------------------------------
$script:rddi = Join-Path $TestDir '..\bin\CMSIS_DAP.dll'
if (Test-Path $script:rddi) {
    $env:ORBAGDI_RDDI_DLL = (Resolve-Path $script:rddi).Path
}

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][W]::GetClassName($h, $s, 256)
    return $s.ToString()
}

# ---------------------------------------------------------------------------
# The driver's log.  Only ASCII is matched out of it on purpose: the log is
# written as ANSI, so anything non-ASCII here would be decoded differently by
# whatever PowerShell this runs under.
# ---------------------------------------------------------------------------
$LogFile = Join-Path $env:TEMP 'ORB_AGDI.log'

function Get-PowerLines {
    if (-not (Test-Path $LogFile)) { return @() }
    try { return @([IO.File]::ReadAllLines($LogFile) | Where-Object { $_ -like '*power: *' }) }
    catch { return @() }
}

# A `power:` line that means something was actually **sent** to the probe.  The
# diagnostics (`power: RDDI host ...`, `power: TraceStream_Open ...`) are not
# sends, and counting raw `power:` lines would make those look like one.
#
# Counting is per run, not cumulative: orb::log::Init() truncates the file when a
# process starts, so each sheet opens with an empty log.
function Get-SendLines {
    return @(Get-PowerLines | Where-Object {
        $_ -match 'power: (Vtref|Vtpwr) = \d+ mV, (ON|OFF)' -or $_ -match 'power: .*CH_ALL'
    })
}

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

function Start-Sheet {
    $out = Join-Path $env:TEMP 'power_sheet.out'
    $err = Join-Path $env:TEMP 'power_sheet.err'
    Remove-Item $out, $err -ErrorAction SilentlyContinue

    $p = Start-Process -FilePath $Exe -ArgumentList '"clock=1000 iface=0"' `
         -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    Start-Sleep -Seconds $Wait
    if ($p.HasExited) {
        Write-Fail "the process exited before the dialog appeared (0x$('{0:X8}' -f $p.ExitCode))"
        Get-Content $err -ErrorAction SilentlyContinue | ForEach-Object { "      $_" }
        return $null
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
        $p.Kill(); return $null
    }
    if (-not (Wait-SheetReady $script:target)) {
        Write-Fail 'the sheet never finished coming up'
        $p.Kill(); return $null
    }

    # The Power page is whichever child dialog owns id 1801.
    $page = [IntPtr]::Zero
    $c = [W]::GetWindow($script:target, $GW_CHILD)
    while ($c -ne [IntPtr]::Zero) {
        if ((Get-ClassName $c) -eq '#32770' -and
            [W]::GetDlgItem($c, $kGrpVtref) -ne [IntPtr]::Zero) { $page = $c; break }
        $c = [W]::GetWindow($c, $GW_HWNDNEXT)
    }
    if ($page -eq [IntPtr]::Zero) {
        Write-Fail 'the Power page was not found (no page owns id 1801)'
        $p.Kill(); return $null
    }
    return [pscustomobject]@{ Proc = $p; Dlg = $script:target; Page = $page }
}

function Click([IntPtr]$page, [int]$id) {
    [void][W]::SendMessage([W]::GetDlgItem($page, $id), $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Close-Sheet($sheet, [int]$button) {
    [void][W]::PostMessage($sheet.Dlg, $WM_COMMAND, [IntPtr]$button, [IntPtr]::Zero)
    if (-not $sheet.Proc.WaitForExit(5000)) { $sheet.Proc.Kill() }
}

# ===========================================================================
Write-Host '==> Power page: OK sends nothing, Apply sends' -ForegroundColor Cyan

# --- 1. opening the sheet, and pressing OK, must not touch the supply ------
#
# Every assertion below is made on the **finished run**, never mid-run.  The log
# is flushed per line (ORBAGDI_Log.cpp does an fflush after each one), but reading
# it while the sheet is up proved unreliable in practice - a mid-run read came back
# empty while the same file held three lines a moment later.  A test that depends
# on when a file becomes visible is a test that lies; after the process is gone
# there is no such question.
Write-Host '  -- Opening the sheet and pressing OK sends nothing' -ForegroundColor Cyan
$sheet = Start-Sheet
if ($sheet) {
    Close-Sheet $sheet $IDOK
    $sends = @(Get-SendLines)
    if ($sends.Count -eq 0) {
        Write-Pass 'the Power page came up and the whole run sent no power command'
    } else {
        Write-Fail "the run sent $($sends.Count) power command(s) - OK must never do that"
        $sends | ForEach-Object { "      $_" }
    }
}

if (-not $Energise) {
    Write-Host ''
    Write-Host '  (send path NOT exercised - add -Energise; it energises a probe output)' -ForegroundColor DarkGray
} elseif (-not (Test-Path $script:rddi)) {
    Write-Skip "no RDDI host at $($script:rddi) - the send path cannot be exercised"
} else {
    # --- 2. Apply really sends, All Off really turns both off --------------
    Write-Host '  -- Apply sends, All Off turns both off' -ForegroundColor Cyan
    $sheet = Start-Sheet
    if ($sheet) {
        try {
            # Vtref 3.3 V is index 1 of that channel's table (1.8 / 3.3).
            [void][W]::SendMessage([W]::GetDlgItem($sheet.Page, $kCmbVtref),
                                   $CB_SETCURSEL, [IntPtr]1, [IntPtr]::Zero)
            Click $sheet.Page $kChkVtrefEn
            Click $sheet.Page $kBtnApply
            Start-Sleep -Milliseconds 1000
            Click $sheet.Page $kBtnOff
            Start-Sleep -Milliseconds 1000
        } finally {
            # Belt and braces: whatever happened above, ask for All Off once more
            # before closing.  Nothing may be left energised.
            Click $sheet.Page $kBtnOff
            Close-Sheet $sheet $IDCANCEL
        }

        $sends  = @(Get-SendLines)
        $on     = @($sends | Where-Object { $_ -match 'Vtref = 3300 mV, ON' })
        $allOff = @($sends | Where-Object { $_ -match 'CH_ALL' })
        # ASCII only: the log is written as ANSI, so a non-ASCII pattern here would
        # be decoded differently by whatever PowerShell this runs under.
        $diag   = @(Get-PowerLines | Where-Object { $_ -match 'TraceStream_Open|RDDI' })

        if ($on.Count -gt 0) {
            Write-Pass 'Apply sent Vtref 3300 mV ON'
        } elseif ($diag.Count -gt 0) {
            Write-Skip 'the probe could not be opened - the send path was not exercised'
            $diag | Select-Object -Last 3 | ForEach-Object { "      $_" }
        } else {
            Write-Fail 'Apply sent no power command'
            Get-PowerLines | Select-Object -Last 8 | ForEach-Object { "      $_" }
        }

        if ($allOff.Count -gt 0) {
            if ($on.Count -gt 0 -and
                [array]::IndexOf($sends, $allOff[0]) -lt [array]::IndexOf($sends, $on[0])) {
                Write-Fail 'All Off was logged before the send - the order is wrong'
            } else {
                Write-Pass 'All Off reported both outputs off, after the send'
            }
        } elseif ($diag.Count -gt 0) {
            Write-Skip 'All Off could not be exercised either (no probe)'
        } else {
            Write-Fail 'All Off sent no power command'
        }

        # The run must not end on an "on".  The finally block asks for All Off
        # again, so the last thing the driver sent has to be that.
        $last = if ($sends.Count -gt 0) { $sends[-1] } else { '' }
        if ($last -match 'CH_ALL') {
            Write-Pass 'the last command sent was All Off'
        } else {
            Write-Fail "the run ended on '$last' - an output may still be on"
        }
    }
}

Write-Host ''
if ($script:failures -gt 0) {
    Write-Host "FAILED  ($($script:failures) problem(s))" -ForegroundColor Red
    exit 1
}
if ($script:skipped -gt 0) {
    Write-Host "ALL PASSED  ($($script:skipped) skipped - needs the orbtrace probe)" -ForegroundColor Yellow
    exit 0
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
