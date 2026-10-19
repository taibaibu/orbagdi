/**
 * @file ORBAGDI_Ui.h
 * @brief Reporting key information into uVision's own windows.
 *
 * ORBAGDI_Log.h writes a file that nobody opens until something has already gone
 * wrong.  This is the other half: the handful of facts a user needs *while* the
 * session is running, pushed into a window uVision already has open.
 *
 * The channel is the one Keil's own target drivers use.  AG_Init(AG_INITITEM)
 * hands the driver two things:
 *
 *     AG_INITPHANDLEP  ->  the main frame's HWND
 *     AG_INITUSRMSG    ->  a message registered with RegisterWindowMessage()
 *
 * and one line of text is then
 *
 *     SendMessage(mainFrame, uvMsg, MSG_UV2_CMDLINE, text);
 *
 * MSG_UV2_CMDLINE / MSG_UV2_TRNLINE are **not** in AGDI.H - they are uVision's
 * private wParam values.  They were read out of Keil's own driver: ULP2CM3.dll's
 * txtout() at VA 0x1008B1B0 does
 *
 *     mov  edx, [pio + 0x78]      ; pio->FlashLoad, bit 0x20
 *     mov  ecx, [Uv2Msg]          ; 0x102a3c98, stored by AG_INITUSRMSG
 *     and  dl, 0x20
 *     ...  neg / sbb / and 0xFFFFFFFA / add 0xC
 *     push &txtbuf                ; lParam
 *     push eax                    ; wParam: 0x0C, or 0x06 while flashing
 *     push ecx                    ; msg  = Uv2Msg
 *     push [hMfrm]                ; hwnd = 0x102a3c8c, stored by AG_INITPHANDLEP
 *     call SendMessageA
 *
 * which decodes to
 *
 *     0x0C  MSG_UV2_CMDLINE  - the command window (debug mode, the normal case)
 *     0x06  MSG_UV2_TRNLINE  - the build/output window (during a flash download)
 *
 * The call is in-process - this DLL runs inside uVision - so the text pointer in
 * lParam is valid with no marshalling.  Before uVision has handed the handles
 * over (the option string arrives earlier, via EnumUvARM7/DllUv3Cap), the report
 * falls back to AG_CB_MSGSTRING, the documented channel, which lands in the same
 * message pane.
 *
 * What belongs here: things a user must be able to see without opening a log.
 * Driver banner and effective options, what the probe and target answered, which
 * flash algorithm was loaded, how each flash pass ended, and every failure.  What
 * does not: per-access traces, retry loops, packet dumps - those stay in the log
 * file, which is why ORB_LOG_* is still the right thing to call for them.
 */
#pragma once

#include "ORBAGDI.h"    // HWND, DWORD, pCBF (via AGDI.H)

namespace orb {
namespace ui {

/// Called by AG_Init(AG_INITITEM) as uVision hands the handles over.  Any of the
/// three may arrive before the others, so this is called again whenever one
/// changes; a NULL/0 value simply leaves that channel unavailable.
void Attach(HWND mainFrame, DWORD uvMsg, pCBF cb);

/// AG_UNINIT: stop talking to a window that is going away.
void Detach();

/// A flash download prints into the build window instead of the command window,
/// exactly as Keil's txtout() does.  Set on entry to the flash passes and
/// cleared again afterwards.
void SetFlashMode(bool on);

/// True once uVision has given us something to talk to.
bool Ready();

// ---------------------------------------------------------------------------
// The reports.  Each one also goes to the log file, so a line that scrolled past
// on screen is still recoverable, and the level keeps ORBAGDI_LOG_LEVEL working
// the way it did before.
// ---------------------------------------------------------------------------
void Info (const char* fmt, ...);
void Warn (const char* fmt, ...);
void Error(const char* fmt, ...);

/// Something the session cannot continue past.  Reported like Error(), and - if
/// we are on the thread that owns uVision's window, so the box is safe to raise -
/// also put up as a message box.  A session that stops working must not do it
/// silently; that is how "the debugger does nothing" reports are born.
void Fatal(const char* fmt, ...);

} // namespace ui
} // namespace orb
