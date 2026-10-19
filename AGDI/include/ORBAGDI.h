/**
 * @file ORBAGDI.h
 * @brief ORBAGDI - AGDI layer master header.
 *
 * Layering (bottom -> top):
 *
 *      Keil uVision  ->  AGDI layer (this project)  ->  RDDI layer (ORBMDK)
 *
 * This DLL plays the role of Keil's "CMSIS-DAP Debugger" AGDI driver. It is
 * built as ORB_AGDI.dll and deployed into Keil's ARM\BIN (it leaves Keil's own
 * CMSIS_AGDI.dll untouched). At run time it dynamically loads the RDDI DLL
 * (CMSIS_DAP.dll, provided by ORBMDK) and drives a Cortex-M target over it.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Version
//
// The numbers live in ORBAGDI_Version.h, which the resource compiler reads too
// (ORBAGDI.rc), so the log banner and the DLL's file properties always agree.
// ---------------------------------------------------------------------------
#include "ORBAGDI_Version.h"

// ---------------------------------------------------------------------------
// AGDI contract.
//
// Defining _IN_TARG_ makes AGDI.H expand its prototypes as
//      extern __declspec(dllexport) U32 AG_xxx(...);
// i.e. the exports we have to implement, instead of the Keil-side function
// pointer variables (which would clash with our definitions).
// ---------------------------------------------------------------------------
#ifndef _IN_TARG_
#define _IN_TARG_
#endif
#include "AGDI.H"

namespace orb {

inline constexpr uint32_t kVersionMajor = ORBAGDI_VERSION_MAJOR;
inline constexpr uint32_t kVersionMinor = ORBAGDI_VERSION_MINOR;
inline constexpr uint32_t kVersionPatch = ORBAGDI_VERSION_PATCH;

} // namespace orb
