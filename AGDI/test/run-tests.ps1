<#
    ORBAGDI settings-sheet test runner - the one command to run.

    It builds the preview (which compiles the REAL src\ORBAGDI_Settings.cpp, so
    what is tested is what uVision loads) and then runs the external checks:

        test\e2e_sheet.ps1        opens the sheet, presses OK from the outside, and
                                  compares the option string that comes back with
                                  the one that went in - once per trace source, and
                                  once for Cancel

        test\inspect_sheet.ps1    walks the window tree of the open dialog and
                                  checks the tab control and its titles, the seven
                                  pages, the geometry (nothing clipped) and the
                                  Info box

        test\check_log_share.ps1  checks the driver's log is still readable while
                                  the driver holds it open (COMPAT_ANALYSIS.md §10.2)

        test\grey_sheet.ps1       walks the Trace page's grey-state matrix: for each
                                  source, every control is either operable or greyed,
                                  and which one it is has to match what the driver
                                  actually reads (IsWindowEnabled, never a picture)

    No script is known to src\: there are no test hooks in the driver, and the
    dialog is driven the way a user drives it.  `SettingsPreview.exe --ui` exists
    only so a human can look at the sheet.

    Two more checks are opt-in too:

        test\trace_stream.ps1     the streaming-trace architecture criteria
                                  (P0-A/B/C/D/F/G).  Needs the orbtrace probe
                                  attached and a target whose trace is enabled.

        test\power_sheet.ps1      the Power page: that OK and Cancel send nothing.
                                  `-Power` runs only that half - it never touches
                                  the supply, and it is the half that matters
                                  (it is what stops a stray OK from energising
                                  the target).  `-PowerSend` additionally
                                  exercises Apply, which **energises a probe
                                  output**; the rule from 2026-10-14 is **not to
                                  test that**, so it is a separate switch.

    Two more scripts in this folder are tools rather than checks, so they are not
    run here:

        test\shot_sheet.ps1       screenshot the sheet, any page, to a PNG
        test\show_info.ps1        print the text of the Info page (or one section)

    The sheet checks are **opt-in** (`-Sheet`).  They need SettingsPreview.exe,
    which only changes when the settings page does - running them after every
    driver change costs a dialog per case and proves nothing new.  The default
    run is the AGDI probe, which is the one that exercises the driver.

    Usage:
        .\test\run-tests.ps1                # build + the AGDI probe (hardware)
        .\test\run-tests.ps1 -Sheet         # also run the four sheet checks
        .\test\run-tests.ps1 -Stream        # also run the streaming-trace checks
        .\test\run-tests.ps1 -Power         # also run the Power page (safe half only)
        .\test\run-tests.ps1 -PowerSend     # ...plus the send half (ENERGISES AN OUTPUT)
        .\test\run-tests.ps1 -NoProbe       # build only, run nothing
        .\test\run-tests.ps1 -Clean         # wipe test\obj and test\bin first
        .\test\run-tests.ps1 -SkipBuild     # reuse the existing exe
        .\test\run-tests.ps1 -Show          # open the sheet for a look
        .\test\run-tests.ps1 -Dump          # dump the control tree of each page

    Exit code: 0 when every check passed, 1 otherwise.
#>
[CmdletBinding()]
param([switch]$Clean, [switch]$SkipBuild, [switch]$Sheet, [switch]$Show,
      [switch]$Dump, [switch]$NoProbe, [switch]$Stream, [switch]$Power,
      [switch]$PowerSend)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Exe     = Join-Path $TestDir 'bin\SettingsPreview.exe'
$Probe   = Join-Path $TestDir 'bin\ORBAGDI_AgdiProbe.exe'

$script:failures = 0
function Write-Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Fail([string]$m) { Write-Host "    FAIL  $m" -ForegroundColor Red
                                  $script:failures++ }

