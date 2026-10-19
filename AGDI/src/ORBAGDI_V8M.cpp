/**
 * @file ORBAGDI_V8M.cpp
 * @brief ARMv8-M Security Extension support.  See ORBAGDI_V8M.h for the mapping.
 */
#include "pch.h"
#include "ORBAGDI_V8M.h"
#include "ORBAGDI_Dap.h"

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// Register addresses (DbgCM/Debug.h:307-431)
// ---------------------------------------------------------------------------
//   NVIC base 0xE000E000
//   DBG_OFS   0x0DF0   -> core debug block at 0xE000EDF0
//   PFR1      +0x0D44, SCSR +0x018, AUTHSTATUS +0x1C8
// ---------------------------------------------------------------------------
const uint32_t kAddrNvic = 0xE000E000u;
const uint32_t kAddrDbg  = kAddrNvic + 0x0DF0u;

const uint32_t kAddrHcsr = kAddrDbg + 0x000u; // DBG_HCSR
const uint32_t kAddrScsr = kAddrDbg + 0x018u; // DBG_SCSR
const uint32_t kAddrAuth = kAddrDbg + 0x1C8u; // DBG_AUTHSTATUS
const uint32_t kAddrPfr1 = kAddrNvic + 0x0D44u; // NVIC_PFR1

// NVIC: Processor Feature Register 1 (Debug.h:428-431)
const uint32_t kNvicPfSec    = 0x000000F0u; // Security Field Mask
const uint32_t kNvicPfSecImp = 0x00000010u; // Security Extensions Implemented

// Debug Authentication Status Register (Debug.h:380-388)
const uint32_t kDauthNse  = 0x00000001u; // Non-Secure Invasive Debug Enabled
const uint32_t kDauthNsi  = 0x00000002u; // Non-Secure Invasive Debug Implemented
const uint32_t kDauthNsne = 0x00000004u; // Non-Secure Non-Invasive Debug Enabled
const uint32_t kDauthNsni = 0x00000008u; // Non-Secure Non-Invasive Debug Implemented
const uint32_t kDauthSe   = 0x00000010u; // Secure Invasive Debug Enabled
const uint32_t kDauthSi   = 0x00000020u; // Secure Invasive Debug Implemented
const uint32_t kDauthSne  = 0x00000040u; // Secure Non-Invasive Debug Enabled
const uint32_t kDauthSni  = 0x00000080u; // Secure Non-Invasive Debug Implemented

// Debug Security Control and Status Register (Debug.h:374-378)
const uint32_t kCds    = 0x00010000u; // Current Domain Secure
const uint32_t kCdskey = 0x00020000u; // Current Domain Secure Key

// Debug Halting Control and Status Register (Debug.h:341-343)
const uint32_t kSSde = 0x00100000u; // Secure Debug Enabled Flag (v8-M)

// Official `BITS_SET(val, bits)` (DbgCM/AGDI.CPP:18054).
inline bool BitsSet(uint32_t val, uint32_t bits) { return (val & bits) == bits; }

// ---------------------------------------------------------------------------
// Error texts (DbgCM/AGDI.CPP:165-222, verbatim)
// ---------------------------------------------------------------------------
const char kStopErr[] =
    "Could not stop Cortex-M device!\n"
    "Please check the JTAG cable.";
const char kFatalAccErr[] =
    "Cannot access target.\n"
    "Shutting down debug session.";
const char kFatalRstErr[] =
    "Cannot reset target.\n"
    "Shutting down debug session.";
const char kFatalReInitErr[] =
    "Cannot reconfigure target.\n"
    "Shutting down debug session.";

// Specialized error messages for non-secure debug.
const char kNonSecureStopErr[]   = "Secure Cortex-M device stop can be delayed!";
const char kNonSecureAccErr[]    = "Cannot access secure target.";
const char kNonSecureRstErr[]    = "Cannot reset secure target.";
const char kNonSecureReInitErr[] = "Cannot reconfigure secure target.";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct State {
    unsigned cpuType;     // CPU type this session was classified as
    V8mStatus st;
    State() : cpuType(0u) { memset(&st, 0, sizeof(st)); }
};

