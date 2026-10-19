<#
    Checks that the driver's log stays readable while the driver holds it open.

    This guards COMPAT_ANALYSIS.md §10.2.  The log was opened with _wfopen_s, whose default
    sharing mode does not include FILE_SHARE_WRITE, so the driver locked every
    other reader out for the whole session - including its own Info page, which
    then reported "the log file cannot be opened" while holding the very file it
    was being asked to show (ERROR_SHARING_VIOLATION, errno 13 / win32 32).

    Two facts are checked, and they are different things:

      * the file can be opened for reading with read/write sharing while the
        driver is running.  That is the raw OS-level property, and it is what the
        driver's own reader asks for (_wfsopen(..., _SH_DENYNO));
      * the same open with read-only sharing is *refused*.  That is not a bug -
        the driver's handle has write access, and a reader that will not share
        writes cannot coexist with it.  Asserting it keeps the first check
        honest: if the driver ever stopped holding the file, both would pass and
        the test would prove nothing.

    Usage:  .\test\check_log_share.ps1
            .\test\check_log_share.ps1 -Exe <path> -Wait 6

    Exit code: 0 when every check passed.
#>
param(
    [string]$Exe,
    [int]$Wait = 6
)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\SettingsPreview.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first (test\build.ps1)" }

$script:failures = 0
function Write-Pass([string]$m) { Write-Host "    PASS  $m" -ForegroundColor Green }
function Write-Fail([string]$m) { Write-Host "    FAIL  $m" -ForegroundColor Red
                                  $script:failures++ }

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class L {
  public delegate bool EP(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
}
'@

$LogPath = Join-Path $env:TEMP 'ORB_AGDI.log'

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][L]::GetClassName($h, $s, 256)
    return $s.ToString()
}

# Start from a clean slate: a log left over from an earlier run would make the
# "the driver created it" step meaningless.
if (Test-Path $LogPath) { Remove-Item $LogPath -Force }

$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds $Wait
if ($p.HasExited) { Write-Fail "the preview exited early (0x$('{0:X8}' -f $p.ExitCode))"; exit 1 }

if (Test-Path $LogPath) {
    $size = (Get-Item $LogPath).Length
    Write-Pass "the driver created $LogPath ($size bytes)"
} else {
    Write-Fail "the driver did not create $LogPath - the rest cannot be judged"
}

# --- the two sharing questions ---------------------------------------------
$shared = $false
try {
    $fs = [System.IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object System.IO.StreamReader($fs)
    $head = $sr.ReadLine()
    $sr.Close()
    $shared = $true
    if ($head -and $head -match '^\[\d\d:\d\d:\d\d\.\d\d\d\]') {
        Write-Pass "readable while the driver holds it, and it reads as a log line"
    } else {
        Write-Pass "readable while the driver holds it (first line: '$head')"
    }
} catch {
    Write-Fail "cannot read the log while the driver holds it - the Info page will say so too ($($_.Exception.Message))"
}

try {
    $fs = [System.IO.File]::Open($LogPath, 'Open', 'Read', 'Read')
    $fs.Close()
    Write-Fail 'read-only sharing was allowed - the driver is not holding the file, so this test proves nothing'
} catch {
    Write-Pass 'read-only sharing is refused, so the driver really is holding it open for writing'
}

# --- shut the sheet down ---------------------------------------------------
$script:t = [IntPtr]::Zero
$script:pid2 = [uint32]$p.Id
$cb = [L+EP]{ param($h,$l)
    $o = 0
    [void][L]::GetWindowThreadProcessId($h, [ref]$o)
    if ($o -ne $script:pid2) { return $true }
    if ((Get-ClassName $h) -eq '#32770') { $script:t = $h; return $false }
    return $true }
[void][L]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:t -ne [IntPtr]::Zero) {
    [void][L]::PostMessage($script:t, 0x0111, [IntPtr]2, [IntPtr]0)
}
if (-not $p.WaitForExit(8000)) { $p.Kill() }

if ($shared) {
    try {
        $null = [System.IO.File]::ReadAllText($LogPath)
        Write-Pass 'readable after the driver released it as well'
    } catch {
        Write-Fail "still unreadable after release: $($_.Exception.Message)"
    }
}

Write-Host ''
if ($script:failures -gt 0) {
    Write-Host "FAILED  ($($script:failures) problem(s))" -ForegroundColor Red
    exit 1
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
