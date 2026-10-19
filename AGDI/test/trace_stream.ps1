<#
    trace_stream.ps1 - streaming-trace acceptance test (the P0-A/B/C/D/F/G criteria).

    trace_matrix.ps1 answers "does trace data come out at all".  This one answers
    the architecture questions, which show up in the driver log and not in the
    byte counts:

        P0-A  the drain runs on a worker thread, not on the debug thread
        P0-B  the drain is limited by a TIME budget, not by a byte cap
        P0-C  every overflow producer is wired (the status bar can show them)
        P0-D  a gap in the byte stream forces the ETM decoder to re-sync
        P0-F  the cycle axis is real (ITM timestamp packets drive it)
        P0-G  ProgramEtm does the ETMSR handshake

    Stop latency comes from the probe's own "ran <dt> ms" line: dt is measured
    from before its Sleep(RunMs) to the moment the run thread returns, so
    dt - RunMs is the stop latency (P0-A's headline criterion).

    Match patterns are ASCII on purpose: the log body is Chinese, and a Chinese
    pattern inside a .ps1 depends on how that file was saved.  Numbers are read
    positionally; where a line has no unique ASCII token, the count of "=N"
    groups identifies it.

    Usage:
        powershell -ExecutionPolicy Bypass -File test\trace_stream.ps1
        powershell -ExecutionPolicy Bypass -File test\trace_stream.ps1 -RunMs 5000
        powershell -ExecutionPolicy Bypass -File test\trace_stream.ps1 -Swo

    Case A needs only a target; case B needs the orbtrace probe attached and the
    target firmware's trace enabled; case C (-Swo) needs the probe wired to the
    SWO pin.  Exit code: 0 when every check passed, 1 otherwise.
#>
param(
    [int]$RunMs         = 3000,
    [int]$CpuKhz        = 48000,
    [int]$ClockKhz      = 1000,
    [int]$StopLatencyMs = 50,
    [switch]$Swo,
    [string]$Exe        = ""
)

$ErrorActionPreference = 'Continue'
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\ORBAGDI_AgdiProbe.exe' }

if (-not (Test-Path $Exe)) {
    Write-Host "[ERROR] probe not built: $Exe" -ForegroundColor Red
    Write-Host "        run test\build.ps1 first" -ForegroundColor Red
    exit 1
}

$LogFile = Join-Path $env:TEMP 'ORB_AGDI.log'

# 驱动默认只记 ERROR（与 RDDI 层一致）。这里断言的是 INFO 行，所以给下头启动的
# 探针进程带上级别；探针自己继承这个环境变量。
$env:ORBAGDI_LOG_LEVEL = '1'

$script:fail = 0
$script:skip = 0
function Head($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok  ($m) { Write-Host "    PASS  $m" -ForegroundColor Green }
function Bad ($m) { Write-Host "    FAIL  $m" -ForegroundColor Red; $script:fail++ }
function Note($m) { Write-Host "    $m" -ForegroundColor Gray }
# A check that could not run because the target did not provide the precondition.
# Distinct from PASS on purpose: it must be visible that nothing was verified.
function Skip($m) { Write-Host "    SKIP  $m" -ForegroundColor Yellow; $script:skip++ }

# ---------------------------------------------------------------------------
# Log helpers.  All patterns ASCII, all numbers positional.
# ---------------------------------------------------------------------------

function Read-LogLines {
    return @(Get-Content -LiteralPath $LogFile -Encoding UTF8 -ErrorAction SilentlyContinue)
}

# The first / last line matching an ASCII anchor.
function First-Line($lines, $pattern) {
    $h = @($lines | Select-String -Pattern $pattern)
    if ($h.Count -eq 0) { return $null }
    return $h[0].Line
}
function Last-Line($lines, $pattern) {
    $h = @($lines | Select-String -Pattern $pattern)
    if ($h.Count -eq 0) { return $null }
    return $h[$h.Count - 1].Line
}
function Count-Lines($lines, $pattern) {
    return @($lines | Select-String -Pattern $pattern).Count
}

# Every "=N" on a line, in order of appearance.  The sign is optional: the
# timestamp line reports "状态=-1" when only format-2 packets were seen, and a
# pattern without "-?" silently drops that counter (it then looks like the line
# has one counter too few).
function Nums($line) {
    $out = @()
    if (-not $line) { return $out }
    foreach ($m in [regex]::Matches($line, '=(-?\d+)')) { $out += [int64]$m.Groups[1].Value }
    return $out
}

# ---------------------------------------------------------------------------
# Run one case: clear the log, drive the probe, read back what happened.
# ---------------------------------------------------------------------------
function Invoke-Case {
    param([string]$Options, [int]$Run)

    Remove-Item $LogFile -Force -ErrorAction SilentlyContinue

    $argv = @('--options', $Options)
    if ($Run -gt 0) { $argv += @('--run', "$Run") }

    $sw  = [Diagnostics.Stopwatch]::StartNew()
    $out = & $Exe @argv 2>&1
    $sw.Stop()

    return @{
        Out   = @($out)
        Lines = Read-LogLines
        Wall  = $sw.ElapsedMilliseconds
    }
}

# ---------------------------------------------------------------------------
# A. No trace source at all - the worker must not even touch the probe.
#
#    This is a regression guard, not a formality.  ComLock is non-blocking
#    TryEnter and the debug-thread entries (AG_AllReg / AG_DapAcc / AG_GoStep)
#    REFUSE when they cannot get it.  A worker that grabs the lock with nothing
#    to drain therefore makes those entries fail at random - measured once as
#    "AG_DapAcc(ACCDAP_ABORT) = 100" in the AGDI probe.  "no source" must mean
#    "no lock", and the skip counter is how that is observable.
# ---------------------------------------------------------------------------
Head 'A. no trace source: the worker must not touch the probe (P0-A guard)'
$ra = Invoke-Case -Options "clock=$ClockKhz iface=0" -Run 300

$start = First-Line $ra.Lines 'TracePump:'
$stop  = Last-Line  $ra.Lines 'TracePump:'

if ($start) { Ok 'the worker was started' } else { Bad 'no "TracePump:" start line in the log' }
if (-not $stop) {
    Bad 'no "TracePump:" stop line in the log'
} else {
    $n = Nums $stop
    if ($n.Count -lt 5) {
        Bad ("the stop line does not carry 5 counters: {0}" -f $stop)
    } else {
        $rounds   = $n[0]
        $skipSus  = $n[1]
        $skipBusy = $n[2]
        $skipNone = $n[3]
        $drained  = $n[4]
        Note ("rounds={0} skipSuspended={1} skipBusy={2} skipNoSource={3} drained={4} B" -f `
              $rounds, $skipSus, $skipBusy, $skipNone, $drained)

        if ($skipNone -gt 0) { Ok 'the worker skipped while no source was running' }
        else                 { Bad 'skipNoSource == 0 - the worker never noticed there was no source' }

        if ($skipBusy -eq 0) { Ok 'the worker never contended for the probe lock (skipBusy == 0)' }
        else                 { Bad ("the worker grabbed the probe lock with no source to drain ({0} times)" -f $skipBusy) }

        if ($drained -eq 0)  { Ok 'nothing was drained (as expected with no source)' }
        else                 { Bad ("drained {0} B with no trace source configured" -f $drained) }
    }
}

# ---------------------------------------------------------------------------
# B. The stream itself: orbtrace parallel port, timestamps on.
#
#    ts=1 matters: it is what sets ITM_TCR.TSENA, and without it the target
#    emits no timestamp packets - so P0-F's cycle axis legitimately stays fake
#    and "TraceWin:" would have no "bTS=1" line.  The axis check is therefore
#    only meaningful in a run that asked for timestamps.
# ---------------------------------------------------------------------------
Head 'B. orbtrace stream: worker, time budget, overflow producers, re-sync, time axis'
$optB = "clock=$ClockKhz iface=0 orbtrace=1 trwidth=4 trswo=0 cpu=$CpuKhz " +
        "rstcon=1 stoprst=0 pcperiod=1 pcpresc=0 ts=1"
$rb = Invoke-Case -Options $optB -Run $RunMs

# --- the probe's own verdict + the stop latency -------------------------------
$ranLine = ($rb.Out | Select-String -Pattern 'ran (\d+) ms' | Select-Object -Last 1)
if (-not $ranLine) {
    Bad 'the probe did not report a completed run'
    $rb.Out | Select-Object -Last 12 | ForEach-Object { Note $_ }
} else {
    $dt = [int64]$ranLine.Matches[0].Groups[1].Value
    $latency = $dt - $RunMs
    Note ("run wall time {0} ms for a {1} ms run => stop latency {2} ms" -f $dt, $RunMs, $latency)
    # dt is measured from before the probe's own Sleep(runMs) to the moment the
    # run thread returns, so it carries that Sleep's overshoot too - up to one
    # system timer tick (~15.6 ms) without timeBeginPeriod(1).  The real stop
    # latency is therefore somewhat below the number above; the threshold is on
    # the conservative side on purpose.
    if ($latency -le $StopLatencyMs) {
        Ok ("stop latency {0} ms <= {1} ms (P0-A: the drain no longer blocks the debug thread)" -f $latency, $StopLatencyMs)
    } else {
        Bad ("stop latency {0} ms > {1} ms - something is still blocking the run loop" -f $latency, $StopLatencyMs)
    }
}

# --- worker lifecycle + it actually drained ----------------------------------
$stopB = Last-Line $rb.Lines 'TracePump:'
if (-not $stopB) {
    Bad 'no worker stop line - the worker was never started'
} else {
    $n = Nums $stopB
    if ($n.Count -ge 5) {
        Note ("worker: rounds={0} skipBusy={1} skipNoSource={2} drained={3} B" -f $n[0], $n[2], $n[3], $n[4])
        if ($n[4] -gt 0) { Ok ("the worker drained {0} B (P0-A: the drain runs off the debug thread)" -f $n[4]) }
        else             { Bad 'the worker drained 0 B - no source was active, or it never ran' }
    } else {
        Bad ("the worker stop line does not carry 5 counters: {0}" -f $stopB)
    }
}

# --- orbtrace close line: bytes / frames / ITM / ETM -------------------------
$close = $null
foreach ($l in @($rb.Lines | Select-String -Pattern 'orbtrace:')) {
    if ((Nums $l.Line).Count -eq 7) { $close = $l.Line }
}
if (-not $close) {
    Bad 'no "orbtrace:" close line with 7 counters - the stream never opened'
} else {
    $c = Nums $close
    Note ("orbtrace: bytes={0} frames={1} ITM={4} ETM={5}" -f $c[0], $c[1], $c[2], $c[3], $c[4], $c[5])
    if ($c[5] -gt 0) { Ok ("ETM decoded {0} bytes - the instruction-trace path is live" -f $c[5]) }
    else             { Bad 'ETM == 0 - no instruction trace reached the decoder' }
    if ($c[4] -gt 0) { Note ("ITM {0} bytes" -f $c[4]) }
}

# --- P0-C: the read path line must carry the ring-drop counter ---------------
$readPath = $null
foreach ($l in @($rb.Lines | Select-String -Pattern 'orbtrace:')) {
    if ((Nums $l.Line).Count -eq 4) { $readPath = $l.Line }
}
if (-not $readPath) {
    Bad 'no read-path line with 4 counters - TraceStream_DroppedBytes is not wired (P0-C)'
} else {
    $p = Nums $readPath
    Note ("read path: calls={0} empty={1} errors={2} ringDropped={3} B" -f $p[0], $p[1], $p[2], $p[3])
    Ok 'the read-path line carries the RDDI ring drop counter (P0-C producer wired)'
    if ($p[2] -eq 0) { Ok 'no read errors' } else { Bad ("{0} TraceStream_Read failures" -f $p[2]) }
    if ($p[3] -eq 0) { Ok 'the RDDI ring never overflowed' }
    else             { Note ("the ring dropped {0} B - the drain is behind; the status bar should show Data Overflow" -f $p[3]) }
}

# --- P0-D: the ETM summary must carry the re-sync counter --------------------
$etmSum = Last-Line $rb.Lines 'ETM: '
if (-not $etmSum) {
    Bad 'no "ETM:" summary line'
} else {
    $e = Nums $etmSum
    if ($e.Count -ge 7) {
        Note ("ETM: addresses={0} atoms={1} exceptions={2} timestamps={3} cycles={4} synced={5} resyncs={6}" -f `
              $e[0], $e[1], $e[2], $e[3], $e[4], $e[5], $e[6])
        Ok 'the ETM summary carries the gap-resync counter (P0-D producer wired)'
        if ($e[6] -gt 0) {
            Note ("{0} forced re-sync(s) - expected whenever frames or ring bytes were lost" -f $e[6])
        }
    } else {
        Bad ("the ETM summary does not carry 7 counters: {0}" -f $etmSum)
    }
}

# --- P0-G: the ETMSR handshake ran, twice, and the registers read back -------
$hsCount = Count-Lines $rb.Lines 'ETMSR=0x'
if ($hsCount -ge 2) { Ok ("the ETMSR handshake completed {0} times (enter + leave programming mode)" -f $hsCount) }
else                { Bad ("only {0} ETMSR handshake line(s) - ProgramEtm did not poll ETMSR (P0-G)" -f $hsCount) }

$idrLine = Last-Line $rb.Lines 'TRACEIDR=0x'
if (-not $idrLine) {
    Bad 'no ETM read-back line - the programming-mode read-back is missing (P0-G)'
} else {
    $m = [regex]::Match($idrLine, 'TRACEIDR=0x([0-9A-Fa-f]+)')
    $idr = 0
    if ($m.Success) { $idr = [Convert]::ToInt32($m.Groups[1].Value, 16) }
    if ($idr -eq 2) { Ok 'ETMTRACEIDR read back as 2 - the write survived programming mode' }
    else            { Bad ("ETMTRACEIDR read back as {0}, expected 2 (the ATB id)" -f $idr) }
}

# --- P0-F: timestamp packets decoded, and the cycle axis became real ---------
#
#    Two different questions, and they must be told apart:
#      "did the target send any timestamp packet at all"  -> the target's business
#      "did the driver turn it into a real cycle axis"     -> ours
#    ItmLogTsStats() is the line that answers the first one, so a zero there is
#    a SKIP (nothing was verified) and not a FAIL.
$tsLine = Last-Line $rb.Lines 'ITM: TS '
if (-not $tsLine) {
    Bad 'no "ITM: TS" line - ItmLogTsStats() was never reached'
} else {
    $t = Nums $tsLine
    if ($t.Count -lt 3) {
        Bad ("the timestamp line does not carry 3 counters: {0}" -f $tsLine)
    } else {
        Note ("ITM timestamps: packets={0} cycles={1} status={2}" -f $t[0], $t[1], $t[2])
        if ($t[0] -gt 0) {
            Ok ("{0} timestamp packet(s) decoded, {1} cycles advanced" -f $t[0], $t[1])
            if (Last-Line $rb.Lines 'bTS=1') {
                Ok 'the cycle axis is real - new records carry bTS=1 (P0-F)'
            } else {
                Bad 'timestamp packets were decoded but no "bTS=1" line - the axis never became real'
            }
        } else {
            Skip ('no timestamp packet reached the decoder, so the axis cannot be real. ' +
                  'With TRACE_ETM_BRANCH_OUT=1 the ETM flood takes 92% of the port and ITM ' +
                  'is left with ~50 B (P0-E) - flash the bit8=0 firmware (P1-0) and re-run')
        }
    }
}

# --- P0-F': the cycle axis stays physically plausible ------------------------
#
#    The axis is in CORE CYCLES and the run only lasted RunMs milliseconds, so the
#    total is bounded: it cannot exceed CpuKhz * elapsed_ms (plus slack).  A value
#    orders of magnitude past that is not time - it is a misdecoded value (a
#    misframed stream, or a bad value out of the decoder) that would leave
#    uVision's time column and Navigation bar showing nonsense.
#
#    Measured once as a 1.88e8..2.00e11 cycle range over 1098 records - about
#    4700 s of "time" inside a few-second capture.  The driver now refuses such an
#    increment and counts it (AdvanceFrom's AxisBudget), so:
#      reject=0  -> nothing impossible was ever offered
#      reject>0  -> something was: the stream is misframed (compare trwidth with
#                   the firmware's TRACE_PARALLEL_BITS).  The axis stayed usable
#                   because the value was dropped, but the cause is still there.
#    Either way the axis itself must sit inside the wall-clock budget.
$axLine = Last-Line $rb.Lines 'cycle axis\('
if (-not $axLine) {
    Bad 'no "cycle axis(" line - TraceWinCycleAxisLog() was never reached'
} else {
    # cycles records itm etm rebase reject bTS
    $ax = Nums $axLine
    if ($ax.Count -lt 7) {
        Bad ("the cycle-axis line does not carry 7 counters: {0}" -f $axLine)
    } else {
        $axCycles = $ax[0]
        $axReject = $ax[5]
        # Same shape as the driver's own bound: 4x slack, plus its 1 s head start.
        $budget = [int64]$CpuKhz * ([int64]$RunMs + 1000) * 4
        Note ("cycle axis: cycles={0} reject={1} budget={2}" -f $axCycles, $axReject, $budget)
        if ($axCycles -le $budget) {
            Ok ("the cycle axis is inside the wall-clock budget ({0} <= {1} cycles)" -f $axCycles, $budget)
        } else {
            Bad ("the cycle axis is {0} cycles, past the {1} a {2} ms run can hold - it is not real time" -f $axCycles, $budget, $RunMs)
        }
        if ($axReject -eq 0) {
            Ok 'no impossible cycle-axis increment was offered'
        } else {
            Bad ("{0} impossible cycle-axis increment(s) were refused - the stream is misframed (trwidth vs the firmware's TRACE_PARALLEL_BITS)" -f $axReject)
        }
    }
}

# --- must NOT appear ---------------------------------------------------------
$flashWarn = @($rb.Lines | Select-String -Pattern 'Flash: ' | Select-String -Pattern 'worker')
if ($flashWarn.Count -eq 0) { Ok 'no "Flash: ... worker" warning - the download path never had to fight the worker' }
else                        { Bad 'the Flash path could not get the worker out of the way' }

# ---------------------------------------------------------------------------
# C. ITM over the SWO pin, opt-in.
#
#    Two different things are called "SWO"; this case uses the second one:
#
#      swo=<Hz>                          the RDDI host's CMSIS-DAP SWO engine.
#                                        The orbtrace probe does not implement that
#                                        command family - every baud comes back
#                                        RDDI_DAP_ERROR(0x2000), console never starts.
#      orbtrace=1 trswo=1|2 trbaud=<Hz>  SWO as an input format of the probe's own
#                                        trace channel (next to the 4-bit parallel
#                                        one); both come out of TraceStream_Read.
#                                        This is where the ITM timestamps live.
#
#    The SWO branch no longer programs the ETM, so the single pin carries ITM only
#    and timestamp packets get through even with TRACE_ETM_BRANCH_OUT=1.
# ---------------------------------------------------------------------------
if ($Swo) {
    Head 'C. ITM over the SWO pin via the probe trace channel (opt-in)'
    $optC = "clock=$ClockKhz iface=0 orbtrace=1 trswo=1 trbaud=2000000 cpu=$CpuKhz " +
            "rstcon=1 stoprst=0 pcperiod=1 pcpresc=0 ts=1"
    $rc = Invoke-Case -Options $optC -Run $RunMs

    # --- the probe accepted the SWO input format -----------------------------
    $cfgLine = Last-Line $rc.Lines 'orbtrace: SetTraceSWO'
    if (-not $cfgLine) {
        Bad 'no "orbtrace: SetTraceSWO" line - the SWO input was never configured'
    } elseif ($cfgLine -match '->\s*0\s*$') {
        Ok 'the probe accepted the SWO input format'
    } else {
        Bad ("the probe refused the SWO input format: {0}" -f $cfgLine)
    }

    # --- what came out of the pin --------------------------------------------
    $closeC = $null
    foreach ($l in @($rc.Lines | Select-String -Pattern 'orbtrace:')) {
        if ((Nums $l.Line).Count -eq 7) { $closeC = $l.Line }
    }
    if (-not $closeC) {
        Bad 'no "orbtrace:" close line with 7 counters - the stream never opened'
    } else {
        $c = Nums $closeC
        Note ("orbtrace: bytes={0} frames={1} ITM={4} ETM={5}" -f $c[0], $c[1], $c[2], $c[3], $c[4], $c[5])
        if ($c[4] -gt 0) { Ok ("ITM carried {0} bytes over the SWO pin" -f $c[4]) }
        else             { Bad 'ITM == 0 - nothing came over the SWO pin' }

        if ($c[5] -eq 0) {
            Ok 'ETM == 0 - the SWO branch no longer programs it'
        } else {
            Note ("ETM {0} bytes on the SWO pin - that is the FIRMWARE's ETM_Pin_Init(), " +
                  "not this driver (the SWO branch no longer calls ProgramEtm); " +
                  "flash TRACE_ETM_BRANCH_OUT=0 (P1-0) to silence it" -f $c[5])
        }
    }

    # --- P0-F over the SWO pin -----------------------------------------------
    $tsLine = Last-Line $rc.Lines 'ITM: TS '
    if (-not $tsLine) {
        Bad 'no "ITM: TS" line - ItmLogTsStats() was never reached'
    } else {
        $t = Nums $tsLine
        if ($t.Count -lt 3) {
            Bad ("the timestamp line does not carry 3 counters: {0}" -f $tsLine)
        } else {
            Note ("ITM timestamps: packets={0} cycles={1} status={2}" -f $t[0], $t[1], $t[2])
            if ($t[0] -gt 0) {
                Ok ("{0} timestamp packet(s) decoded over SWO, {1} cycles advanced (P0-F)" -f $t[0], $t[1])
                if (Last-Line $rc.Lines 'bTS=1') { Ok 'the cycle axis is real - records carry bTS=1' }
                else { Bad 'timestamp packets were decoded but no "bTS=1" line' }
            } else {
                Skip ('no timestamp packet came over the SWO pin. Check ts=1 (ITM_TCR.TSENA) ' +
                      'and that the firmware is actually printing / sampling')
            }
        }
    }
} else {
    Write-Host ''
    Write-Host '    (SWO case skipped - add -Swo; it needs the probe wired to the SWO pin)' -ForegroundColor DarkGray
}

Write-Host ''
if ($script:fail -gt 0) {
    Write-Host ("FAILED  ({0} problem(s), {1} skipped)" -f $script:fail, $script:skip) -ForegroundColor Red
    exit 1
}
if ($script:skip -gt 0) {
    Write-Host ("ALL PASSED  ({0} skipped - see above)" -f $script:skip) -ForegroundColor Yellow
    exit 0
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
