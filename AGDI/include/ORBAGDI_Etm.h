/**
 * @file ORBAGDI_Etm.h
 * @brief 指令 trace（ETM / MTB）解码 —— AGDI 层的共享接收端。
 *
 * 解码器本身**不是自研**：来自 orbuculum（`traceDec/`，BSD-3-Clause），
 * 三套引擎 + 调度器，覆盖全部带 trace 的 ARM 内核：
 *
 *     ETMv3.5  Cortex-M3 / M4
 *     ETMv4    Cortex-M7 / M33 / M55 / M85
 *     MTB      Cortex-M0+ / M23（片上 RAM 环形缓冲，**不是字节流**）
 *
 * 之所以换掉原来的自研骨架：它的分支地址格式用的是 ETMv4 的短/长地址编码，
 * 而 ISYNC 又少读了一个 Info Byte —— 两者决定了它解不出任何正确地址
 * （实测症状：窗口里全是 `0xB9xxxxxx`）。详见 COMPAT_ANALYSIS.md §10.21。
 *
 * 本头文件只暴露"接收端"接口。ETM 字节流有两个来源：
 *   - 片上 ETB 的 formatter        （ORBAGDI_Etb.cpp）
 *   - orbtrace 探针的 OFLOW tag=2  （ORBAGDI_OrbTrace.cpp）
 * 两者共用**同一个**解码器实例，解出的指令地址直接进 uVision 的 Trace Data
 * 窗口（`TraceWinEtmInstruction()`，Keil 标准流程）。
 *
 * 归属：**AGDI 层**。RDDI 层不做 trace 解码（见 `COMPAT_ANALYSIS.md` §8）。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// ============================================================================
// 协议选择
// ============================================================================
enum EtmProtocol {
    ETM_PROTO_AUTO = 0,   // 由 EtmTraceSelectProtocol() 按 CPUID 判断
    ETM_PROTO_ETM35,      // Cortex-M3 / M4
    ETM_PROTO_ETM4,       // Cortex-M7 / M33 / M55 / M85
    ETM_PROTO_MTB         // Cortex-M0+ / M23
};

/// 按目标 CPUID 的 PARTNO 选引擎。连接时调一次；读不到 CPUID 就保持现状。
void EtmTraceSelectProtocol(orb::Dap& dap);

/// 直接指定引擎（`etmver=` 这类显式覆盖用）。
void EtmTraceSetProtocol(EtmProtocol p);

/// 当前生效的协议名（"ETM3.5" / "ETM4" / "MTB"），日志用。
const char* EtmTraceProtocolName();

// ============================================================================
// 数据与状态
// ============================================================================

/// 喂一批 ETM 字节。
///
/// **不要在任何情况下"强制同步"。** ETM 的分支地址是相对的：必须等到
/// A-SYNC（5×0x00 + 0x80）或 ISYNC 才有基准地址。orbuculum 的解码器自己
/// 会等（`traceDecoder_etm35.c:830` 的 `&& j->rxedISYNC` 就是那道门），
/// 而"跳过 sync 搜索"正是自研骨架当初的做法之一 —— 没有基准地址就开始
/// 上报，等于直接产出垃圾。
void EtmTraceFeed(const uint8_t* data, size_t len);

/// 强制解码器进入"未同步"（包 `TRACEDecoderForceSync`）。
///
/// **字节流出现缺口时必须调**：ETM 分支地址是相对的，缺口之后解出来全是假地址，
/// 而解码器自己不知道丢了字节。照 orbuculum `Src/orbmortem.c:1084-1088`
/// （环回绕 ⇒ "any guesses about sync status are invalid" ⇒ ForceSync(false)）。
///
/// 三个缺口来源：RDDI 环溢出丢帧、OFLOW/COBS 丢帧（ORBAGDI_OrbTrace.cpp）、
/// ETB 回绕（ORBAGDI_Etb.cpp）。调完之后丢弃到下一个 A-SYNC/ISYNC 才有基准地址。
void EtmTraceForceSync(bool isSynced);

/// 累计解出的指令地址数（`EV_CH_ADDRESS` 事件）。日志与统计用。
unsigned EtmTraceBranchCount();

/// 停止时的汇总（地址 / 原子 / 异常计数 + 同步状态 + 协议）。采集段结束时调一次。
///
/// **这是 ETM 解码路径上唯一允许的常规日志。** 不要在解码回调里逐条打日志：
/// 那是热路径（每 3 字节左右一次，实测 3 秒 600 万条地址），而 `ORB_LOG_*`
/// 没有限流，一条 INFO 就是一条 `fprintf`。
void EtmTraceLogSummary(const char* where);

/// 解码器是否已同步（收到过 A-SYNC 或 ISYNC）。
bool EtmTraceIsSynced();

/// 丢掉解码器状态与计数（会话开始 / 断开 / 换协议）。
void EtmTraceReset();

} // namespace agdi
} // namespace orb
