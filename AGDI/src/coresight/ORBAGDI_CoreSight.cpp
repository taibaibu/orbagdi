/**
 * @file ORBAGDI_CoreSight.cpp
 * @brief CoreSight 公共层的实现：CPUID 识别与寄存器原语。
 */
#include "pch.h"
#include "ORBAGDI_CoreSight.h"

namespace orb {
namespace agdi {

uint32_t CpuPartNo(uint32_t cpuid)
{
    return (cpuid >> 4) & 0xFFFu;
}

const char* CpuNameFromPartNo(uint32_t part)
{
    // M0 / M1 没有任何 trace 部件，故意不给名字。
    switch (part) {
    case 0xC23u: return "Cortex-M3";
    case 0xC24u: return "Cortex-M4";
    case 0xC27u: return "Cortex-M7";
    case 0xD21u: return "Cortex-M33";
    case 0xD22u: return "Cortex-M55";
    case 0xD23u: return "Cortex-M85";
    case 0xC60u: return "Cortex-M0+";
    case 0xD20u: return "Cortex-M23";
    default:     return "unknown";
    }
}

// ---------------------------------------------------------------------------
// ROM 表
// ---------------------------------------------------------------------------
bool ReadCid(orb::Dap& dap, uint32_t adr, bool r32, uint32_t& cid)
{
    uint8_t b[16];
    if (r32) {
        if (!dap.MemRead(adr, b, 16)) return false;
    } else {
        for (uint32_t n = 0; n < 16; n += 4) {
            if (!dap.MemRead(adr + n, &b[n], 1)) return false;
        }
    }
    cid = (uint32_t)b[0] | ((uint32_t)b[4] << 8) |
          ((uint32_t)b[8] << 16) | ((uint32_t)b[12] << 24);
    return true;
}

bool ReadPid(orb::Dap& dap, uint32_t adr, bool r32, uint64_t& pid)
{
    uint8_t b[32];
    if (r32) {
        if (!dap.MemRead(adr, b, 32)) return false;
    } else {
        for (uint32_t n = 0; n < 32; n += 4) {
            if (!dap.MemRead(adr + n, &b[n], 1)) return false;
        }
    }
    const uint64_t hi = (uint64_t)b[0] | ((uint64_t)b[4] << 8) |
                        ((uint64_t)b[8] << 16) | ((uint64_t)b[12] << 24);
    const uint64_t lo = (uint64_t)b[16] | ((uint64_t)b[20] << 8) |
                        ((uint64_t)b[24] << 16) | ((uint64_t)b[28] << 24);
    pid = (hi << 32) | lo;
    return true;
}

namespace {

/// 递归的一层。`ptr` 是 ADIv5 ROM 表项地址或表基址。
bool ScanRomLevel(orb::Dap& dap, uint32_t ptr, int level, CsRomMap& out)
{
    if (level == 0) {
        if (ptr == 0xFFFFFFFFu) return false;
        if ((ptr & 0x2u) == 0) ptr |= 0x3u;   // 强制 ADIv5 格式
    }
    if (level >= 10) return false;
    if ((ptr & 0x1u) == 0) return true;       // 这一项为空

    const bool     r32 = (ptr & 0x2u) != 0;
    const uint32_t adr = ptr & 0xFFFFF000u;

    uint32_t cid = 0;
    uint64_t pid = 0;
    if (!ReadCid(dap, adr + kOffCid, r32, cid)) return false;
    if (!ReadPid(dap, adr + kOffPid, r32, pid)) return false;

    if (cid == kCidRomTable) {
        const uint32_t cnt = r32 ? 960u : 240u;
        for (uint32_t n = 0; n < cnt; ++n) {
            uint32_t val = 0;
            if (r32) {
                if (!dap.MemRead32(adr + 4u * n, val)) return false;
            } else {
                uint8_t b[4];
                if (!dap.MemRead(adr + 16u * n + 0,  &b[0], 1)) return false;
                if (!dap.MemRead(adr + 16u * n + 4,  &b[1], 1)) return false;
                if (!dap.MemRead(adr + 16u * n + 8,  &b[2], 1)) return false;
                if (!dap.MemRead(adr + 16u * n + 12, &b[3], 1)) return false;
                val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                      ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            }
            if (val == 0) return true;          // 表尾
            if (!ScanRomLevel(dap, adr + val, level + 1, out)) return false;
        }
        return true;
    }

    // 组件项：真实基址在表项下方，偏移量由 PIDR4 的 4 KB 块数决定。
    const uint32_t n4k  = ((1u << (uint32_t)((pid >> 36) & 0xFu)) - 1u) * 4096u;
    const uint32_t comp = adr - n4k;

    switch (pid & (kPidMaskJep | kPidMaskPn)) {
    case (kPidArmJep | kPnEtb):
        if (out.etb == 0) { out.etb = comp; out.etbIsTmc = false; }
        break;

    case (kPidArmJep | kPnTmc): {
        if (out.etb == 0) {
            uint32_t devid = 0;
            if (dap.MemRead32(comp + kOffTmcDev, devid)) {
                if ((devid & kTmcCfgTypeMask) == kTmcCfgEtb) {
                    out.etb      = comp;
                    out.etbIsTmc = true;
                }
            }
        }
        break;
    }

    case (kPidArmJep | kPnTf):
        out.funnelSeen = true;
        break;

    case (kPidArmJep | kPnEtmCm3):
    case (kPidArmJep | kPnEtmCm4):
    case (kPidArmJep | kPnEtmSc3):
    case (kPidArmJep | kPnEtmCm7):
        if (out.etm == 0) out.etm = comp;
        break;

    default:
        // 表里漏了哪个 PN，日志会直接指出来。
        ORB_LOG_INFO("CoreSight: ROM 组件 0x%08X 未识别 (PID=0x%016llX PN=0x%03X)",
                     comp, (unsigned long long)pid, (unsigned)(pid & kPidMaskPn));
        break;
    }

    // 另外两条识别路：v8-M/ETMv4 看 DEVARCH（adr+0xFBC），无 DEVARCH 的 ETMv3 看
    // DEVTYPE（adr+0xFCC）。只认 PIDR 换一颗器件就认不出 ETM。
    if (out.etm == 0) {
        uint32_t dev = 0;
        if (dap.MemRead32(adr + 0xFBCu, dev) && (dev & kDevArchPresentM) != 0) {
            if ((dev & (kDevArchArchM | kDevArchArchPartM)) ==
                (kDevArchIdArm | kArchPartEtm)) {
                out.etm        = comp;
                out.etmArchVer = (dev & kDevArchVerM) >> kDevArchVerP;
                if (out.etmArchVer == 2) out.etmArchVer = 3;   // ETMv3 报 2
            }
        } else if (dap.MemRead32(adr + 0xFCCu, dev) &&
                   (dev & kDevTypeTypeM) == kDevTypeEtm) {
            out.etm        = comp;
            out.etmArchVer = 3;
        }
    }

    return true;
}

} // namespace

bool ScanRomTable(orb::Dap& dap, uint32_t romBase, CsRomMap& out)
{
    out.Reset();
    return ScanRomLevel(dap, romBase, 0, out);
}

bool UnlockComponent(orb::Dap& dap, uint32_t base)
{
    return dap.MemWrite32(base + kOffEtmLar, kCsLockKey);
}

bool ModifyReg32(orb::Dap& dap, uint32_t addr, uint32_t clearMask, uint32_t setMask)
{
    uint32_t v = 0;
    if (!dap.MemRead32(addr, v)) return false;

    const uint32_t want = (v & ~clearMask) | setMask;
    if (want == v) return true;          // 已经是目标值，不产生一次写事务
    return dap.MemWrite32(addr, want);
}

bool SetTrcEna(orb::Dap& dap, bool on)
{
    return ModifyReg32(dap, kScsDemcr, kDemcrTrcEna, on ? kDemcrTrcEna : 0u);
}

} // namespace agdi
} // namespace orb
