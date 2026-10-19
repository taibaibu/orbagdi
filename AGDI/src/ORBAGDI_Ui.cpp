/**
 * @file ORBAGDI_Ui.cpp
 * @brief Implementation of the "report to uVision's windows" channel.
 *
 * See ORBAGDI_Ui.h for where the two wParam values come from and why the call is
 * in-process.  Two things about the implementation are worth knowing:
 *
 *  - the lock is only ever held while copying the four state fields.  SendMessage
 *    goes to uVision's GUI thread, and holding a critical section across it would
 *    deadlock the moment uVision's handler called back into one of our entry
 *    points;
 *  - a line is always written to the log first.  If the UI channel is not up yet
 *    (or uVision has already torn the session down) the report is still
 *    recoverable, which is the whole reason the log exists.
 */
#include "pch.h"
#include "ORBAGDI_Ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace orb {
namespace ui {
namespace {

// uVision's private wParam values for the registered message it hands over as
// AG_INITUSRMSG.  Read out of Keil's own driver - see the header for the
// disassembly this comes from.
const WPARAM kCmdLine = 0x0C;   // MSG_UV2_CMDLINE - the command window
const WPARAM kTrnLine = 0x06;   // MSG_UV2_TRNLINE - the build/output window

CRITICAL_SECTION g_cs;
bool             g_csInit = false;

HWND  g_frame = nullptr;   // AG_INITPHANDLEP
DWORD g_msg   = 0;         // AG_INITUSRMSG (a RegisterWindowMessage value)
pCBF  g_cb    = nullptr;   // AG_INITCALLBACK
bool  g_flash = false;     // which of the two wParam values to use

void Lock() {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    EnterCriticalSection(&g_cs);
}
void Unlock() { LeaveCriticalSection(&g_cs); }

/// MessageBoxW needs UTF-16 and the log path is already wide, so the only
/// conversion is the (ANSI) body text.
std::wstring Widen(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

/// Pushes one already-formatted, already NUL-terminated line into uVision.
void Emit(const char* text) {
    // Copy the state out, then release: SendMessage runs uVision's handler on its
    // own thread and must not happen with our lock held.
    HWND  frame = nullptr;
    DWORD msg   = 0;
    pCBF  cb    = nullptr;
    bool  flash = false;
    Lock();
    frame = g_frame;
    msg   = g_msg;
    cb    = g_cb;
    flash = g_flash;
    Unlock();

    if (frame && msg) {
        // In-process, so lParam may be a pointer into this DLL's stack.
        SendMessageA(frame, msg, flash ? kTrnLine : kCmdLine,
                     reinterpret_cast<LPARAM>(text));
        return;
    }
    // Before the handles arrive (the option string is delivered earlier, through
    // EnumUvARM7/DllUv3Cap) and after uVision has torn the session down, the
    // documented callback is the only way out.  AG_CB_MSGSTRING is the same
    // message pane the command window writes to.
    if (cb) cb(AG_CB_MSGSTRING, const_cast<char*>(text));
}

/// The single place a line is produced: log file first, then the UI.
void EmitLine(int level, const char* prefix, const char* body) {
    const char* pfx = prefix ? prefix : "";

    // The file keeps its own timestamp and level tag, so the prefix is added
    // here rather than passed through Write().
    orb::log::Write(level, "%s%s", pfx, body);

    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s\n", pfx, body);
    Emit(line);
}

} // namespace

// ---------------------------------------------------------------------------
void Attach(HWND mainFrame, DWORD uvMsg, pCBF cb) {
    Lock();
    if (mainFrame) g_frame = mainFrame;
    if (uvMsg)     g_msg   = uvMsg;
    if (cb)        g_cb    = cb;
    const bool haveWindow = (g_frame != nullptr) && (g_msg != 0);
    const bool haveCb     = (g_cb != nullptr);
    Unlock();

    // Only worth a line the first time it becomes usable; uVision calls
    // AG_INITITEM several times and repeating this on every one would be noise.
    static bool announced = false;
    if (announced || (!haveWindow && !haveCb)) return;
    announced = true;

    if (haveWindow) {
        ORB_LOG_INFO("UI: reporting to uVision's window (frame %p, msg 0x%04X)",
                     static_cast<void*>(mainFrame), static_cast<unsigned>(uvMsg));
    } else {
        ORB_LOG_WARN("UI: uVision gave no main window (msg 0x%04X) - key "
                     "information goes to the callback only",
                     static_cast<unsigned>(uvMsg));
    }
}

void Detach() {
    Lock();
    g_frame = nullptr;
    g_msg   = 0;
    g_cb    = nullptr;
    g_flash = false;
    Unlock();
}

void SetFlashMode(bool on) {
    Lock();
    g_flash = on;
    Unlock();
}

bool Ready() {
    Lock();
    const bool r = (g_frame != nullptr && g_msg != 0) || (g_cb != nullptr);
    Unlock();
    return r;
}

// ---------------------------------------------------------------------------
void Info(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EmitLine(orb::log::kInfo, nullptr, body);
}

void Warn(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EmitLine(orb::log::kWarn, "Warning: ", body);
}

void Error(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EmitLine(orb::log::kError, "Error: ", body);
}

void Fatal(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    EmitLine(orb::log::kError, "Error: ", body);

    // Raise the box only from the thread that owns uVision's window.  The flash
    // and wait-for-break paths run on uVision's worker threads, and a modal box
    // from one of those would sit behind a window that is not pumping.
    HWND frame = nullptr;
    Lock();
    frame = g_frame;
    Unlock();
    if (!frame) return;
    if (GetWindowThreadProcessId(frame, nullptr) != GetCurrentThreadId()) {
        ORB_LOG_INFO("UI: not on uVision's window thread - the message box is "
                     "skipped, the report above stands");
        return;
    }

    char text[1150];
    _snprintf_s(text, sizeof(text), _TRUNCATE,
                "%s\n\nSee %ls for the details.", body,
                orb::log::FilePath() ? orb::log::FilePath() : L"the driver log");
    MessageBoxW(frame, Widen(text).c_str(), L"ORBAGDI", MB_ICONERROR | MB_OK);
}

} // namespace ui
} // namespace orb
