<#
    Layout / control-tree check of the settings sheet, from the outside.

    Opens SettingsPreview.exe, walks the window tree of the dialog while it is
    on screen, and checks the things that used to be checked by a hook inside
    src\ORBAGDI_Settings.cpp:

      1. the tab control is a real SysTabControl32 and holds seven items
         (the 4-6 character titles are what has to fit, so seven is the count);
      2. all seven pages exist, each identified by a control id that only that
         page uses - not by enumeration order, which is not guaranteed;
      3. every page sits inside the tab control's rectangle;
      4. no control on any page reaches past that page's own client area.  This
         is the bug class behind "the last checkbox on the Debug page is
         clipped": a page template taller than the tab's display rectangle loses
         whatever hangs over the bottom edge and says nothing;
      5. the tab strip is wide enough for seven short labels;
      6. the Info page really holds the scrollable read-only text box, because
         every message the sheet raises has nowhere else to go;
      7. each tab title reads as the whole word.  Counting items and measuring
         the strip was not enough to catch this: the titles were inserted with
         TCM_INSERTITEMA (the TabCtrl_InsertItem macro picks its A/W form from
         the build's character set, and this is an ANSI build) while holding
         wchar_t text, so the control saw 'D' followed by the NUL of the UTF-16
         code unit and drew "D".  Seven items, a strip wide enough, every other
         check green - and the user still saw D / T / F / P / R / I / P.

    The Power page is the odd one out: it has no option key (nothing about the
    probe's supply is persisted) and only its Apply button touches the hardware.
    This script never presses anything but Cancel, so it cannot energise an
    output - which is exactly why the page can be checked here at all.

    Messages that take no pointer are sent directly (TCM_GETITEMCOUNT).  For the
    one that does need a pointer (TCM_GETITEMW), the buffer is allocated inside
    the target process with VirtualAllocEx, because a pointer from here would
    make the target read from an address that is not valid in its own space.

    Usage:  .\test\inspect_sheet.ps1
            .\test\inspect_sheet.ps1 -Exe <path> -Wait 6

    Exit code: 0 when every check passed.
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
public class W {
  public delegate bool EP(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT r, uint n);
  [DllImport("user32.dll")] public static extern long GetWindowLong(IntPtr h, int idx);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

  // To read a tab title back, the TCITEMW the control fills in has to live in
  // the target process - a pointer from this process is meaningless there.
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  // SIZE_T declared as uint: every size here is a few hundred bytes, and a
  // 32-bit value zero-extends correctly on a 64-bit host.
  [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, uint size, uint type, uint protect);
  [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, uint size, uint type);
  [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] buf, uint size, out uint done);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, uint size, out uint done);
}
'@

$DialogTitle = 'ORBTRACE Debugger Settings'
$GW_CHILD    = 5
$GW_HWNDNEXT = 2
$WM_COMMAND  = 0x0111
$IDCANCEL    = 2

$GWL_STYLE   = -16
$WS_VSCROLL  = 0x00200000
$ES_MULTILINE = 0x0004
$ES_READONLY  = 0x0800

$TCM_FIRST        = 0x1300
$TCM_GETITEMCOUNT = $TCM_FIRST + 4
$TCM_GETITEMW     = $TCM_FIRST + 60

# One control id per page that no other page uses - the anchor that says which
# page dialog is which.
$pages = @(
    @{ Name = 'Debug';  Marker = 1101; Kind = 'COMBOBOX' },
    @{ Name = 'Trace';  Marker = 1201; Kind = 'COMBOBOX' },
    @{ Name = 'Flash';  Marker = 1301; Kind = 'EDIT'     },
    @{ Name = 'Pack';   Marker = 1401; Kind = 'BUTTON'   },
    @{ Name = 'Record'; Marker = 1701; Kind = 'BUTTON'   },
    @{ Name = 'Power';  Marker = 1801; Kind = 'BUTTON'   },
    @{ Name = 'Info';   Marker = 1500; Kind = 'EDIT'     }
)

