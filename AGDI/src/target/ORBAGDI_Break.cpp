#include "pch.h"

#include <string.h>

namespace orb {

namespace {
constexpr uint32_t kFpCtrl    = kFpbBase + 0x00;
constexpr uint32_t kFpComp0   = kFpbBase + 0x08;
constexpr uint32_t kFpCtrlKey = 0x00000002u; // write KEY to modify CTRL
constexpr uint32_t kFpCtrlEn  = 0x00000001u;
constexpr uint16_t kBkpt      = 0xBE00u;     // Thumb BKPT #0
} // namespace

Breakpoints::Breakpoints()
    : m_used(0), m_hwSlots(0), m_swUsed(0), m_wpSlots(0), m_fpbOn(false), m_trcEna(false),
      m_dap(nullptr), m_core(nullptr) {
    memset(m_slots, 0, sizeof(m_slots));
}

void Breakpoints::Init(Dap* dap, Core* core) {
    m_dap = dap;
    m_core = core;
}

void Breakpoints::ResetDetection() {
    m_hwSlots = 0;
    m_wpSlots = 0;
    m_fpbOn   = false;
    m_trcEna  = false;
}

// ---------------------------------------------------------------------------
// FPB probing
// ---------------------------------------------------------------------------
bool Breakpoints::ProbeFpb() {
    if (!m_dap || !m_dap->IsConnected()) { m_hwSlots = 0; return false; }

    uint32_t ctrl = 0;
    if (!m_dap->MemRead32(kFpCtrl, ctrl)) {
        ORB_LOG_INFO("Bp: no FPB (read failed) -> 0 hardware breakpoints");
        m_hwSlots = 0;
        return false;
    }
    // NUM_CODE lives in bits [7:4]; NUM_LIT in [14:12].
    uint32_t numCode = (ctrl >> 4) & 0x0Fu;
    uint32_t numLit  = (ctrl >> 12) & 0x0Fu;
    m_fpbRev = static_cast<int>((ctrl >> 28) & 0x0Fu);
    ORB_LOG_INFO("Bp: FP_CTRL = 0x%08X (REV=%d, NUM_CODE=%u, NUM_LIT=%u)",
                 ctrl, m_fpbRev, numCode, numLit);

    // A couple of cores report "count - 1"; clamp to a sane maximum.
    m_hwSlots = (numCode > 16) ? 16 : static_cast<int>(numCode);
    m_fpbOn = false;
    return m_hwSlots > 0;
}

// ---------------------------------------------------------------------------
// DWT 探测（数据观察点）
// v6-M/v7-M 都是 NUM_COMP[31:28] 布局。比较器只在 DEMCR.TRCENA 置位时匹配，所以一起探测：
// 装了却不触发的观察点，在 uVision 看来和坏掉的一样。
// ---------------------------------------------------------------------------
bool Breakpoints::ProbeDwt() {
    m_wpSlots = 0;
    if (!m_dap || !m_dap->IsConnected()) return false;

    uint32_t ctrl = 0;
    if (!m_dap->MemRead32(kDwtCtrl, ctrl)) {
        ORB_LOG_INFO("Bp: no DWT (read failed) -> 0 data watchpoints");
        return false;
    }
    const uint32_t num = (ctrl & kDwtNumCompMask) >> kDwtNumCompPos;
    m_wpSlots = (num > 16u) ? 16 : static_cast<int>(num);
    ORB_LOG_INFO("Bp: DWT_CTRL = 0x%08X (NUM_COMP=%u)", ctrl, num);

    if (m_wpSlots > 0) {
        uint32_t demcr = 0;
        if (m_dap->MemRead32(kDcbDemcr, demcr)) {
            if (demcr & kDemcrTrcEna) {
                m_trcEna = true;
            } else if (m_dap->MemWrite32(kDcbDemcr, demcr | kDemcrTrcEna)) {
                m_trcEna = true;
            } else {
                ORB_LOG_WARN("Bp: cannot set DEMCR.TRCENA; watchpoints will not fire");
            }
        }
    }
    return m_wpSlots > 0;
}

// ---------------------------------------------------------------------------
// Slot helpers
// ---------------------------------------------------------------------------
int Breakpoints::IndexOf(uint32_t addr) const {
    const uint32_t a = addr & ~1u;
    for (int i = 0; i < kMaxSlots; ++i) {
        const BpKind k = m_slots[i].kind;
        // Watchpoints live in the same table but never shadow a code address.
        if ((k == BpKind::Hw || k == BpKind::Sw) && (m_slots[i].addr & ~1u) == a) return i;
    }
    return -1;
}

bool Breakpoints::Contains(uint32_t addr) const { return IndexOf(addr) >= 0; }

// ---------------------------------------------------------------------------
// 断点地址 → FPB 比较器值（FPB rev 0）
// 只有一个编码器，安装与使能/禁用共用：曾因 InstallHw() 写 REPLACE、SetEnabled() 只写裸地址，
// 任何一次重新使能都把 REPLACE 写成 0 = "remap" ⇒ 断点安静地不再触发。
//   bit0 ENABLE；bits[31:30] 低/高半字 BKPT（由地址 bit1 选）；bits[28:2] 比较地址（0x1FFFFFFC）
// FPB rev 1（Cortex-M7）编码不同（0xFFFFFFFE、无 BKPT 位），这里不处理。
// ---------------------------------------------------------------------------
static uint32_t FpComparatorValue(uint32_t addr) {
    return (addr & 0x1FFFFFFCu) |
           ((addr & 0x02u) ? 0x80000000u   /* FPB_BKPT_H */
                           : 0x40000000u)  /* FPB_BKPT_L */
           | 1u;
}

// ---------------------------------------------------------------------------
// Hardware breakpoints (FPB)
// ---------------------------------------------------------------------------
bool Breakpoints::InstallHw(BpSlot& s) {
    if (!m_dap) return false;
    if (!m_fpbOn) {
        uint32_t ctrl = 0;
        if (!m_dap->MemWrite32(kFpCtrl, kFpCtrlKey | kFpCtrlEn)) return false;
        if (!m_dap->MemRead32(kFpCtrl, ctrl)) return false;
        // 使能后回读确认，别假设成功：编译器把值写进未使能的 FPB 就是"装好了却永不触发"，从外部看
        // 和"地址装错"一样。本会话已出现过丢失的 AP 写，重试一次再放弃。
        if (!(ctrl & kFpCtrlEn)) {
            m_dap->MemWrite32(kFpCtrl, kFpCtrlKey | kFpCtrlEn);
            if (m_dap->MemRead32(kFpCtrl, ctrl) && !(ctrl & kFpCtrlEn)) {
                ORB_LOG_ERROR("Bp: FP_CTRL = 0x%08X after enabling - ENABLE did not "
                              "stick, hardware breakpoints cannot fire", ctrl);
            } else {
                ORB_LOG_INFO("Bp: FPB enable took effect on the retry");
            }
        } else {
            ORB_LOG_INFO("Bp: FPB enabled (FP_CTRL = 0x%08X)", ctrl);
        }
        m_fpbOn = true;
    }
    const uint32_t comp = kFpComp0 + 4u * static_cast<uint32_t>(s.hwIndex);
    // FPB rev 0（M3/M4，本板 FP_CTRL[31:28]=0）不能只写地址：bits[31:30] 是 REPLACE，
    // REPLACE=0 就是 "remap the instruction" —— 比较器匹配后什么也不发生，回读还完美。
    // 断点要 bit0 ENABLE + bits[31:30] BKPT（低/高半字由地址 bit1 选）+ bits[28:2] 比较地址。
    // 这就是"断点装上了、目标却跑过去"的成因（FP_COMP0 回读 0x08024D55）。rev1 编码不同，不处理。
    const uint32_t value = FpComparatorValue(s.addr);
    if (!m_dap->MemWrite32(comp, value)) return false;
    // 回读比较器真实内容：硬件断点写的值别处看不到（RDDI 的 AP 日志只打重复写的缓冲、不打内容），
    // 没有它分不清"没使能"和"地址装错"。
    uint32_t back = 0;
    const bool readback = m_dap->MemRead32(comp, back);
    if (readback && back != value) {
        ORB_LOG_ERROR("Bp: FP_COMP%u reads back 0x%08X but 0x%08X was written",
                      static_cast<unsigned>(s.hwIndex), back, value);
    }
    ORB_LOG_INFO("Bp: FP_COMP%u <- 0x%08X (break on 0x%08X) readback %s0x%08X",
                 static_cast<unsigned>(s.hwIndex), value, s.addr,
                 readback ? "" : "FAILED ", back);
    s.kind = BpKind::Hw;
    return true;
}

bool Breakpoints::UninstallHw(BpSlot& s) {
    if (!m_dap) return false;
    const uint32_t comp = kFpComp0 + 4u * static_cast<uint32_t>(s.hwIndex);
    return m_dap->MemWrite32(comp, 0u);
}

// ---------------------------------------------------------------------------
// Software breakpoints (BKPT)
// ---------------------------------------------------------------------------
bool Breakpoints::InstallSw(BpSlot& s) {
    if (!m_dap || !m_core) return false;
    const uint32_t wordAddr = s.addr & ~3u;
    uint32_t orig = 0;
    if (!m_dap->MemRead32(wordAddr, orig)) return false;
    s.origData = orig;
    if (!m_dap->MemWrite16(s.addr & ~1u, kBkpt)) return false;
    s.kind = BpKind::Sw;
    return true;
}

bool Breakpoints::UninstallSw(BpSlot& s) {
    if (!m_dap) return false;
    return m_dap->MemWrite32(s.addr & ~3u, s.origData);
}

// ---------------------------------------------------------------------------
// 数据观察点（DWT 地址比较器）
// 比较器 n：COMP=M+0x20+16n（匹配地址）、MASK=M+0x24+16n（忽略掩码，0=精确）、
// FUNCTION=M+0x28+16n（5=读、6=写、7=读写）。数据地址匹配总是字对齐，故下面 & ~3。
// ---------------------------------------------------------------------------
int Breakpoints::FreeSlot(BpKind) {
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::None) return i;
    }
    return -1;
}

