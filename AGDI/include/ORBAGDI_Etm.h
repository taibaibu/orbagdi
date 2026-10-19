/**
 * @file ORBAGDI_Etm.h
 * @brief 指令 trace（ETM / MTB）解码的共享接收端。
 * 解码器来自 orbuculum（BSD-3），三个源（ETB formatter、orbtrace OFLOW tag=2）
 * 共用同一实例，解出的地址直接进 uVision Trace Data 窗口。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// ---- 协议选择 ----
enum EtmProtocol {
    ETM_PROTO_AUTO = 0,   // 由 EtmTraceSelectProtocol() 按 CPUID 判断
    ETM_PROTO_ETM35,      // Cortex-M3 / M4
    ETM_PROTO_ETM4,       // Cortex-M7 / M33 / M55 / M85
    ETM_PROTO_MTB         // Cortex-M0+ / M23
};

/// 按 CPUID.PARTNO 选引擎，连接时调一次；读不到 CPUID 保持现状。
void EtmTraceSelectProtocol(orb::Dap& dap);

/// 直接指定引擎（etmver= 这类显式覆盖）。
void EtmTraceSetProtocol(EtmProtocol p);

/// 当前协议名（"ETM3.5" / "ETM4" / "MTB"），日志用。
const char* EtmTraceProtocolName();

// ---- 数据与状态 ----

/// 喂一批 ETM 字节。
/// 不要强制同步：分支地址是相对的，必须等到 A-SYNC / ISYNC 才有基准地址。
void EtmTraceFeed(const uint8_t* data, size_t len);

/// 强制解码器进入未同步状态。
/// 字节流有缺口时必须调，否则后续解出的全是假地址。
/// 缺口来源：RDDI 环溢出、OFLOW/COBS 丢帧、ETB 回绕。
void EtmTraceForceSync(bool isSynced);

/// 累计解出的指令地址数。
unsigned EtmTraceBranchCount();

/// 停止时的汇总（地址/原子/异常计数 + 同步状态 + 协议）。
/// 这是解码路径上唯一允许的常规日志，回调里逐条打会把热路径拖死。
void EtmTraceLogSummary(const char* where);

/// 解码器是否已同步（收到过 A-SYNC 或 ISYNC）。
bool EtmTraceIsSynced();

/// 丢掉解码器状态与计数（会话开始 / 断开 / 换协议）。
void EtmTraceReset();

} // namespace agdi
} // namespace orb