function Get-ClassName([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][W]::GetClassName($h, $s, 256)
    return $s.ToString()
}
function Get-Text([IntPtr]$h) {
    $s = New-Object System.Text.StringBuilder 256
    [void][W]::GetWindowText($h, $s, 256)
    return $s.ToString()
}
function Get-Children([IntPtr]$h) {
    $r = New-Object System.Collections.ArrayList
    $c = [W]::GetWindow($h, $GW_CHILD)
    while ($c -ne [IntPtr]::Zero) {
        [void]$r.Add($c)
        $c = [W]::GetWindow($c, $GW_HWNDNEXT)
    }
    return $r
}

# --- reading a tab title back ----------------------------------------------
$script:hProc  = [IntPtr]::Zero
$script:remote = [IntPtr]::Zero

function Start-RemoteBuffer([uint32]$targetPid) {
    # PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
    $script:hProc = [W]::OpenProcess(0x0038, $false, $targetPid)
    if ($script:hProc -eq [IntPtr]::Zero) { return $false }
    # MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    $script:remote = [W]::VirtualAllocEx($script:hProc, [IntPtr]::Zero, [uint32]512, 0x3000, 0x04)
    return ($script:remote -ne [IntPtr]::Zero)
}
function Stop-RemoteBuffer {
    if ($script:remote -ne [IntPtr]::Zero) {
        [void][W]::VirtualFreeEx($script:hProc, $script:remote, [uint32]0, 0x8000)
        $script:remote = [IntPtr]::Zero
    }
    if ($script:hProc -ne [IntPtr]::Zero) {
        [void][W]::CloseHandle($script:hProc)
        $script:hProc = [IntPtr]::Zero
    }
}

# TCITEMW, 32-bit layout: mask, dwState, dwStateMask, pszText, cchTextMax,
# iImage, lParam.  The text buffer sits 64 bytes in and every pointer is four
# bytes, because the preview is a 32-bit process.
$TCIF_TEXT = 0x0001
$TEXT_OFF  = 64
$TEXT_CCH  = 128

function Get-TabTitle([IntPtr]$tab, [int]$index) {
    $buf = New-Object byte[] 512
    [BitConverter]::GetBytes([uint32]$TCIF_TEXT).CopyTo($buf, 0)
    [BitConverter]::GetBytes([uint32]0).CopyTo($buf, 4)
    [BitConverter]::GetBytes([uint32]0).CopyTo($buf, 8)
    [BitConverter]::GetBytes([int]($script:remote.ToInt64() + $TEXT_OFF)).CopyTo($buf, 12)
    [BitConverter]::GetBytes([int]$TEXT_CCH).CopyTo($buf, 16)
    [BitConverter]::GetBytes([int]0).CopyTo($buf, 20)
    [BitConverter]::GetBytes([int]0).CopyTo($buf, 24)

    $done = [uint32]0
    if (-not [W]::WriteProcessMemory($script:hProc, $script:remote, $buf,
                                     [uint32]512, [ref]$done)) { return $null }

    [void][W]::SendMessageW($tab, $TCM_GETITEMW, [IntPtr]$index, $script:remote)

    $text = New-Object byte[] ($TEXT_CCH * 2)
    if (-not [W]::ReadProcessMemory($script:hProc,
                [IntPtr]($script:remote.ToInt64() + $TEXT_OFF), $text,
                [uint32]($TEXT_CCH * 2), [ref]$done)) { return $null }

    $s = [System.Text.Encoding]::Unicode.GetString($text)
    $z = $s.IndexOf([char]0)
    if ($z -ge 0) { $s = $s.Substring(0, $z) }
    return $s
}
# ---------------------------------------------------------------------------
$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds $Wait
if ($p.HasExited) { Write-Fail "the process exited early (0x$('{0:X8}' -f $p.ExitCode))"; exit 1 }

$script:target = [IntPtr]::Zero
$script:pid2   = [uint32]$p.Id
$cb = [W+EP]{
    param($h, $l)
    $owner = 0
    [void][W]::GetWindowThreadProcessId($h, [ref]$owner)
    if ($owner -ne $script:pid2) { return $true }
    if ((Get-Text $h) -eq $DialogTitle) { $script:target = $h; return $false }
    return $true
}
[void][W]::EnumWindows($cb, [IntPtr]::Zero)

