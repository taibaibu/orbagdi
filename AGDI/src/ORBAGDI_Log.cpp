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

/// uVision delivers the option string (EnumUvARM7(2) / DllUv3Cap(109)) *before*
/// the first AG_Init(AG_INITFEATURES), which is where the log used to be
/// created - and created with "w", so everything the driver had learned about
/// the project was thrown away again.  Proof of that is a report that cannot
/// show "no flash algorithm configured" together with the option string that
/// caused it.  Open the log on the first message instead; Init() then only
/// records the final path and stays out of the way.
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
        // _wfsopen, not _wfopen_s, and deliberately so: the default sharing
        // mode of _wfopen_s denies readers, which locked every other reader out
        // of the log for the whole session.  The Info page tails this very file
        // (its "Log tail" section), so the sheet was showing "the log file
        // cannot be opened" while the driver was holding it open and writing to
        // it - measured, not guessed: another process gets "being used by
        // another process" for as long as the driver is loaded.  A log wants
        // exactly the opposite of an exclusive lock: we write, everything else
        // may read.  _SH_DENYWR allows reads and still keeps a second writer
        // from truncating the file underneath us.
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
