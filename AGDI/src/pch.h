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

// ---------------------------------------------------------------------------
// Appending format - the only safe way to build a string in a loop.
//
// `n += snprintf(buf + n, sizeof(buf) - n, ...)` has a classic trap: snprintf
// returns how much it *would* have written, so on truncation n grows past
// sizeof(buf).  The next `sizeof(buf) - n` then underflows to a huge size_t and
// snprintf writes straight through the stack.  This project had one call site
// that reached it by design (the breakpoint diagnostic in Agdi.cpp: 32 entries
// x 11 bytes into a 256-byte buffer).
//
// Here `used` is clamped to `cap`, so once truncated every later call exits
// early and nothing is ever written out of bounds.  The return value is the
// length the call wanted (may exceed what was written); callers normally only
// look at `used`.
// ---------------------------------------------------------------------------
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
