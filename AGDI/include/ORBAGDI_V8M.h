/**
 * @file ORBAGDI_V8M.h
 * @brief ARMv8-M 安全扩展（Secure / Non-Secure 调试），移植 Keil DbgCM 的 #if DBGCM_V8M 块。
 * 调试口只对非安全调试器开放时，CPU 跑安全代码就停不下来；
 * 参考实现把这种错误降级成警告而不是判死，否则“看不到安全态”会变成“调试器挂了”。
 * 寄存器访问全部走 orb::Dap 的 MEM-AP。
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// 官方 SEV_ERR_*。
enum : unsigned {
    kSevErrStop   = 1, // Cannot stop target
    kSevErrReset  = 2, // Cannot reset target
    kSevErrAccess = 3, // Cannot access target
    kSevErrReinit = 4, // Cannot reinit target
};

// 参考实现的 ARM_CM* 枚举，调用方把自己识别的 CPU 映射到这套值上。
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
};

// CM23 / CM33 / CM35P 是 v8-M，其余不是。
bool IsV8M(unsigned cpuType);

// CPUID.PARTNO → 上面的枚举。
// 认不出的 PARTNO 映射成非 v8-M，避免凭空给未知器件加上安全扩展。
unsigned V8mCpuTypeFromPartNo(unsigned partNo);

// v8-M Mainline（CM33/CM35P）；Mainline 能读 NVIC_PFR1，Baseline 只能查 DBG_AUTHSTATUS。
bool IsV8MMainline(unsigned cpuType);

// 对应 pio->bSecureExt / bCPUSecure / b*DbgEna / b*TrcEna 与 DSCSR.CDSKEY。
struct V8mStatus {
    bool secureExt;  // Security Extensions available
    bool cpuSecure;  // current CPU security state
    bool nsDbgEna;   // Non-Secure Debug enabled
    bool nsTrcEna;   // Non-Secure Trace enabled
    bool sDbgEna;    // Secure Debug enabled
    bool sTrcEna;    // Secure Trace enabled
    bool cdskey;     // DSCSR.CDSKEY - the CDS value is locked
    bool detected;   // DetectSecurityExtensions() ran to completion
};

V8mStatus V8mGetStatus();

// 判断器件有没有安全扩展；非 v8-M 返回 false 并把状态清零。
// 成功后会接着跑下面两个 Update，与参考实现一致。
bool V8mDetectSecurityExtensions(orb::Dap& dap, unsigned cpuType);

// 读 DBG_SCSR，发布当前安全状态。
bool V8mUpdateSecurityState(orb::Dap& dap);

// 读 DBG_AUTHSTATUS / DBG_HCSR，发布四个调试/跟踪使能位。
bool V8mUpdateDebugAuthentication(orb::Dap& dap);

// “非安全调试器 + CPU 在安全态”：先刷新认证状态再判断；v7-M 永远为 false。
bool V8mNonSecureCantStop();

// 返回 0 = 致命（调用方应结束会话）；返回 1 = 因 NonSecureCantStop() 降级为警告。
int V8mSevereTargetError(unsigned errType);

// 参考实现最后调 SetPlayDead(msg)，本驱动的等价物是“会话不再连接”，
// 具体收尾由 AGDI 层装的钩子做；没装钩子时只记日志。
typedef void (*V8mPlayDeadFn)(const char* msg);
void V8mSetPlayDeadHook(V8mPlayDeadFn fn);

} // namespace agdi
} // namespace orb
