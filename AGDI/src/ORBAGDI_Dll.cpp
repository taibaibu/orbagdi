/**
 * @file ORBAGDI_Dll.cpp
 * @brief DLL entry point.
 *
 * The DLL is loaded by uVision (32-bit). Nothing heavy happens on attach:
 * the RDDI session is opened lazily from AG_Init(AG_INITFEATURES), because
 * by then the debugger has already set up the target.
 */
#include "pch.h"
#include "ORBAGDI_Agdi.h"
#include "ORBAGDI_DebugAccess.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved;

    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        // No per-thread notifications needed; uVision calls in from its own
        // debugger thread and we spawn none.
        DisableThreadLibraryCalls(hModule);
        // 访问等级状态机的初值。参考在 `InitDll()` 里做同一件事
        // （`DbgCM/DbgCM.cpp:621-643`，`InitDll()` -> `InitDebugAccess()`），而
        // `InitDll()` 就是它的 DLL 初始化入口。两条赋值，不算"heavy"。
        orb::agdi::DebugAccessInit();
        break;

    case DLL_PROCESS_DETACH:
        // Closing the RDDI session here keeps the probe from being left in a
        // claimed state when uVision is closed.
        orb::agdi::DebugAccessUninit();
        orb::agdi::Shutdown();
        break;

    default:
        break;
    }

    return TRUE;
}
