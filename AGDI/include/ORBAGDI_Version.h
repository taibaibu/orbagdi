/**
 * @file ORBAGDI_Version.h
 * @brief Release version - the single place it is written down.
 *
 * Included by both C++ (ORBAGDI.h, for the session banner) and the resource
 * compiler (ORBAGDI.rc, for the DLL's VERSIONINFO), so the number the log
 * prints, the number Explorer shows and the number in the README cannot drift
 * apart.  Keep this file free of anything the RC preprocessor cannot parse -
 * no C++, no windows.h, plain integer macros only.
 *
 * Release rule: bump here, add the entry to README.md "Version history", and
 * rebuild.  `.\build.ps1` recompiles the resource automatically.
 */
#pragma once

#define ORBAGDI_VERSION_MAJOR 0
#define ORBAGDI_VERSION_MINOR 2
#define ORBAGDI_VERSION_PATCH 0
#define ORBAGDI_VERSION_STRING "0.2.0"

// VERSIONINFO wants the numbers comma separated, without the macro name.
#define ORBAGDI_VERSION_COMMA 0,2,0,0
