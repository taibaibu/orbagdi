/**
 * @file ORBAGDI_CoreSight.h
 * @brief CoreSight 公共层：ETM / ETB / ITM 三个后端共用的常量与寄存器原语。
 * 同一个硬件事实只写一处，避免各后端复制后漂移。
 */
#pragma once

#include <stdint.h>

namespace orb {
class Dap;   // 只取引用，不需要 Dap 的完整定义
} // namespace orb

namespace orb {
namespace agdi {

// ---------------------------------------------------------------------------
// 全局常量
// ---------------------------------------------------------------------------
enum : uint32_t {
    kCsLockKey   = 0xC5ACCE55u,  // CoreSight 组件解锁键（ITM/DWT/TPIU/ETM/ETB 通用）
    kAtbIdNull   = 0u,           // ATB 空源
    kAtbIdItm    = 1u,           // ITM 的 TraceBusID
    kAtbIdEtm    = 2u,           // ETM 的 TRACEIDR，必须与 ITM 不同

    kScsCpuid    = 0xE000ED00u,  // CPUID 基址寄存器
    kScsDemcr    = 0xE000EDFCu,  // DEMCR
    kDemcrTrcEna = 0x01000000u,  // DEMCR.TRCENA

    kEtmBaseArch = 0xE0041000u,  // ROM 表扫不到 ETM 时的架构默认基址
};

// ---------------------------------------------------------------------------
// ETM 寄存器（IHI0014Q）
// ---------------------------------------------------------------------------
enum : uint32_t {
    kOffEtmCr       = 0x000u,  // ETMCR
    kOffEtmTrigger  = 0x008u,  // ETMTRIGGER
    kOffEtmSr       = 0x010u,  // ETMSR
    kOffEtmTeevr    = 0x020u,  // ETMTEEVR
    kOffEtmTecr1    = 0x024u,  // ETMTECR1
    kOffEtmCcer     = 0x1E0u,  // ETMCCER   (reg 0x078)
    kOffEtmDevType  = 0x1E4u,  // ETMDEVTYPE(reg 0x079，bit20 = ALT 分支编码)
    kOffEtmTraceIdr = 0x200u,  // ETMTRACEIDR
    kOffEtmLar      = 0xFB0u,  // ETM lock access
};

// ETMCR 位（IHI0014Q Table 3-5）
enum : uint32_t {
    kEtmCrBranchOut  = 0x00000100u,  // bit8  Branch output
    kEtmCrProgramBit = 0x00000400u,  // bit10 编程模式（Progbit）
    kEtmCrEtmen      = 0x00000800u,  // bit11 采集使能
    kEtmCrCycleAcc   = 0x00001000u,  // bit12 周期精确（周期计数包）
    kEtmCrTsEna      = 0x10000000u,  // bit28 时间戳使能（ETMCCER[22]=1 才有效）
};

// ETMSR 位
const uint32_t kEtmSrProgbit = 0x00000002u;  // ETMSR bit[1]，Progbit 的有效值

// ETMCCER 位（IHI0014Q Table 3-52）
enum : uint32_t {
    kEtmCcerTsImpl = 0x00400000u,  // bit22 Timestamping implemented
    kEtmCcerTsBin  = 0x10000000u,  // bit28 1 = 自然二进制，0 = 格雷码
    kEtmCcerTs64   = 0x20000000u,  // bit29 0 = 48 bit，1 = 64 bit
};

// ---------------------------------------------------------------------------
// Component identification (ROM table)
// ---------------------------------------------------------------------------
const uint32_t kCidRomTable = 0xB105100Du;                 // CIDR 全 4 字节拼起来
const uint64_t kPidMaskPn   = 0x0000000000000FFFULL;       // PARTNO
const uint64_t kPidMaskJep  = 0x0000000F000FF000ULL;       // JEP106 + 存在位
const uint64_t kPidArmJep   = 0x00000004000BB000ULL;       // ARM 的 JEP106

enum : uint32_t {
    kPnEtb    = 0x907,   // CoreSight ETB
    kPnTf     = 0x908,   // Trace Funnel
    kPnTmc    = 0x961,   // Trace Memory Controller
    kPnEtmCm3 = 0x924,   // ETM Cortex-M3
    kPnEtmCm4 = 0x925,   // ETM Cortex-M4
    kPnEtmSc3 = 0x926,   // ETM SC300
    kPnEtmCm7 = 0x975,   // ETM Cortex-M7
};

enum : uint32_t {
    kOffCid    = 0xFF0u,  // CIDR
    kOffPid    = 0xFD0u,  // PIDR4..7 + PIDR0..3
    kOffTmcDev = 0xFC8u,  // TMC device ID（ETB/ETR/ETF 类型）
};

// TMC device ID
enum : uint32_t {
    kTmcMemWidthMask = 0x00000700u,
    kTmcMemWidthPos  = 8,
    kTmcCfgTypeMask  = 0x000000C0u,
    kTmcCfgEtb       = 0x00000000u,
    kTmcCfgEtr       = 0x00000040u,
    kTmcCfgEtf       = 0x00000080u,
};

// 组件识别用的 DEVARCH / DEVTYPE 常量：
//   v8-M（M23/M33/M35P）与 ETMv4 走 DEVARCH，没有 DEVARCH 的 ETMv3 只看 DEVTYPE。
enum : uint32_t {
    kDevArchPresentM  = 0x00100000u,  // DEVARCH 存在位
    kDevArchArchM     = 0xFFE00000u,  // Architect ID（JEP106 + PRESENT）
    kDevArchArchPartM = 0x00000FFFu,  // Architecture Part Number
    kDevArchIdArm     = 0x47600000u,  // ARM
    kDevArchVerM      = 0x0000F000u,  // Architecture Version
    kDevArchVerP      = 12u,
    kArchPartEtm      = 0xA13u,       // ARCHID: ETM (v8-M)
    kDevTypeTypeM     = 0x000000FFu,  // DEVTYPE 全字段
    kDevTypeEtm       = 0x13u,        // Trace Source, processor core
};

/// ROM 表扫描结果。0 表示"表里没有这个组件"。
struct CsRomMap {
    uint32_t etb        = 0;      // ETB / TMC(ETB 模式) 的寄存器基址
    bool     etbIsTmc   = false;  // 上面那个是 TMC 而不是老式 ETB
    uint32_t etm        = 0;      // ETM 的寄存器基址
    uint32_t etmArchVer = 0;      // DEVARCH 报的架构版本（0 = 未知）
    bool     funnelSeen = false;  // 表里有 Trace Funnel

