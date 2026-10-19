#include "pch.h"
#include "ORBAGDI_Agdi.h"
#include "ORBAGDI_DebugAccess.h"
#include "ORBAGDI_Pdsc.h"

#include <string.h>

namespace orb {

Core::Core() : m_dap(nullptr), m_attached(false), m_halted(false) {}
Core::~Core() {}

bool Core::Attach(Dap* dap, bool halt) {
    if (!dap || !dap->IsConnected()) return false;
    m_dap = dap;
    m_attached = true;

    if (!halt) {
        // "Connect: without Stop": enable the debug logic and leave the core
        // alone.  C_DEBUGEN has to be set or DHCSR reads report nothing useful,
        // but C_HALT stays clear so a target that was already running keeps
        // running - which is the whole point of the mode.
        if (!m_dap->MemWrite32(kDcbDhcsr, kDhcsrDbgKey | kDhcsrCDebugEn)) {
            ORB_LOG_WARN("Core: could not enable debug without halting");
            return false;
        }
        m_halted = false;
        ORB_LOG_INFO("Core: attached without stopping the target");
        return true;
    }

    // Enable debug: C_DEBUGEN (kept set for the whole session) + halt.
    //
    // A failed halt is fatal, not a warning.  Everything downstream - the flash
    // algorithm's DCRSR writes, the register reads, the single step - assumes a
    // stopped core, and DCRSR/DCRDR are only honoured in debug state.  Reporting
    // success here is what let uVision print "target attached" and then fail on
    // every access with the real cause far away from the message (bug.md E2).
    if (!Halt()) {
        // One retry: a core that is mid-reset, or only just released from
        // nRESET, can refuse the first C_HALT write.
        Sleep(10);
        if (!Halt()) {
            ORB_LOG_ERROR("Core: cannot halt the core - the session cannot be used");
            m_attached = false;
            return false;
        }
        ORB_LOG_INFO("Core: halt succeeded on the retry");
    }
    return true;
}

// ---------------------------------------------------------------------------
// DHCSR helpers
// ---------------------------------------------------------------------------
bool Core::ReadCoreDebug(uint32_t& dhcsr, uint32_t& demcr) {
    if (!m_attached) return false;
    if (!m_dap->MemRead32(kDcbDhcsr, dhcsr)) return false;
    if (!m_dap->MemRead32(kDcbDemcr, demcr)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Run control
// ---------------------------------------------------------------------------
bool Core::Halt() {
    if (!m_attached) return false;
    const uint32_t v = kDhcsrDbgKey | kDhcsrCDebugEn | kDhcsrCHalt;
    if (!m_dap->MemWrite32(kDcbDhcsr, v)) return false;

    // A halt that does not confirm is *not* a halt, and saying otherwise is what
    // let one transient MEM-AP failure turn into a failed flash download:
    // CallAlg() is the only caller that checks this result, and with `true`
    // returned it went on to write DCRSR/DCRDR - which the hardware only honours
    // in debug state - so the setup was silently dropped, the "armed" check
    // failed, and the pass aborted with the misleading
    // "program page 0x... failed (0)" (the 0 being the algorithm's untouched r0).
    // Measured on an STM32F407: the program pass died at page 0x08007C00 after
    // 31 good pages, and an identical re-run passed - i.e. the underlying failure
    // is a single lost transfer, not a target that refuses to stop
    // (COMPAT_ANALYSIS.md §10.19).
    //
    // Hence the retry: one more DHCSR write + wait recovers the common case, and
    // if it still does not confirm the caller must be told the truth.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (WaitForHalt(1000)) {
            m_halted = true;
            return true;
        }
        if (attempt == 0) {
            ORB_LOG_WARN("Core: halt did not confirm (S_HALT) - retrying");
            m_dap->MemWrite32(kDcbDhcsr, v);
        }
    }

    ORB_LOG_WARN("Core: halt did not confirm (S_HALT) after 2 attempts");
    m_halted = false;
    return false;
}

// Counterpart of the C_MASKINTS that Step() arms.  See the header for why the
// clear has to happen while C_HALT is still set: on ARMv7-M C_MASKINTS is only
// writable in debug state, so Resume()'s `C_DEBUGEN` write (C_HALT = 0) leaves
// the mask exactly where Step() put it.  The measured symptom was a run that
// stepped off a breakpoint and then never reached the next one - SysTick was
// masked, so a firmware waiting on its tick counter spun forever.
bool Core::ClearStepMaskInts() {
    if (!m_attached || !m_dap) return false;

    uint32_t before = 0;
    if (!m_dap->MemRead32(kDcbDhcsr, before)) {
        ORB_LOG_WARN("Core: DHCSR unreadable - cannot tell whether C_MASKINTS is set");
        return false;
    }

    // Cheap path: nothing to drop.  DHCSR is one MEM-AP round trip and Resume()
    // runs hundreds of times per flash download.
    if ((before & (kDhcsrCStep | kDhcsrCMaskInts)) == 0u) return true;

    // Read-modify-write: keep C_DEBUGEN, assert C_HALT, and write neither
    // C_STEP nor C_MASKINTS.  C_HALT must be in the same write - that is the
    // precondition for changing C_MASKINTS at all.
    const uint32_t v = kDhcsrDbgKey | kDhcsrCDebugEn | kDhcsrCHalt;
    if (!m_dap->MemWrite32(kDcbDhcsr, v)) {
        ORB_LOG_WARN("Core: could not write DHCSR to clear C_MASKINTS (was 0x%08X)", before);
        return false;
    }
    m_halted = true;                        // the write just halted the core

    uint32_t after = 0;
    if (!m_dap->MemRead32(kDcbDhcsr, after)) {
        ORB_LOG_WARN("Core: DHCSR unreadable after clearing C_MASKINTS (was 0x%08X); "
                     "the clear cannot be confirmed", before);
        return false;
    }
    if ((after & kDhcsrCMaskInts) != 0u) {
        // Do not paper over it: running on from here means running with
        // interrupts masked, which is exactly the freeze this fixes.
        ORB_LOG_ERROR("Core: C_MASKINTS still set after the clear (DHCSR read back "
                      "0x%08X, wrote 0x%08X) - the target cannot be resumed cleanly",
                      after, v);
        return false;
    }
    return true;
}

bool Core::Resume() {
    if (!m_attached) return false;

    // The write below has C_HALT = 0, which the hardware does not allow to
    // change C_MASKINTS.  Drop it first, while the core is still in debug state
    // (Core::Step leaves it armed; see ClearStepMaskInts()).
    if (!ClearStepMaskInts()) {
        ORB_LOG_ERROR("Core: run refused - C_MASKINTS could not be cleared, the "
                      "target would run with interrupts masked");
        return false;
    }

    const uint32_t v = kDhcsrDbgKey | kDhcsrCDebugEn;
    if (!m_dap->MemWrite32(kDcbDhcsr, v)) return false;
    m_halted = false;
    // The target is about to run: nothing that was read stays true.
    m_dap->CacheFlush();
    return true;
}

bool Core::Step() {
    if (!m_attached) return false;

    // Keil's sequence, exactly (DbgCM/AGDI.CPP:2705 Step()):
    //   1. while halted: C_DEBUGEN | C_HALT | C_MASKINTS   (:2741-2748)
    //   2. the step:     C_DEBUGEN | C_STEP | C_MASKINTS   (:2784)
    //   3. poll DHCSR for S_HALT, 50 ms                    (:2814-2826)
    //
    // Three details this driver had wrong, all visible in a session:
    //   - C_MASKINTS was never set.  Without it a pending interrupt is taken
    //     instead of the single instruction, so "one step" ends up in a handler
    //     somewhere else - the "stepping floats around" symptom.  The constant
    //     was defined here and simply never used.
    //   - C_HALT must NOT be set on the step write itself.  Setting it leaves
    //     the core halted: S_HALT never drops and the step executes nothing
    //     (measured: six AG_NSTEP calls, PC frozen at 0x08000D8C).
    //   - C_MASKINTS must be dropped again once the step has landed.  It does
    //     not clear itself, and Resume() cannot clear it (see
    //     ClearStepMaskInts()), so leaving it costs the next run all of its
    //     interrupts - see the tail of this function.
    const uint32_t armed = kDhcsrDbgKey | kDhcsrCDebugEn | kDhcsrCHalt | kDhcsrCMaskInts;
    if (!m_dap->MemWrite32(kDcbDhcsr, armed)) return false;

    const uint32_t step = kDhcsrDbgKey | kDhcsrCDebugEn | kDhcsrCStep | kDhcsrCMaskInts;
    if (!m_dap->MemWrite32(kDcbDhcsr, step)) {
        ClearStepMaskInts();                // the `armed` write already set it
        return false;
    }
    m_halted = false;

    // 50 ms, as the reference uses.  A step that has not halted by then is not
    // going to, and the 2 s this used to wait is what made stepping feel slow.
    if (!WaitForHalt(50)) {
        ORB_LOG_WARN("Core: single step not confirmed within 50 ms");
        ClearStepMaskInts();                // best effort: never leave the mask armed
        return false;
    }
    m_halted = true;

    // Both DHCSR writes above carried C_MASKINTS, and nothing clears it by
    // itself.  Left armed, the next Resume() cannot drop it (its write has
    // C_HALT = 0), so a run that stepped off a breakpoint would continue with
    // interrupts masked - measured as "run never reaches the next breakpoint",
    // with SysTick frozen and the firmware spinning on a tick that never comes.
    if (!ClearStepMaskInts()) {
        ORB_LOG_ERROR("Core: single step completed but C_MASKINTS could not be "
                      "cleared - the core is halted with interrupts masked");
        return false;
    }

    m_dap->CacheFlush();                    // the step executed real code
    return true;
}

bool Core::Reset(bool haltAfterReset) {
    if (!m_attached) return false;

    const cfg::Config& opt = cfg::Get();

    // ---- the device's own reset, when its description has one --------------
    //
    // A pack that carries ResetHardware / ResetSystem / ResetCustomized knows
    // things the generic AIRCR write does not: which clock has to keep running,
    // which register must be unlocked first, whether nRESET even reaches the
    // core.  "Reset: Autodetect" is precisely "use the description when there is
    // one"; an explicit mode asks for that one sequence and nothing else.
    {
        uint32_t type = orb::pdsc::kResetErr;              // Autodetect
        if (opt.reset == cfg::kResetHwReset) {
            type = orb::pdsc::kResetHw;
        } else if (opt.reset == cfg::kResetVectReset) {
            type = orb::pdsc::kResetVec;
        } else if (opt.reset == cfg::kResetSysResetReq ||
                   opt.reset == cfg::kResetCoreAndPeripheral) {
            type = orb::pdsc::kResetSys;
        }

        // "Stop after reset" is the description's ResetCatchSet.  When it has no
        // such sequence the halt is arranged the classic way instead, by
        // pre-loading C_HALT before the reset lands.
        const bool catchTaken = orb::pdsc::ResetCatchSet();
        if (haltAfterReset && !catchTaken) {
            m_dap->MemWrite32(kDcbDhcsr, kDhcsrDbgKey | kDhcsrCDebugEn | kDhcsrCHalt);
        }

        if (orb::pdsc::Reset(type)) {
            ORB_LOG_INFO("Core: reset = PDSC sequence (type=%u, catch=%s)",
                         type, catchTaken ? "ResetCatchSet" : "classic C_HALT");
            if (!haltAfterReset) orb::pdsc::ResetCatchClear();
            m_dap->CacheFlush();            // memory is about to change wholesale
            Sleep(20);
            if (haltAfterReset) WaitForHalt(2000);
            m_halted = haltAfterReset;

            // 参考在 PDSC reset 之后**立刻**重新确认调试访问
            // （`DbgCM/PDSCDebug.cpp:4920-4932`：先给 bootloader 让路再
            // `DebugAccessEnsure()`，失败即 EU24 "Cannot recover from reset"）。
            //
            // 为什么必须在这里：reset 把目标拉回未知状态 —— 时钟、C_DEBUGEN、
            // DBGMCU 都可能变了，链路等级必须重新判定；拿 reset 之前的旧等级去判断
            // 后续访问，等于用一个已经不成立的假设。
            {
                agdi::Context& ctx = agdi::Get();
                if (agdi::DebugAccessEnsure(*m_dap, *this, &ctx.bps) != 0) {
                    ORB_LOG_WARN("Core: reset 之后调试访问未恢复到 kAccNormal");
                }
            }
            return true;
        }

        // The description has no reset for this mode: undo whatever the catch set
        // and take the classic path below.
        //
        // Logged because "the pack had no sequence" and "the sequence ran" are
        // otherwise indistinguishable from the outside - the target ends up
        // running either way.  That is the same silent-failure shape as bug.md
        // §N (the reset enum), and it cost a round of confusion there.
        ORB_LOG_INFO("Core: PDSC has no reset for mode %d (type=%u) - classic path",
                     opt.reset, type);
        if (catchTaken) orb::pdsc::ResetCatchClear();
    }

    // ---- classic path ------------------------------------------------------
    //
    // How to reset comes from the Debug page ("Reset:" combo).  VECTRESET only
    // resets the core, HW RESET pulses nRESET through the probe, and the two
    // system choices write AIRCR.SYSRESETREQ - what differs between them is
    // which peripherals the SoC resets, which the debugger cannot influence.
    const bool hwReset   = (opt.reset == cfg::kResetHwReset);
    const bool vectReset = (opt.reset == cfg::kResetVectReset);

    uint32_t demcr = 0;
    if (!m_dap->MemRead32(kDcbDemcr, demcr)) return false;
    demcr |= kDemcrVcCoreReset;             // vector catch on reset
    if (!m_dap->MemWrite32(kDcbDemcr, demcr)) return false;

    if (haltAfterReset) {
        // Pre-load C_HALT, then trigger the reset.
        m_dap->MemWrite32(kDcbDhcsr, kDhcsrDbgKey | kDhcsrCDebugEn | kDhcsrCHalt);
    }

    if (hwReset) {
        // The pulse and the AIRCR writes below are the whole difference between
        // the modes, and the DAP access log only records address + length (not the
        // value), so which one ran has to be stated here.
        ORB_LOG_INFO("Core: reset = HW RESET (nRESET pulse, SWJ_Pins pin 0x80)");
        if (!m_dap->ResetPin(true)) {
            ORB_LOG_WARN("Core: HW RESET requested but the probe cannot drive nRESET");
            return false;
        }
        Sleep(20);
        m_dap->ResetPin(false);
    } else {
        const uint32_t aircr = vectReset ? kAircrVectReset : kAircrSysResetReq;
        ORB_LOG_INFO("Core: reset = %s (AIRCR 0x%08X <- 0x%08X)",
                     vectReset ? "VECTRESET" : "SYSRESETREQ", kScsAircr, aircr);
        if (!m_dap->MemWrite32(kScsAircr, aircr)) return false;
    }

    m_dap->CacheFlush();                    // memory is about to change wholesale

    // Give the core time to reset and re-lock the debug logic.
    Sleep(20);
    if (haltAfterReset) {
        WaitForHalt(2000);
    }

    // Stop vector-catching reset on every subsequent run.
    demcr &= ~kDemcrVcCoreReset;
    m_dap->MemWrite32(kDcbDemcr, demcr);

    m_halted = haltAfterReset;
    return true;
}

CpuState Core::State() {
    uint32_t dhcsr = 0;
    if (!m_attached || !m_dap->MemRead32(kDcbDhcsr, dhcsr)) return CpuState::Unknown;
    if (dhcsr & kDhcsrSLockup) return CpuState::Lockup;
    if (dhcsr & kDhcsrSHalt)   return CpuState::Halted;
    if (dhcsr & kDhcsrSSleep)  return CpuState::Sleeping;
    return CpuState::Running;
}

bool Core::IsHalted() {
    uint32_t dhcsr = 0;
    if (!m_attached || !m_dap->MemRead32(kDcbDhcsr, dhcsr)) return false;
    return (dhcsr & kDhcsrSHalt) != 0;
}

bool Core::WaitForHalt(uint32_t timeoutMs) {
    if (!m_attached) return false;
    const DWORD start = GetTickCount();
    for (;;) {
        uint32_t dhcsr = 0;
        if (m_dap->MemRead32(kDcbDhcsr, dhcsr)) {
            if (dhcsr & kDhcsrSLockup) return true; // locked up counts as stopped
            if (dhcsr & kDhcsrSHalt)   return true;
        }
        if (GetTickCount() - start >= timeoutMs) return false;
    }
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
bool Core::WaitRegReady(uint32_t timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        uint32_t dhcsr = 0;
        if (m_dap->MemRead32(kDcbDhcsr, dhcsr)) {
            if (dhcsr & kDhcsrSRegRdy) return true;
        }
        if (GetTickCount() - start >= timeoutMs) return false;
    }
}

bool Core::ReadRegSel(uint32_t sel, uint32_t& value) {
    if (!m_attached) return false;
    if (!m_dap->MemWrite32(kDcbDcrsr, sel)) return false;
    if (!WaitRegReady()) return false;
    return m_dap->MemRead32(kDcbDcrdr, value);
}

bool Core::WriteRegSel(uint32_t sel, uint32_t value) {
    if (!m_attached) return false;
    if (!m_dap->MemWrite32(kDcbDcrdr, value)) return false;
    if (!m_dap->MemWrite32(kDcbDcrsr, sel | kDcrsrRegWnR)) return false;
    return WaitRegReady();
}

bool Core::ReadRegs(RgARMCM& r) {
    memset(&r, 0, sizeof(r));
    uint32_t v = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (!ReadRegSel(i, v)) return false;
        r.Rn[i] = v;
    }
    if (!ReadRegSel(kSelXpsr, v)) return false;
    r.xPSR = v;
    if (!ReadRegSel(kSelMsp, v)) return false;
    r.MSP = v;
    if (!ReadRegSel(kSelPsp, v)) return false;
    r.PSP = v;
    if (!ReadRegSel(kSelSys, v)) return false;
    r.SYS.P = v;
    r.DSP = 0;
    r.nCycles = 0;
    return true;
}

