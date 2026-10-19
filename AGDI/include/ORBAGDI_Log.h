/**
 * @file ORBAGDI_Log.h
 * @brief Very small file logger. Never throws.
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace log {

// 等级与 RDDI 层共用一套编号：0 最详细、数值越大越安静、9 全关。
// 两层数字必须一致，否则照一层的习惯填另一层会关掉或全开日志。

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
