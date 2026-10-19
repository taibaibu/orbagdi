# Temporary diagnostic: reproduce uVision's Target-DLL "checker entry" from a
# 32-bit host. Must be run by SysWOW64\WindowsPowerShell (32-bit).
$ErrorActionPreference = 'Continue'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class P {
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate int CheckerFn(IntPtr p, uint nCode);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate int CapFn(uint nCode, IntPtr p);
  [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr LoadLibraryW(string p);
  [DllImport("kernel32", SetLastError=true)]
  public static extern IntPtr GetProcAddress(IntPtr h, string n);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool FreeLibrary(IntPtr h);
}
'@

Write-Output ("is64bit-process = " + [Environment]::Is64BitProcess)

$sweeps = @{}

$targets = @(
    @{ Path = 'D:\Keil_v5\ARM\BIN\ORB_AGDI.dll';   Safe = $true  },
    @{ Path = 'D:\Keil_v5\ARM\BIN\CMSIS_AGDI.dll'; Safe = $false }
)

foreach ($t in $targets) {
    $d = $t.Path
    Write-Output ("--- " + $d)
    $h = [P]::LoadLibraryW($d)
    if ($h -eq [IntPtr]::Zero) {
        $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Output ("    LOAD FAILED  GetLastError=" + $e)
        continue
    }
    Write-Output "    LOAD OK"

    foreach ($name in @('EnumUvARM7', 'DllUv3Cap', 'AG_Init', 'AG_GoStep', 'AG_MemAccX')) {
        $a = [P]::GetProcAddress($h, $name)
        if ($a -eq [IntPtr]::Zero) {
            Write-Output ("    {0,-12} MISSING" -f $name)
        } else {
            Write-Output ("    {0,-12} 0x{1:X}" -f $name, $a.ToInt64())
        }
    }

    $pEu = [P]::GetProcAddress($h, 'EnumUvARM7')
    if ($pEu -ne [IntPtr]::Zero) {
        $eu = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pEu, [P+CheckerFn])
        Write-Output ("    EnumUvARM7(NULL,0) = " + $eu.Invoke([IntPtr]::Zero, 0))
        Write-Output ("    EnumUvARM7(NULL,1) = " + $eu.Invoke([IntPtr]::Zero, 1))
        if ($t.Safe) {
            Write-Output ("    EnumUvARM7(NULL,2) = " + $eu.Invoke([IntPtr]::Zero, 2))
        }
    }
    $pCap = [P]::GetProcAddress($h, 'DllUv3Cap')
    if ($pCap -ne [IntPtr]::Zero) {
        $cap = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pCap, [P+CapFn])
        Write-Output ("    DllUv3Cap(2,  NULL) = " + $cap.Invoke(2, [IntPtr]::Zero))
        Write-Output ("    DllUv3Cap(100,NULL) = " + $cap.Invoke(100, [IntPtr]::Zero))

        # Sweep only the safe target: Keil's own CMSIS_AGDI.dll dereferences p
        # for some nCode and faults at NULL (AccessViolation, not catchable in
        # .NET 4+, which killed the first run of this script).
        if ($t.Safe) {
            $sweep = @{}
            for ($n = 0; $n -le 130; ++$n) {
                try { $sweep[$n] = $cap.Invoke([uint32]$n, [IntPtr]::Zero) }
                catch { $sweep[$n] = -1 }
            }
            $sweeps[$d] = $sweep
        }
    }
    [P]::FreeLibrary($h) | Out-Null
}

# --- report every non-zero DllUv3Cap answer from the swept DLL ---
foreach ($d in @($sweeps.Keys)) {
    Write-Output ""
    Write-Output ("=== DllUv3Cap non-zero answers: " + (Split-Path $d -Leaf) + " ===")
    for ($n = 0; $n -le 130; ++$n) {
        if ($sweeps[$d][$n] -ne 0) {
            Write-Output ("  nCode={0,-4} = {1}" -f $n, $sweeps[$d][$n])
        }
    }
}
