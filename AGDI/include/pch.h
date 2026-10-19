// ORBAGDI precompiled-style header (included manually by every .cpp).
#pragma once

#include <string>
#include <stdint.h>
#include <cstddef>
#include <cstdarg>
#include <cstdio>

#include "ORBAGDI.h"
#include "ORBAGDI_Log.h"
#include "ORBAGDI_Ui.h"
#include "ORBAGDI_Config.h"
#include "ORBAGDI_Rddi.h"
#include "ORBAGDI_Dap.h"
#include "ORBAGDI_Core.h"
#include "ORBAGDI_Break.h"

// 循环里安全拼接字符串。
// snprintf 返回“想写多长”，截断后 used 会超过 cap，下次 cap-used 变成巨大
// size_t 而越界写；这里把 used 钳在 cap，截断后后续调用直接早退。
namespace orb {
namespace fmt {
inline size_t Append(char* buf, size_t cap, size_t& used, const char* fmt, ...)
{
    if (!buf || cap == 0) { used = 0; return 0; }
    if (used >= cap)      { used = cap; return 0; }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[used] = '\0'; return 0; }
    used += static_cast<size_t>(n);
    if (used >= cap) used = cap;      // truncated: clamp so later calls early-exit
    return static_cast<size_t>(n);
}
} // namespace fmt
} // namespace orb
