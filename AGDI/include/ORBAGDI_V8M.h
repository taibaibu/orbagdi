/**
 * @file ORBAGDI_V8M.h
 * @brief ARMv8-M Security Extension support (Secure / Non-Secure debug).
 *
 * Port of Keil's DbgCM `#if DBGCM_V8M` block onto this driver:
 *
 *   IsV8M()                     DbgCM/AGDI.CPP:17958
 *   DetectSecurityExtensions()  DbgCM/AGDI.CPP:17983
 *   UpdateSecurityState()       DbgCM/AGDI.CPP:18028
 *   UpdateDebugAuthentication() DbgCM/AGDI.CPP:18059
 *   NonSecureCantStop()         DbgCM/AGDI.CPP:18087
 *   SevereTargetError()         DbgCM/AGDI.CPP:18111
 *
 * Why this matters even for a v7-M part: a v8-M device whose debug port is open
 * only to a non-secure debugger cannot be halted while the CPU runs secure code.
 * The reference does not treat that as a broken connection - it degrades the
 * error to a warning so the session survives. Reporting it as a fatal error is
 * what turns "the debugger cannot see secure state" into "the debugger died".
 *
 * All register access goes through the MEM-AP of orb::Dap, so nothing here
 * needs the reference's own access layer.
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// Official SEV_ERR_* (DbgCM/COLLECT.H:418-421).
enum : unsigned {
    kSevErrStop   = 1, // Cannot stop target
    kSevErrReset  = 2, // Cannot reset target
    kSevErrAccess = 3, // Cannot access target
    kSevErrReinit = 4, // Cannot reinit target
};

// The CPU types the reference classifies as v8-M (DbgCM/AGDI.CPP:17958).  The
// caller passes whatever its own CPU identification produced; the values here
// are the reference's ARM_CM* enumeration, so a caller with a different scheme
// maps onto them rather than guessing.
enum : unsigned {
    kCpuCM0    = 0,
    kCpuCM0P   = 1,
    kCpuCM1    = 2,
    kCpuCM3    = 3,
    kCpuCM4    = 4,
    kCpuCM7    = 5,
    kCpuSC000  = 6,
    kCpuSC300  = 7,
    kCpuCM23   = 8,   // v8-M Baseline
    kCpuCM33   = 9,   // v8-M Mainline
    kCpuCM35P  = 10,  // v8-M Mainline with Physical Security
};

// Official IsV8M(): CM23 / CM33 / CM35P are v8-M, everything else is not.
bool IsV8M(unsigned cpuType);

// CPUID.PARTNO (SCS 0xE000ED00 bits 15:4) -> the enumeration above.  Keil
// classifies the CPU while identifying the device (its `xxCPU` comes from the
// device database); this driver reads CPUID directly, so the part number has to
// be mapped here.  An unrecognised part number maps to a non-v8-M entry, so an
// unknown device is never credited with Security Extensions it may not have.
unsigned V8mCpuTypeFromPartNo(unsigned partNo);

// True for v8-M Mainline (CM33 / CM35P).  Mainline answers NVIC_PFR1, Baseline
// has to be asked through DBG_AUTHSTATUS instead.
bool IsV8MMainline(unsigned cpuType);

// Official pio->bSecureExt / bCPUSecure / b*DbgEna / b*TrcEna, plus DSCSR's CDSKEY.
struct V8mStatus {
    bool secureExt;  // Security Extensions available
    bool cpuSecure;  // current CPU security state
    bool nsDbgEna;   // Non-Secure Debug enabled
    bool nsTrcEna;   // Non-Secure Trace enabled
    bool sDbgEna;    // Secure Debug enabled
    bool sTrcEna;    // Secure Trace enabled
    bool cdskey;     // DSCSR.CDSKEY - the CDS value is locked
    bool detected;   // DetectSecurityExtensions() ran to completion
};

V8mStatus V8mGetStatus();

// Official DetectSecurityExtensions() (DbgCM/AGDI.CPP:17983).  Decides whether
// the device has Security Extensions at all; for a non-v8-M part this answers
// false and leaves the status cleared.  On success it also runs the two update
// functions below, exactly as the reference does.
bool V8mDetectSecurityExtensions(orb::Dap& dap, unsigned cpuType);

// Official UpdateSecurityState() (DbgCM/AGDI.CPP:18028): read DBG_SCSR and
// publish the CPU's current security state.
bool V8mUpdateSecurityState(orb::Dap& dap);

// Official UpdateDebugAuthentication() (DbgCM/AGDI.CPP:18059): read
// DBG_AUTHSTATUS and DBG_HCSR and publish the four debug/trace enable flags.
bool V8mUpdateDebugAuthentication(orb::Dap& dap);

// Official NonSecureCantStop() (DbgCM/AGDI.CPP:18087): "non-secure debugger,
// CPU in secure mode".  Refreshes the authentication status first, like the
// reference does.  Never true for a v7-M part.
bool V8mNonSecureCantStop();

// Official SevereTargetError() (DbgCM/AGDI.CPP:18111).
//   returns 0 - fatal: the caller should shut the session down
//   returns 1 - degraded to a warning because NonSecureCantStop() is true
int V8mSevereTargetError(unsigned errType);

// Official SevereTargetError() ends in SetPlayDead(msg).  This driver has no
// global PlayDead state - its equivalent is "the session is no longer
// connected" - so the teardown is delegated to a hook the AGDI layer installs.
// With no hook installed the fatal path only logs.
typedef void (*V8mPlayDeadFn)(const char* msg);
void V8mSetPlayDeadHook(V8mPlayDeadFn fn);

} // namespace agdi
} // namespace orb
