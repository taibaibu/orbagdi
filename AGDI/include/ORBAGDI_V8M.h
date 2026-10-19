/**
 * @file ORBAGDI_V8M.h
 * @brief ARMv8-M 安全扩展（Secure / Non-Secure 调试），移植 Keil DbgCM 的 #if DBGCM_V8M 块。
 * 调试口只对非安全调试器开放时，CPU 跑安全代码就停不下来；
 * 参考实现把这种错误降级成警告而不是判死，否则“看不到安全态”会变成“调试器挂了”。
 * 寄存器访问全部走 orb::Dap 的 MEM-AP。
 */
#pragma once

#include <stdint.h>

#include "ORBAGDI_CpuId.h"   // kCpuCM* / IsV8M / IsV8MMainline / CpuTypeFromPartNo 的唯一来源

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
