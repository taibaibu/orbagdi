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
// 比较器 n 位于 kDwtComp + 16*n（MASK +4、FUNCTION +8）。
// DWT 只在 DEMCR.TRCENA 置位时才匹配，装观察点前必须先开。
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

    // 连接内核；Dap 必须已 Connected()。
    // halt=false 对应“Connect without Stop”：只写 C_DEBUGEN 不写 C_HALT，
    // 让本来在跑的目标继续跑，由 uVision 用 AG_WAITFORBRK 等它停下。
    bool Attach(Dap* dap, bool halt = true);
    bool IsAttached() const { return m_attached; }

    // ---- run control ------------------------------------------------------
    bool Halt();
    bool Resume();
    bool Step();
    bool Reset(bool haltAfterReset);

    // 在保持 C_HALT 的情况下清掉 C_STEP | C_MASKINTS，并回读确认。
    // C_MASKINTS 只能在 debug 态（同写 C_HALT）改，Resume() 的写会被硬件忽略，
    // 漏清会让目标带着屏蔽中断跑、tick 主循环永远空转。
    // 写失败或回读仍带屏蔽时返回 false，调用方不能报告干净的停/跑。
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

    // FPU 寄存器（M4F / M7），对应 AG_AllReg(AG_READFPU)。
    // 读尽力而为：没有 FPU 的目标返回全 0 但不报错，否则整个寄存器刷新会失败。
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
