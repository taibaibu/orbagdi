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

namespace orb {

// AG_MemAtt() 属性位（自定义，ARM 没有提供）。
// 用 constexpr 而不是宏：BOM.H 里有同名但取值不同的 ATR_* 宏，宏会被先包含者
// 静默覆盖，命名空间常量则不会被 #define 遮蔽。
inline constexpr uint32_t kAtrRead  = 0x00000001u;
inline constexpr uint32_t kAtrWrite = 0x00000002u;
inline constexpr uint32_t kAtrExec  = 0x00000004u;
inline constexpr uint32_t kAtrBreak = 0x00000008u;
inline constexpr uint32_t kAtrNomap = 0x00000010u;

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

    // FPB 版本（FP_CTRL[31:28]）。
    // rev0 的比较器只能覆盖 0x20000000 以下，RAM 断点必须退化成软件断点。
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

    // 调试访问恢复后按槽位重装比较器。
    // 不重探容量：链路修复不改变器件能力，重探会打乱 uVision 已排好的布局。
    bool Recover();

    // 卸下全部断点/观察点但保留条目（AG_BPDISALL 是禁用不是删除）。
    // 早期误用 Clear() 会让 uVision 的标记跟着属性表一起消失。
    // 返回被禁用的数量。
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