if ($script:target -eq [IntPtr]::Zero) {
    Write-Fail 'the settings dialog was not found'
    $p.Kill(); exit 1
}
Write-Host "    dialog: '$DialogTitle' found" -ForegroundColor DarkGray

$dlg = $script:target

# The window is enumerable before WM_INITDIALOG has finished: the pages are
# created one by one and the controls on them are filled after a probe of the
# RDDI layer.  Checking the tree in that window reports pages and controls that
# simply are not there yet.  A page becomes visible only at the end of
# WM_INITDIALOG, so wait for that.
$script:markers = @(1101, 1201, 1301, 1401, 1701, 1801, 1500)
$deadline = (Get-Date).AddMilliseconds(25000)
while ((Get-Date) -lt $deadline) {
    $ready = $false
    foreach ($k in (Get-Children $dlg)) {
        if ((Get-ClassName $k) -ne '#32770') { continue }
        foreach ($m in $script:markers) {
            if ([W]::GetDlgItem($k, $m) -ne [IntPtr]::Zero) {
                if ([W]::IsWindowVisible($k)) { $ready = $true }
                break
            }
        }
        if ($ready) { break }
    }
    if ($ready) { break }
    Start-Sleep -Milliseconds 150
}

$kids = Get-Children $dlg

# --- the tab control -------------------------------------------------------
$tab = [IntPtr]::Zero
foreach ($k in $kids) {
    if ((Get-ClassName $k) -eq 'SysTabControl32') { $tab = $k; break }
}
if ($tab -eq [IntPtr]::Zero) {
    Write-Fail 'no SysTabControl32 on the sheet - the page selector is missing'
} else {
    Write-Pass 'the page selector is a real SysTabControl32'

    $items = [int][W]::SendMessage($tab, $TCM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($items -eq 7) { Write-Pass 'the tab control holds seven items' }
    else              { Write-Fail "the tab control holds $items items, not 7" }

    $tr = New-Object W+RECT
    [void][W]::GetClientRect($tab, [ref]$tr)
    $tabW = $tr.Right - $tr.Left
    # Seven 4-6 character labels at 8pt need roughly 34 px each; if the strip is
    # narrower than that the titles cannot all be readable.
    if ($tabW -ge (7 * 34)) { Write-Pass "the tab strip is $tabW px wide - room for seven labels" }
    else                    { Write-Fail "the tab strip is only $tabW px wide - seven labels will not fit" }

    # --- and what the titles actually say ----------------------------------
    # A tab control sizes each tab to its own text, so "D" produced six narrow
    # tabs that still passed both checks above.  Read the text.
    if (-not (Start-RemoteBuffer $script:pid2)) {
        Write-Fail 'could not open the target process to read the tab titles back'
    } else {
        $expected = @('Debug', 'Trace', 'Flash', 'Pack', 'Record', 'Power', 'Info')
        $got = @()
        for ($i = 0; $i -lt 7; $i++) { $got += (Get-TabTitle $tab $i) }
        Stop-RemoteBuffer

        $bad = @()
        for ($i = 0; $i -lt 7; $i++) {
            if ($got[$i] -ne $expected[$i]) {
                $bad += ("[{0}] '{1}' not '{2}'" -f $i, $got[$i], $expected[$i])
            }
        }
        if ($bad.Count -eq 0) {
            Write-Pass ("the tab titles read '{0}'" -f ($got -join "' / '"))
        } else {
            Write-Fail ("the tab titles are wrong: " + ($bad -join '; '))
        }
    }
}

# --- the pages -------------------------------------------------------------
foreach ($want in $pages) {
    $page = [IntPtr]::Zero
    foreach ($k in $kids) {
        if ((Get-ClassName $k) -ne '#32770') { continue }
        if ([W]::GetDlgItem($k, $want.Marker) -ne [IntPtr]::Zero) { $page = $k; break }
    }

    if ($page -eq [IntPtr]::Zero) {
        Write-Fail "the '$($want.Name)' page was not created (no control id $($want.Marker))"
        continue
    }

    $pr = New-Object W+RECT
    [void][W]::GetClientRect($page, [ref]$pr)
    $pw = $pr.Right - $pr.Left
    $ph = $pr.Bottom - $pr.Top

    # The page must sit inside the tab control, or part of it is hidden behind
    # the strip's frame.
    if ($tab -ne [IntPtr]::Zero) {
        $wr = New-Object W+RECT
        [void][W]::GetWindowRect($page, [ref]$wr)
        $tt = New-Object W+RECT
        [void][W]::GetWindowRect($tab, [ref]$tt)
        if ($wr.Right -le $tt.Right -and $wr.Bottom -le $tt.Bottom -and
            $wr.Left -ge $tt.Left  -and $wr.Top -ge $tt.Top) {
            Write-Pass "'$($want.Name)' page is inside the tab control"
        } else {
            Write-Fail "'$($want.Name)' page hangs outside the tab control"
        }
    }

    # Every control has to fit inside the page.
    $clipped = @()
    $kidsOfPage = Get-Children $page
    foreach ($c in $kidsOfPage) {
        $r = New-Object W+RECT
        [void][W]::GetWindowRect($c, [ref]$r)
        [void][W]::MapWindowPoints([IntPtr]::Zero, $page, [ref]$r, 2)
        if ($r.Right -gt $pr.Right -or $r.Bottom -gt $pr.Bottom -or
            $r.Left -lt 0 -or $r.Top -lt 0) {
            $t = Get-Text $c
            if ($t.Length -gt 28) { $t = $t.Substring(0, 28) + '...' }
            $clipped += ("'{0}' (id {1}) at {2},{3}..{4},{5}" -f $t, ([W]::GetDlgCtrlID($c)),
                         $r.Left, $r.Top, $r.Right, $r.Bottom)
        }
    }

    if ($clipped.Count -eq 0) {
        Write-Pass "'$($want.Name)' page: $($kidsOfPage.Count) controls, none clipped (${pw}x${ph} px)"
    } else {
        Write-Fail "'$($want.Name)' page: $($clipped.Count) of $($kidsOfPage.Count) controls are clipped"
        $clipped | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkYellow }
    }

    # The Info page is the one place messages go, so its box has to be there and
    # has to be readable.
    if ($want.Name -eq 'Info') {
        $box = [W]::GetDlgItem($page, 1500)
        $style = [int][W]::GetWindowLong($box, $GWL_STYLE)
        $multiline = ($style -band $ES_MULTILINE) -ne 0
        $readonly  = ($style -band $ES_READONLY)  -ne 0
        $vscroll   = ($style -band $WS_VSCROLL)   -ne 0
        if ($multiline -and $readonly -and $vscroll) {
            Write-Pass 'the Info page holds a scrollable read-only text box'
        } else {
            Write-Fail ("the Info box is not right: multiline={0} readonly={1} vscroll={2}" -f `
                        $multiline, $readonly, $vscroll)
        }
    }

    if ($Dump) {
        Write-Host "      -- $($want.Name) --" -ForegroundColor DarkGray
        foreach ($c in $kidsOfPage) {
            $r = New-Object W+RECT
            [void][W]::GetWindowRect($c, [ref]$r)
            [void][W]::MapWindowPoints([IntPtr]::Zero, $page, [ref]$r, 2)
            $t = Get-Text $c
            if ($t.Length -gt 30) { $t = $t.Substring(0, 30) + '...' }
            Write-Host ("      {0,-16} id={1,-5} {2,4},{3,-4} {4,4}x{5,-4} '{6}'" -f `
                        (Get-ClassName $c), ([W]::GetDlgCtrlID($c)),
                        $r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top), $t) `
                        -ForegroundColor DarkGray
        }
    }
}

# ---------------------------------------------------------------------------
[void][W]::PostMessage($dlg, $WM_COMMAND, [IntPtr]$IDCANCEL, [IntPtr]0)
if (-not $p.WaitForExit(5000)) { $p.Kill() }

Write-Host ''
if ($script:failures -gt 0) {
    Write-Host "FAILED  ($script:failures problem(s))" -ForegroundColor Red
    exit 1
}
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
