/**
 * @file ORBAGDI_Itm.h
 * @brief ITM / SWO 控制台 + DWT PC 采样的执行历史（AGDI trace）。
 * 经 RDDI 的 CMSIS_DAP_SWO_* 取流并解码，软件激励包交给 uVision 的 ITM 窗口；
 * 硬件编程都在本模块做，失败只关控制台，不影响调试。
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// 已加载的 RDDI 宿主是否导出 SWO 入口。
bool ItmHostReady();

// 向宿主通告 SWO 参数，编程 DEMCR/ITM/TPIU/DWT 并启动采集；失败返回 false。
// acprBaseKhz 是目标侧 TPIU 异步分频的基准（与探针侧 baudHz 不是一回事）。
bool ItmStart(orb::Dap& dap, int baudHz, int acprBaseKhz);

// ---- 目标侧 trace 编程 ----
// 探针只负责抓引脚，目标侧的 DEMCR/TPIU/ITM/DWT 必须由驱动经 SWD 配好。
// ItmStart() 是这部分加宿主 SWO 引擎；orbtrace 路径只用这部分。
struct TraceSetup {
    enum Mode { SwoNrz, SwoManchester, Parallel };
    Mode mode;
    int  width;    // parallel port width in bits (1/2/4); ignored for SWO
    int  baudHz;   // SWO bit clock; ignored for parallel
    int  acprBaseKhz;  // 目标 TPIU 异步分频的基准时钟
};

bool ItmProgramTarget(orb::Dap& dap, const TraceSetup& setup);

// ---- 复位后的重新武装 ----
// 复位会清掉目标侧 trace 寄存器，而它们只在 Connect() 里编程一次，
// 于是“跑→停→Reset→再跑”第二次不再产生任何包，导航条停在上一次数据上。
// 做法是把上次实际写进目标的那份配置重写一遍，不重开会话、不动统计。
bool ItmRearmTarget(orb::Dap& dap);

// Stop capture and switch the trace hardware back off.
void ItmStop(orb::Dap& dap);

bool ItmActive();

// ---- ETB trace arm / disarm ----
// 为 ETB 采集编程 ITM/DWT/DEMCR，不碰 TPIU/SWO 寄存器。
// 用 ITMENA | DWTENA | ATBID，不带 SYNCENA（与 Keil 的 ETB 路径一致）。
bool ItmArmForEtb(orb::Dap& dap);

// 关掉目标的 ITM/DWT trace 硬件（与 SWO 停止序列共用）。
void ItmTraceOff(orb::Dap& dap);

// 排空 SWO 缓冲并把解码后的包转给 uVision。
// timeMs 是时间预算而不是字节上限，写死轮数会在高速链路上成为天花板。
// 返回本轮搬走的字节数；调用方是抽数 worker 与停机收尾。
unsigned ItmPump(orb::Dap& dap, unsigned timeMs);

// ---- ETB capture feed ----
// ETB 采集从流起点开始，不需要 48-bit 同步搜索，否则会丢掉开头没有 sync 的包。
void ItmResyncDecoder();

// 打回未同步，重新找 48-bit sync。
// 字节流出现缺口后必须调，否则解码器把数据当包头，printf 输出垃圾。
void ItmLoseSync();

// 把已采集的 ITM 字节喂给同一个解码器与回调；必须在 uVision 线程上跑。
void ItmFeedBytes(const uint8_t* data, int len);

// 打一行时间轴现场：ITM: TS 包=N 周期=M 状态=S (<where>)。
// 各 ITM 来源关闭时调一次；前缀用 ASCII，测试脚本按它定位。
void ItmLogTsStats(const char* where);

// ---- DWT PC-sample execution history (AG_HistFunc) ----
// Keil 约定：记录 1 最新，编号越大越旧。

// Forget everything (start of a debug session).
void HistReset();

// Number of records currently held.
unsigned HistCount();

// Fetch one record.  Returns false when the record is out of range.
bool HistPc(unsigned record, uint32_t& pc);

} // namespace agdi
} // namespace orb
