#include "pch.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>      // _SH_DENYWR, for _wfsopen below
#include <atomic>

namespace orb {
namespace log {

// ---------------------------------------------------------------------------
// 配置来源与 RDDI 层（ORBMDK_Log.cpp）保持同一套手顺：
//
//   等级：%TEMP%\ORBAGDI_LOG_LEVEL（内容一个整数，热更新，最多 1 秒生效）
//         → 环境变量 ORBAGDI_LOG_LEVEL → 默认 kError
//   路径：环境变量 ORBAGDI_LOG_FILE → Init() 给的目录 → 当前目录
//
// ---------------------------------------------------------------------------

namespace {

const char*    const kEnvLevel    = "ORBAGDI_LOG_LEVEL";
const wchar_t* const kEnvFile     = L"ORBAGDI_LOG_FILE";
const wchar_t* const kLevelFile   = L"ORBAGDI_LOG_LEVEL";  // %TEMP% 下的热更新文件
const wchar_t* const kDefaultName = L"ORB_AGDI.log";

CRITICAL_SECTION g_cs;
bool        g_csInit       = false;
FILE*       g_file         = nullptr;
int         g_level        = kError;
bool        g_levelForced  = false;   // SetLevel() 之后不再看文件/环境
ULONGLONG   g_levelStamp   = 0;
wchar_t     g_path[MAX_PATH] = L"";

// 无锁快照：热路径（被过滤掉的日志）只读这个值，不抢锁。
// kUnresolved = 还没解析过，第一次询问时才去读文件/环境。
const int   kUnresolved    = 0x7FFFFFFF;
std::atomic<int> g_levelFast{kUnresolved};
std::atomic<ULONGLONG> g_nextProbe{0};    // 下一次允许"因为被过滤而重解析"的时刻

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

/// %TEMP%\<name>；失败返回 false。
bool TempFile(wchar_t* out, size_t cap, const wchar_t* name) {
    wchar_t dir[MAX_PATH] = L"";
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return false;
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%s", dir, name) > 0;
}

/// 读 %TEMP%\ORBAGDI_LOG_LEVEL：>=0 = 有效等级；-1 = 没有这个文件；
/// -2 = 文件在但内容不认识（按"恢复默认"处理，与 RDDI 层同）。
int ReadLevelFile() {
    wchar_t path[MAX_PATH] = L"";
    if (!TempFile(path, MAX_PATH, kLevelFile)) return -1;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return -1;

    int lvl = -1;
    if (fscanf_s(f, "%d", &lvl) != 1) lvl = -1;
    fclose(f);

    if (lvl < kDebug || lvl > kNone) return -2;
    return lvl;
}

/// 与 RDDI 层一致：什么都没配时只留 ERROR。要看进度就配
/// `%TEMP%\ORBAGDI_LOG_LEVEL`（或环境变量 ORBAGDI_LOG_LEVEL），别改这里。
int DefaultLevel() { return kError; }

/// 解析阈值（**调用方必须持有 g_cs**）。每条日志都查文件太浪费，
/// 1 秒最多查一次，因此改动最多延迟 1 秒生效。
int ResolveLevelLocked() {
    if (g_levelForced) return g_level;

    const ULONGLONG now = GetTickCount64();
    if (now - g_levelStamp < 1000) return g_level;
    g_levelStamp = now;

    const int fileLvl = ReadLevelFile();
    if (fileLvl >= kDebug) {
        g_level = fileLvl;                 // 0 = DEBUG（最详细）.. 9 = NONE（全关）
    } else if (fileLvl == -2) {
        g_level = DefaultLevel();          // 文件在、内容无效：回默认
    } else {
        char buf[16] = {0};
        if (GetEnvironmentVariableA(kEnvLevel, buf, sizeof(buf)) > 0) {
            const int v = atoi(buf);
            g_level = (v >= kDebug && v <= kNone) ? v : DefaultLevel();
        } else {
            g_level = DefaultLevel();
        }
    }

    g_levelFast.store(g_level, std::memory_order_relaxed);
    return g_level;
}

/** 首次解析（闸门发现快照还是 kUnresolved 时调用）。 */
int EnsureResolved() {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    const int lv = ResolveLevelLocked();
    LeaveCriticalSection(&g_cs);
    return lv;
}

/// 便宜的门：快照通过就放行（可能偏旧）；没通过时可能是阈值刚被调细，
/// 重解析一次再判。重解析每 1 秒最多一次 —— 否则被过滤的行会逐条抢锁。
bool FastPass(int level) {
    int lv = g_levelFast.load(std::memory_order_relaxed);
    if (lv == kUnresolved) lv = EnsureResolved();
    if (level >= lv && lv < kNone) return true;

    const ULONGLONG now = GetTickCount64();
    if (now < g_nextProbe.load(std::memory_order_relaxed)) return false;
    g_nextProbe.store(now + 1000, std::memory_order_relaxed);

    return level >= EnsureResolved() && g_levelFast.load(std::memory_order_relaxed) < kNone;
}

/// 从 ORBAGDI_LOG_FILE / baseDir 拼出路径（调用方必须持有 g_cs）。
/// baseDir 为空表示用"当前目录下的 ORB_AGDI.log"。
void ResolvePathLocked(const wchar_t* baseDir) {
    wchar_t path[MAX_PATH] = L"";

    wchar_t env[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(kEnvFile, env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wcscpy_s(path, env);               // 环境变量优先：可以指到任意可写目录
    } else if (baseDir && *baseDir) {
        // Callers pass either "C:\dir" or "C:\dir\" - GetTempPathW keeps the
        // trailing separator - so trim first, otherwise we emit "C:\dir\\...".
        wchar_t dir[MAX_PATH] = L"";
        wcsncpy_s(dir, baseDir, _TRUNCATE);
        size_t len = wcslen(dir);
        while (len > 1 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/') &&
               dir[len - 2] != L':') {
            dir[--len] = L'\0';
        }
        _snwprintf_s(path, _TRUNCATE, L"%s\\%s", dir, kDefaultName);
    } else {
        wcscpy_s(path, kDefaultName);
    }

    if (wcscmp(path, g_path) == 0) return;

    // 路径变了：老句柄关掉，下次写入按新路径重开。
    if (g_file) { fclose(g_file); g_file = nullptr; }
    wcscpy_s(g_path, path);
}

/// 确保日志文件已打开（调用方必须持有 g_cs）。
void OpenLocked() {
    if (g_file || g_path[0] == L'\0') return;

    // 用 _wfsopen 而非 _wfopen_s：后者默认拒绝共享读，Info 页 tail 这个文件时会整个会话
    // 都打不开（实测"正被另一进程使用"）。_SH_DENYWR 允许读，又挡住第二个写者截断。
    g_file = _wfsopen(g_path, L"w", _SH_DENYWR);
}

/// 首条消息到来就建文件：选项串在首次 AG_Init 之前就送到，原来只在 Init 里用 "w"
/// 打开会把驱动已记下的项目信息冲掉（导致报错里看不到引发它的选项串）。
void EnsureEarlyOpen() {
    if (g_file) return;
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (!g_file && g_path[0] == L'\0') {
        wchar_t dir[MAX_PATH] = L"";
        const DWORD n = GetTempPathW(MAX_PATH, dir);
        ResolvePathLocked((n > 0 && n < MAX_PATH) ? dir : nullptr);
    }
    OpenLocked();
    LeaveCriticalSection(&g_cs);
}

} // namespace

void Init(const wchar_t* baseDir) {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    ResolvePathLocked(baseDir);

    // 首条消息之前就把阈值定下来（日志文件打开时可能已经没有消息要写了）。
    ResolveLevelLocked();

    if (g_level < kNone) OpenLocked();
    LeaveCriticalSection(&g_cs);
}

void Shutdown() {
    if (!g_csInit) return;
    EnterCriticalSection(&g_cs);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    LeaveCriticalSection(&g_cs);
}

void SetLevel(int level) {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    g_level = level;
    g_levelForced = true;                  // 之后不再被文件/环境影响
    g_levelFast.store(level, std::memory_order_relaxed);
    LeaveCriticalSection(&g_cs);
}

int GetLevel() {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    const int lv = ResolveLevelLocked();
    LeaveCriticalSection(&g_cs);
    return lv;
}

const wchar_t* FilePath() { return g_path; }

void Write(int level, const char* fmt, ...) {
    if (level < kDebug || level > kNone) return;

    // 编号越小越详细 ⇒ 比阈值**更详细**的才要丢掉（level < 阈值）。
    // 与 RDDI 层同一方向；反过来写会把 WARN/ERROR 一起丢掉。
    if (!FastPass(level)) return;

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

    EnsureEarlyOpen();

    bool written = false;
    EnterCriticalSection(&g_cs);
    // 权威判定放在锁内：阈值可能刚被调粗（临时文件改成 8），闸门的快照还是旧的。
    if (level >= ResolveLevelLocked() && g_level < kNone && g_file) {
        fputs(line, g_file);
        fflush(g_file);
        written = true;
    }
    LeaveCriticalSection(&g_cs);

    // 只在真的 cache 时送 DebugView：被阈值挡掉的行走这条路等于白送。
    if (written) OutputDebugStringA(line);
}

} // namespace log
} // namespace orb