State& S()
{
    // Deliberately leaked, like the other module states in this driver.
    static State* s = new State();
    return *s;
}

// The reference reaches the target through its own global access layer; here the
// Dap is remembered when the detection runs, so the refresh path inside
// NonSecureCantStop() has something to read through.
orb::Dap*   g_dap      = nullptr;
V8mPlayDeadFn g_playDead = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

// DbgCM/AGDI.CPP:17958.
bool IsV8M(unsigned cpuType)
{
    switch (cpuType) {
    case kCpuCM0:   // ARM Cortex-M0
    case kCpuCM0P:  // ARM Cortex-M0+
    case kCpuCM1:   // ARM Cortex-M1
    case kCpuCM3:   // ARM Cortex-M3
    case kCpuCM4:   // ARM Cortex-M4
    case kCpuSC000: // ARM SC000 (Secure Cortex-M0)
    case kCpuSC300: // ARM SC300 (Secure Cortex-M3)
    case kCpuCM7:   // ARM Cortex-M7
        return false;
    case kCpuCM23:  // ARM Cortex-M23 (v8-M Baseline)
    case kCpuCM33:  // ARM Cortex-M33 (v8-M Mainline)
    case kCpuCM35P: // ARM Cortex-M35P (v8-M Mainline with Physical Security)
        return true;
    }
    return false;
}

bool IsV8MMainline(unsigned cpuType)
{
    return cpuType == kCpuCM33 || cpuType == kCpuCM35P;
}

// CPUID.PARTNO values (ARM Architecture Reference Manual, CPUID register).
unsigned V8mCpuTypeFromPartNo(unsigned partNo)
{
    switch (partNo) {
    case 0xC20: return kCpuCM0;   // Cortex-M0
    case 0xC60: return kCpuCM0P;  // Cortex-M0+
    case 0xC21: return kCpuCM1;   // Cortex-M1
    case 0xC23: return kCpuCM3;   // Cortex-M3
    case 0xC24: return kCpuCM4;   // Cortex-M4
    case 0xC27: return kCpuCM7;   // Cortex-M7
    case 0xD20: return kCpuCM23;  // Cortex-M23 (v8-M Baseline)
    case 0xD21: return kCpuCM33;  // Cortex-M33 (v8-M Mainline)
    case 0xD22: return kCpuCM35P; // Cortex-M35P (v8-M Mainline + Physical Security)
    default:    return kCpuCM3;   // unknown -> classified as non-v8-M
    }
}

V8mStatus V8mGetStatus()
{
    return S().st;
}

void V8mSetPlayDeadHook(V8mPlayDeadFn fn)
{
    g_playDead = fn;
}

// ---------------------------------------------------------------------------
// State updates
// ---------------------------------------------------------------------------

// DbgCM/AGDI.CPP:18059.
bool V8mUpdateDebugAuthentication(orb::Dap& dap)
{
    V8mStatus& s = S().st;
    if (!IsV8M(S().cpuType)) return true;   // nothing to update
    if (!s.secureExt)        return true;

    uint32_t val1 = 0, val2 = 0;
    if (!dap.MemRead32(kAddrAuth, val1)) return false;
    if (!dap.MemRead32(kAddrHcsr, val2)) return false;

    s.nsDbgEna = BitsSet(val1, kDauthNsi | kDauthNse);
    s.nsTrcEna = BitsSet(val1, kDauthNsni | kDauthNsne);
    // S_SDE covers "Secure Debug enabled until the next instruction".
    s.sDbgEna  = BitsSet(val1, kDauthSi | kDauthSe) || ((val2 & kSSde) != 0);
    s.sTrcEna  = BitsSet(val1, kDauthSni | kDauthSne);
    return true;
}

