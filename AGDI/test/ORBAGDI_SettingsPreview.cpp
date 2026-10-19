/**
 * @file ORBAGDI_SettingsPreview.cpp
 * @brief Standalone shell that shows the real ORBAGDI settings sheet.
 *
 * uVision only shows the settings sheet inside a live debug session, which makes
 * the layout awkward to inspect while it is being edited.  This program builds
 * the **same** sheet: it links the real `ORBAGDI_Settings.cpp` and calls the same
 * `orb::cfg::ShowDialog()` that `DllUv3Cap(1)` calls, then reports the option
 * string the sheet produced.
 *
 * It contains no test logic of its own on purpose.  Everything that is checked -
 * the layout, the control tree, the option string that comes back out - is
 * checked by the PowerShell scripts in this folder, from the outside, by driving
 * the dialog the way a user would.  That keeps `src\` free of test hooks: what
 * uVision loads is exactly what is tested.
 *
 *   test\e2e_sheet.ps1       opens the sheet, presses OK from outside, and
 *                            compares the written string with the input
 *   test\inspect_sheet.ps1   enumerates the control tree and checks the geometry
 *   test\run-tests.ps1       builds and runs both
 *
 * Build:  powershell -ExecutionPolicy Bypass -File test\build.ps1
 * Run:    test\bin\SettingsPreview.exe [--ui] [option string]
 *
 *           --ui   show a message box with the result when the sheet closes
 *                  (for looking at it by hand; the scripts do not pass it)
 *
 * Output: two lines on stdout, which is the contract the scripts read:
 *           result : OK (accepted) | Cancel (options unchanged)
 *           options: <the option string in force now>
 *
 * Exit code: 0 = OK was pressed, 1 = cancelled / the sheet did not open.
 */
#include "pch.h"

#include <stdio.h>
#include <string.h>

namespace {

// A sample that populates every page, so a bare run shows a realistic sheet
// rather than one full of empty fields.
const char kSample[] =
    "rddi=CMSIS_DAP.DLL;iface=-1;clock=1000;port=1;ap=0;"
    "swo=115200;cpu=72000;orbtrace=1;trwidth=4;trswo=0;trbaud=2000000;"
    "troflow=1;ports=FFFFFFFF;pcpresc=2;"
    "flm=\"%KEIL_ROOT%\\ARM\\Flash\\STM32F4xx_512.FLM\"";

} // namespace

int main(int argc, char** argv) {
    bool ui = false;
    const char* arg = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ui") == 0) ui = true;
        else if (!arg) arg = argv[i];
    }

    char args[orb::cfg::kArgsMax];
    strncpy_s(args, arg ? arg : kSample, _TRUNCATE);

    char out[orb::cfg::kArgsMax];
    out[0] = '\0';

    const bool ok = orb::cfg::ShowDialog(nullptr, args, out, sizeof(out));

    // Cancel restores the incoming string, so report what is in force now.
    const char* result = ok ? out : args;
    const char* state  = ok ? "OK (accepted)" : "Cancel (options unchanged)";

    printf("result : %s\n", state);
    printf("options: %s\n", result);
    fflush(stdout);

    if (ui) {
        char text[2 * orb::cfg::kArgsMax];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "Result: %s\n\nOption string:\n%s", state, result);
        MessageBoxA(nullptr, text, "ORBAGDI settings preview",
                    MB_ICONINFORMATION | MB_OK);
    }
    return ok ? 0 : 1;
}
