/**
 * @file ORBAGDI_Itm.h
 * @brief ITM / SWO console + DWT PC-sample execution history (AGDI trace).
 *
 * The AGDI layer streams the target's SWO pin through the RDDI host's
 * CMSIS_DAP_SWO_* entry points (the same four exports Keil's own
 * CMSIS_AGDI.dll binds), decodes the ITM packet stream and hands the
 * software-stimulus packets to uVision's ITM / printf viewers with
 * AG_Serial(AG_ITMBOUT, ...).  DWT PC-sample packets feed the execution
 * history ring that backs AG_HistFunc.
 *
 * The console is off by default and is switched on by driver options:
 *      swo=<baud>     SWO baud rate in Hz                (0 = off)
 *      cpu=<kHz>      core clock for the TPIU prescaler  (default 72000)
 *
 * All hardware programming (DEMCR / ITM / TPIU / DWT) is done by ItmStart so
 * the target needs no SWO setup of its own; only the application's ITM writes
 * are required.  Failures are soft: the console stays off and the rest of the
 * debugger is unaffected.
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// Does the loaded RDDI host export the SWO entry points?
bool ItmHostReady();

// Announce the SWO port/transport/baud to the RDDI host, program DEMCR / ITM /
// TPIU / DWT on the target and start SWO capture.
// Returns false (console left off) when SWO is unavailable or was refused.
// `acprBaseKhz` is the clock the target's TPIU async prescaler divides by
// (`orb::cfg::SwoAcprBaseKhz()`).  Which *physical* clock that is - the core
// clock or an independent TRACECLKIN - is still undecided (see that function and
// the A/B experiment in Todo.md); what is *not* optional is that this is the
// target-side role, distinct from the probe-side rate `baudHz` came from.
bool ItmStart(orb::Dap& dap, int baudHz, int acprBaseKhz);

// ---- Target-side trace programming ----------------------------------------
// Programs DEMCR / TPIU / ITM / DWT **on the target** so that it actually emits
// trace.  This is the half of the job the probe cannot do: the ORBTrace
// firmware only captures and formats the trace pins (its trace interface has
// exactly two control requests, "Set Input Format" and "Set Async Baudrate"),
// so whoever wants trace has to configure the target over SWD first.  Keil does
// it from its Trace settings, orbuculum does it from a GDB script
// (orbuculum/Support/gdbtrace.init), and this driver does it here.
//
// ItmStart() is this plus the RDDI host's SWO engine; the orbtrace path wants
// only this part, because that probe has no CMSIS-DAP SWO command family.
struct TraceSetup {
    enum Mode { SwoNrz, SwoManchester, Parallel };
    Mode mode;
    int  width;    // parallel port width in bits (1/2/4); ignored for SWO
    int  baudHz;   // SWO bit clock; ignored for parallel
    int  acprBaseKhz;  // clock the **target's** TPIU async prescaler divides by
                       // (`cfg::SwoAcprBaseKhz()`; currently the core clock - the
                       // HCLK-vs-TRACECLKIN question is recorded there).
};

bool ItmProgramTarget(orb::Dap& dap, const TraceSetup& setup);

// ---- 复位后的重新武装（2026-10-15）-----------------------------------------
// 复位把目标侧的 trace 寄存器整套打回默认（`DEMCR.TRCENA`、TPIU、`ITM_TCR`、
// DWT、ETMCR），而它们的编程只发生在 `Connect()` 里 —— `Connect()` 第二句就是
// `if (c.connected) return true;`。于是"跑一次 → 停 → 按 Reset → 再跑"的第二次：
// 目标一个 trace 包都不发，记录缓冲不再增长，uVision 的 Navigation 条就停在上一次
// 的范围上（用户报告的原话："复位重新跑 stop 后导航条还是之前的数据，没有新的"）。
//
// 官方在**复位那一步**做同一件事：`ResetTarget()` 每次复位后都写
// `DEMCR = TRCENA | MONCONF_RST_VECT_CATCH`（`DbgCM/AGDI.CPP:2550`，注释
// "Enable Trace"）；trace 寄存器子系统则由 `Trace_Recovery()`（= `Trace_Setup()`）
// 重配。本函数是这两者的合并版：**把上一次实际编进目标的那份配置原文重写一遍**
// （`ItmProgramTarget` 内部本来就写 `DEMCR |= TRCENA`）。
//
// 为什么用"上次那份"而不是让调用方按选项重算：SWO 那条路的目标侧 TPIU 分频是按
// **探针实际接受**的波特率算的（`ItmStart` 里的 `ProbeSwoBaud`），按选项重算会在
// 探针拒档时漂移 —— 症状是复位后 SWO 出乱码，比"没有数据"更难查。
//
// 不动采集侧：不重开探针/宿主的会话（本来就在跑，重开会丢掉已收到的字节），
// 也不碰任何采集统计与记录缓冲。之前没武装过（Trace 页关着或那次武装失败）时
// 返回 false。
bool ItmRearmTarget(orb::Dap& dap);

// Stop capture and switch the trace hardware back off.
void ItmStop(orb::Dap& dap);

bool ItmActive();

// ---- ETB trace arm / disarm -----------------------------------------------
// Program the target's ITM / DWT / DEMCR for ETB capture.  Unlike ItmStart this
// touches no TPIU/SWO register - the trace goes to the on-chip ETB, which the
// ETB module reads back over the MEM-AP.  Keil's ETB path uses
// ITMENA | DWTENA | ATBID, without SYNCENA.
bool ItmArmForEtb(orb::Dap& dap);

// Switch the target's ITM / DWT trace hardware back off (shared with the SWO
// stop sequence).
void ItmTraceOff(orb::Dap& dap);

// Drain the SWO buffer and forward the decoded packets to uVision.
//
// `timeMs` 是**时间预算**，不是字节上限 —— 与官方 `Trace_Read(time, ts)` 同口径
// （`DbgCM/Trace.cpp:2011-2039`：`do { SWV_Read(time); ... } while (GetTickCount()
// - tick < time)`）。原先写死的 `round < 32` 是一个隐含的 2 MB 上限，与链路速率
// 无关，换成 USB3/USB4 时就成了硬天花板。
//
// 返回本轮从探针搬走的字节数（0 = 排空或源没开）。
//
// 调用者：抽数 worker（`ORBAGDI_TracePump.cpp`）以及停机收尾那一次。**不再**由
// uVision 的运行循环调用 —— 官方同样如此，运行循环只 `Sleep(50)`
// （`DbgCM/AGDI.CPP:3155-3173`）。
unsigned ItmPump(orb::Dap& dap, unsigned timeMs);

// ---- ETB capture feed -----------------------------------------------------
// An ETB capture starts at the beginning of the ITM stream, so the 48-bit sync
// hunt the SWO path relies on is unnecessary (and would drop the first packets
// when the capture contains no sync).  ItmResyncDecoder() puts the decoder at
// that known boundary; Keil's own ETB ITM decoder works the same way.

// Place the decoder at a known stream boundary (call once before a capture).
void ItmResyncDecoder();

// 与 ItmResyncDecoder() 相反：打回"未同步"，重新找 48-bit sync。
// 用在字节流出现缺口之后（如 SWO 缓冲溢出丢字节）—— 否则解码器会把数据当包头，
// printf 里出垃圾。ITM_TCR 置了 SYNCENA，目标会周期性发同步包，能自愈。
void ItmLoseSync();

// Feed already-captured ITM bytes (e.g. read out of the ETB) through the same
// decoder and the same uVision callbacks the SWO path uses.  Must run on
// uVision's own thread.
void ItmFeedBytes(const uint8_t* data, int len);

// 打一行时间轴的现场证据（P0-F）：`ITM: TS 包=N 周期=M 状态=S (<where>)`
// 各 ITM 来源关闭时调一次（ItmStop / OrbTraceStop）。
// `包 = 0` = 目标没发时间戳包（TSENA 没置，或 ETM 把端口吃光，见 §10.32）；
// `包 > 0` 而窗口没有真实时间才是驱动的问题。
// 前缀用 ASCII：测试脚本按它定位（`test\trace_stream.ps1`）。
void ItmLogTsStats(const char* where);

// ---- DWT PC-sample execution history (AG_HistFunc) ------------------------
// Keil's convention: record 1 is the newest sample and increasing record
// numbers run towards older samples.

// Forget everything (start of a debug session).
void HistReset();

// Number of records currently held.
unsigned HistCount();

// Fetch one record.  Returns false when the record is out of range.
bool HistPc(unsigned record, uint32_t& pc);

} // namespace agdi
} // namespace orb
