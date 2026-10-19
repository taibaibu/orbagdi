<#
    Prints the text of the settings sheet's Info page.

    The Info page is where the sheet reports everything: what uVision told it,
    where it looked for the RDDI host and the packs, every message it raised, and
    the tail of %TEMP%\ORB_AGDI.log.  Reading it from a script is the fastest way
    to answer "what does the driver say when it is opened with these options",
    without a screenshot and without leaving the console.

    The box is a real edit control, so its content is fetched with WM_GETTEXT.
    GetWindowText will not do: across processes it only reads a window's caption,
    and an edit control's caption is not its text - the first attempt at this
    returned an empty string for exactly that reason.

    Usage:
        .\test\show_info.ps1                              # the sample string
        .\test\show_info.ps1 -ExeArgs 'port=1 clock=5000'
        .\test\show_info.ps1 -Section Log                 # just the log tail
        .\test\show_info.ps1 -Raw                         # no decoration

    -Section takes one of: Target, Where, Messages, Log, All (default All).

    Exit code: 0 when the Info page was read, 1 when the sheet did not come up.
#>
param(
    [string]$Exe,
    [string]$ExeArgs = '',
    [string]$Section = 'All',
    [switch]$Raw,
    [int]$Wait = 6
)

$ErrorActionPreference = 'Stop'

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\SettingsPreview.exe' }
if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first (test\build.ps1)" }

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class I {
  public delegate bool EP(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")]
  public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
}
'@

$GW_CHILD    = 5
$GW_HWNDNEXT = 2
$WM_COMMAND  = 0x0111
$WM_GETTEXT  = 0x000D
$IDCANCEL    = 2
$kIdEditInfo = 1500
$markers     = @(1101, 1201, 1301, 1401, 1801, 1500)

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][I]::GetClassName($h, $s, 256)
    return $s.ToString()
}

# ---------------------------------------------------------------------------
$p = if ($ExeArgs) { Start-Process -FilePath $Exe -ArgumentList ('"' + $ExeArgs + '"') -PassThru }
     else           { Start-Process -FilePath $Exe -PassThru }
Start-Sleep -Seconds $Wait
if ($p.HasExited) { throw "the preview exited before the sheet appeared (0x$('{0:X8}' -f $p.ExitCode))" }

$script:target = [IntPtr]::Zero
$script:pid2   = [uint32]$p.Id
$cb = [I+EP]{
    param($h, $l)
    $owner = 0
    [void][I]::GetWindowThreadProcessId($h, [ref]$owner)
    if ($owner -ne $script:pid2) { return $true }
    if ((Get-ClassName $h) -eq '#32770') { $script:target = $h; return $false }
    return $true
}
[void][I]::EnumWindows($cb, [IntPtr]::Zero)
$dlg = $script:target
if ($dlg -eq [IntPtr]::Zero) { $p.Kill(); throw 'the settings dialog was not found' }

# The Info box is filled by RefreshInfo at the end of WM_INITDIALOG; reading it
# before that returns whatever was there a moment ago, which is nothing.
$deadline = (Get-Date).AddMilliseconds(25000)
$box = [IntPtr]::Zero
while ($box -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    $c = [I]::GetWindow($dlg, $GW_CHILD)
    while ($c -ne [IntPtr]::Zero) {
        if ((Get-ClassName $c) -eq '#32770') {
            $b = [I]::GetDlgItem($c, $kIdEditInfo)
            if ($b -ne [IntPtr]::Zero) { $box = $b; break }
        }
        $c = [I]::GetWindow($c, $GW_HWNDNEXT)
    }
    if ($box -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 150 }
}

$text = ''
if ($box -ne [IntPtr]::Zero) {
    $sb = New-Object System.Text.StringBuilder 65536
    [void][I]::SendMessage($box, $WM_GETTEXT, [IntPtr]65536, $sb)
    $text = $sb.ToString()
}

[void][I]::PostMessage($dlg, $WM_COMMAND, [IntPtr]$IDCANCEL, [IntPtr]0)
if (-not $p.WaitForExit(5000)) { $p.Kill() }

if ($box -eq [IntPtr]::Zero) { Write-Host 'the Info box was not found'; exit 1 }

# --- slice it up -----------------------------------------------------------
# The page is a handful of "== Heading ==" sections; -Section picks one.
$lines = $text -split "`r`n"
if ($Section -eq 'All') {
    if ($Raw) { $text.TrimEnd() } else { Write-Host $text.TrimEnd() }
    exit 0
}

$want = switch ($Section) {
    'Target'   { 'Target' }
    'Where'    { 'Where this driver looks' }
    'Messages' { 'Messages' }
    'Log'      { 'Log tail' }
    default    { throw "-Section must be one of: Target, Where, Messages, Log, All" }
}

$inside = $false
$out = @()
foreach ($ln in $lines) {
    if ($ln -match '^==\s*(.+?)\s*==\s*$') {
        $inside = ($Matches[1] -like "*$want*")
        continue
    }
    if ($inside) { $out += $ln }
}
if ($out.Count -eq 0) { Write-Host "(no '$Section' section on the Info page)"; exit 0 }
if ($Raw) { ($out -join "`n").TrimEnd() } else { Write-Host (($out -join "`n").TrimEnd()) }
exit 0
