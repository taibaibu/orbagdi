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
        // "Connect: without Stop"：置 C_DEBUGEN 让 DHCSR 可读，但不置 C_HALT，
        // 已在运行的目标继续运行 —— 这正是该模式的意义。
        if (!m_dap->MemWrite32(kDcbDhcsr, kDhcsrDbgKey | kDhcsrCDebugEn)) {
            ORB_LOG_WARN("Core: could not enable debug without halting");
            return false;
        }
        m_halted = false;
        ORB_LOG_INFO("Core: attached without stopping the target");
        return true;
    }

    // 开调试：C_DEBUGEN（整会话保持）+ halt。
    // halt 失败必须致命：DCRSR/DCRDR 只在 debug 态生效，下游（Flash 算法、寄存器读、单步）
    // 全都假设核心已停。这里报成功正是"附着成功、之后每次访问都失败"的成因。
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

    // 没确认的 halt 不算 halt：CallAlg() 是唯一检查它的调用方，返回 true 就去写 DCRSR/DCRDR，
    // 而硬件只在 debug 态接受 ⇒ 配置被静默丢弃、page 0 失败（实测是单次传输丢失，重跑就好）。
    // 故重试一次 DHCSR 写入 + 等待，仍不确认就如实回报调用方。
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

// Step() 布下的 C_MASKINTS 的对手方。清必须趁 C_HALT 还置着：ARMv7-M 上 C_MASKINTS 只在
// debug 态可写，Resume() 的写（C_HALT=0）不会把它清掉。
// 症状：单步离开断点后再也到不了下一个 —— SysTick 被屏蔽，等 tick 的固件空转。
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

    // 照抄 Keil 的 Step() 序列：halt 态写 C_DEBUGEN|C_HALT|C_MASKINTS → 单步写
    // C_DEBUGEN|C_STEP|C_MASKINTS → 轮询 S_HALT 50 ms。
    // 三个曾出错的地方：① C_MASKINTS 从没置过 ⇒ 挂起中断被先响应（"单步乱跑"）；
    // ② 单步那次写不能带 C_HALT（带了 S_HALT 不落、一条都不执行）；③ 落步后必须清 C_MASKINTS。
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

    // 上面两次 DHCSR 写都带了 C_MASKINTS，它不会自己清，Resume() 也清不掉（那次写 C_HALT=0）
    // ⇒ 中断一直被屏蔽："运行再也到不了下一个断点"，SysTick 冻住。
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

    // ---- 器件描述里有自己的复位序列时：用它 ----
    // pack 知道通用 AIRCR 写不知道的事（哪个时钟保活、先解锁谁、nRESET 是否到内核）。
    // "Reset: Autodetect" 即"有描述就用描述"，显式模式则只要那一条序列。
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

            // 参考在 PDSC reset 之后立刻重新确认调试访问（先给 bootloader 让路，再 DebugAccessEnsure()，
            // 失败即 EU24）。必须在这里：reset 把目标拉回未知状态（时钟、C_DEBUGEN、DBGMCU 都可能变），
            // 拿 reset 前的旧等级判断后续访问，等于用一个已经不成立的假设。
            {
                agdi::Context& ctx = agdi::Get();
                if (agdi::DebugAccessEnsure(*m_dap, *this, &ctx.bps) != 0) {
                    ORB_LOG_WARN("Core: reset 之后调试访问未恢复到 kAccNormal");
                }
            }
            return true;
        }

        // 描述里没有该模式的复位序列：撤销 catch 设置的项，走下面的经典路径。
        // 要记日志 —— "pack 没有序列"与"序列跑了"从外部看不出区别（目标最终都在跑）。
        ORB_LOG_INFO("Core: PDSC has no reset for mode %d (type=%u) - classic path",
                     opt.reset, type);
        if (catchTaken) orb::pdsc::ResetCatchClear();
    }

    // ---- 经典路径：复位方式来自 Debug 页的 "Reset:" ----
    // VECTRESET 只复位内核，HW RESET 经探针拉 nRESET，两个系统选项都写 AIRCR.SYSRESETREQ，
    // 差别只在 SoC 复位哪些外设（调试器管不着）。
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
    const uint64_t start = GetTickCount64();
    for (;;) {
        uint32_t dhcsr = 0;
        if (m_dap->MemRead32(kDcbDhcsr, dhcsr)) {
            if (dhcsr & kDhcsrSLockup) return true; // locked up counts as stopped
            if (dhcsr & kDhcsrSHalt)   return true;
        }
        if (GetTickCount64() - start >= timeoutMs) return false;
    }
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
bool Core::WaitRegReady(uint32_t timeoutMs) {
    const uint64_t start = GetTickCount64();
    for (;;) {
        uint32_t dhcsr = 0;
        if (m_dap->MemRead32(kDcbDhcsr, dhcsr)) {
            if (dhcsr & kDhcsrSRegRdy) return true;
        }
        if (GetTickCount64() - start >= timeoutMs) return false;
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
// FPU 寄存器（RgARMFPU: S0..S31 然后 FPSCR）
// 与核心寄存器同一对 DCRSR/DCRDR，选择子范围见头文件（kSelFpuS0 .. +31，然后 Fpscr）。
// 读是有意做成 best effort：第一个失败的选择子就当"没有 FPU"，回零结构 + AG_OK，
// 而不是 AG_INVALOP（那会让 uVision 整个寄存器刷新中止）。
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
        // FPSCR 的 AGDI 寄存器 id 是 0x1F（COLLECT.H 的 nFPSCR，AGDI.CPP 的寄存器分支用它）；
        // AGDI.H 里没有 AR_nFPSCR —— 它的 v8-M 列表到 0x1E 为止。
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