int Breakpoints::WatchIndexOf(uint32_t addr) const {
    const uint32_t a = addr & ~3u;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::Watch && (m_slots[i].addr & ~3u) == a) return i;
    }
    return -1;
}

int Breakpoints::WatchCount() const {
    int n = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::Watch) ++n;
    }
    return n;
}

bool Breakpoints::InstallWatch(BpSlot& s) {
    if (!m_dap || s.hwIndex < 0) return false;
    const uint32_t off = static_cast<uint32_t>(s.hwIndex) * 16u;
    // Disable first: a stale MATCHED flag on a live comparator would stop the
    // core immediately on the next resume.
    if (!m_dap->MemWrite32(kDwtFunc + off, kDwtFuncDisabled)) return false;
    if (!m_dap->MemWrite32(kDwtMask + off, 0u))                return false;  // exact address
    if (!m_dap->MemWrite32(kDwtComp + off, s.addr))            return false;
    if (!m_dap->MemWrite32(kDwtFunc + off, s.dwFunc))          return false;
    s.kind = BpKind::Watch;
    return true;
}

bool Breakpoints::UninstallWatch(BpSlot& s) {
    if (!m_dap || s.hwIndex < 0) return false;
    const uint32_t off = static_cast<uint32_t>(s.hwIndex) * 16u;
    return m_dap->MemWrite32(kDwtFunc + off, kDwtFuncDisabled);
}

