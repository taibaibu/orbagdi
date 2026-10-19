<#
    Screenshot of the settings sheet - the "look at it" tool.

    Opens SettingsPreview.exe, optionally selects a page, captures the dialog with
    PrintWindow and writes a PNG.  Nothing is asserted here: the pass/fail checks
    live in e2e_sheet.ps1 and inspect_sheet.ps1.  This one exists because some
    things are only visible to an eye - a greyed control, a clipped label, a tab
    that reads "D" - and because a picture is the fastest way to show someone else.

    Selecting a page uses no screen coordinates.  TCM_GETITEMRECT reports each
    tab's rectangle in the control's own client space, so the click is posted
    straight to the tab control with the coordinates it already expects.  The
    mouse is never moved and the window never has to be foreground, which is what
    made the first version of this script need a hand-tuned "click at 196,42".

    The dialog is captured with PW_RENDERFULLCONTENT so themed parts (the tab
    strip) come out drawn rather than blank.

    Usage:
        .\test\shot_sheet.ps1                       # Debug page
        .\test\shot_sheet.ps1 -Page Info            # any of the seven
        .\test\shot_sheet.ps1 -Page Trace -Out trace.png
        .\test\shot_sheet.ps1 -ExeArgs 'port=1'     # drive the sheet with options

    Exit code: 0 when the PNG was written, 1 when the sheet did not come up.
#>
param(
    [string]$Exe,
    [string]$Out,
    [string]$Page = 'Debug',
    [string]$ExeArgs = '',
    [int]$Wait = 6
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\SettingsPreview.exe' }
if (-not $Out) { $Out = Join-Path $env:TEMP 'sheet.png' }
if (-not (Test-Path $Exe)) { throw "missing $Exe - build it first (test\build.ps1)" }

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class S {
  public delegate bool EP(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

  // TCM_GETITEMRECT takes a pointer, so the RECT has to live in the target
  // process - see inspect_sheet.ps1 for the same pattern.
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, uint size, uint type, uint protect);
  [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, uint size, uint type);
  [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] buf, uint size, out uint done);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, uint size, out uint done);
}
'@

$GW_CHILD    = 5
$GW_HWNDNEXT = 2
$WM_COMMAND  = 0x0111
$IDCANCEL    = 2
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP   = 0x0202
$MK_LBUTTON     = 0x0001
$TCM_GETITEMRECT = 0x1300 + 10

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][S]::GetClassName($h, $s, 256)
    return $s.ToString()
}

$Pages = @('Debug', 'Trace', 'Flash', 'Pack', 'Record', 'Power', 'Info')
$pageIndex = [array]::IndexOf($Pages, $Page)
if ($pageIndex -lt 0) { throw "-Page must be one of: $($Pages -join ', ')" }

# ---------------------------------------------------------------------------
$p = if ($ExeArgs) { Start-Process -FilePath $Exe -ArgumentList ('"' + $ExeArgs + '"') -PassThru }
     else           { Start-Process -FilePath $Exe -PassThru }
Start-Sleep -Seconds $Wait
if ($p.HasExited) { throw "the preview exited before the sheet appeared (0x$('{0:X8}' -f $p.ExitCode))" }

$script:target = [IntPtr]::Zero
$script:pid2   = [uint32]$p.Id
$cb = [S+EP]{
    param($h, $l)
    $owner = 0
    [void][S]::GetWindowThreadProcessId($h, [ref]$owner)
    if ($owner -ne $script:pid2) { return $true }
    if ((Get-ClassName $h) -eq '#32770') { $script:target = $h; return $false }
    return $true
}
[void][S]::EnumWindows($cb, [IntPtr]::Zero)
$dlg = $script:target
if ($dlg -eq [IntPtr]::Zero) { $p.Kill(); throw 'the settings dialog was not found' }

# The dialog is enumerable before WM_INITDIALOG has finished filling it; a page
# becomes visible only at the end.  Capturing before that photographs a
# half-built sheet.
$script:markers = @(1101, 1201, 1301, 1401, 1801, 1500)
$deadline = (Get-Date).AddMilliseconds(25000)
$ready = $false
while (-not $ready -and (Get-Date) -lt $deadline) {
    $c = [S]::GetWindow($dlg, $GW_CHILD)
    while ($c -ne [IntPtr]::Zero) {
        if ((Get-ClassName $c) -eq '#32770') {
            foreach ($m in $script:markers) {
                if ([S]::GetDlgItem($c, $m) -ne [IntPtr]::Zero) {
                    if ([S]::IsWindowVisible($c)) { $ready = $true }
                    break
                }
            }
        }
        if ($ready) { break }
        $c = [S]::GetWindow($c, $GW_HWNDNEXT)
    }
    if (-not $ready) { Start-Sleep -Milliseconds 150 }
}

# --- select the page -------------------------------------------------------
$tab = [IntPtr]::Zero
$c = [S]::GetWindow($dlg, $GW_CHILD)
while ($c -ne [IntPtr]::Zero) {
    if ((Get-ClassName $c) -eq 'SysTabControl32') { $tab = $c; break }
    $c = [S]::GetWindow($c, $GW_HWNDNEXT)
}

if ($pageIndex -gt 0) {
    if ($tab -eq [IntPtr]::Zero) { throw 'no SysTabControl32 - cannot select a page' }

    $hProc  = [S]::OpenProcess(0x0038, $false, $script:pid2)
    $remote = [S]::VirtualAllocEx($hProc, [IntPtr]::Zero, [uint32]64, 0x3000, 0x04)
    if ($hProc -eq [IntPtr]::Zero -or $remote -eq [IntPtr]::Zero) {
        throw 'could not allocate a RECT inside the target process'
    }

    $zero = New-Object byte[] 16
    $done = [uint32]0
    [void][S]::WriteProcessMemory($hProc, $remote, $zero, [uint32]16, [ref]$done)
    [void][S]::SendMessageW($tab, $TCM_GETITEMRECT, [IntPtr]$pageIndex, $remote)

    $raw = New-Object byte[] 16
    [void][S]::ReadProcessMemory($hProc, $remote, $raw, [uint32]16, [ref]$done)
    [void][S]::VirtualFreeEx($hProc, $remote, [uint32]0, 0x8000)
    [void][S]::CloseHandle($hProc)

    $left   = [BitConverter]::ToInt32($raw, 0)
    $top    = [BitConverter]::ToInt32($raw, 4)
    $right  = [BitConverter]::ToInt32($raw, 8)
    $bottom = [BitConverter]::ToInt32($raw, 12)

    if ($right -le $left -or $bottom -le $top) { throw "the '$Page' tab has no rectangle" }

    $x = [int](($left + $right) / 2)
    $y = [int](($top + $bottom) / 2)
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    Write-Host "  selecting '$Page' (tab $pageIndex) at $x,$y" -ForegroundColor DarkGray

    [void][S]::PostMessage($tab, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
    [void][S]::PostMessage($tab, $WM_LBUTTONUP,   [IntPtr]0,          $lp)
    Start-Sleep -Milliseconds 700
}

# --- capture ---------------------------------------------------------------
$r = New-Object S+RECT
[void][S]::GetWindowRect($dlg, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$dc  = $g.GetHdc()
[void][S]::PrintWindow($dlg, $dc, 2)     # PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Host ("  {0}  ({1}x{2})" -f $Out, $w, $h) -ForegroundColor Green

[void][S]::PostMessage($dlg, $WM_COMMAND, [IntPtr]$IDCANCEL, [IntPtr]0)
if (-not $p.WaitForExit(5000)) { $p.Kill() }
exit 0
