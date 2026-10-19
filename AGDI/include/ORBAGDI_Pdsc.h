#pragma once

// PDSC（CMSIS-Pack 调试描述）支持，只做 flash 下载需要的部分。
// uVision 解析描述并执行序列，驱动只负责填上下文、调 Notify()；
// 脚本里每次目标访问都经 AGDI 入口回到本 DLL。

#include <stdint.h>

struct bom;   // BOM.H

namespace orb {
namespace pdsc {

// ---- session ----

// 保存 uVision 回调给出的 BOM（通往调试描述的唯一途径）。
void SetBom(struct bom* bom);

// 只登记备用 BOM 实例，不顶掉已有的 ioc：两者实测不是同一对象，
// 而 uVision 的 Trace Data 取数门读的是 ioc。
void SetBomAlt(struct bom* bom);
bool HaveBom();

// uVision 的 BOM 对象（pio），没拿到时返回 nullptr。
struct bom* Bom();

// 把 bTrcDisp 写到所有已知 BOM 实例上。
// uVision 会先后给出两个实例，只写一个会出现“标志是 1、窗口却不取数”。
void SetTraceDisplayFlag(bool on);

/// __traceout：交给 pack 的 TraceStart 序列。
/// bit0 = SWO、bit1 = TPIU，bits[21:16] = 并行位宽。写 0 会让 pack 整个空转，
/// 真正的寄存器编程由 pack 的 EnableTraceSWO / EnableTraceTPIU 完成。
void SetTraceOut(uint32_t v);

// BOM 的 Thumb 位：uVision 据此选择反汇编指令集，每次寄存器刷新时发布。
void SetThumbState(bool thumb);

// BOM 的 "last hit Bp"：uVision 靠它知道目标停了、停在哪、为什么停。
// acc 取 HIT_* 常量，bpNum 不确定时传 0xFFFF。
void SetHitBp(uint32_t addr, uint16_t acc, uint16_t bpNum, uint32_t pc);
void ClearHitBp();

// 向 uVision 要目标调试描述；没有可取的就保持原状，可重复调用。
bool Refresh();

// Forget everything (AG_UNINIT, or a session that failed to connect).
void Shutdown();

// uVision 提供了调试描述并启用了它。
bool Available();

// 且 flash 编程序列已启用：下载走序列而不是 FLM。
bool FlashSeqReady();

// One-line state for the log, or NULL when there is nothing worth reporting.
const char* Status();

// ---- flash description ----

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

// ---- debug sequences: connect / reset / recovery ----
// 让器件“变得可调试”的序列（解锁调试口、配 AP、决定复位后停在哪），
// 与 flash 序列共用同一解释器，所以放在这里；每个序列都是可选的。

/// 下一次连接/复位怎么做，取值即 __connection 里序列分支用的 AV_CONN_RST_*。
enum ResetType {
    kResetErr = 0,   // let the description decide (Autodetect)
    kResetHw  = 1,   // the reset line          - AV_CONN_RST_HW
    kResetSys = 2,   // AIRCR.SYSRESETREQ       - AV_CONN_RST_SYS
    kResetVec = 3,   // AIRCR.VECTRESET         - AV_CONN_RST_VEC
};

/// 告诉序列这次连接的性质：holdReset = 复位线被拉住时连接，
/// preConnReset = 连接前已打过复位脉冲。
void SetResetMode(uint32_t resetType, bool holdReset, bool preConnReset);

/// The description carries at least one debug (non-flash) sequence.
bool DebugSeqReady();

/// How many it carries, for the session report.
uint32_t DebugSeqCount();

// The connection group, in the order the reference driver runs them.
bool ConnectDebugPort();   // DebugPortSetup

/// 描述里确实有 DebugPortSetup：序列不存在时 ConnectDebugPort() 也返回 true，
/// 不能用它判断“跑成功没有”。
bool DebugPortSetupAvailable();

bool StartDebugPort();     // DebugPortStart
bool UnlockDevice();       // DebugDeviceUnlock
bool StartCore();          // DebugCoreStart

/// 断开组，尽力而为：描述清理不干净也不能阻止会话结束。
void StopCore();           // DebugCoreStop
void StopDebugPort();      // DebugPortStop

/// 经描述复位，依次回退 ResetHardware -> ResetSystem -> ResetProcessor -> ResetCustomized。
bool Reset(uint32_t resetType);

/// “复位后停在这”。ResetCatchSet() 返回 false 表示描述没有该序列或执行失败。
bool ResetCatchSet();
bool ResetCatchClear();

/// 经描述拉/放复位线（recover under reset 用）。
/// 返回 false 表示描述没有该序列，没有后备实现，调用方要自己知道线没被驱动。
bool ResetHardwareAssert();     // SEQ_ResetHardwareAssert
bool ResetHardwareDeassert();   // SEQ_ResetHardwareDeassert

/// RecoverySupportStart / Stop：设备侧扛住调试连接丢失（如低功耗）。
bool RecoveryStart();
bool RecoveryStop();

/// RecoveryAcknowledge：恢复成功收尾时调，描述没有该序列也算成功。
bool RecoveryAcknowledge();

/// TraceStart / TraceStop - the device-side trace capture setup.
bool TraceStart();
bool TraceStop();

/// 描述里是否有 TraceStart：有的话驱动不再自己写 DBGMCU_CR，
/// 否则会和 pack 的 EnableTraceSWO / EnableTraceTPIU 写重。
bool TraceSeqAvailable();

// ---- flash sequences ----

// 可选钩子：序列不存在时返回 true，只有擦+写描述的器件也能编程。
bool Init();
bool Uninit();
bool EraseDone();
bool ProgramDone();

// 必需：调用方只在 FlashSeqReady() 为真时走这条路。
bool EraseSector(uint32_t adr);
bool EraseChip(uint32_t adr, uint32_t size);
bool ProgramPage(uint32_t adr, uint32_t size, uint32_t dataAdr);

// 下一次序列运行所用的 Debug Context（DBGCON_*），序列按它分支。
void     SetDebugContext(uint32_t ctx);
uint32_t DebugContext();

}}