bool Breakpoints::AddWatch(uint32_t addr, uint32_t dwFunc, int& dwtIndex) {
    dwtIndex = -1;
    if (!m_dap || m_wpSlots <= 0) return false;

    const uint32_t a = addr & ~3u;

    // Already armed at this address: refresh the access type in place.
    const int exist = WatchIndexOf(a);
    if (exist >= 0) {
        BpSlot& s = m_slots[exist];
        s.addr   = a;
        s.dwFunc = dwFunc;
        if (!InstallWatch(s)) return false;
        dwtIndex = s.hwIndex;
        return true;
    }

    int  used = 0;
    bool taken[32] = {false};
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::Watch && m_slots[i].hwIndex >= 0 &&
            m_slots[i].hwIndex < 32) {
            taken[m_slots[i].hwIndex] = true;
            ++used;
        }
    }
    if (used >= m_wpSlots) {
        ORB_LOG_WARN("Bp: no free DWT comparator for 0x%08X (have %d)", addr, m_wpSlots);
        return false;
    }

    int dwt = -1;
    for (int c = 0; c < m_wpSlots; ++c) {
        if (!taken[c]) { dwt = c; break; }
    }
    const int free = FreeSlot(BpKind::Watch);
    if (dwt < 0 || free < 0) return false;

    BpSlot& s = m_slots[free];
    s = BpSlot{};
    s.addr    = a;
    s.hwIndex = dwt;
    s.dwFunc  = dwFunc;
    if (!InstallWatch(s)) { s = BpSlot{}; return false; }

    ++m_used;
    dwtIndex = dwt;
    ORB_LOG_INFO("Bp: watchpoint 0x%08X func=%u -> DWT%02d", a, dwFunc, dwt);
    return true;
}

