/**
 * @file ORBAGDI_CpuId.cpp
 * @brief CPU 身份识别表：CPUID.PARTNO → 名字 / 架构 / ETM 变种 / 内核编号。
 */
#include "pch.h"
#include "ORBAGDI_CpuId.h"

namespace orb {
namespace agdi {

namespace {

// ARM 分配的 CPUID.PARTNO（CPUID bits 15:4）。
// 官方实现是一个大 switch（DbgCM/Debug.cpp:1048-1167 的 CPUID 分支），
// 合并成一张表后，新增内核对所有调用方同时生效。
//
// etm 一列是这颗内核的 trace 出口：M0/M1 没有，M0+/M23 走 MTB，
// M3/M4 是 ETMv3.5，M7 之后是 ETMv4。
// SC000/SC300 没有公开的 CPUID.PARTNO，只能留在 kCpu* 枚举里。
const CpuDesc kCpuTable[] = {
    { 0xC20u, "Cortex-M0",   kCpuCM0,   TraceArch::ArmV6M, EtmVariant::None },
    { 0xC60u, "Cortex-M0+",  kCpuCM0P,  TraceArch::ArmV6M, EtmVariant::Mtb  },
    { 0xC21u, "Cortex-M1",   kCpuCM1,   TraceArch::ArmV6M, EtmVariant::None },
    { 0xC23u, "Cortex-M3",   kCpuCM3,   TraceArch::ArmV7M, EtmVariant::V35  },
    { 0xC24u, "Cortex-M4",   kCpuCM4,   TraceArch::ArmV7M, EtmVariant::V35  },
    { 0xC27u, "Cortex-M7",   kCpuCM7,   TraceArch::ArmV7M, EtmVariant::V4   },
    { 0xD20u, "Cortex-M23",  kCpuCM23,  TraceArch::ArmV8M, EtmVariant::Mtb  },
    { 0xD21u, "Cortex-M33",  kCpuCM33,  TraceArch::ArmV8M, EtmVariant::V4   },
    { 0xD31u, "Cortex-M35P", kCpuCM35P, TraceArch::ArmV8M, EtmVariant::V4   },
    { 0xD22u, "Cortex-M55",  kCpuCM55,  TraceArch::ArmV8M, EtmVariant::V4   },
    { 0xD23u, "Cortex-M85",  kCpuCM85,  TraceArch::ArmV8M, EtmVariant::V4   },
};

} // namespace

CpuId ReadCpuId(orb::Dap& dap)
{
    CpuId id;

    // 全 0 / 全 1 是“总线没答话”，不是某颗内核；两者都当读不到。
    if (!dap.MemRead32(orb::kScsCpuid, id.cpuid)) return id;
    if (id.cpuid == 0u || id.cpuid == 0xFFFFFFFFu) return id;

    id.valid  = true;
    id.partNo = CpuPartNo(id.cpuid);
    id.desc   = CpuLookupPartNo(id.partNo);
    return id;
}

const CpuDesc* CpuLookupPartNo(uint32_t partNo)
{
    // 十几行，线性扫比建哈希表更省事，也更容易在调试器里直接看。
    for (const CpuDesc& d : kCpuTable) {
        if (d.partNo == partNo) return &d;
    }
    return nullptr;
}

uint32_t CpuPartNo(uint32_t cpuid)
{
    return (cpuid >> 4) & 0xFFFu;
}

const char* CpuNameFromPartNo(uint32_t part)
{
    const CpuDesc* d = CpuLookupPartNo(part);
    return d ? d->name : "unknown";
}

unsigned CpuTypeFromPartNo(uint32_t part)
{
    const CpuDesc* d = CpuLookupPartNo(part);
    // 认不出的器件按非 v8-M 处理，避免凭空给它加上安全扩展。
    return d ? d->cpu : kCpuCM3;
}

bool IsV8M(unsigned cpuType)
{
    switch (cpuType) {
    case kCpuCM23:  // v8-M Baseline
    case kCpuCM33:  // v8-M Mainline
    case kCpuCM35P: // v8-M Mainline with Physical Security
    case kCpuCM55:  // v8.1-M Mainline
    case kCpuCM85:  // v8.1-M Mainline
        return true;
    default:
        return false;   // M0/M0+/M1/M3/M4/M7/SC000/SC300 以及未知值
    }
}

bool IsV8MMainline(unsigned cpuType)
{
    return cpuType == kCpuCM33 || cpuType == kCpuCM35P ||
           cpuType == kCpuCM55 || cpuType == kCpuCM85;
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

} // namespace agdi
} // namespace orb
