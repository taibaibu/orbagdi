/**
 * @file ORBAGDI_CpuId.h
 * @brief CPU 身份识别的唯一出口：一张 CPUID.PARTNO 表，名字 / 架构 / ETM 变种 / 内核编号
 *        全从这张表的同一行读出来。
 *
 * 以前同一批 PARTNO 散在四处：CoreSight 层报内核名、TraceBehavior 层分架构与 ETM 变种、
 * V8M 层判安全扩展、AGDI 层自己右移取位。加一颗新内核要改四个 switch，漏一个就会
 * 出现“日志认得出、trace 认不出”的错位。现在只在 CpuId.cpp 的表里加一行。
 */
#pragma once

#include <stdint.h>

namespace orb {
class Dap;   // 只取引用，不需要 Dap 的完整定义
} // namespace orb

namespace orb {
namespace agdi {

/// 目标架构。RISC-V 目前只有枚举位与分支占位，尚未实现探测。
enum class TraceArch {
    Unknown,
    ArmV6M,
    ArmV7M,
    ArmV8M,
    RiscV,
};

/// ETM 变种（决定 ATB 流怎么解码）。
enum class EtmVariant {
    None,   // 没有 ETM（M0/M1）
    Mtb,    // 指令跟踪走 MTB，不在 ETM 里
    V35,    // ETMv3.5（M3/M4）
    V4,     // ETMv4（M7/M33/M35P/M55/M85）
};

// 官方 ARM_CM*（DbgCM COLLECT.H）的内核编号。调用方把识别结果映射到这套值上，
// v8-M 判定（IsV8M / IsV8MMainline）也以这套值为准。
enum : unsigned {
    kCpuCM0    = 0,
    kCpuCM0P   = 1,
    kCpuCM1    = 2,
    kCpuCM3    = 3,
    kCpuCM4    = 4,
    kCpuCM7    = 5,
    kCpuSC000  = 6,
    kCpuSC300  = 7,
    kCpuCM23   = 8,   // v8-M Baseline
    kCpuCM33   = 9,   // v8-M Mainline
    kCpuCM35P  = 10,  // v8-M Mainline with Physical Security
    kCpuCM55   = 11,  // v8.1-M Mainline
    kCpuCM85   = 12,  // v8.1-M Mainline
};

/// 一颗内核的全部静态事实。表里加一行就是“支持一种新内核”。
struct CpuDesc {
    uint16_t    partNo;   // CPUID.PARTNO（CPUID bits 15:4）
    const char* name;     // 日志与对话框显示的名字
    unsigned    cpu;      // kCpuCM* 编号
    TraceArch   arch;     // 指令集架构
    EtmVariant  etm;      // trace 出口
};

/// 一次 CPUID 读取的结果。
struct CpuId {
    bool           valid  = false;    // 读到了，且不是 0 / 全 1
    uint32_t       cpuid  = 0;        // CPUID 原值
    uint32_t       partNo = 0;        // CPUID.PARTNO
    const CpuDesc* desc   = nullptr;  // nullptr = PARTNO 不在表里
};

/// 读 CPUID 并查表。valid=false 表示这颗寄存器读不到（未连接 / RISC-V）。
CpuId ReadCpuId(orb::Dap& dap);

/// PARTNO → 表项；未识别返回 nullptr（调用方自己决定兜底，不要在这里猜）。
const CpuDesc* CpuLookupPartNo(uint32_t partNo);

/// CPUID → PARTNO（bits 15:4）。
uint32_t CpuPartNo(uint32_t cpuid);

/// PARTNO → 内核名；未识别返回 "unknown"。
const char* CpuNameFromPartNo(uint32_t part);

/// PARTNO → kCpuCM*；未识别返回 kCpuCM3（按非 v8-M 处理，避免凭空加上安全扩展）。
unsigned CpuTypeFromPartNo(uint32_t part);

/// CM23/CM33/CM35P/CM55/CM85 是 v8-M，其余不是。
bool IsV8M(unsigned cpuType);

/// v8-M Mainline（CM33/CM35P/CM55/CM85）；Mainline 能读 NVIC_PFR1，Baseline 只能查 DBG_AUTHSTATUS。
bool IsV8MMainline(unsigned cpuType);

const char* ArchName(TraceArch a);
const char* EtmVariantName(EtmVariant v);

} // namespace agdi
} // namespace orb