bool Breakpoints::RemoveWatch(uint32_t addr) {
    const int idx = WatchIndexOf(addr);
    if (idx < 0) return true;
    BpSlot& s = m_slots[idx];
    const bool ok = UninstallWatch(s);
    if (m_used > 0) --m_used;
    s = BpSlot{};
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Breakpoints::Add(uint32_t addr, bool& usedHw) {
    usedHw = false;
    if (!m_dap || !m_core) return false;
    if (IndexOf(addr) >= 0) return true;

    // Find a free slot.
    int free = -1;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::None) { free = i; break; }
    }
    if (free < 0) return false;

    BpSlot& s = m_slots[free];
    s = BpSlot{};
    s.addr = addr & ~1u;
    s.hwIndex = -1;

    // Prefer a hardware comparator when one is available.
    int hwUsed = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::Hw) ++hwUsed;
    }
    if (hwUsed < m_hwSlots) {
        // Pick the lowest free comparator index.
        bool taken[32] = {false};
        for (int i = 0; i < kMaxSlots; ++i) {
            if (m_slots[i].kind == BpKind::Hw && m_slots[i].hwIndex >= 0 &&
                m_slots[i].hwIndex < 32) {
                taken[m_slots[i].hwIndex] = true;
            }
        }
        for (int c = 0; c < m_hwSlots; ++c) {
            if (!taken[c]) { s.hwIndex = c; break; }
        }
        if (s.hwIndex >= 0 && InstallHw(s)) {
            ++m_used;
            usedHw = true;
            return true;
        }
        ORB_LOG_WARN("Bp: FPB install failed at 0x%08X", addr);
    }

    // Fall back to a BKPT patch.
    if (m_swUsed < kMaxSwSlots) {
        if (InstallSw(s)) {
            ++m_swUsed;
            ++m_used;
            return true;
        }
        ORB_LOG_WARN("Bp: BKPT install failed at 0x%08X", addr);
    }
    s = BpSlot{};
    return false;
}

bool Breakpoints::Remove(uint32_t addr) {
    const int idx = IndexOf(addr);
    if (idx < 0) return true;
    BpSlot& s = m_slots[idx];
    bool ok = true;
    if (s.kind == BpKind::Hw) {
        ok = UninstallHw(s);
    } else if (s.kind == BpKind::Sw) {
        ok = UninstallSw(s);
        if (m_swUsed > 0) --m_swUsed;
    } else if (s.kind == BpKind::Watch) {
        ok = UninstallWatch(s);
    }
    if (m_used > 0) --m_used;
    s = BpSlot{};

    // 这个地址也要扫一遍比较器：槽表是意图不是事实，遗留的已装比较器就是清不掉的断点，
    // 会让核心永远停下（实测 FP_COMP1/FP_COMP3 都有值，而表里只登记了一条）。
    if (m_dap && m_fpbOn) {
        const uint32_t want = FpComparatorValue(addr & ~1u);
        for (int c = 0; c < m_hwSlots; ++c) {
            uint32_t v = 0;
            if (!m_dap->MemRead32(kFpComp0 + 4u * static_cast<uint32_t>(c), v)) break;
            if (v == want) {
                ORB_LOG_WARN("Bp: stale comparator FP_COMP%d still held 0x%08X - "
                             "clearing it", c, v);
                m_dap->MemWrite32(kFpComp0 + 4u * static_cast<uint32_t>(c), 0u);
            }
        }
    }
    return ok;
}

