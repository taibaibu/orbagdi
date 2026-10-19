#include "pch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <share.h>      // _SH_DENYWR, for _wfsopen below

namespace orb {
namespace log {

namespace {

CRITICAL_SECTION g_cs;
bool        g_csInit = false;
FILE*       g_file   = nullptr;
int         g_level  = kInfo;
wchar_t     g_path[MAX_PATH] = L"";

void EnsureCs() {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
}

const char* LevelTag(int level) {
    switch (level) {
        case kError: return "ERR";
        case kWarn:  return "WRN";
        case kInfo:  return "INF";
        case kDebug: return "DBG";
        default:     return "???";
    }
}

/// 日志在第一条消息到来时就建：选项串在首次 AG_Init 之前就送到，原来在 Init 里用 "w"
/// 打开，把驱动已记下的项目信息全冲掉（导致报错里看不到引发它的选项串）。
/// Init() 只记最终路径，不再碰日志文件。
void EnsureEarlyOpen() {
    if (g_file) return;

    char buf[16] = {0};
    if (GetEnvironmentVariableA("ORBAGDI_LOG_LEVEL", buf, sizeof(buf)) > 0 &&
        atoi(buf) >= kNone) {
        return;                     // NONE = logging switched off on purpose
    }

    wchar_t dir[MAX_PATH] = L"";
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    Init((n > 0 && n < MAX_PATH) ? dir : nullptr);
}

} // namespace

void Init(const wchar_t* baseDir) {
    EnsureCs();
    if (baseDir && *baseDir) {
        // Callers pass either "C:\dir" or "C:\dir\" - GetTempPathW keeps the
        // trailing separator - so trim first, otherwise we emit "C:\dir\\...".
        wchar_t dir[MAX_PATH] = L"";
        wcsncpy_s(dir, baseDir, _TRUNCATE);
        size_t len = wcslen(dir);
        while (len > 1 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/') &&
               dir[len - 2] != L':') {
            dir[--len] = L'\0';
        }
        _snwprintf_s(g_path, _TRUNCATE, L"%s\\ORB_AGDI.log", dir);
    } else {
        wcscpy_s(g_path, L"ORB_AGDI.log");
    }

    // Environment override for the level.  Same numbering as the RDDI layer:
    // 0 = DEBUG (most verbose) .. 9 = NONE (off).
    char buf[16] = {0};
    DWORD n = GetEnvironmentVariableA("ORBAGDI_LOG_LEVEL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        int v = atoi(buf);
        if (v >= 0 && v <= kNone) g_level = v;
    }

    if (g_level >= kNone) return; // NONE = silent

    EnterCriticalSection(&g_cs);
    if (!g_file) {
        // 用 _wfsopen 而非 _wfopen_s：后者默认拒绝共享读，Info 页 tail 这个文件时会整个会话
        // 都打不开（实测"正被另一进程使用"）。_SH_DENYWR 允许读，又挡住第二个写者截断。
        g_file = _wfsopen(g_path, L"w", _SH_DENYWR);
    }
    LeaveCriticalSection(&g_cs);
}

void Shutdown() {
    if (!g_csInit) return;
    EnterCriticalSection(&g_cs);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    LeaveCriticalSection(&g_cs);
}

void SetLevel(int level) { g_level = level; }
int  GetLevel() { return g_level; }
const wchar_t* FilePath() { return g_path; }

void Write(int level, const char* fmt, ...) {
    // 编号越小越详细 ⇒ 比阈值**更详细**的才要丢掉（level < g_level）。
    // 与 RDDI 层同一方向；反过来写会把 WARN/ERROR 一起丢掉。
    if (level < g_level || g_level >= kNone) return;

    EnsureEarlyOpen();

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u][%s] %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                LevelTag(level), body);

    OutputDebugStringA(line);

    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace log
} // namespace orb
