# Diagnose why the RDDI host cannot find the orbtrace trace interface.
#
# The host's ScanForOrbTrace() (ORBMDK_OrbTrace.cpp) does exactly this:
#   1. SetupDiGetClassDevs(&WinUsbGuid, ..., DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
#   2. for each interface path: parse vid/pid, keep ORBTrace 1209:3443 / 3442
#   3. CreateFileW + WinUsb_Initialize
#   4. WinUsb_QueryInterfaceSettings(alt 0) -> require class 0xFF and
#      subclass 'T' (0x54) for the trace interface
#
# This walks the same four steps and prints where each candidate stops, so
# "no orbtrace device found" turns into a specific step.
#
# Usage:  powershell -ExecutionPolicy Bypass -File test\probe_orbtrace_usb.ps1
$ErrorActionPreference = 'Continue'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class U {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA {
        public int    cbSize;
        public Guid   InterfaceClassGuid;
        public int    Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct USB_INTERFACE_DESCRIPTOR {
        public byte bLength;
        public byte bDescriptorType;
        public byte bInterfaceNumber;
        public byte bAlternateSetting;
        public byte bNumEndpoints;
        public byte bInterfaceClass;
        public byte bInterfaceSubClass;
        public byte bInterfaceProtocol;
        public byte iInterface;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr enumerator,
                                                    IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr h, IntPtr devInfo,
                                                          ref Guid g, uint index,
                                                          ref SP_DEVICE_INTERFACE_DATA d);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr h,
                                                              ref SP_DEVICE_INTERFACE_DATA d,
                                                              IntPtr detail, int size,
                                                              ref int needed, IntPtr info);
    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr h);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string path, uint access, uint share,
                                            IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);

    [DllImport("winusb.dll", SetLastError = true)]
    public static extern bool WinUsb_Initialize(IntPtr file, out IntPtr wu);
    [DllImport("winusb.dll", SetLastError = true)]
    public static extern bool WinUsb_QueryInterfaceSettings(IntPtr wu, byte alt,
                                                            out USB_INTERFACE_DESCRIPTOR d);
    [DllImport("winusb.dll", SetLastError = true)]
    public static extern bool WinUsb_Free(IntPtr wu);
}
'@

$WinUsbGuid = [Guid]'DEE824EF-729B-4A0E-9C14-B7117D33A817'
$DIGCF_PRESENT         = 0x2
$DIGCF_DEVICEINTERFACE = 0x10

Write-Host "WinUSB interface class GUID: $WinUsbGuid"
Write-Host ("process bitness: {0}" -f $(if ([IntPtr]::Size -eq 8) { '64-bit' } else { '32-bit' }))
Write-Host ''

$h = [U]::SetupDiGetClassDevs([ref]$WinUsbGuid, [IntPtr]::Zero, [IntPtr]::Zero,
                              ($DIGCF_PRESENT -bor $DIGCF_DEVICEINTERFACE))
