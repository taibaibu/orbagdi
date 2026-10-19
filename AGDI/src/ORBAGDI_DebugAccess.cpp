/**
 * @file ORBAGDI_DebugAccess.cpp
 * @brief 调试访问的等级探测与恢复 —— `DbgCM/DebugAccess.cpp` 的对应件。
 *
 * 逐函数对应关系（行号指 `DbgCM/DebugAccess.cpp`）：
 *
 *   dbgAccLevel                :54
 *   DebugAccessDetection()     :58-135
 *   _DebugProtocolRecovery()   :137-224
 *   _DebugPortRecovery()       :226-287
 *   _TraceRecovery()           :289-334
 *   _BreakpointUnitRecovery()  :336-349
 *   _WatchpointUnitRecovery()  :351-397
 *   _DebugSetupRecovery()      :399-506
 *   DebugAccessRecovery()      :508-677
 *   DebugAccessEnsure()        :679-...
 *   ConfigureDebugAccessRecovery() :727-798
 *   InitDebugAccess()          :800-804
 *
 * ⚠ **恢复不发生在访问路径上。** 参考只在 flash 操作之后（`Flash.cpp:1864/1909/1981`）、
 *   reset 之后（`PDSCDebug.cpp:4928`）、以及 **DSMonitor 后台线程**里
 *   （`DSMonitor.cpp:327`）调这套。内存窗口/寄存器窗口自己不触发恢复 ——
 *   等它再访问时，后台已经修好了。这也是本驱动现在的挂法（见 TracePump）。
 */
#include "pch.h"
#include "ORBAGDI_DebugAccess.h"

#include "ORBAGDI_Agdi.h"        // Get() - ConfigureDebugAccessRecovery 要投停止请求
#include "ORBAGDI_Break.h"
#include "ORBAGDI_Core.h"
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_Rddi.h"
// `TraceWinOptions()` / `kTraceEnable` —— `_TraceRecovery()` 的门控，对应参考的
// `TraceConf.Opt & TRACE_ENABLE`（DbgCM/DebugAccess.cpp:489）。
#include "ORBAGDI_TraceWin.h"

// BOM：`Notify` 与 `UV_UPDATE_SELECTIVE` / `VW_*` 窗口掩码（BOM.H:2390, 2514-2530）。
// 恢复成功之后要经它把"已经修好了"告诉 uVision —— 见 NotifyUvRefreshWindows()。
#include "BOM.H"

