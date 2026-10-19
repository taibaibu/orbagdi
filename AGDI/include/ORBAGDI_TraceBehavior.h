/**
 * @file ORBAGDI_TraceBehavior.h
 * @brief 目标识别与 trace 组件准备，夹在 app 调用点与 CoreSight 寄存器操作之间。
 * 只描述“要做什么”，不出现寄存器位；业务代码因此不必手写裸的 MemRead/Write 序列。
 */
#pragma once

#include <stdint.h>

#include "ORBAGDI_CoreSight.h"

namespace orb {
class Dap;
}

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
    V4,     // ETMv4（M7/M33/M55/M85）
};

struct TargetProfile {
    TraceArch   arch     = TraceArch::Unknown;
    EtmVariant  etm      = EtmVariant::None;
    uint32_t    cpuid    = 0;
    uint32_t    partNo   = 0;
    const char* coreName = "unknown";
};

/// 读 CPUID 并归类内核 / 架构 / ETM 变种。
/// 探测不到任何已知架构时返回 false（out.arch 保持 Unknown）。
bool ProbeTarget(orb::Dap& dap, TargetProfile& out);

const char* ArchName(TraceArch a);
const char* EtmVariantName(EtmVariant v);

// ---------------------------------------------------------------------------
// trace 时钟（全局使能）
// ---------------------------------------------------------------------------
/// ARM: DEMCR.TRCENA。RISC-V: 走 dmcontrol 的 trace 授权位（待实现）。
bool TraceClockOn(orb::Dap& dap);
bool TraceClockOff(orb::Dap& dap);

// ---------------------------------------------------------------------------
// 组件可写性（CoreSight 锁）
// ---------------------------------------------------------------------------
bool PrepareItmDwt(orb::Dap& dap);                 ///< ITM + DWT
bool PrepareTpiu(orb::Dap& dap);                   ///< TPIU（复位后常被锁）
bool PrepareEtm(orb::Dap& dap, uint32_t etmBase);  ///< ETM，基址可能来自 ROM 表
bool PrepareEtb(orb::Dap& dap, uint32_t etbBase);  ///< ETB / TMC

// ---------------------------------------------------------------------------
// 组件布局
// ---------------------------------------------------------------------------
/// MEM-AP 的 ROM_ADDR；读不到时给 ARMv7-M 的架构默认值。
uint32_t TraceRomBase(orb::Dap& dap);

/// 扫 ROM 表得出 ETM/ETB 基址。只读，不改任何硬件状态。
/// romBaseOut 非空时回写本次用的表基址（诊断日志用）。
bool DiscoverTraceLayout(orb::Dap& dap, CsRomMap& out, uint32_t* romBaseOut = nullptr);

} // namespace agdi
} // namespace orb
