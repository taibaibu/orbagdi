/**
 * @file ORBAGDI_DebugAccess.h
 * @brief 调试访问的等级探测与恢复 —— `DbgCM/DebugAccess.{h,cpp}` 的对应件。
 *
 * 参考的做法**不是**"访问失败就重试"，也不是"失败就动线复位"。它是一套
 * **访问等级状态机** + **逐级恢复阶梯**：
 *
 *  1. `dbgAccLevel` 记住"现在能访问到哪一级"（六级，见下）。
 *  2. `DebugAccessDetection()` 逐级往下问一个问题，问到哪级答不上就停在哪级。
 *  3. `DebugAccessRecovery()` 从坏的那一级开始**逐级 fall-through** 往上修，
 *     每修好一级都读一个寄存器确认，确认了才推进下一级。
 *  4. 最底层那一级（`DA_INACTIVE`，DP 连 `CTRL/STAT` 都不应答）才动**线复位**；
 *     而且只要器件描述里有 `DebugPortSetup` 序列，就**优先跑序列**，线复位只作兜底。
 *  5. 日常恢复由**后台线程**做（参考是 DSMonitor），所以内存窗口/寄存器窗口
 *     再访问时链路已经修好了 —— 恢复不发生在访问路径上。
 *
 * 调用点也照参考：`DebugAccessEnsure()` 只在 flash 操作之后、reset 之后调
 * （`AGDI.CPP:4459`、`Flash.cpp:1864/1909/1981`、`PDSCDebug.cpp:4928`），
 * 不在 `AG_MemAccX` 里调。
 */
#pragma once

#include "ORBAGDI.h"      // AG_RECOVERY / AG_RECOVCONF_ERR_* (AGDI.H)
#include "ORBAGDI_Dap.h"

namespace orb {
class Core;
class Breakpoints;

namespace agdi {

// 访问等级。值与语义逐条对应 `DbgCM/DebugAccess.h:36-44`。
enum AccessLevel : int {
    kAccNormal     = 0,  // 内存访问正常
    kAccAccessPort = 1,  // 能访问 AP，内存访问失败
    kAccDebugPower = 2,  // 能访问 DP、调试系统已上电，AP 访问失败或未就绪
    kAccDebugPort  = 3,  // 能访问 DP，调试系统上不了电
    kAccInactive   = 4,  // 协议错误，调试口看起来不活动
    kAccDbgServer  = 5,  // 多调试连接的调试服务器挂了（致命，不可恢复）
};

// 当前访问等级（参考的 `dbgAccLevel`，同名同义）。
extern int dbgAccLevel;

// 会话开始/结束时复位等级与内部状态。
void DebugAccessInit();
void DebugAccessUninit();

// 探测当前能访问到哪一级，结果写进 `dbgAccLevel`。返回 0 = 探测本身没出错
// （等级仍可能是坏的）。
int DebugAccessDetection(Dap& dap, Core& core);

// 从 `dbgAccLevel` 那一级开始往上修。返回 0 = 修到 `kAccNormal`。
int DebugAccessRecovery(Dap& dap, Core& core, Breakpoints* bps);

// 探测 + 需要就恢复。参考的 `DebugAccessEnsure()`。
int DebugAccessEnsure(Dap& dap, Core& core, Breakpoints* bps);

// ---------------------------------------------------------------------------
// 复位下恢复 —— `AG_RECOVERY_CONF`
// ---------------------------------------------------------------------------
// uVision 先从 `AG_GETFEATURE(DBGRECOVERY)` 得知本驱动支持恢复（见 AG_Init 处的
// 握手），然后在它自己的恢复流程里发 `AG_EXECITEM | AG_RECOVERY_CONF`。参考的
// 处理在 `DbgCM/DebugAccess.cpp:727-798`。
//
// `underReset` 是"把目标按在复位里再重建连接"——对一个 AHB/AP 已经卡死的目标，
// 这往往是唯一能把它拉回来的手段；`exitDebug` 是 uVision 从恢复对话框里要求退出
// 调试会话，驱动要让目标停下来，运行循环才走得完。
//
// 返回 `AG_RECOVCONF_ERR_OK`（uVision 期望的答复）。
int ConfigureDebugAccessRecovery(const AG_RECOVERY* pRecovery);

// ---------------------------------------------------------------------------
// PlayDead —— 官方 `PlayDead` / `SetPlayDead` / `ExecPlayDead`
// （DbgCM/AGDI.CPP:77、:1161-1234）
// ---------------------------------------------------------------------------
// 触发点只有一个：uVision 从"无法访问目标硬件"的恢复对话框里按了**停止调试**
// （`AG_EXECITEM | AG_RECOVERY_CONF` 带 `exitDebug`）。官方在那一处分两种情况
// （`DbgCM/DebugAccess.cpp:785-795`）：
//
//     if (iRun) { StopRun = 1; }          // 目标在跑：友好路，运行循环自己会停
//     else      { SetPlayDead(NULL); }    // 目标没在跑：**强杀**
//
// 后一种正是那个对话框出现的场景 —— 运行循环早就退出了（日志里
// `GoStep: stop cause … HALTED`），`StopRun`（本驱动的 `stopReq`）投出去
// **没有任何人消费**，于是"停止调试"按下去毫无反应。官方接着在 `AG_Init` 末尾
// 执行 `ExecPlayDead()`：
//
//     StopTarget();                                      // 收摊
//     PostMessage(hMfrm, Uv2Msg, MSG_UV2_TERMINATE, 0);  // ← 这一下才结束会话
//
// `PlayDeadRequest()` 置位（= 官方 `SetPlayDead`），`PlayDeadPending()` 查询；
// 真正的执行在 `AG_Init` 里（见 ORBAGDI_Agdi.cpp 的 `PlayDeadExec()`）。
void PlayDeadRequest();
bool PlayDeadPending();
void PlayDeadClear();

/// 参考的 `recoverUnderReset`（`DbgCM/DebugAccess.cpp:53`）：复位线当前是不是
/// 由我们拉着。`DebugAccessRecovery()` 在链路修好之后据此把它放开（`:608-625`）。
extern bool recoverUnderReset;

}}  // namespace orb::agdi
