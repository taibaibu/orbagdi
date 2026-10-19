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

    const CpuId id = ReadCpuId(dap);
    if (!id.valid) {
        // ARM 的 CPUID 读不到。RISC-V 没有这颗寄存器，将来在这里探测
        // misa / dmstatus 并把 arch 置为 TraceArch::RiscV。
        return false;
    }

    out.cpuid  = id.cpuid;
    out.partNo = id.partNo;

    if (!id.desc) {
        // CPUID 能读但 PARTNO 不认识：仍按 v7-M 处理，ETM 变种交给调用方兜底。
        out.arch = TraceArch::ArmV7M;
        out.etm  = EtmVariant::None;
        ORB_LOG_WARN("TraceBehavior: 未识别的 PARTNO 0x%03X (CPUID=0x%08X)，按 ARMv7-M 处理",
                     out.partNo, out.cpuid);
        return true;
    }

    out.coreName = id.desc->name;
    out.arch     = id.desc->arch;
    out.etm      = id.desc->etm;
    out.desc     = id.desc;
    return true;
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
