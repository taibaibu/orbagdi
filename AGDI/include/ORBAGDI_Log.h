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
// separator is accepted. 环境变量 ORBAGDI_LOG_FILE 优先于它（可直接指定完整路径）。
void Init(const wchar_t* baseDir);
void Shutdown();
void Write(int level, const char* fmt, ...);

// 等级解析顺序（与 RDDI 层 ORBMDK_Log.cpp 同一套手顺）：
//   %TEMP%\ORBAGDI_LOG_LEVEL（内容一个整数，运行中热更新，最多 1 秒生效）
//   → 环境变量 ORBAGDI_LOG_LEVEL → 默认 kError（0=DEBUG .. 9=NONE）。
// 删掉那个临时文件即回到环境变量/默认值，不用重启宿主。
// SetLevel() 之后不再看文件与环境；GetLevel() 返回解析后的当前阈值。
void SetLevel(int level);
int  GetLevel();
const wchar_t* FilePath();

} // namespace log
} // namespace orb

#define ORB_LOG_ERROR(...) ::orb::log::Write(::orb::log::kError, __VA_ARGS__)
#define ORB_LOG_WARN(...)  ::orb::log::Write(::orb::log::kWarn,  __VA_ARGS__)
#define ORB_LOG_INFO(...)  ::orb::log::Write(::orb::log::kInfo,  __VA_ARGS__)
#define ORB_LOG_DEBUG(...) ::orb::log::Write(::orb::log::kDebug, __VA_ARGS__)
