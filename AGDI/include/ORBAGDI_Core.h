/**
 * @file ORBAGDI_Core.h
 * @brief Cortex-M core layer: halt / run / step / reset / register access.
 *
 * Register access goes through the Debug Control Block (DCRSR/DCRDR), which
 * works on every ARMv6-M / v7-M / v8-M core and does not depend on RDDI
 * extensions.
 */
#pragma once

#include "ORBAGDI.h"
#include "ORBAGDI_Dap.h"
#include <stdint.h>

namespace orb {

// ---- Cortex-M system / debug addresses -----------------------------------
enum : uint32_t {
    kItmBase     = 0xE0000000u,
    kDwtBase     = 0xE0001000u,
    kFpbBase     = 0xE0002000u,
    kDcbBase     = 0xE000EDF0u,   // DHCSR
    kDcbDhcsr    = 0xE000EDF0u,
    kDcbDcrsr    = 0xE000EDF4u,
    kDcbDcrdr    = 0xE000EDF8u,
    kDcbDemcr    = 0xE000EDFCu,
    kDcbDfsr     = 0xE000ED30u,   // DFSR - debug fault status (why the core halted)
    kScsAircr    = 0xE000ED0Cu,   // AIRCR
    kScsCpuid    = 0xE000ED00u,
};

// ---- DHCSR ---------------------------------------------------------------
enum : uint32_t {
    kDhcsrDbgKey    = 0xA05F0000u,
    kDhcsrCDebugEn  = 0x00000001u,
    kDhcsrCHalt     = 0x00000002u,
    kDhcsrCStep     = 0x00000004u,
    kDhcsrCMaskInts = 0x00000008u,
    kDhcsrSRegRdy   = 0x00010000u,
    kDhcsrSHalt     = 0x00020000u,
    kDhcsrSSleep    = 0x00040000u,
    kDhcsrSLockup   = 0x00080000u,
};

// ---- DEMCR ---------------------------------------------------------------
enum : uint32_t {
    kDemcrVcCoreReset = 0x00000001u,
    kDemcrVcHalt      = 0x00000100u,
    kDemcrTrcEna      = 0x01000000u,
};

// ---- DWT: data watchpoints (v6-M/v7-M function codes) --------------------
// DWT control/compare registers. Comparator n sits at kDwtComp + 16*n, with
// MASK at +4 and FUNCTION at +8. DWT comparators only match while DEMCR.TRCENA
// (kDemcrTrcEna) is set, so arming a watchpoint must set it first.
enum : uint32_t {
    kDwtCtrl         = kDwtBase + 0x00u,
    kDwtPcsr         = kDwtBase + 0x1Cu,
    kDwtComp         = kDwtBase + 0x20u,   // + 16*n
    kDwtMask         = kDwtBase + 0x24u,   // + 16*n
    kDwtFunc         = kDwtBase + 0x28u,   // + 16*n

    kDwtNumCompMask  = 0xF0000000u,        // DWT_CTRL[31:28]
    kDwtNumCompPos   = 28,
    kDwtFuncMask     = 0x0000000Fu,
    kDwtFuncDisabled = 0x00000000u,
    kDwtWpRead       = 0x00000005u,        // watchpoint on data read
    kDwtWpWrite      = 0x00000006u,        // watchpoint on data write
    kDwtWpReadWrite  = 0x00000007u,        // watchpoint on data read or write
    kDwtMatched      = 0x01000000u,        // FUNCTION.MATCHED (set on hit)
    kDwtMaskMax      = 0x0000001Fu,        // DWT_MASK is 5 bits
};

// ---- DCRSR register selectors -------------------------------------------
enum : uint32_t {
    kDcrsrRegWnR = 0x00010000u,
    kSelR0       = 0x00, // .. kSelR15 = 0x0F
    kSelXpsr     = 0x10,
    kSelMsp      = 0x11,
    kSelPsp      = 0x12,
    kSelSys      = 0x14, // CONTROL / FAULTMASK / BASEPRI / PRIMASK
    kSelFpuS0    = 0x40, // .. 0x5F  = S0..S31 (M4F / M7)
    kSelFpuFpscr = 0x60,
};

// AIRCR reset requests: VECTKEY (0x05FA) | SYSRESETREQ / VECTRESET.
enum : uint32_t {
    kAircrSysResetReq = 0x05FA0004u,
    kAircrVectReset   = 0x05FA0001u,
};

enum class CpuState { Unknown, Running, Halted, Lockup, Sleeping };

class Core {
public:
    Core();
    ~Core();

    // The Dap must already be Connected().
    //
    // `halt` is the Debug page's "Connect: without Stop": C_DEBUGEN is still
    // written (the debug logic has to be enabled to read anything at all) but
    // C_HALT is left clear, so a target that was already running keeps running.
    // uVision then uses AG_WAITFORBRK to notice when it stops.
    bool Attach(Dap* dap, bool halt = true);
    bool IsAttached() const { return m_attached; }

    // ---- run control ------------------------------------------------------
    bool Halt();
    bool Resume();
    bool Step();
    bool Reset(bool haltAfterReset);

    // Drops DHCSR.C_STEP | C_MASKINTS with C_HALT still asserted, then reads
    // DHCSR back and confirms the mask is gone.
    //
    // C_MASKINTS is what Step() arms (Keil's sequence), and on ARMv7-M it can
    // only change while the core is in debug state - i.e. in a write that also
    // sets C_HALT.  Resume()'s write has C_HALT = 0, so the hardware ignores
    // it and the mask survives the resume; the target then runs with interrupts
    // masked and a tick-driven main loop spins forever.  Hence Step() calls
    // this when the step completes and Resume() calls it before letting go.
    //
    // Returns false when DHCSR could not be written or the read-back still
    // shows C_MASKINTS: the caller must not report a clean stop / run then.
    bool ClearStepMaskInts();

    CpuState State();
    bool IsHalted();
    bool WaitForHalt(uint32_t timeoutMs);

    // ---- registers --------------------------------------------------------
    // sel: kSelR0..kSelR15 / kSelXpsr / kSelMsp / kSelPsp / kSelSys / FPU sels.
    bool ReadRegSel(uint32_t sel, uint32_t& value);
    bool WriteRegSel(uint32_t sel, uint32_t value);

    bool ReadRegs(RgARMCM& r);       // GPR + xPSR + MSP + PSP + SYS
    bool WriteRegs(const RgARMCM& r);

    // FPU registers (Cortex-M4F / M7), the RgARMFPU block AG_AllReg(AG_READFPU)
    // has to answer with.  Best effort on read: a target without an FPU leaves
    // the block zeroed and still reports success.  uVision asks for these
    // whenever the device has one, and answering AG_INVALOP there is printed as
    // "*** error 123: AGDI: invalid operation" - the whole register refresh then
    // fails, which is what leaves the Registers window empty.
    bool ReadFpuRegs(RgARMFPU& f);
    bool WriteFpuRegs(const RgARMFPU& f);

    // ---- helpers ----------------------------------------------------------
    bool ReadCoreDebug(uint32_t& dhcsr, uint32_t& demcr);

    // Logical register index (as used by AG_RegAcc / AR_n*) <-> DCRSR selector.
    static bool RegIndexToSel(uint32_t regIndex, uint32_t& sel);

private:
    bool WaitRegReady(uint32_t timeoutMs = 500);

    Dap* m_dap;
    bool m_attached;
    bool m_halted;
};

} // namespace orb