bool Core::WriteRegs(const RgARMCM& r) {
    for (uint32_t i = 0; i < 16; ++i) {
        if (!WriteRegSel(i, r.Rn[i])) return false;
    }
    if (!WriteRegSel(kSelXpsr, r.xPSR)) return false;
    if (!WriteRegSel(kSelMsp,  r.MSP))  return false;
    if (!WriteRegSel(kSelPsp,  r.PSP))  return false;
    if (!WriteRegSel(kSelSys,  r.SYS.P)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// FPU registers (RgARMFPU: S0..S31 then FPSCR)
//
// The FP extension registers are transferred through the same DCRSR/DCRDR pair
// as the core registers, using the selector range the header already documents
// for M4F/M7 (kSelFpuS0 .. kSelFpuS0+31, then kSelFpuFpscr).
//
// A read is best effort by design: the very first failing selector means "this
// target has no FPU" (or does not expose it through DCRSR), and the right answer
// to uVision is a zeroed structure with AG_OK - not AG_INVALOP, which aborts its
// whole register refresh.
// ---------------------------------------------------------------------------
bool Core::ReadFpuRegs(RgARMFPU& f) {
    memset(&f, 0, sizeof(f));
    if (!m_attached) return false;

    uint32_t v = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        if (!ReadRegSel(kSelFpuS0 + i, v)) {
            ORB_LOG_WARN("Core: FPU S%u not readable (DCRSR sel 0x%02X) - the "
                         "target may have no FPU", i, kSelFpuS0 + i);
            return true;
        }
        f.Sn[i] = v;
    }
    if (!ReadRegSel(kSelFpuFpscr, v)) {
        ORB_LOG_WARN("Core: FPSCR not readable (DCRSR sel 0x%02X)", kSelFpuFpscr);
        return true;
    }
    f.FPSCR = v;
    return true;
}

bool Core::WriteFpuRegs(const RgARMFPU& f) {
    if (!m_attached) return false;
    for (uint32_t i = 0; i < 32; ++i) {
        if (!WriteRegSel(kSelFpuS0 + i, f.Sn[i])) return false;
    }
    return WriteRegSel(kSelFpuFpscr, f.FPSCR);
}

bool Core::RegIndexToSel(uint32_t regIndex, uint32_t& sel) {
    if (regIndex <= AR_nR15) { sel = regIndex; return true; }
    switch (regIndex) {
        case AR_nPSR: sel = kSelXpsr; return true;
        case AR_nMSP: sel = kSelMsp;  return true;
        case AR_nPSP: sel = kSelPsp;  return true;
        case AR_nSYS: sel = kSelSys;  return true;
        // FPSCR.  Its AGDI register id is 0x1F (DbgCM/COLLECT.H:216 defines
        // nFPSCR as 0x1F, and DbgCM/AGDI.CPP:5079 uses it as the case label of
        // the register-access switch this function mirrors).  AGDI.H itself has
        // no AR_nFPSCR - its v8-M list ends at 0x1E.
        case 0x1Fu:   sel = kSelFpuFpscr; return true;
        default: break;
    }
    if (regIndex >= AR_nS0 && regIndex <= AR_nS31) {
        sel = kSelFpuS0 + (regIndex - AR_nS0);
        return true;
    }
    return false;
}

} // namespace orb