# ---------------------------------------------------------------------------
Write-Step 'Build'
if ($SkipBuild) {
    Write-Host '    reusing what is already in test\bin'
} else {
    $buildArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $TestDir 'build.ps1'))
    if ($Clean) { $buildArgs += '-Clean' }
    & powershell @buildArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

# ---------------------------------------------------------------------------
# The AGDI probe is the default check: it drives the driver itself on real
# hardware, so it is the one that can fail when src\ changes.
if ($NoProbe) {
    Write-Host ''
    Write-Host '    (AGDI probe skipped -NoProbe)' -ForegroundColor DarkGray
} else {
    if (-not (Test-Path $Probe)) { throw "missing $Probe - build it first" }
    Write-Step 'AGDI layer: connect, memory, registers, run control'
    & $Probe | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the AGDI probe did not pass' }
}

# ---------------------------------------------------------------------------
# The settings-sheet checks are opt-in.  SettingsPreview.exe only changes when
# the settings page does, and every case costs a dialog - running them after a
# driver change proves nothing new.
if ($Sheet) {
    if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first" }

    Write-Step 'End to end: open the sheet, press OK, compare the option string'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $TestDir 'e2e_sheet.ps1') -Exe $Exe | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the end-to-end option-string check did not pass' }

    Write-Step 'Layout: control tree and geometry of the open sheet'
    $inspectArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $TestDir 'inspect_sheet.ps1'), '-Exe', $Exe)
    if ($Dump) { $inspectArgs += '-Dump' }
    & powershell @inspectArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the layout check did not pass' }

    Write-Step 'Log: readable while the driver holds it open'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $TestDir 'check_log_share.ps1') -Exe $Exe | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the log-sharing check did not pass' }

    Write-Step 'Grey state: the Trace page greys exactly what the driver ignores'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $TestDir 'grey_sheet.ps1') -Exe $Exe | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the grey-state matrix did not hold' }
} else {
    Write-Host ''
    Write-Host '    (settings-sheet checks skipped - add -Sheet when the page changed)' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# The Power page's send path is opt-in for a reason of its own: it **energises a
# probe output**.  Nothing may be wired to it, and it is not part of a routine
# run.  The "OK sends nothing" half of it is safe, but it lives in the same
# script, so the whole thing is behind a switch.
# ---------------------------------------------------------------------------
if ($Power -or $PowerSend) {
    # !! `-Power` only runs the half that proves OK / Cancel send nothing.
    #   `-PowerSend` additionally energises a probe output - **do not use it**:
    #   the rule from 2026-10-14 is that the power outputs are not to be tested.
    Write-Step $(if ($PowerSend) { 'Power: OK sends nothing, Apply sends (ENERGISES AN OUTPUT)'
                                 } else { 'Power: OK sends nothing' })
    $powerArgs = @('-ExecutionPolicy', 'Bypass', '-File',
                   (Join-Path $TestDir 'power_sheet.ps1'), '-Exe', $Exe)
    if ($PowerSend) { $powerArgs += '-Energise' }
    & powershell @powerArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the power-page check did not pass' }
} else {
    Write-Host ''
    Write-Host '    (power-page check skipped - add -Power; never energises anything)' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# The streaming-trace checks are opt-in as well: they need the orbtrace probe
# attached and a target whose trace is enabled, and they take a run per case.
if ($Stream) {
    Write-Step 'Trace stream: worker, time budget, overflow producers, re-sync, time axis'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $TestDir 'trace_stream.ps1') | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Fail 'the trace-stream checks did not pass' }
} else {
    Write-Host ''
    Write-Host '    (trace-stream checks skipped - add -Stream; needs the orbtrace probe)' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
if ($Show) {
    Write-Step 'Opening the sheet'
    & $Exe --ui
}

Write-Host ''
if ($script:failures -gt 0) {
    Write-Host "FAILED  ($script:failures problem(s))" -ForegroundColor Red
    exit 1
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
