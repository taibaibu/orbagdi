#pragma once

// PDSC ("CMSIS-Pack Debug Description") support, limited to what a flash
// download needs.
//
// uVision owns the debug description and the sequence interpreter.  It parses
// the PDSC/SDF belonging to the connected device and hands the result to the
// driver through the BOM (UV_PDSCDBG_GET_PROPERTIES in BOM.H), and it runs a
// sequence when the driver asks it to (UV_PDSCDBG_EXEC_SEQUENCE).  Running a
// sequence therefore means filling a PDSC_SEQUENCE_CONTEXT - sequence id, debug
// access variables, debug context - and calling Notify(); every target access
// the script performs comes back into this DLL through the ordinary AGDI
// entry points (AG_MemAccX, AG_DapAcc, ...).
//
// None of this is invented here: the BOM is the very object uVision returns for
// AG_CB_GETBOMPTR, and the request/response protocol is the one the reference
// driver uses (DbgCM/PDSCDebug.cpp, DbgCM/AGDI.CPP).

#include <stdint.h>

struct bom;   // BOM.H

namespace orb {
namespace pdsc {

// ---- session --------------------------------------------------------------

// Called from AG_Init(AG_INITITEM, AG_INITCALLBACK).  uVision's callback is the
// only route to the BOM, and the BOM is the only route to the debug
// description, so a driver that never stores it can never support PDSC.
void SetBom(struct bom* bom);

// 只登记备用 BOM 实例，不顶掉已拿到的 ioc。用于 dbgblk::pDbgX->pBom 那条路：
// 它与 AG_CB_GETBOMPTR 返回的 ioc 实测**不是同一个对象**，而 uVision 的
// Trace Data 取数门读的是 ioc，所以 ioc 必须始终是主指针。见 SetBomAlt() 实现。
void SetBomAlt(struct bom* bom);
bool HaveBom();

// uVision 的 BOM 对象（`pio`）。Trace Data 窗口的 TDI 注册要经它 Notify；
// 没拿到 BOM 时返回 nullptr。
struct bom* Bom();

// 把 `bTrcDisp`（"Sxxx supports Trace-Display-Window"）写到**所有已知的 BOM 实例**上。
//
// uVision 会先后给出两个 BOM（AG_CB_GETBOMPTR 与 dbgblk::pDbgX->pBom），二者不保证
// 是同一个实例；而 Trace Data 窗口的取数门读的是 uVision 自己持有的那一个。只写一个
// 实例会出现"bTrcDisp 是 1、窗口却永不取数"。见 SetBom() 的说明。
void SetTraceDisplayFlag(bool on);

/// `__traceout` —— 交给器件 pack 的 `TraceStart` 序列的参数。
///
///   bit0 = SWO 选中、bit1 = TPIU(并行) 选中、bits[21:16] = 并行位宽
///
/// 这是驱动唯一该做的"trace 物理层"动作：真正的寄存器编程（引脚/AF/`DBGMCU_CR`）由 pack 的
/// `EnableTraceSWO` / `EnableTraceTPIU` 完成（器件厂写的、读-改-写）。**写 0 会让 pack 整个
/// 空转**，于是那些配置只能靠固件手写顶着 —— 见 COMPAT_ANALYSIS.md §10.28。
void SetTraceOut(uint32_t v);

// BOM 里的 Thumb 位（"Thumb State (ARM)"，BOM.H:3636）。uVision 用它决定反汇编
// 按哪套指令集解码；官方驱动在每次寄存器刷新时发布它（DbgCM/AGDI.CPP:3624
// `pio->Thumb = (RegARM.xPSR & T_Bit) ? 1 : 0;`）。放在这里是因为只有本模块
// 包含了 BOM.H。
void SetThumbState(bool thumb);

// BOM 的 "last hit Bp" 记录（`struct hitbp`，BOM.H:3064/3952）。uVision 靠它知道
// 目标停下来了、停在哪、为什么停，然后刷新调试视图——寄存器窗口就是靠这个信号
// 才会被填充。官方在每次停下时填写（DbgCM/AGDI.CPP:7274 SetHitBp()），并在每次
// AG_GoStep 入口先清成 HIT_NOREASON（:8025）。acc 取 BOM.H:3051-3062 的
// HIT_* 常量；bpNum 不确定时传 0xFFFF。
void SetHitBp(uint32_t addr, uint16_t acc, uint16_t bpNum, uint32_t pc);
void ClearHitBp();

// Ask uVision for the debug description of the connected device.  Keeps the
// previous state when uVision has nothing to offer.  Safe to call repeatedly.
bool Refresh();

// Forget everything (AG_UNINIT, or a session that failed to connect).
void Shutdown();

// uVision offered a debug description and enabled its use.
bool Available();

// ... and flash programming sequences are enabled: from here on the download
// goes through the sequences instead of an FLM algorithm.
bool FlashSeqReady();

// One-line state for the log, or NULL when there is nothing worth reporting.
const char* Status();

// ---- flash description ----------------------------------------------------

struct FlashInfo {
    uint32_t addr;         // flash device start address
    uint32_t size;         // total size in bytes
    uint32_t pageSize;     // programming granularity
    uint32_t sectorSize;   // erase granularity of the covering block (0 = unknown)
    uint32_t progTimeout;  // per page, milliseconds (0 = driver default)
    uint32_t eraseTimeout; // per sector, milliseconds (0 = driver default)
    bool     haveArg;      // the covering block carries an argument
    uint64_t arg;          // that argument
};

// Flash description covering 'adr' (a PDSC may describe several devices).
bool FlashInfoFor(uint32_t adr, FlashInfo& out);

// Program page size covering 'adr' (0 when unknown).
uint32_t PageSizeFor(uint32_t adr);

// ---- debug sequences: connect / reset / recovery ---------------------------
//
// These are the sequences a modern device's debug description uses to *become*
// debuggable: unlocking a protected debug port, setting up the access port,
// choosing where the core stops after reset.  They are not flash sequences, but
// they run through exactly the same interpreter - a sequence id, the access
// variables, a debug context, and Notify() - so they live here.
//
// Leaving them out is why a device whose description needs DebugPortSetup to
// unlock can be recognised, identified, and still refuse to be attached to: the
// classic path assumes the DP is usable the moment it answers.  Every sequence
// below is optional; a description that has none leaves that path untouched.
//
// The names are the ones Keil's own driver carries (ULP2CM3.dll):
//   "Prepares Debug Port for connection."
//   "Connect to Debug Port and power up."
//   "Power down Debug Port and disconnect from it."
//   "Unlock device after connection to Debug Port."
//   "Configure device specific logic for trace capture."
//   "Clear configuration of device specific logic for trace capture."
//   "Configure processor where to stop after reset."
//   "Clear target configuration applied in \"ResetSetCatch\" sequence."
//   "Execute custom reset sequence."
//   "Configure device specific logic supporting recovery from a connection
//    loss, e.g. because of a low-power mode."

/// How the next connect or reset is performed.  These are the bits that go into
/// __connection, which is what the reset sequences branch on; the values are the
/// AV_CONN_RST_* ones from PDSCDebugEngine.h.
enum ResetType {
    kResetErr = 0,   // let the description decide (Autodetect)
    kResetHw  = 1,   // the reset line          - AV_CONN_RST_HW
    kResetSys = 2,   // AIRCR.SYSRESETREQ       - AV_CONN_RST_SYS
    kResetVec = 3,   // AIRCR.VECTRESET         - AV_CONN_RST_VEC
};

/// How to describe the connection to the sequences.
///   holdReset     - we are connecting while the reset line is held low
///   preConnReset  - we pulsed reset before connecting
void SetResetMode(uint32_t resetType, bool holdReset, bool preConnReset);

/// The description carries at least one debug (non-flash) sequence.
bool DebugSeqReady();

/// How many it carries, for the session report.
uint32_t DebugSeqCount();

// The connection group, in the order the reference driver runs them.
bool ConnectDebugPort();   // DebugPortSetup

/// 器件描述里确实有 `DebugPortSetup`（存在且没被 disable）—— 与 `TraceSeqAvailable()`
/// 同一个套路：`ConnectDebugPort()` 在序列不存在时返回 true，不能拿它判"跑成功没有"。
bool DebugPortSetupAvailable();

bool StartDebugPort();     // DebugPortStart
bool UnlockDevice();       // DebugDeviceUnlock
bool StartCore();          // DebugCoreStart

/// The disconnect group.  Best effort by design: the session is going away, and
/// a description that cannot clean up must not stop it from ending.
void StopCore();           // DebugCoreStop
void StopDebugPort();      // DebugPortStop

/// Reset through the description.  Falls back
/// ResetHardware -> ResetSystem -> ResetProcessor -> ResetCustomized, the order
/// the reference driver uses; returns false only when the description has none
/// of them or the one that ran failed.
bool Reset(uint32_t resetType);

/// "Stop here after reset".  ResetCatchSet() answers "did the description take
/// over?" - false means it has no such sequence (or the one it has failed), so
/// the caller has to arrange the halt itself the classic way.
bool ResetCatchSet();
bool ResetCatchClear();

/// Assert / deassert the reset line through the description.
///
/// These two are the "recover under reset" pair: `AG_RECOVERY_CONF` asks the
/// driver to hold the target in reset while uVision re-establishes a debug
/// connection it has lost (`DbgCM/DebugAccess.cpp:737-783`), and the recovery
/// ladder releases the line once the DP answers again (`:608-625`).
///
/// false means the description carries no such sequence.  There is deliberately
/// no fallback: the reference's own default implementation is a `TODO`
/// (`PDSCDebug.cpp:4057-4082` only runs the sequence), so the caller has to know
/// that the line was *not* driven.
bool ResetHardwareAssert();     // SEQ_ResetHardwareAssert
bool ResetHardwareDeassert();   // SEQ_ResetHardwareDeassert

/// RecoverySupportStart / Stop - the device-side half of surviving a lost debug
/// connection (a low-power mode that would otherwise take the DP down with it).
bool RecoveryStart();
bool RecoveryStop();

/// RecoveryAcknowledge - "Acknowledge recovery from a connection loss to the
/// device specific logic supporting the recovery" (SEQ_RecoveryAcknowledge).
///
/// The reference runs it at the very end of a successful recovery, and only when
/// the description is enabled (`DbgCM/DebugAccess.cpp:659-663`).  Optional by
/// design: the reference marks it "No default implementation" and swallows an
/// absent sequence (`PDSCDebug.cpp:4260-4273`).
bool RecoveryAcknowledge();

/// TraceStart / TraceStop - the device-side trace capture setup.
bool TraceStart();
bool TraceStop();

/// Does the description carry a `TraceStart` sequence?
///
/// This is the gate for the driver's own `DBGMCU_CR` fallback, and it mirrors the
/// reference's test exactly: `DbgCM/PDSCDebug.cpp:4279 PDSCDebug_TraceStart()`
/// runs `_ExecuteSequence(SEQ_TraceStart)` and, when the sequence exists, returns
/// without touching anything - the comment under it reads "No default
/// implementation".  The pack's `EnableTraceSWO` / `EnableTraceTPIU` already write
/// the trace pins and `DBGMCU_CR`; a driver that writes them too is writing a
/// second time (COMPAT_ANALYSIS.md §10.28).
///
/// The driver's own write is the fallback for a description that has no such
/// sequence (older device, or a session without PDSC at all).
bool TraceSeqAvailable();

// ---- flash sequences ------------------------------------------------------

// Optional hooks: return true when the sequence is absent, so a device that
// only describes erase+program still gets programmed.
bool Init();
bool Uninit();
bool EraseDone();
bool ProgramDone();

// Required: the caller only takes this path when FlashSeqReady() is true.
bool EraseSector(uint32_t adr);
bool EraseChip(uint32_t adr, uint32_t size);
bool ProgramPage(uint32_t adr, uint32_t size, uint32_t dataAdr);

// Debug Context under which the next sequence runs (DBGCON_* in
// PDSCDebugEngine.h); the sequences branch on it.
void     SetDebugContext(uint32_t ctx);
uint32_t DebugContext();

}}
