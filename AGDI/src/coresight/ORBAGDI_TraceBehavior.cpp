/**
 * @file ORBAGDI_TraceBehavior.cpp
 * @brief 目标识别与 trace 组件准备的 ARM 实现（src\coresight 下，与寄存器实现同层）。
 */
#include "pch.h"
#include "ORBAGDI_TraceBehavior.h"

namespace orb {
namespace agdi {

namespace {

// ARM 架构固定的组件地址（CoreSight 组件布局的一部分，不属于寄存器定义）。
const uint32_t kArmItmBase  = 0xE0000000u;
const uint32_t kArmDwtBase  = 0xE0001000u;
const uint32_t kArmTpiuBase = 0xE0040000u;

// MEM-AP ROM_ADDR 读不到时的架构默认值。
const uint32_t kArmRomTableDefault = 0xE00FF000u;

} // namespace

bool ProbeTarget(orb::Dap& dap, TargetProfile& out)
{
    out = TargetProfile();

    uint32_t cpuid = 0;
    if (!dap.MemRead32(kScsCpuid, cpuid) || cpuid == 0 || cpuid == 0xFFFFFFFFu) {
        // ARM 的 CPUID 读不到。RISC-V 没有这颗寄存器，将来在这里探测
        // misa / dmstatus 并把 arch 置为 TraceArch::RiscV。
        return false;
    }

    out.cpuid    = cpuid;
    out.partNo   = CpuPartNo(cpuid);
    out.coreName = CpuNameFromPartNo(out.partNo);

    switch (out.partNo) {
    case 0xC20u:  // Cortex-M0
    case 0xC21u:  // Cortex-M1
        out.arch = TraceArch::ArmV6M;
        out.etm  = EtmVariant::None;
        break;
    case 0xC60u:  // Cortex-M0+
        out.arch = TraceArch::ArmV6M;
        out.etm  = EtmVariant::Mtb;
        break;
    case 0xC23u:  // Cortex-M3
    case 0xC24u:  // Cortex-M4
        out.arch = TraceArch::ArmV7M;
        out.etm  = EtmVariant::V35;
        break;
    case 0xC27u:  // Cortex-M7
        out.arch = TraceArch::ArmV7M;
        out.etm  = EtmVariant::V4;
        break;
    case 0xD20u:  // Cortex-M23
        out.arch = TraceArch::ArmV8M;
        out.etm  = EtmVariant::Mtb;
        break;
    case 0xD21u:  // Cortex-M33
    case 0xD22u:  // Cortex-M55
    case 0xD23u:  // Cortex-M85
        out.arch = TraceArch::ArmV8M;
        out.etm  = EtmVariant::V4;
        break;
    default:
        // CPUID 能读但 PARTNO 不认识：仍按 v7-M 处理，ETM 变种交给调用方兜底。
        out.arch = TraceArch::ArmV7M;
        out.etm  = EtmVariant::None;
        ORB_LOG_WARN("TraceBehavior: 未识别的 PARTNO 0x%03X (CPUID=0x%08X)，按 ARMv7-M 处理",
                     out.partNo, cpuid);
        break;
    }

    return true;
}

const char* ArchName(TraceArch a)
{
    switch (a) {
    case TraceArch::ArmV6M: return "ARMv6-M";
    case TraceArch::ArmV7M: return "ARMv7-M";
    case TraceArch::ArmV8M: return "ARMv8-M";
    case TraceArch::RiscV:  return "RISC-V";
    default:                return "unknown";
    }
}

const char* EtmVariantName(EtmVariant v)
{
    switch (v) {
    case EtmVariant::Mtb: return "MTB";
    case EtmVariant::V35: return "ETMv3.5";
    case EtmVariant::V4:  return "ETMv4";
    default:              return "none";
    }
}

bool TraceClockOn(orb::Dap& dap)
{
    // ARM: DEMCR.TRCENA。RISC-V 没有这个位，将来在这里按 arch 分支到 dmcontrol。
    return SetTrcEna(dap, true);
}

bool TraceClockOff(orb::Dap& dap)
{
    return SetTrcEna(dap, false);
}

bool PrepareItmDwt(orb::Dap& dap)
{
    const bool itm = UnlockComponent(dap, kArmItmBase);
    const bool dwt = UnlockComponent(dap, kArmDwtBase);
    return itm && dwt;
}

bool PrepareTpiu(orb::Dap& dap)
{
    return UnlockComponent(dap, kArmTpiuBase);
}

bool PrepareEtm(orb::Dap& dap, uint32_t etmBase)
{
    if (etmBase == 0) return false;
    return UnlockComponent(dap, etmBase);
}

bool PrepareEtb(orb::Dap& dap, uint32_t etbBase)
{
    if (etbBase == 0) return false;
    return UnlockComponent(dap, etbBase);
}

uint32_t TraceRomBase(orb::Dap& dap)
{
    uint32_t romBase = 0;
    if (!dap.ReadApReg(0xF, DAP_AP_REG_ROMADDR, romBase) ||
        romBase == 0 || romBase == 0xFFFFFFFFu) {
        romBase = kArmRomTableDefault;
    }
    return romBase;
}

bool DiscoverTraceLayout(orb::Dap& dap, CsRomMap& out, uint32_t* romBaseOut)
{
    const uint32_t romBase = TraceRomBase(dap);
    if (romBaseOut) *romBaseOut = romBase;
    return ScanRomTable(dap, romBase, out);
}

} // namespace agdi
} // namespace orb
