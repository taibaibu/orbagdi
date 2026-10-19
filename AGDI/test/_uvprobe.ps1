# Temporary diagnostic: read uVision's real command state (menu grey/enabled).
$ErrorActionPreference = 'Continue'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class M {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr h, uint id, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr h, int pos);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr h, uint id, StringBuilder s, int n, uint flags);

  const uint BYPOS = 0x400;

  public static IntPtr VisibleWindow(uint targetPid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == targetPid && IsWindowVisible(h)) {
        var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
        if (t.Length > 0) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  // Per top-level submenu: how many items, how many are grey/disabled.
  public static List<string> Report(IntPtr hwnd) {
    var res = new List<string>();
    IntPtr bar = GetMenu(hwnd);
    if (bar == IntPtr.Zero) { res.Add("  (GetMenu returned NULL)"); return res; }
    int n = GetMenuItemCount(bar);
    res.Add("  menu bar submenus = " + n);
    for (int i = 0; i < n; ++i) {
      var top = new StringBuilder(128);
      GetMenuStringW(bar, (uint)i, top, 128, BYPOS);
      IntPtr sub = GetSubMenu(bar, i);
      if (sub == IntPtr.Zero) { res.Add("  [" + i + "] " + top + " (no submenu)"); continue; }
      int m = GetMenuItemCount(sub);
      int grey = 0, dis = 0;
      var names = new StringBuilder();
      for (int j = 0; j < m; ++j) {
        uint st = GetMenuState(sub, (uint)j, BYPOS);
        if (st == 0xFFFFFFFF) continue;
        bool g = (st & 0x1) != 0;   // MF_GRAYED
        bool d = (st & 0x2) != 0;   // MF_DISABLED
        if (g) ++grey;
        if (d) ++dis;
        if ((g || d) && names.Length < 200) {
          var s = new StringBuilder(128);
          GetMenuStringW(sub, (uint)j, s, 128, BYPOS);
          names.Append(s.ToString().Replace("&", "")).Append(" | ");
        }
      }
      res.Add(string.Format("  [{0}] {1}: items={2} greyed={3} disabled={4}  <<{5}>>",
                            i, top.ToString(), m, grey, dis, names.ToString()));
    }
    return res;
  }
}
'@

$p = Get-Process -Id 19620 -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "pid not running"; exit }
Write-Output ("responding = " + $p.Responding)
$h = [M]::VisibleWindow([uint32]19620)
Write-Output ("visible main hwnd = 0x{0:X}" -f $h.ToInt64())
if ($h -ne [IntPtr]::Zero) {
    [M]::Report($h) | ForEach-Object { Write-Output $_ }
}
