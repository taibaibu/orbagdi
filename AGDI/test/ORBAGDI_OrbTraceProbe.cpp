/**
 * @file ORBAGDI_OrbTraceProbe.cpp
 * @brief Walk the exact steps the RDDI host's orbtrace scan walks, and print
 *        where each interface stops.
 *
 * The host cannot find the probe's trace interface - its log says only
 * "no orbtrace device found", with no hint which of the four steps failed:
 *
 *   1. SetupDiGetClassDevs(&WinUsbGuid, ..., DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
 *   2. SetupDiEnumDeviceInterfaces -> path -> ParseVidPid -> ORBTrace 1209:3443/3442
 *   3. CreateFileW + WinUsb_Initialize
 *   4. WinUsb_QueryInterfaceSettings(alt 0) -> class 0xFF, subclass 'T'/'P'/'V'
 *
 * (see orbrddi-main/src/ORBMDK_OrbTrace.cpp, ScanForOrbTrace).
 *
 * This program does the same four steps and prints the result of each, so
 * "no orbtrace device found" becomes "candidate mi_02 stopped at step 4,
 * GetLastError=6".  It links nothing from the driver: it is a pure WinUSB
 * enumeration test.
 *
 * Usage:  test\bin\ORBAGDI_OrbTraceProbe.exe
 * Exit code: 0 when a usable trace interface (class 0xFF, subclass 'T') was found.
 */
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <stdlib.h>

// Same value the RDDI host uses (ORBMDK_OrbTrace.cpp, ORBMDK_USB_Bulk.cpp).
static const GUID kWinUsbGuid =
    { 0xDEE824EF, 0x729B, 0x4A0E, { 0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17 } };

static int g_foundTrace = 0;

static void ParseVidPid(const wchar_t* path, unsigned* vid, unsigned* pid) {
    *vid = 0; *pid = 0;
    const wchar_t* p = wcsstr(path, L"vid_");
    if (p) swscanf_s(p, L"vid_%4x&pid_%4x", vid, pid);
}

static const char* SubClassName(unsigned char sub) {
    switch (sub) {
    case 0x54: return "TRACE";
    case 0x50: return "power";
    case 0x56: return "version";
    case 0x58: return "control-proxy (the host does not know this one)";
    default:   return "?";
    }
}

int main(void) {
    printf("orbtrace interface scan (mirrors the RDDI host's ScanForOrbTrace)\n");
    printf("    WinUSB GUID : {DEE824EF-729B-4A0E-9C14-B7117D33A817}\n");
    printf("    bitness     : %s\n\n", (sizeof(void*) == 8) ? "64-bit" : "32-bit");

    HDEVINFO devInfo = SetupDiGetClassDevs(&kWinUsbGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("STEP 1 FAILED: SetupDiGetClassDevs, GetLastError=%lu\n", GetLastError());
        return 1;
    }

    int candidates = 0, orbtrace = 0;
    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kWinUsbGuid, index, &did); ++index) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(malloc(need));
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, need, nullptr, nullptr)) {
            ++candidates;

            unsigned vid = 0, pid = 0;
            ParseVidPid(detail->DevicePath, &vid, &pid);
            const bool isOrb = (vid == 0x1209 && (pid == 0x3443 || pid == 0x3442));
            if (!isOrb) { free(detail); continue; }

            ++orbtrace;
            printf("candidate: %ls\n", detail->DevicePath);

            // ---- step 3 -----------------------------------------------------
            // FILE_FLAG_OVERLAPPED matters.  Without it WinUsb_Initialize fails
            // with ERROR_INVALID_HANDLE (6) on every interface of this device,
            // including the CMSIS-DAP one the host's own bulk path opens fine -
            // and that path passes FILE_FLAG_OVERLAPPED.  The RDDI host's
            // OpenInterfacePath (ORBMDK_OrbTrace.cpp) passes FILE_ATTRIBUTE_NORMAL
            // instead, which is why its orbtrace scan finds nothing while its
            // bulk scan works.  Both flag sets are tried here so the difference
            // is visible rather than asserted.
            HANDLE file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                printf("    STEP 3 FAILED: CreateFileW, GetLastError=%lu\n\n", GetLastError());
                free(detail);
                continue;
            }

            {
                WINUSB_INTERFACE_HANDLE probe = nullptr;
                const BOOL ok = WinUsb_Initialize(file, &probe);
                printf("    as the host opens it (FILE_ATTRIBUTE_NORMAL): WinUsb_Initialize %s",
                       ok ? "OK" : "FAILED");
                if (ok) { printf("\n"); WinUsb_Free(probe); }
                else    { printf(", GetLastError=%lu\n", GetLastError()); }
                CloseHandle(file);
            }

            file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                               nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                printf("    STEP 3 FAILED: CreateFileW(overlapped), GetLastError=%lu\n\n",
                       GetLastError());
                free(detail);
                continue;
            }

            WINUSB_INTERFACE_HANDLE wu = nullptr;
            if (!WinUsb_Initialize(file, &wu)) {
                printf("    STEP 3 FAILED: WinUsb_Initialize, GetLastError=%lu\n\n", GetLastError());
                CloseHandle(file);
                free(detail);
                continue;
            }

            // ---- step 4 -----------------------------------------------------
            USB_INTERFACE_DESCRIPTOR ifd;
            if (!WinUsb_QueryInterfaceSettings(wu, 0, &ifd)) {
                printf("    STEP 4 FAILED: WinUsb_QueryInterfaceSettings, GetLastError=%lu\n\n",
                       GetLastError());
            } else {
                printf("    iface=%u class=0x%02X subclass=0x%02X('%c') protocol=0x%02X -> %s\n",
                       ifd.bInterfaceNumber, ifd.bInterfaceClass, ifd.bInterfaceSubClass,
                       (ifd.bInterfaceSubClass >= 32 && ifd.bInterfaceSubClass < 127)
                           ? ifd.bInterfaceSubClass : '?',
                       ifd.bInterfaceProtocol, SubClassName(ifd.bInterfaceSubClass));
                if (ifd.bInterfaceClass != 0xFF) {
                    printf("    -> the host skips it: class is not 0xFF\n");
                } else if (ifd.bInterfaceSubClass == 0x54) {
                    g_foundTrace = 1;
                    if (ifd.bInterfaceProtocol != 0x10) {
                        printf("    -> trace protocol is not 0x10 (OFLOW v1.0); the host "
                               "would treat the stream as raw bytes\n");
                    }
                }
                printf("\n");
            }

            WinUsb_Free(wu);
            CloseHandle(file);
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    printf("WinUSB candidates      : %d\n", candidates);
    printf("ORBTrace 1209:344x     : %d\n", orbtrace);
    if (candidates == 0) {
        printf("The WinUSB interface class is empty - nothing is registered under that GUID,\n"
               "so no scan can see the probe.  Driver binding, not code.\n");
    } else if (!orbtrace) {
        printf("WinUSB interfaces exist but none carries the ORBTrace VID/PID.\n");
    } else if (!g_foundTrace) {
        printf("ORBTrace interfaces are present but none passed the trace test - see the\n"
               "step that failed above.\n");
    } else {
        printf("A trace interface (class 0xFF, subclass 'T') is present and openable.\n"
               "The host should have found it; if it still says \"no orbtrace device\n"
               "found\", the failure is inside the host's own scan.\n");
    }
    return g_foundTrace ? 0 : 1;
}