    void Reset() { *this = CsRomMap(); }
};

bool ReadCid(orb::Dap& dap, uint32_t adr, bool r32, uint32_t& cid);
bool ReadPid(orb::Dap& dap, uint32_t adr, bool r32, uint64_t& pid);

/// 从 romBase 开始递归扫 ROM 表，把认出来的组件填进 out（不影响硬件状态）。
bool ScanRomTable(orb::Dap& dap, uint32_t romBase, CsRomMap& out);

// ---------------------------------------------------------------------------
// CPUID
// ---------------------------------------------------------------------------
/// CPUID.PARTNO（bits 15:4）—— ARM 分配的内核编号。
uint32_t CpuPartNo(uint32_t cpuid);

/// PARTNO → 内核名；未识别返回 "unknown"。
const char* CpuNameFromPartNo(uint32_t part);

// ---------------------------------------------------------------------------
// 寄存器原语
// ---------------------------------------------------------------------------
/// 解锁一个 CoreSight 组件（往 base+LAR 写解锁键）。base 是该组件的基址。
bool UnlockComponent(orb::Dap& dap, uint32_t base);

/// 读-改-写：清 clearMask 里的位、置 setMask 里的位。值未变时不写。
bool ModifyReg32(orb::Dap& dap, uint32_t addr, uint32_t clearMask, uint32_t setMask);

/// 置/清 DEMCR.TRCENA（全局 trace 使能）。
bool SetTrcEna(orb::Dap& dap, bool on);

} // namespace agdi
} // namespace orb
