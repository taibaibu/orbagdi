/**
 * @file ORBAGDI_DebugAccess.h
 * @brief 调试访问的等级探测与恢复（对应 DbgCM/DebugAccess）。
 * 参考实现不是“失败就重试/线复位”，而是等级状态机 + 逐级恢复阶梯。
 */
#pragma once

#include "ORBAGDI.h"      // AG_RECOVERY / AG_RECOVCONF_ERR_* (AGDI.H)
#include "ORBAGDI_Dap.h"

namespace orb {
class Core;
class Breakpoints;

namespace agdi {

// 访问等级，值与语义对应 DbgCM/DebugAccess.h。
enum AccessLevel : int {
    kAccNormal     = 0,  // 内存访问正常
    kAccAccessPort = 1,  // 能访问 AP，内存访问失败
    kAccDebugPower = 2,  // 能访问 DP、调试系统已上电，AP 访问失败或未就绪
    kAccDebugPort  = 3,  // 能访问 DP，调试系统上不了电
    kAccInactive   = 4,  // 协议错误，调试口看起来不活动
    kAccDbgServer  = 5,  // 调试服务器挂了（致命，不可恢复）
};

// 当前访问等级。
extern int dbgAccLevel;

// 会话开始/结束时复位等级与内部状态。
void DebugAccessInit();
void DebugAccessUninit();

// 探测当前能访问到哪一级，结果写进 dbgAccLevel；返回 0 表示探测本身没出错。
int DebugAccessDetection(Dap& dap, Core& core);

// 从 dbgAccLevel 那一级逐级往上修；返回 0 表示已恢复到 kAccNormal。
int DebugAccessRecovery(Dap& dap, Core& core, Breakpoints* bps);

// 探测 + 需要就恢复。
int DebugAccessEnsure(Dap& dap, Core& core, Breakpoints* bps);

// ---- 复位下恢复：AG_RECOVERY_CONF ----

// underReset 把目标按在复位里再重建连接（AP 卡死时唯一手段）；
// exitDebug 表示 uVision 要求退出调试会话。
// 返回 AG_RECOVCONF_ERR_OK。
int ConfigureDebugAccessRecovery(const AG_RECOVERY* pRecovery);

// ---- PlayDead ----

// 目标没在跑时 uVision 按“停止调试”：StopRun 无人消费，必须走这条路强杀。
// 置位后由 AG_Init 执行，PostMessage(MSG_UV2_TERMINATE) 结束会话。
void PlayDeadRequest();
bool PlayDeadPending();
void PlayDeadClear();

/// 复位线当前是不是由我们拉着；链路修好后据此放开。
extern bool recoverUnderReset;

}}  // namespace orb::agdi