namespace orb {
namespace agdi {

// 参考的 `dbgAccLevel`（DbgCM/DebugAccess.cpp:54）。
int dbgAccLevel = kAccNormal;

// 参考的 `recoverUnderReset`（DbgCM/DebugAccess.cpp:53）：复位线是不是被我们拉着。
// `AG_RECOVERY_CONF` 置它，`DebugAccessRecovery()` 在修好链路之后据它放开。
bool recoverUnderReset = false;

// ---------------------------------------------------------------------------
// 恢复熔断（2026-10-14，用户指令"做，加兜底"）
// ---------------------------------------------------------------------------
// **恢复阶梯原来永不放弃。** 实测现场（15:41:29–15:42:28）：一次越界读把**探针
// 自己**打飞（板子是活的，是 orbtrace 跑飞），此后每个 DP 访问都返回
// `0x100E`（OPERATION_TIMEOUT）——
//     **59 秒里 585 次 DP 超时、593 次"逐级恢复"**，一次都没成，而 uVision 的
//     "Trying to recover debug connection" 是**模态框**，一轮一轮地调进来，
//     界面彻底卡死（用户只能杀进程）。
//
// 熔断之后：连续失败 `kRecoveryGiveUp` 次就**直接返回失败、不再碰探针** ——
// 没有 DAP 流量、没有超时等待，uVision 那一轮立刻结束，它自己会收掉对话框并
// 提示"无法访问目标硬件"。要重新尝试，走一次 Connect（会话边界会清计数）。
enum : unsigned { kRecoveryGiveUp = 3 };
static unsigned s_recoveryFails = 0;

// ---------------------------------------------------------------------------
// PlayDead —— 官方 `PlayDead` / `SetPlayDead`（DbgCM/AGDI.CPP:77、:1161-1177）
// ---------------------------------------------------------------------------
// 语义与官方一致：**置位 = 这次会话已经判死，uVision 要求退出调试**。官方的
// `PlayDead` 是 DLL 全局的，所有 uVision 入口在末尾都查它（`AG_MemAcc` 等，
// `DbgCM/AGDI.CPP:4776`），查到就 `ExecPlayDead()`。本驱动的执行点在 `AG_Init`
// 末尾一处 —— uVision 在那个对话框里正是靠 `AG_EXECITEM|AG_PROFILEINFO` 这一项
// 不停进门，所以那一处就够了（见 ORBAGDI_Agdi.cpp 的 `PlayDeadExec()`）。
static bool s_playDead = false;

void PlayDeadRequest() { s_playDead = true; }
bool PlayDeadPending() { return s_playDead; }
void PlayDeadClear()   { s_playDead = false; }

// DP CTRL/STAT 的电源应答位（DbgCM/DebugAccess.cpp:51 `DP_POWERED_MASK`）。
const uint32_t kDpPoweredMask = 0xA0000000u;  // CDBGPWRUPACK | CSYSPWRUPACK
// AP CSW.DBGSTAT（"Could read AP and device access is enabled"）。
const uint32_t kCswDbgStat    = 0x02000000u;

void DebugAccessInit()
{
    // 参考的 `InitDebugAccess()`（DbgCM/DebugAccess.cpp:800-804）。
    recoverUnderReset = false;
    dbgAccLevel       = kAccNormal;
    s_recoveryFails   = 0;      // 新会话：熔断计数从头开始
    s_playDead        = false;  // 新会话：上一次的"要求退出"不该继承
}

void DebugAccessUninit()
{
    // 等级与"复位下恢复"的闩锁属于刚结束的这次会话。参考只在 DLL 初始化时清
    // （`:642` 从 InitDll() 调），本驱动在会话结束时多清一次：下一个目标不该继承
    // 上一个会话"复位线正被拉着"的判断 —— 那会让下一次恢复去放开一根没人拉过的线。
    recoverUnderReset = false;
    dbgAccLevel       = kAccNormal;
    // 熔断计数同样按会话清：上一个会话"探针跑飞"不该让下一个会话一上来就放弃。
    s_recoveryFails   = 0;
    // "要求退出调试"同理：它属于刚刚结束的那次会话。
    s_playDead        = false;
}

// ---------------------------------------------------------------------------
// _DebugProtocolRecovery —— 参考 :137-224
// ---------------------------------------------------------------------------
// **最底层那一级**，只在 `dbgAccLevel == kAccInactive`（DP 连 CTRL/STAT 都不应答，
// 链路已判定死亡）时才会被调到。
//
// 参考的顺序是 PDSC 优先：
//     if (PDSCDebug_IsEnabled()) { PDSCDebug_DebugPortSetup(); ... }
//     else { SWJ_Reset(); SWJ_Switch(); SWJ_Reset(); SWD_ReadID(); }
// 即**有器件描述就绝不动线**，跑描述里的序列；只有没有描述可依时才做线复位。
// 本驱动照这个来 —— 线复位是会打断 SWD 事务状态的侵入性操作，不该轻易做。
// ⚠ **2026-10-14：加了两级兜底，三级依次尝试、失败就往下走**（用户指令"做，加兜底"）。
//
// 现场（15:41:29–15:42:28）：一次越界读把**探针自己**打飞（板子是活的，是 orbtrace
// 跑飞），此后每个 DP 访问都返回 `0x100E`（OPERATION_TIMEOUT）。原来这里只有一条路：
// `DebugSeqReady()` 为真 ⇒ 跑描述里的序列 ⇒ 而 uVision 根本没给器件描述
// （日志：`PDSC: uVision has no debug description for this target`）⇒ **空转** ⇒
// `return 1` ⇒ 下一轮再来。59 秒 585 次超时、593 次"逐级恢复"，一次没成。
//
// 现在：
//   [1/3] 器件描述里的 DebugPortSetup 序列（有描述时优先 —— **绝不动线**）
//   [2/3] 重新做一次 DAP 探测（参考 #else 分支的正解）
//   [3/3] **完整重建 DAP**：Disconnect + Connect —— 重开 RDDI 句柄、重选接口、
//         重跑 `CMSIS_DAP_Connect`（SWJ/SWD 切换序列 + 第一次 DPIDR）、重新上电。
//         探针自己跑飞时前两级都救不回来（它们只是"再问一次"，而探针的状态机已经
//         乱了），**只有从零重建这条链路能把它拉回来** —— 用户实测：不需要给板子断电。
static int _DebugProtocolRecovery(Dap& dap)
{
    const rddi::Api& api = rddi::Get();

    // ---- [1/3] 器件描述里的 DebugPortSetup 序列 ------------------------------
    // 参考的顺序是 PDSC 优先：**有器件描述就绝不动线**，跑描述里的序列；只有没有
    // 描述可依时才做别的。本驱动照这个来 —— 线复位会打断 SWD 事务状态，不该轻易做。
    // 门必须问"有没有这个序列"：`ConnectDebugPort()` 在序列不存在时返回 true
    // （`required=false` 的契约），拿它判"跑成功没有"会让这一级空转报成功。
    if (orb::pdsc::DebugPortSetupAvailable()) {
        ORB_LOG_WARN("DbgAcc: 协议级恢复[1/3] —— 跑器件描述的 DebugPortSetup 序列");
        if (orb::pdsc::ConnectDebugPort()) return 0;
        ORB_LOG_WARN("DbgAcc: 协议级恢复[1/3] 失败 —— 往下走 DAP 探测");
    } else {
        ORB_LOG_INFO("DbgAcc: 协议级恢复[1/3] 跳过 —— 描述里没有这个序列（走 [2/3]）");
    }

    // 会话正在卸载：[2/3]/[3/3] 是秒级的，不再往下做。
    if (InterlockedCompareExchange(&Get().uninitReq, 0, 0) != 0) {
        ORB_LOG_INFO("DbgAcc: 协议级恢复中止 —— 会话正在卸载（AG_UNINIT）");
        return 1;
    }

    // ---- [2/3] 重新探测 DAP -------------------------------------------------
    // 参考的 #else 分支（SW_DP，:193-215）。注意它里面那两句"复位"**都是空函数**：
    //
    //     int SWJ_Reset(void)       { return (0); }   // "RDDI will handle this
    //     int SWJ_Switch(WORD val)  { return (0); }   //  automatically, no need to
    //                                                 //  send the sequence again"
    //
    // 也就是说**参考从不在线级自己发复位序列** —— 那是 RDDI 宿主的事。真正做事的是
    // `SWD_ReadID()`：重新做一次 DAP 探测（`CMSIS_DAP_DetectNumberOfDAPs` +
    // `CMSIS_DAP_DetectDAPIDList`）。
    //
    // ⚠ 曾经在这里自己发过 `CMSIS_DAP_SWJ_Sequence(51, 全1)` 的线复位 —— 那是凭印象
    //   编的，参考里没有这个东西，而且线级复位会打断 SWD 事务状态机，对被测设备是
    //   侵入性操作。已删除，不要再加回来。
    if (api.CMSIS_DAP_DetectNumberOfDAPs && api.CMSIS_DAP_DetectDAPIDList) {
        ORB_LOG_WARN("DbgAcc: 协议级恢复[2/3] —— 重新探测 DAP");
        int numDaps = 0;
        if (api.CMSIS_DAP_DetectNumberOfDAPs(dap.Handle(), &numDaps) == 0 && numDaps > 0) {
            int idcode = 0;
            api.CMSIS_DAP_DetectDAPIDList(dap.Handle(), &idcode, 1);
            // 参考按 ID 白名单校验（SWD.cpp:167-183）。这里只判"读到了"：白名单是
            // 参考自己维护的，本驱动支持的器件更广，照抄白名单会把没列进去的判成失败。
            if (idcode != 0 && static_cast<uint32_t>(idcode) != 0xFFFFFFFFu) {
                ORB_LOG_INFO("DbgAcc: 协议级恢复[2/3] 成功 (%d DAP, ID=0x%08X)",
                             numDaps, idcode);
                return 0;
            }
            ORB_LOG_WARN("DbgAcc: 协议级恢复[2/3] DAP ID 读不到 (0x%08X)", idcode);
        } else {
            ORB_LOG_WARN("DbgAcc: 协议级恢复[2/3] 探测回来 %d 个 DAP", numDaps);
        }
        ORB_LOG_WARN("DbgAcc: 协议级恢复[2/3] 失败 —— 往下走完整重建");
    } else {
        ORB_LOG_WARN("DbgAcc: 宿主没有 DAP 探测接口 —— 跳过 [2/3]");
    }

    // ---- [3/3] 完整重建 DAP -------------------------------------------------
    if (!api.Close) {
        ORB_LOG_WARN("DbgAcc: 协议级恢复[3/3] 跳过 —— 宿主没有 RDDI_Close");
        return 1;
    }
    ORB_LOG_WARN("DbgAcc: 协议级恢复[3/3] —— **完整重建 DAP**（断开 + 重连，"
                 "重跑 SWJ/SWD 切换序列）");
    dap.Disconnect();
    if (!dap.Connect()) {
        ORB_LOG_WARN("DbgAcc: 协议级恢复[3/3] 失败 —— 完整重建没把它拉回来");
        return 1;
    }
    ORB_LOG_INFO("DbgAcc: 协议级恢复[3/3] 成功 —— DAP 已完整重建");
    return 0;
}

// ---------------------------------------------------------------------------
// _DebugPortRecovery —— 参考 :226-287
// ---------------------------------------------------------------------------
// 上电调试口/调试系统 + 清粘滞位。PDSC 优先（`PDSCDebug_DebugPortStart()`）。
static int _DebugPortRecovery(Dap& dap)
{
    if (orb::pdsc::DebugSeqReady()) {
        ORB_LOG_WARN("DbgAcc: 调试口恢复 —— 跑器件描述的 DebugPortStart 序列");
        return orb::pdsc::StartDebugPort() ? 0 : 1;
    }

    // 兜底 = 参考 SW_DP 分支 :239-280：请求上电 -> 等 ACK -> 带 MASKLANE 再写 ->
    // 写 DP ABORT 清 STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR。
    // `Dap::PowerUpDebug()` 就是照这一段移植的（它自己的注释里有出处）。
    ORB_LOG_WARN("DbgAcc: 调试口恢复 —— 重新上电 + 清粘滞位");
    return dap.PowerUpDebug() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// _TraceRecovery —— 参考 :289-334
// ---------------------------------------------------------------------------
// 参考按协议分派 CSTF / ETB / Trace，最后跑器件描述的 TraceStart 序列。
// 本驱动没有 CSTF（见 COMPAT_ANALYSIS.md §10.28 六），所以这里只做 TraceStart；
// 目标侧 trace 寄存器的重配由下一次 ItmProgramTarget 完成。
//
// 门控照参考（`:489` 的 `TraceConf.Opt & TRACE_ENABLE`）：用户没开 trace，恢复流程
// 就不该去动器件那侧的 trace 硬件。本驱动的等价物是 `TraceWinOptions()`（官方
// `TraceOpt`，`DbgCM/Trace.cpp:114`）—— 它是**正在采集**那个副本，比参考的
// "Trace 页已使能"略紧（目标停着时恢复会跳过 TraceStart）。下一次起跑本来就会
// 再跑一次 TraceStart，所以这个差别不改变最终状态。
static int _TraceRecovery()
{
    if (!orb::pdsc::DebugSeqReady()) return 0;
    if ((TraceWinOptions() & kTraceEnable) == 0) return 0;
    return orb::pdsc::TraceStart() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// _BreakpointUnitRecovery / _WatchpointUnitRecovery —— 参考 :336-397
// ---------------------------------------------------------------------------
// 参考把缓存的 `RegFPB` / `RegDWT` 整块写回目标。本驱动的等价物是把已登记的
// 比较器重新装一遍（`Breakpoints::Recover()`）。参考把观察点恢复（`:500`）放在
// 断点恢复（`:496`）**之后**，因为 DWT 比较器要 DEMCR.TRCENA 先补上；本驱动的
// `Recover()` 同时重装 FPB 与 DWT，两步合一，顺序不变。
static int _BreakpointUnitRecovery(Breakpoints* bps)
{
    if (!bps) return 0;
    return bps->Recover() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// _DebugSetupRecovery —— 参考 :399-506
// ---------------------------------------------------------------------------
// 把"调试被关掉了"这件事补回来：DHCSR.C_DEBUGEN + DEMCR.TRCENA。
// PDSC 优先（`PDSCDebug_DebugCoreStart()`）—— 但描述**只管 DHCSR 那一半**；
// DEMCR.TRCENA 是驱动无条件自己补的（参考 :437 那一段在 if/else 之外）。
//
// ⚠ 末尾那两段"子系统恢复"（trace / 断点观察点）**必须在这里、而且必须在放开复位
//   之前**跑完 —— 这是参考的顺序：`_TraceRecovery()`（`:490`）与
//   `_BreakpointUnitRecovery()`（`:496`）都在 `_DebugSetupRecovery()` 里，而
//   `DebugAccessRecovery()` 放开复位是在它返回**之后**（`:604` → `:608`）。
//   先建好调试设施再放核心：目标从复位里出来时断点就已经在位。反过来先放复位，
//   核心会带着空 FPB 先跑一段。
static int _DebugSetupRecovery(Dap& dap, Core& core, Breakpoints* bps)
{
    uint32_t dhcsr = 0, demcr = 0;

    // 参考 :404-435：核心调试的开启优先交给器件描述
    // （`PDSCDebug_DebugCoreStart()` 自己带 DHCSR 兜底实现）；只有没有描述可依时，
    // 本层才自己写 DHCSR。
    if (orb::pdsc::DebugSeqReady()) {
        if (!orb::pdsc::StartCore()) return 1;
    } else {
        if (!core.ReadCoreDebug(dhcsr, demcr)) return 1;

        if ((dhcsr & kDhcsrCDebugEn) == 0) {
            // 参考 :420-432：只清 S_xxx 位，保留其余（比如复位向量捕获停住的现场）。
            const uint32_t want = (dhcsr & ~0xFFFF0000u) | kDhcsrDbgKey | kDhcsrCDebugEn;
            if (!dap.MemWrite32(kDcbDhcsr, want)) return 1;
        }
    }

    // 参考 :437-486：这一段在 `if (PDSCDebug_IsEnabled())` 的 **if/else 之外** ——
    // 走不走器件描述都要做。TRCENA 是 DWT 比较器（观察点）与 ITM 的前提；少了它，
    // 下面 `Recover()` 装好的比较器一个都不会触发，而且**表面无异常**。
    // 本驱动没有 `MONCONF_RST_VECT_CATCH` 选项，也没有参考的 DWT 周期计数器
    // 重新同步（`:460-486`，本驱动的周期轴另有实现），所以这里只补 TRCENA。
    if (!dap.MemRead32(kDcbDemcr, demcr)) return 1;
    if ((demcr & kDemcrTrcEna) == 0) {
        if (!dap.MemWrite32(kDcbDemcr, demcr | kDemcrTrcEna)) return 1;
    }

    // 参考 :488-502：子系统恢复，顺序 trace → 断点 → 观察点。
    if (_TraceRecovery()) return 1;
    if (_BreakpointUnitRecovery(bps)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// DebugAccessDetection —— 参考 :58-135
// ---------------------------------------------------------------------------
// 逐级往下问**一个问题**，问到哪一级答不上来就停在哪一级。
int DebugAccessDetection(Dap& dap, Core& core)
{
    (void)core;
    uint32_t value = 0;

    // 清掉"上次访问出过错"的标记 —— 接下来这几探本身就会重新置起它（如果还坏着）。
    dap.ClearAccessSuspect();

    switch (dbgAccLevel) {
    case kAccNormal:
        // 内存访问：读 DHCSR。0xE0000000..0xE00FFFFF 不参与缓存，所以这是真读。
        if (dap.MemRead32(kDcbDhcsr, value) && (value & kDhcsrCDebugEn) != 0) {
            return 0;
        }
        dbgAccLevel = kAccAccessPort;
        [[fallthrough]];

    case kAccAccessPort:
        // AP 访问：读 CSW，看 DBGSTAT。
        if (dap.ReadApReg(0, DAP_AP_REG_CSW, value) && (value & kCswDbgStat) != 0) {
            return 0;
        }
        dbgAccLevel = kAccDebugPort;
        [[fallthrough]];

    case kAccDebugPort:
    case kAccDebugPower:
        // DP 访问：读 CTRL/STAT，看有没有上电。
        if (dap.ReadDP(DAP_DP_REG_CTRL_STAT, value)) {
            if ((value & kDpPoweredMask) == kDpPoweredMask) dbgAccLevel = kAccDebugPower;
            return 0;
        }
        // 连 DP 都不应答 = 协议错误（参考 :126-127 的注释 "protocol error"）。
        dbgAccLevel = kAccInactive;
        [[fallthrough]];

    case kAccInactive:
    case kAccDbgServer:
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 复位线的兜底驱动 —— 参考在这一支是 TODO
// ---------------------------------------------------------------------------
// 参考 `DbgCM/DebugAccess.cpp:747-752`（assert）与 `:770-776`（deassert）的 else
// 分支都只有一句
//     DEVELOP_MSG("Todo: \nAssert HW Chip Reset");
// 也就是说**官方驱动在没有器件描述时根本拉不动复位线**。而 AHB 卡死的目标只有复位
// 能救（日志里 `Dap: DP write reg=2 failed` 刷屏那种，等级在 4 与 3 之间来回跳），
// 于是那条路在官方实现里是断的。
//
// 这里补上兜底：走探针自己的 nRESET 引脚（CMSIS-DAP `SWJ_Pins`，`Dap::ResetPin()`）。
// 它不依赖任何 pack 描述，任何能驱动 nRESET 的调试器都可用。
//
// ⚠ **这是本驱动的实现，不是"行到行兼容"。** 参考那一支是空实现，所以这里的行为
//   只能靠实测确认；日志会把走的是描述序列还是兜底写清楚。
static bool ResetLineAssert(Dap& dap)
{
    if (orb::pdsc::Available()) {                 // 参考的 PDSCDebug_IsEnabled()
        if (orb::pdsc::ResetHardwareAssert()) return true;
        ORB_LOG_WARN("DbgAcc: 描述里没有 ResetHardwareAssert 序列 —— 改走兜底 nRESET");
    } else {
        ORB_LOG_INFO("DbgAcc: 器件描述不可用 —— 走兜底 nRESET（参考此处是 TODO）");
    }
    return dap.ResetPin(true);
}

static bool ResetLineDeassert(Dap& dap)
{
    if (orb::pdsc::Available()) {
        if (orb::pdsc::ResetHardwareDeassert()) return true;
        ORB_LOG_WARN("DbgAcc: 描述里没有 ResetHardwareDeassert 序列 —— 改走兜底 nRESET");
    }
    return dap.ResetPin(false);
}

// ---------------------------------------------------------------------------
// DebugAccessRecovery —— 参考 :508-677
// ---------------------------------------------------------------------------
// 从 `dbgAccLevel` 那一级开始**逐级 fall-through** 往上修，每修好一级读一个
// 寄存器确认，确认了才推进下一级；任一步失败就保持当前等级，下次接着修。
//
// ⚠ 改名成 `_Inner`：公开入口 `DebugAccessRecovery()` 只做**熔断计数**
//   （见文件上方 `s_recoveryFails` 的说明）。这样内部那一堆 `return 1` 一个都不用动。
static int _DebugAccessRecoveryInner(Dap& dap, Core& core, Breakpoints* bps)
{
    const bool recovering = (dbgAccLevel != kAccNormal);
    if (dbgAccLevel == kAccNormal) return 0;
    if (dbgAccLevel == kAccDbgServer) return 1;   // 致命，不可恢复（参考 :691）

    uint32_t value = 0;
    int status = 0;

    ORB_LOG_WARN("DbgAcc: 访问等级 %d，开始逐级恢复", dbgAccLevel);

    switch (dbgAccLevel) {
    case kAccInactive:
        status = _DebugProtocolRecovery(dap);
        if (status) return status;
        dbgAccLevel = kAccDebugPort;
        [[fallthrough]];

    case kAccDebugPort:
        if (!dap.ReadDP(DAP_DP_REG_CTRL_STAT, value)) return 1;
        if ((value & kDpPoweredMask) != kDpPoweredMask) {
            status = _DebugPortRecovery(dap);
            if (status) return status;
        }
        dbgAccLevel = kAccDebugPower;
        [[fallthrough]];

    case kAccDebugPower:
        // 参考 :565-588：DP_SELECT 在 POR 之后值不可预测，先写回去；再确认 AP 可用。
        if (!dap.WriteDP(DAP_DP_REG_SELECT, 0x00000000u)) return 1;
        if (!dap.ReadApReg(0, DAP_AP_REG_CSW, value)) return 1;
        if ((value & kCswDbgStat) == 0) return 1;
        dap.InvalidateCswCache();     // 下一步要按需重编 CSW
        dbgAccLevel = kAccAccessPort;
        [[fallthrough]];

    case kAccAccessPort:
        if (!dap.MemRead32(kDcbDhcsr, value)) return 1;

        // 参考 :604：调试设置 + 子系统恢复（trace / 断点 / 观察点）都在这一步里，
        // 而且都在放开复位**之前** —— 见 _DebugSetupRecovery() 的说明。
        status = _DebugSetupRecovery(dap, core, bps);
        if (status) return status;

        // 参考 :608-641：链路修好之后把复位线**放开**。这根线是
        // `ConfigureDebugAccessRecovery()` 为了恢复而拉住的（`recoverUnderReset`），
        // 一直拉着目标就一直是死的 —— 所以"修好"的定义里必须包含放开它。
        // 位置也在参考的同一处：`_DebugSetupRecovery()` 之后。
        if (recoverUnderReset) {
            recoverUnderReset = false;
            ResetLineDeassert(dap);

            // 参考 :638：nRESET 放开之后给目标 5 ms。复位恢复会被外部电路拉长，
            // 紧接着就发 OnRecoveryExec 的话，uVision 会在核心还没稳住时去读
            // 各个窗口。
            Sleep(5);
        }

        // 参考 :652-653：恢复完成的**第一件事是通知 uVision**。
        //
        //     nR = pCbFunc(AG_CB_EXECHOOK, "OnRecoveryExec");
        //
        // uVision 从这个执行钩子跑它自己的恢复收尾（重新读各个窗口、复位调试
        // 视图）。少了这一步，链路其实已经修好了，uVision 那边仍按"坏了"显示 ——
        // 实测症状就是"目标复位成功了，但 Keil 没反应"。
        Callback(AG_CB_EXECHOOK, const_cast<char*>("OnRecoveryExec"));

        dbgAccLevel = kAccNormal;
        [[fallthrough]];

    case kAccNormal:
        // 参考 :657-663：回到正常等级之后再告诉**器件**那一侧"连接回来了"，
        // 器件为恢复专门留的硬件/状态可以收了。只在这次确实是"从坏到好"时发
        // （`recovering`），且只在描述可用时（参考的 PDSCDebug_IsEnabled()）。
        if (recovering && orb::pdsc::Available()) {
            orb::pdsc::RecoveryAcknowledge();
        }
        [[fallthrough]];

    default:
        break;
    }

    if (recovering && dbgAccLevel == kAccNormal) {
        ORB_LOG_INFO("DbgAcc: 访问已恢复 (kAccNormal)");
    }
    return (dbgAccLevel == kAccNormal) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// DebugAccessRecovery —— 熔断包装（见文件上方 `s_recoveryFails`）
// ---------------------------------------------------------------------------
int DebugAccessRecovery(Dap& dap, Core& core, Breakpoints* bps)
{
    // 会话正在卸载：不碰探针，立刻失败返回（见 Context::uninitReq）。
    if (InterlockedCompareExchange(&Get().uninitReq, 0, 0) != 0) return 1;

    if (dbgAccLevel == kAccNormal) { s_recoveryFails = 0; return 0; }

    if (s_recoveryFails >= kRecoveryGiveUp) {
        // **不再碰探针**：没有 DAP 流量、没有超时等待 ⇒ uVision 那一轮立刻结束，
        // 它会自己收掉"Trying to recover debug connection"那个模态框。
        // 只在**刚熔断那一次**报一条 ERROR，之后静默（否则每轮一行 = 刷屏）。
        if (s_recoveryFails == kRecoveryGiveUp) {
            ++s_recoveryFails;
            ORB_LOG_ERROR("DbgAcc: 恢复**连续失败 %u 次 —— 放弃（熔断）**。链路已判死："
                          "查探针/目标供电与接线，然后**重新 Connect**（会重置熔断）。"
                          "在此之前不再尝试恢复，也不再产生探针流量。",
                          static_cast<unsigned>(kRecoveryGiveUp));
        }
        return 1;
    }

    const int rc = _DebugAccessRecoveryInner(dap, core, bps);
    if (rc == 0) s_recoveryFails = 0; else ++s_recoveryFails;
    return rc;
}

// ---------------------------------------------------------------------------
// 恢复成功之后通知 uVision 刷新调试窗口 —— 参考 :711-721
// ---------------------------------------------------------------------------
// 掩码照抄参考（`:717`）：它刷的是"链路坏掉时被挂起"的那几类窗口。
static void NotifyUvRefreshWindows()
{
    struct bom* pio = orb::pdsc::Bom();
    if (!pio || !pio->Notify) {
        ORB_LOG_INFO("DbgAcc: 恢复成功（没有 BOM，无法通知 uVision 刷新窗口）");
        return;
    }
    ORB_LOG_INFO("DbgAcc: 恢复成功，通知 uVision 刷新调试窗口");
    pio->Notify(UV_UPDATE_SELECTIVE,
                (void*)(INT_PTR)(VW_MEMORY | VW_WATCHWINDOW | VW_LOGIC_ANALYZER |
                                 VW_PERF_ANALYZER | VW_RTXVIEW | VW_PERIDIALOGS |
                                 VW_AGDIDIALOGS));
}

// ---------------------------------------------------------------------------
// DebugAccessEnsure —— 参考 :679-...
// ---------------------------------------------------------------------------
int DebugAccessEnsure(Dap& dap, Core& core, Breakpoints* bps)
{
    // 会话正在卸载：连探测都不做（见 Context::uninitReq）。
    if (InterlockedCompareExchange(&Get().uninitReq, 0, 0) != 0) return 1;

    const int before = dbgAccLevel;

    DebugAccessDetection(dap, core);

    if (dbgAccLevel == kAccNormal) return 0;
    if (dbgAccLevel == kAccDbgServer) return 1;

    const int status = DebugAccessRecovery(dap, core, bps);
    if (status || dbgAccLevel != kAccNormal) return status ? status : 1;

    // 参考 :699-721：恢复成功之后还有最后一步 —— 把"已经修好了"告诉 uVision。
    //
    //     ReadD32(DBG_HCSR, &value);
    //     if (value & S_HALT) {                 // Target stopped after recovery
    //         Invalidate(); UpdateAllDlg();
    //         pio->Notify(UV_UPDATE_SELECTIVE, VW_MEMORY | VW_WATCHWINDOW | ...);
    //     }
    //
    // 链路坏掉时 uVision 会把 Memory / Watch / 逻辑分析仪窗口挂起不再刷新，而
    // "已经修好了"只有 driver 知道。`Invalidate()`/`UpdateAllDlg()` 是参考自己
    // 窗口的内部函数，driver 侧没有对应物，`UV_UPDATE_SELECTIVE` 才是等价动作。
    //
    // `before != kAccNormal` 是"这次真的是从坏到好"；参考再收一道 `value & S_HALT`
    // （目标停在原地才值得刷）。
    if (before != kAccNormal && core.IsHalted()) {
        NotifyUvRefreshWindows();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ConfigureDebugAccessRecovery —— 参考 :727-798
// ---------------------------------------------------------------------------
// uVision 的入口是 `AG_EXECITEM | AG_RECOVERY_CONF`。它只在先问过
// `AG_GETFEATURE(DBGRECOVERY)` 并拿到 1 之后才会发这一项 —— 那个握手在
// `ORBAGDI_Agdi.cpp` 的 AG_GETFEATURE 里。
int ConfigureDebugAccessRecovery(const AG_RECOVERY* pRecovery)
{
    // 参考 :733-735 是 `if (pRecovery == 0 || PlayDead) return (AG_RECOVCONF_ERR_OK);`。
    // `PlayDead` 是参考那边"目标已断开、这次会话已收摊"的标志，本驱动的等价物是
    // `Context::connected`。会话没了还去拉复位线 / 投停止请求，动的是下一次连接
    // 的句柄。
    Context& c = Get();
    if (!pRecovery || !c.connected) return AG_RECOVCONF_ERR_OK;

    if (pRecovery->underReset && !recoverUnderReset) {
        // 打开"复位下恢复"：把目标按在复位里，再从头把调试口建起来。
        //
        // 这是对付"AHB/AP 卡死"的正解 —— 一条挂住的总线靠 SWD 线复位救不回来
        // （RDDI 会答 `SWD link recovery failed: target still not responding`），
        // 而复位会把挂住的事务连同总线一起清掉。
        if (!ResetLineAssert(c.dap)) {
            ORB_LOG_WARN("DbgAcc: 复位线**没能**被拉住（描述里没有该序列，"
                         "探针也驱动不了 nRESET）—— 这次恢复大概率白跑");
        }

        recoverUnderReset = true;
        // 目标在复位里，接下来必然是完整的一次重建（参考 :758-759）。
        dbgAccLevel = kAccInactive;
        ORB_LOG_INFO("DbgAcc: 复位下恢复已打开（等级置 kAccInactive，走完整恢复）");

    } else if (!pRecovery->underReset && recoverUnderReset) {
        // 关闭：把线放开。等级**保持不变**（参考 :781-782），因为链路状态并没有
        // 因为这一次放开而变好。
        ResetLineDeassert(c.dap);
        recoverUnderReset = false;
        ORB_LOG_INFO("DbgAcc: 复位下恢复已关闭（等级保持 %d）", dbgAccLevel);
    }

    if (pRecovery->exitDebug) {
        // 参考 :785-795：uVision 从恢复对话框里要求退出调试，**分两种情况** ——
        //
        //     if (iRun) { StopRun = 1; }        // 目标在跑：友好路
        //     else      { SetPlayDead(NULL); }  // 目标没在跑：强杀
        //
        // 本驱动的 `stopReq` 是 `StopRun` 的等价物，由 `AG_STOPREQ` 置位、被
        // `RunAndWait`/`AG_WAITFORBRK` 的循环轮询 ⇒ **只对"目标在跑"有效**。
        //
        // 2026-10-14 实测（用户："这个窗口停止无效"）：对话框出现时运行循环早已
        // 退出（日志里 `GoStep: stop cause … HALTED`），`stopReq` 投出去没有任何
        // 人消费 —— 按"停止调试"什么都不发生，日志停在这一行，此后再无一条驱动
        // 日志，连 `AG_UNINIT` 都没来（会话从来没被拆掉，所以对话框不走）。
        // 官方对这种情况走强杀那一支，本驱动原先**整支缺失**（旧注释写着
        // "本驱动没有 PlayDead 这套"）。
        if (InterlockedCompareExchange(&c.running, 0, 0) != 0) {
            InterlockedExchange(&c.stopReq, 1);
            ORB_LOG_INFO("DbgAcc: exitDebug —— 目标在跑，投递停止请求（友好路）");
        } else {
            PlayDeadRequest();
            ORB_LOG_INFO("DbgAcc: exitDebug —— 目标已停，走强杀路"
                         "（PlayDead → 收摊 + MSG_UV2_TERMINATE）");
        }
    }

    return AG_RECOVCONF_ERR_OK;
}

}}  // namespace orb::agdi
