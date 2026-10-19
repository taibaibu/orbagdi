/**
 * @file ORBAGDI_Break.h
 * @brief Breakpoint management: FPB hardware breakpoints + BKPT software
 *        breakpoints.
 */
#pragma once

#include "ORBAGDI_Dap.h"
#include "ORBAGDI_Core.h"
#include <stdint.h>

// uVision "collect" attribute bits (subset). Mirrors ARM's COLLECT.H so that
// AG_BpInfo() answers match what uVision expects.
#define ATRX_EXEC    0x00000001u
#define ATRX_READ    0x00000002u
#define ATRX_WRITE   0x00000004u
#define ATRX_WATCH   0x00000008u
#define ATRX_EXECD   0x00000100u
#define ATRX_ITAKEN  0x00000200u
#define ATRX_BREAK   0x00000400u
#define ATRX_BPDIS   0x00000800u

// AG_MemAtt() attribute bits (own definition; ARM does not ship them).
#define ATR_READ     0x00000001u
#define ATR_WRITE    0x00000002u
#define ATR_EXEC     0x00000004u
#define ATR_BREAK    0x00000008u
#define ATR_NOMAP    0x00000010u

namespace orb {

// None/Hw/Sw are execution breakpoints; Watch is a data-address watchpoint
// implemented with a DWT address comparator.
enum class BpKind { None, Hw, Sw, Watch };

struct BpSlot {
    BpKind   kind;
    uint32_t addr;      // instruction address (thumb, bit0 ignored)
    int      hwIndex;   // FPB comparator index (Hw) / DWT comparator index (Watch)
    uint32_t origData;  // saved 32-bit word (Sw only)
    uint32_t dwFunc;    // Watch only: DWT function code (DWT_WP_R/W/RW)
};

class Breakpoints {
public:
    Breakpoints();

    void Init(Dap* dap, Core* core);

    int  HwCapacity() const { return m_hwSlots; }
    int  Count() const { return m_used; }
    int  Capacity() const { return m_hwSlots + kMaxSwSlots + m_wpSlots; }

    // FP_CTRL[31:28] - the FPB revision.  It decides whether a comparator can be
    // used outside the code region: rev 0 is restricted to addresses below
    // 0x20000000, so a breakpoint in RAM has to become a software breakpoint
    // (DbgCM/AGDI.CPP:5535 `if ((FPB_Ver > 0) || pB->Adr < 0x20000000)`).
    int  FpbRev() const { return m_fpbRev; }

    // Returns true when the breakpoint had to be implemented in hardware.
    bool Add(uint32_t addr, bool& usedHw);
    bool Remove(uint32_t addr);
    bool SetEnabled(uint32_t addr, bool enable);
    bool Contains(uint32_t addr) const;
    int  IndexOf(uint32_t addr) const;

    // ---- data watchpoints (DWT address comparators) -----------------------
    int  WatchCapacity() const { return m_wpSlots; }
    int  WatchCount() const;
    // dwFunc is one of DWT_WP_R / DWT_WP_W / DWT_WP_RW.
    bool AddWatch(uint32_t addr, uint32_t dwFunc, int& dwtIndex);
    bool RemoveWatch(uint32_t addr);
    int  WatchIndexOf(uint32_t addr) const;

    void Clear();          // uninstall everything (target unload)
    void ResetDetection(); // re-detect FPB/DWT (new target)

    // Re-install every registered comparator after a debug-access recovery.
    //
    // Counterpart of the reference's `_BreakpointUnitRecovery()` /
    // `_WatchpointUnitRecovery()` (DbgCM/DebugAccess.cpp:336-397), which write the
    // cached `RegFPB` / `RegDWT` images back to the target.  This driver keeps no
    // register image, so it reinstalls per slot instead - same effect, and it can
    // never write back a comparator uVision never asked for.
    //
    // Deliberately does **not** re-probe capacities: the device's capabilities do
    // not change when the link is repaired, and re-probing would disturb what
    // uVision has already laid out.
    bool Recover();

    // Uninstall every breakpoint and watchpoint from the target but keep the
    // entries, so our list and uVision's stay in step.
    //
    // AG_BpInfo(AG_BPDISALL) means *disable*, not delete: the reference walks its
    // list and sets ATRX_BPDIS on each entry rather than dropping anything
    // (DbgCM/AGDI.CPP:7529).  Both cases used to call Clear(), so a disable
    // looked like a delete and uVision's markers flickered against an attribute
    // table that had just been emptied.  Returns how many were disabled.
    int  DisableAll();

    // Addresses currently held, for a caller that has to update its own
    // per-address bookkeeping (the attribute table in the AGDI layer).
    // Returns how many were written, at most 'max'.
    int  Addresses(uint32_t* out, int max) const;

    // FPB / DWT probing.
    bool ProbeFpb();
    bool ProbeDwt();

private:
    static constexpr int kMaxSlots   = 32;
    static constexpr int kMaxSwSlots = 16;

    bool InstallHw(BpSlot& s);
    bool UninstallHw(BpSlot& s);
    bool InstallSw(BpSlot& s);
    bool UninstallSw(BpSlot& s);
    bool InstallWatch(BpSlot& s);
    bool UninstallWatch(BpSlot& s);
    int  FreeSlot(BpKind autoSlotKinds);

    BpSlot  m_slots[kMaxSlots];
    int     m_used;
    int     m_hwSlots;     // number of FPB code comparators
    int     m_fpbRev = 0;  // FP_CTRL[31:28], captured by ProbeFpb()
    int     m_swUsed;
    int     m_wpSlots;     // number of DWT address comparators
    bool    m_fpbOn;
    bool    m_trcEna;      // DEMCR.TRCENA set (required to arm DWT comparators)
    Dap*    m_dap;
    Core*   m_core;
};

} // namespace orb