int Breakpoints::DisableAll() {
    int n = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        const BpSlot& s = m_slots[i];
        if (s.kind == BpKind::None) continue;
        if (SetEnabled(s.addr, false)) ++n;
    }
    return n;
}

int Breakpoints::Addresses(uint32_t* out, int max) const {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < kMaxSlots && n < max; ++i) {
        if (m_slots[i].kind == BpKind::None) continue;
        out[n++] = m_slots[i].addr;
    }
    return n;
}

bool Breakpoints::SetEnabled(uint32_t addr, bool enable) {
    int idx = IndexOf(addr);
    if (idx < 0) idx = WatchIndexOf(addr);
    if (idx < 0) return false;
    BpSlot& s = m_slots[idx];
    if (s.kind == BpKind::Hw) {
        if (!m_dap) return false;
        const uint32_t comp = kFpComp0 + 4u * static_cast<uint32_t>(s.hwIndex);
        // Same encoder as InstallHw.  Writing the bare address here meant
        // REPLACE = 0 ("remap"), so re-enabling a breakpoint silently disarmed it.
        const uint32_t value = enable ? FpComparatorValue(s.addr) : 0u;
        return m_dap->MemWrite32(comp, value);
    }
    if (s.kind == BpKind::Sw) {
        if (!m_dap) return false;
        if (enable) {
            if (!m_dap->MemWrite16(s.addr & ~1u, kBkpt)) return false;
            // Keep the saved word; it was captured at install time.
        } else {
            if (!m_dap->MemWrite32(s.addr & ~3u, s.origData)) return false;
        }
        return true;
    }
    if (s.kind == BpKind::Watch) {
        if (!m_dap) return false;
        const uint32_t off = static_cast<uint32_t>(s.hwIndex) * 16u;
        return m_dap->MemWrite32(kDwtFunc + off, enable ? s.dwFunc : kDwtFuncDisabled);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 恢复：把已登记的比较器重新装一遍（对应参考的 *_UnitRecovery()，它整块写回缓存镜像）
// 本驱动不缓存寄存器镜像，按槽位重装：效果相同，且不会把 uVision 没设过的比较器写回去。
// ⚠ 软件断点（写进 RAM/flash 的 BKPT）不动 —— 链路恢复不影响已写入的内容。
// ---------------------------------------------------------------------------
bool Breakpoints::Recover() {
    if (!m_dap || !m_dap->IsConnected()) return true;

    int n = 0;
    bool ok = true;
    for (int i = 0; i < kMaxSlots; ++i) {
        BpSlot& s = m_slots[i];
        if (s.kind == BpKind::Hw) {
            if (InstallHw(s)) ++n; else ok = false;
        } else if (s.kind == BpKind::Watch) {
            if (InstallWatch(s)) ++n; else ok = false;
        }
    }
    if (n > 0) ORB_LOG_INFO("Bp: 恢复时重装了 %d 个比较器", n);
    return ok;
}

void Breakpoints::Clear() {
    for (int i = 0; i < kMaxSlots; ++i) {
        if (m_slots[i].kind == BpKind::Hw)         UninstallHw(m_slots[i]);
        else if (m_slots[i].kind == BpKind::Sw)    UninstallSw(m_slots[i]);
        else if (m_slots[i].kind == BpKind::Watch) UninstallWatch(m_slots[i]);
        m_slots[i] = BpSlot{};
    }
    if (m_fpbOn && m_dap) {
        m_dap->MemWrite32(kFpCtrl, kFpCtrlKey); // disable FPB
        m_fpbOn = false;
    }
    m_used = 0;
    m_swUsed = 0;
}

} // namespace orb