// DbgCM/AGDI.CPP:18028.
bool V8mUpdateSecurityState(orb::Dap& dap)
{
    V8mStatus& s = S().st;
    if (!IsV8M(S().cpuType)) return true;   // nothing to update

    if (s.secureExt) {
        uint32_t val = 0;
        if (!dap.MemRead32(kAddrScsr, val)) return false;
        s.cpuSecure = (val & kCds) != 0;
        s.cdskey    = (val & kCdskey) != 0;
    } else {
        s.cpuSecure = false;
        s.cdskey    = false;
    }
    return true;
}

// DbgCM/AGDI.CPP:17983.
bool V8mDetectSecurityExtensions(orb::Dap& dap, unsigned cpuType)
{
    State&     st = S();
    V8mStatus& s  = st.st;

    st.cpuType  = cpuType;
    g_dap       = &dap;

    if (!IsV8M(cpuType)) {
        // Nothing to detect.
        s.secureExt = false;
        s.detected  = true;
        return true;
    }

    if (IsV8MMainline(cpuType)) {
        // Mainline answers explicitly through the Processor Feature Register.
        uint32_t val = 0;
        if (!dap.MemRead32(kAddrPfr1, val)) return false;
        s.secureExt = ((val & kNvicPfSec) == kNvicPfSecImp);
    } else {
        // Baseline: Secure Invasive or Secure Non-Invasive Debug implemented?
        uint32_t val = 0;
        if (!dap.MemRead32(kAddrAuth, val)) return false;
        s.secureExt = ((val & (kDauthSi | kDauthSni)) != 0);
    }

    s.detected = true;

    if (!s.secureExt) return true;

    if (!V8mUpdateDebugAuthentication(dap)) return false;
    if (!V8mUpdateSecurityState(dap))       return false;
    return true;
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

// DbgCM/AGDI.CPP:18087.
bool V8mNonSecureCantStop()
{
    if (!IsV8M(S().cpuType)) {
        // No Non-Secure vs Secure debug.
        return false;
    }

    // Get the Debug Authentication Status up-to-date.  The reference reports the
    // failure through OutError() and then answers from the state it has; a failed
    // refresh here likewise leaves the last known state in place.
    if (g_dap) V8mUpdateDebugAuthentication(*g_dap);

    if (S().st.secureExt && !S().st.sDbgEna) {
        return true;
    }
    return false;
}

// DbgCM/AGDI.CPP:18111.
int V8mSevereTargetError(unsigned errType)
{
    const char* msg    = nullptr;
    int         result = 0;

    // The reference calls NonSecureCantStop() twice - once to choose the message
    // and once for the branch below.  Each call refreshes the authentication
    // status, so this costs two extra register reads; it is kept as-is rather
    // than folded into one call, so the behaviour matches the reference exactly.
    if (V8mNonSecureCantStop()) {
        switch (errType) {
        case kSevErrStop:   msg = kNonSecureStopErr;   break;
        case kSevErrReset:  msg = kNonSecureRstErr;    break;
        case kSevErrAccess: msg = kNonSecureAccErr;    break;
        case kSevErrReinit: msg = kNonSecureReInitErr; break;
        default: break;
        }
    } else {
        switch (errType) {
        case kSevErrStop:   msg = kStopErr;        break;
        case kSevErrReset:  msg = kFatalRstErr;    break;
        case kSevErrAccess: msg = kFatalAccErr;    break;
        case kSevErrReinit: msg = kFatalReInitErr; break;
        default: break;
        }
    }

    if (V8mNonSecureCantStop() && msg) {
        // Non-secure debugger on a secure CPU: a degraded warning, the session
        // survives.  The reference prints "Warning: <msg>" through txtout().
        ORB_LOG_WARN("V8M: Warning: %s", msg);
        result = 1;
    } else {
        // Fatal.  The reference calls SetPlayDead(msg) here; this driver's
        // equivalent teardown is delegated to the hook.
        ORB_LOG_ERROR("V8M: fatal target error - %s", msg ? msg : "(unknown)");
        if (g_playDead) g_playDead(msg);
    }

    return result;
}

} // namespace agdi
} // namespace orb
