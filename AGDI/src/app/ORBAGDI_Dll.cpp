/**
 * @file ORBAGDI_Dll.cpp
 * @brief DLL 入口：加载时不做重活，RDDI 会话留到 AG_Init(AG_INITFEATURES) 再惰性打开。
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
