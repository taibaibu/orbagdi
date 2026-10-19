/**
 * @file ORBAGDI_Log.h
 * @brief Very small file logger. Never throws.
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace log {

// 等级编号与 RDDI 层（`orbrddi-main/src/ORBMDK_Log.cpp` 的 `kLevelName[]`）
// **同一口径**：0 = DEBUG 最详细，数值越大越安静，9 = NONE 全关。
// 两层用同一个数字就能同时调；改之前本层是"1=最安静、4=最详细、0=静默"，
// 与 RDDI 正好相反 —— 按一层的习惯填另一层，必然把日志整个关掉或全打开。
// （本层只有 4 档，中间那几档留给以后与 RDDI 对齐，编号直接沿用它的值。）

enum Level {
    kDebug = 0,
    kInfo  = 1,
    kWarn  = 7,
    kError = 8,
    kNone  = 9,
};

// baseDir: folder that receives "ORB_AGDI.log" (usually %TEMP%). A trailing
// separator is accepted.
void Init(const wchar_t* baseDir);
void Shutdown();
void Write(int level, const char* fmt, ...);

// Default kInfo. ORBAGDI_LOG_LEVEL in the environment overrides it.
void SetLevel(int level);
int  GetLevel();
const wchar_t* FilePath();

} // namespace log
} // namespace orb

#define ORB_LOG_ERROR(...) ::orb::log::Write(::orb::log::kError, __VA_ARGS__)
#define ORB_LOG_WARN(...)  ::orb::log::Write(::orb::log::kWarn,  __VA_ARGS__)
#define ORB_LOG_INFO(...)  ::orb::log::Write(::orb::log::kInfo,  __VA_ARGS__)
#define ORB_LOG_DEBUG(...) ::orb::log::Write(::orb::log::kDebug, __VA_ARGS__)