if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr](-1)) {
    Write-Host ("SetupDiGetClassDevs FAILED, GetLastError={0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    exit 1
}

$candidates = 0
$orbtrace   = 0
$index = 0
while ($true) {
    $did = New-Object U+SP_DEVICE_INTERFACE_DATA
    $did.cbSize = [Runtime.InteropServices.Marshal]::SizeOf([type]'U+SP_DEVICE_INTERFACE_DATA')
    if (-not [U]::SetupDiEnumDeviceInterfaces($h, [IntPtr]::Zero, [ref]$WinUsbGuid, $index, [ref]$did)) { break }
    $index++

    $need = 0
    [void][U]::SetupDiGetDeviceInterfaceDetail($h, [ref]$did, [IntPtr]::Zero, 0, [ref]$need, [IntPtr]::Zero)
    if ($need -le 0) { continue }

    $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal($need)
    # SP_DEVICE_INTERFACE_DETAIL_DATA_W: DWORD cbSize then WCHAR[1]; the struct
    # is padded to 8 on 64-bit, 6 on 32-bit - the value the API expects.
    $cbSize = if ([IntPtr]::Size -eq 8) { 8 } else { 6 }
    [Runtime.InteropServices.Marshal]::WriteInt32($buf, $cbSize)

    $ok = [U]::SetupDiGetDeviceInterfaceDetail($h, [ref]$did, $buf, $need, [ref]$need, [IntPtr]::Zero)
    if (-not $ok) {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
        continue
    }
    $pathPtr = [IntPtr]::new($buf.ToInt64() + $cbSize)
    $path = [Runtime.InteropServices.Marshal]::PtrToStringUni($pathPtr)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)

    $candidates++
    if ($path -notmatch 'vid_1209&pid_(3443|3442)') { continue }

    $orbtrace++
    Write-Host "--- ORBTrace interface ---"
    Write-Host "  path: $path"

    # step 3: CreateFileW + WinUsb_Initialize
    $file = [U]::CreateFileW($path, 0xC0000000, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($file -eq [IntPtr](-1)) {
        Write-Host ("  CreateFileW FAILED, GetLastError={0}  <- step 3" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        continue
    }
    $wu = [IntPtr]::Zero
    if (-not [U]::WinUsb_Initialize($file, [ref]$wu)) {
        Write-Host ("  WinUsb_Initialize FAILED, GetLastError={0}  <- step 3" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        [void][U]::CloseHandle($file)
        continue
    }

    # step 4: the interface descriptor decides trace / power / version
    $fh = $file.ToInt64()
    $wh = $wu.ToInt64()
    Write-Host ("  file=0x{0:X} winusb=0x{1:X}" -f $fh, $wh)

    $ifd = New-Object U+USB_INTERFACE_DESCRIPTOR
    if (-not [U]::WinUsb_QueryInterfaceSettings($wu, 0, [ref]$ifd)) {
        Write-Host ("  WinUsb_QueryInterfaceSettings FAILED, GetLastError={0}  <- step 4" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    } else {
        $sub = [char]$ifd.bInterfaceSubClass
        $what = switch ($ifd.bInterfaceSubClass) {
            0x54 { "TRACE" }
            0x50 { "power" }
            0x56 { "version" }
            default { "?" }
        }
        Write-Host ("  ifaceNumber={0} class=0x{1:X2} subclass=0x{2:X2}('{3}') protocol=0x{4:X2} -> {5}" -f `
                    $ifd.bInterfaceNumber, $ifd.bInterfaceClass, $ifd.bInterfaceSubClass, $sub,
                    $ifd.bInterfaceProtocol, $what)
        if ($ifd.bInterfaceClass -ne 0xFF) {
            Write-Host "  NOTE: class is not 0xFF - the host skips it (isVendor test)"
        }
        if ($ifd.bInterfaceSubClass -eq 0x54 -and $ifd.bInterfaceProtocol -ne 0x10) {
            Write-Host "  NOTE: trace protocol is not 0x10 (OFLOW v1.0) - the host would use raw bytes"
        }
    }

    [void][U]::WinUsb_Free($wu)
    [void][U]::CloseHandle($file)
}

[void][U]::SetupDiDestroyDeviceInfoList($h)

Write-Host ''
Write-Host ("WinUSB candidates enumerated : {0}" -f $candidates)
Write-Host ("ORBTrace (1209:344x) seen    : {0}" -f $orbtrace)
if ($candidates -eq 0) {
    Write-Host 'The WinUSB interface class is empty: no interface is registered under this GUID,'
    Write-Host 'so the host cannot see any of them. That is a driver-binding problem, not a code one.'
} elseif ($orbtrace -eq 0) {
    Write-Host 'WinUSB interfaces exist but none is an ORBTrace 1209:3443/3442.'
} else {
    Write-Host 'The host should have found the interfaces above; compare with its log line'
    Write-Host '("found trace iface N" vs "no orbtrace device found").'
}
