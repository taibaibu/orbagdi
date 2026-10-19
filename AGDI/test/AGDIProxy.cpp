/*
 * AGDIProxy.cpp - a logging shim in front of Keil's own CMSIS_AGDI.dll
 *
 * Why this exists
 * ---------------
 * uVision's Registers window stays empty with ORB_AGDI.dll, while it fills
 * completely with CMSIS_AGDI.dll on the same project and the same device
 * (STM32F407VETx) - Core / Banked / System / Internal / FPU all populated.
 *
 * Every AGDI-level comparison made so far comes back "identical":
 *   - the same 15 exports;
 *   - AG_AllReg: the same four branches (READ/WRITE/READFPU/WRITEFPU), the same
 *     23-dword RgARMCM copy, the same 33-dword RgARMFPU copy, one communication
 *     mutex, and it returns 0 on success (disassembled from CMSIS_AGDI.dll);
 *   - EnumUvARM7 and DllUv3Cap answer for the same code sets;
 *   - the same 13 AG_INITITEM items arrive.
 * Continuing to guess at jump tables is guesswork.  This shim produces the fact
 * list instead: every AGDI entry uVision calls on the driver that DOES work,
 * with its arguments and its return value, so it can be diffed against
 * %TEMP%\ORB_AGDI.log line by line.
 *
 * Build
 * -----
 *   cl /nologo /LD /O2 AGDIProxy.cpp /Fe:CMSIS_AGDI.dll
 *
 *   No .def file is needed: every entry point is declared extern "C" with
 *   __declspec(dllexport), which exports the plain undecorated name - the same
 *   names uVision resolves out of the real DLL.
 *
 * Install (fully reversible)
 * --------------------------
 *   1. copy  D:\MDK5\ARM\BIN\CMSIS_AGDI.dll       D:\MDK5\ARM\BIN\CMSIS_AGDI_real.dll
 *   2. copy  the CMSIS_AGDI.dll built above   ->  D:\MDK5\ARM\BIN\CMSIS_AGDI.dll
 *   3. run a uVision session with CMSIS-AGDI selected (Debug / Settings)
 *   4. read  %TEMP%\AGDIProxy.log
 *   5. uninstall: copy CMSIS_AGDI_real.dll back over CMSIS_AGDI.dll
 *
 * If CMSIS_AGDI_real.dll is missing the shim says so in the log and fails every
 * call loudly rather than pretending to work.
 *
 * What to look for in the log
 * ---------------------------
 *   AG_Init(0x....)          - the whole handshake; compare against ORB_AGDI's
 *                              "Init: nCode=..." lines.  Any code that appears
 *                              here and never in our log is a candidate.
 *   AG_RegAcc(...)           - uVision never calls this on ORB_AGDI.  If it
 *                              calls it here, that is the register window.
 *   AG_AllReg(AG_READ) -> ... PC=...   - proof that uVision does read registers
 *                              through this entry on a working session.
 *   AG_MemAtt / AG_BpInfo / AG_GoStep  - the rest of the traffic, for context.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>   // strcat_s / strcpy_s / strrchr

typedef unsigned int   U32;
typedef unsigned short U16;
typedef int            I32;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static HMODULE          g_real = nullptr;
static FILE*            g_log  = nullptr;
static CRITICAL_SECTION g_lock;

static void LogOpen(void) {
    char path[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, path) == 0) return;
    strcat_s(path, MAX_PATH, "AGDIProxy.log");
    fopen_s(&g_log, path, "a");
}

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}

static void* Real(const char* name) {
    if (!g_real) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(g_real, name));
}

// ---------------------------------------------------------------------------
// DllMain - load the real driver from the same directory
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_lock);
    LogOpen();

    char path[MAX_PATH] = {0};
    GetModuleFileNameA(inst, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) {
        strcpy_s(slash + 1, static_cast<size_t>(MAX_PATH - (slash + 1 - path)),
                 "CMSIS_AGDI_real.dll");
    }
    g_real = LoadLibraryA(path);
    Log("=== AGDI proxy attached; real driver %s: %s",
        path, g_real ? "loaded" : "LOAD FAILED (calls will fail)");
    return TRUE;
}

// ---------------------------------------------------------------------------
// The 15 entry points, forwarded verbatim
//
// Signatures are spelled with plain integer / void* types on purpose: this file
// only forwards, so the exact struct types do not matter, and it then needs no
// AGDI.H to build.
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) int EnumUvARM7(void* p, U32 nCode) {
    typedef int (*F)(void*, U32);
    F f = reinterpret_cast<F>(Real("EnumUvARM7"));
    int r = f ? f(p, nCode) : 0;
    Log("EnumUvARM7(nCode=%u) -> %d", nCode, r);
    return r;
}

__declspec(dllexport) int DllUv3Cap(U32 nCode, void* p) {
    typedef int (*F)(U32, void*);
    F f = reinterpret_cast<F>(Real("DllUv3Cap"));
    int r = f ? f(nCode, p) : 0;
    Log("DllUv3Cap(nCode=%u, p=%p) -> %d", nCode, p, r);
    return r;
}

__declspec(dllexport) U32 AG_Init(U16 nCode, void* vp) {
    typedef U32 (*F)(U16, void*);
    F f = reinterpret_cast<F>(Real("AG_Init"));
    U32 r = f ? f(nCode, vp) : 1u;
    // The handshake is the interesting part: one line per item, same shape as
    // ORB_AGDI's "Init: nCode=0x...." so the two logs can be diffed directly.

    //ÔÝÊ±ÆÁ±Î£¬26.10.14/19:53
    //Log("AG_Init(0x%04X, vp=%p) -> %u", nCode, vp, r);
    return r;
}

__declspec(dllexport) U32 AG_MemAcc(U16 nCode, void* pB, void* pA, U32 nMany) {
    typedef U32 (*F)(U16, void*, void*, U32);
    F f = reinterpret_cast<F>(Real("AG_MemAcc"));
    U32 r = f ? f(nCode, pB, pA, nMany) : 1u;
    // Address is the first dword of GADR.
    const U32 adr = pA ? *reinterpret_cast<U32*>(pA) : 0u;
    Log("AG_MemAcc(nCode=0x%02X, adr=0x%08X, nMany=%u) -> %u", nCode, adr, nMany, r);
    return r;
}

__declspec(dllexport) U32 AG_MemAccX(void* pIn) {
    typedef U32 (*F)(void*);
    F f = reinterpret_cast<F>(Real("AG_MemAccX"));
    U32 r = f ? f(pIn) : 1u;
    Log("AG_MemAccX(pIn=%p) -> %u", pIn, r);
    return r;
}

__declspec(dllexport) U32 AG_DapAcc(void* pIn) {
    typedef U32 (*F)(void*);
    F f = reinterpret_cast<F>(Real("AG_DapAcc"));
    U32 r = f ? f(pIn) : 1u;
    Log("AG_DapAcc(pIn=%p) -> %u", pIn, r);
    return r;
}

__declspec(dllexport) U32 AG_AllReg(U16 nCode, void* vp) {
    typedef U32 (*F)(U16, void*);
    F f = reinterpret_cast<F>(Real("AG_AllReg"));
    U32 r = f ? f(nCode, vp) : 1u;
    if (nCode == 0x01u && vp) {
        // RgARMCM: DWORD Rn[16], xPSR, MSP, PSP, DSP, SYS, INT64 nCycles.
        // Rn[15] is the PC - the same value ORB_AGDI logs.
        const U32* w = reinterpret_cast<const U32*>(vp);
        Log("AG_AllReg(AG_READ) -> %u  PC=0x%08X SP=0x%08X LR=0x%08X xPSR=0x%08X",
            r, w[15], w[13], w[14], w[16]);
    } else if (nCode == 0x1Au && vp) {
        const U32* w = reinterpret_cast<const U32*>(vp);
        Log("AG_AllReg(AG_READFPU) -> %u  S0=0x%08X FPSCR=0x%08X", r, w[0], w[32]);
    } else {
        Log("AG_AllReg(nCode=0x%02X, vp=%p) -> %u", nCode, vp, r);
    }
    return r;
}

__declspec(dllexport) U32 AG_RegAcc(U16 nCode, U32 nReg, void* pV) {
    typedef U32 (*F)(U16, U32, void*);
    F f = reinterpret_cast<F>(Real("AG_RegAcc"));
    U32 r = f ? f(nCode, nReg, pV) : 1u;
    // uVision never calls this on ORB_AGDI.  If it shows up here, the register
    // window is fed per register and that is the whole difference.
    Log("AG_RegAcc(nCode=0x%02X, reg=0x%02X) -> %u%s", nCode, nReg, r,
        (pV && nCode == 0x01u) ? "" : "");
    if (pV && nCode == 0x01u) {
        Log("           value = 0x%08X", *reinterpret_cast<U32*>(pV));
    }
    return r;
}

__declspec(dllexport) U32 AG_MemAtt(U16 nCode, U32 nAttr, void* pA) {
    typedef U32 (*F)(U16, U32, void*);
    F f = reinterpret_cast<F>(Real("AG_MemAtt"));
    U32 r = f ? f(nCode, nAttr, pA) : 1u;
    const U32 adr = pA ? *reinterpret_cast<U32*>(pA) : 0u;
    Log("AG_MemAtt(nCode=0x%02X, attr=0x%04X, adr=0x%08X) -> %u", nCode, nAttr, adr, r);
    return r;
}

__declspec(dllexport) U32 AG_BpInfo(U16 nCode, void* vp) {
    typedef U32 (*F)(U16, void*);
    F f = reinterpret_cast<F>(Real("AG_BpInfo"));
    U32 r = f ? f(nCode, vp) : 1u;
    const U32 adr = vp ? (*reinterpret_cast<U32*>(vp) & ~1u) : 0u;
    Log("AG_BpInfo(nCode=0x%02X, adr=0x%08X) -> 0x%X", nCode, adr, r);
    return r;
}

__declspec(dllexport) void* AG_BreakFunc(U16 nCode, U16 n1, void* pA, void* pB) {
    typedef void* (*F)(U16, U16, void*, void*);
    F f = reinterpret_cast<F>(Real("AG_BreakFunc"));
    void* r = f ? f(nCode, n1, pA, pB) : nullptr;
    Log("AG_BreakFunc(nCode=0x%02X, n1=0x%04X, pB=%p) -> %p", nCode, n1, pB, r);
    return r;
}

__declspec(dllexport) U32 AG_GoStep(U16 nCode, U32 nSteps, void* pA) {
    typedef U32 (*F)(U16, U32, void*);
    F f = reinterpret_cast<F>(Real("AG_GoStep"));
    const U32 in = pA ? *reinterpret_cast<U32*>(pA) : 0u;
    U32 r = f ? f(nCode, nSteps, pA) : 1u;
    const U32 out = pA ? *reinterpret_cast<U32*>(pA) : 0u;
    Log("AG_GoStep(nCode=0x%02X, nSteps=%u, adr=0x%08X) -> %u  adr'=0x%08X",
        nCode, nSteps, in, r, out);
    return r;
}

__declspec(dllexport) U32 AG_Serial(U16 nCode, U32 nSerNo, U32 nMany, void* vp) {
    typedef U32 (*F)(U16, U32, U32, void*);
    F f = reinterpret_cast<F>(Real("AG_Serial"));
    U32 r = f ? f(nCode, nSerNo, nMany, vp) : 1u;
    Log("AG_Serial(nCode=0x%02X, nSerNo=%u, nMany=%u) -> %u", nCode, nSerNo, nMany, r);
    return r;
}

__declspec(dllexport) U32 AG_HistFunc(U32 nCode, I32 indx, I32 dir, void* vp) {
    typedef U32 (*F)(U32, I32, I32, void*);
    F f = reinterpret_cast<F>(Real("AG_HistFunc"));
    U32 r = f ? f(nCode, indx, dir, vp) : 1u;
    Log("AG_HistFunc(nCode=%u, indx=%d, dir=%d) -> %u", nCode, indx, dir, r);
    return r;
}

__declspec(dllexport) U32 AG_BreakResAlloc(U32 nAlloc, void* pBp) {
    typedef U32 (*F)(U32, void*);
    F f = reinterpret_cast<F>(Real("AG_BreakResAlloc"));
    U32 r = f ? f(nAlloc, pBp) : 1u;
    Log("AG_BreakResAlloc(nAlloc=%u) -> %u", nAlloc, r);
    return r;
}

} // extern "C"
