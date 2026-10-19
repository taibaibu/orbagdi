#include "pch.h"

#include <string.h>

namespace orb {
namespace rddi {

namespace {

HMODULE  g_mod = nullptr;
Api      g_api = {};
int      g_lastErr = 0;
char     g_lastErrText[256] = "";

// Resolve one symbol; returns false when missing.
template <typename T>
bool Resolve(HMODULE mod, const char* name, T& fn, bool required) {
    fn = reinterpret_cast<T>(GetProcAddress(mod, name));
    if (!fn && required) {
        ORB_LOG_ERROR("RDDI: missing export %s", name);
        return false;
    }
    return true;
}

bool ResolveAll(HMODULE mod) {
    bool ok = true;
    // Core / handle management.
    ok &= Resolve(mod, "RDDI_Open",       g_api.Open,        true);
    ok &= Resolve(mod, "RDDI_Close",      g_api.Close,       true);
    ok &= Resolve(mod, "RDDI_GetLastError", g_api.GetLastError, false);
    // DAP configuration / connection (optional, CMSIS-DAP path may not need them).
    ok &= Resolve(mod, "DAP_Configure",       g_api.DAP_Configure,       false);
    ok &= Resolve(mod, "DAP_Connect",         g_api.DAP_Connect,         false);
    ok &= Resolve(mod, "DAP_Disconnect",      g_api.DAP_Disconnect,      false);
    ok &= Resolve(mod, "DAP_GetNumberOfDAPs", g_api.DAP_GetNumberOfDAPs, false);
    ok &= Resolve(mod, "DAP_GetDAPIDList",    g_api.DAP_GetDAPIDList,    false);
    // DAP register access (required).
    ok &= Resolve(mod, "DAP_ReadReg",       g_api.DAP_ReadReg,       true);
    ok &= Resolve(mod, "DAP_WriteReg",      g_api.DAP_WriteReg,      true);
    ok &= Resolve(mod, "DAP_RegAccessBlock", g_api.DAP_RegAccessBlock, false);
    ok &= Resolve(mod, "DAP_RegReadRepeat",  g_api.DAP_RegReadRepeat,  false);
    ok &= Resolve(mod, "DAP_RegWriteRepeat", g_api.DAP_RegWriteRepeat, false);
    ok &= Resolve(mod, "DAP_SetCommTimeout", g_api.DAP_SetCommTimeout, false);
    // CMSIS-DAP interface detection / configuration.
    ok &= Resolve(mod, "CMSIS_DAP_Detect",               g_api.CMSIS_DAP_Detect,               true);
    ok &= Resolve(mod, "CMSIS_DAP_Identify",             g_api.CMSIS_DAP_Identify,             false);
    ok &= Resolve(mod, "CMSIS_DAP_ConfigureInterface",   g_api.CMSIS_DAP_ConfigureInterface,   false);
    // Optional here so that a host without it still opens the settings sheet; the
    // connect sequence checks for it and says so in the log.
    ok &= Resolve(mod, "CMSIS_DAP_Connect",              g_api.CMSIS_DAP_Connect,              false);
    ok &= Resolve(mod, "CMSIS_DAP_DetectNumberOfDAPs",   g_api.CMSIS_DAP_DetectNumberOfDAPs,   false);
    ok &= Resolve(mod, "CMSIS_DAP_DetectDAPIDList",      g_api.CMSIS_DAP_DetectDAPIDList,      false);
    ok &= Resolve(mod, "CMSIS_DAP_Commands",             g_api.CMSIS_DAP_Commands,             false);
    ok &= Resolve(mod, "CMSIS_DAP_SWJ_Sequence",         g_api.CMSIS_DAP_SWJ_Sequence,         false);
    ok &= Resolve(mod, "CMSIS_DAP_SWJ_Pins",             g_api.CMSIS_DAP_SWJ_Pins,             false);
    ok &= Resolve(mod, "CMSIS_DAP_Delay",                g_api.CMSIS_DAP_Delay,                false);
    ok &= Resolve(mod, "CMSIS_DAP_ResetTarget",          g_api.CMSIS_DAP_ResetTarget,          false);
    ok &= Resolve(mod, "CMSIS_DAP_SWJ_Clock",            g_api.CMSIS_DAP_SWJ_Clock,            false);
    // SWO / ITM streaming trace (optional; the ITM console stays off without it).
    ok &= Resolve(mod, "CMSIS_DAP_ConfigureDebugger", g_api.CMSIS_DAP_ConfigureDebugger, false);
    ok &= Resolve(mod, "CMSIS_DAP_SWO_Control",  g_api.CMSIS_DAP_SWO_Control,  false);
    ok &= Resolve(mod, "CMSIS_DAP_SWO_Status",   g_api.CMSIS_DAP_SWO_Status,   false);
    ok &= Resolve(mod, "CMSIS_DAP_SWO_Baudrate", g_api.CMSIS_DAP_SWO_Baudrate, false);
    ok &= Resolve(mod, "CMSIS_DAP_SWO_Data",     g_api.CMSIS_DAP_SWO_Data,     false);
    // orbtrace 配置 + raw trace 流（可选；没有它时探针 trace 源整体不参与）。
    ok &= Resolve(mod, "OrbTrace_Available",      g_api.OrbTrace_Available,      false);
    ok &= Resolve(mod, "OrbTrace_GetInfo",        g_api.OrbTrace_GetInfo,        false);
    ok &= Resolve(mod, "OrbTrace_SetTraceWidth",  g_api.OrbTrace_SetTraceWidth,  false);
    ok &= Resolve(mod, "OrbTrace_SetTraceSWO",    g_api.OrbTrace_SetTraceSWO,    false);
    ok &= Resolve(mod, "OrbTrace_SetSWOBaudrate", g_api.OrbTrace_SetSWOBaudrate, false);
    ok &= Resolve(mod, "OrbTrace_SetVoltage",     g_api.OrbTrace_SetVoltage,     false);
    ok &= Resolve(mod, "OrbTrace_SetVoltageEnable", g_api.OrbTrace_SetVoltageEnable, false);
    ok &= Resolve(mod, "TraceStream_Open",        g_api.TraceStream_Open,        false);
    ok &= Resolve(mod, "TraceStream_Close",       g_api.TraceStream_Close,       false);
    ok &= Resolve(mod, "TraceStream_Read",        g_api.TraceStream_Read,        false);
    ok &= Resolve(mod, "TraceStream_Status",      g_api.TraceStream_Status,      false);
    ok &= Resolve(mod, "TraceStream_DroppedBytes", g_api.TraceStream_DroppedBytes, false);
    return ok;
}

// Build the candidate RDDI DLL path: same folder as this module.
std::wstring SelfDir() {
    wchar_t path[MAX_PATH] = L"";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfDir), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return std::wstring(path);
}

} // namespace

bool Load() {
    if (g_mod) return true;

    // 1) the project's option string (Settings dialog), 2) explicit override,
    // 3) CMSIS_DAP.dll next to us, 4) plain name.
    const orb::cfg::Config& opt = orb::cfg::Get();
    std::wstring fromCfg;
    if (opt.rddiDll[0]) {
        const int len = MultiByteToWideChar(CP_ACP, 0, opt.rddiDll, -1, nullptr, 0);
        if (len > 1) {
            fromCfg.resize(static_cast<size_t>(len - 1));
            MultiByteToWideChar(CP_ACP, 0, opt.rddiDll, -1, &fromCfg[0], len);
        }
    }

    wchar_t envPath[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"ORBAGDI_RDDI_DLL", envPath, MAX_PATH);

    wchar_t candidates[4][MAX_PATH];
    int count = 0;
    if (!fromCfg.empty() && fromCfg.size() < MAX_PATH) {
        wcscpy_s(candidates[count++], fromCfg.c_str());
    }
    if (n > 0 && n < MAX_PATH) {
        wcscpy_s(candidates[count++], envPath);
    }
    std::wstring local = SelfDir() + L"\\CMSIS_DAP.dll";
    wcscpy_s(candidates[count++], local.c_str());
    wcscpy_s(candidates[count++], L"CMSIS_DAP.dll");

    for (int i = 0; i < count; ++i) {
        HMODULE mod = LoadLibraryW(candidates[i]);
        if (!mod) continue;
        if (!ResolveAll(mod)) {
            ORB_LOG_ERROR("RDDI: %ls loaded but exports are incomplete", candidates[i]);
            FreeLibrary(mod);
            continue;
        }
        g_mod = mod;
        ORB_LOG_INFO("RDDI: loaded %ls", candidates[i]);
        return true;
    }
    ORB_LOG_ERROR("RDDI: failed to load CMSIS_DAP.dll (last error %lu)", GetLastError());
    return false;
}

void Unload() {
    if (g_mod) {
        FreeLibrary(g_mod);
        g_mod = nullptr;
    }
    memset(&g_api, 0, sizeof(g_api));
}

bool IsLoaded() { return g_mod != nullptr; }
HMODULE Module() { return g_mod; }
const Api& Get() { return g_api; }

bool HasSwoTrace() {
    return g_api.CMSIS_DAP_SWO_Control && g_api.CMSIS_DAP_SWO_Baudrate &&
           g_api.CMSIS_DAP_SWO_Data && g_api.CMSIS_DAP_SWO_Status;
}

bool HasOrbTrace() {
    // 打开 / 读 / 关是必需的；配置类缺失时 ORBAGDI_OrbTrace 会用
    // TraceStream_Open 的配置串兜底。
    return g_api.OrbTrace_Available && g_api.TraceStream_Open &&
           g_api.TraceStream_Read && g_api.TraceStream_Close;
}

int LastErrorCode() {
    if (g_api.GetLastError) {
        int code = 0;
        char txt[256] = "";
        if (g_api.GetLastError(&code, txt, sizeof(txt)) == 0) {
            g_lastErr = code;
            strncpy_s(g_lastErrText, txt, _TRUNCATE);
        }
    }
    return g_lastErr;
}

void LastErrorText(char* buf, size_t len) {
    if (!buf || len == 0) return;
    strncpy_s(buf, len, g_lastErrText, _TRUNCATE);
}

char* InterfaceConfig(char* buf, size_t cch) {
    if (!buf || cch == 0) return nullptr;
    buf[0] = '\0';

    const orb::cfg::Config& opt = orb::cfg::Get();
    if (opt.port == orb::cfg::kPortJtag) {
        strncpy_s(buf, cch, "Port=JTAG", _TRUNCATE);
    }
    if (!opt.swj) {
        if (buf[0]) strncat_s(buf, cch, ";", _TRUNCATE);
        strncat_s(buf, cch, "SWJ=0", _TRUNCATE);
    }
    // nullptr rather than "" for the plain SWD case: the host is told nothing
    // when there is nothing to say.
    return buf[0] ? buf : nullptr;
}

} // namespace rddi
} // namespace orb
