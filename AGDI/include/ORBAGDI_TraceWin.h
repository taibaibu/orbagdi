/**
 * @file ORBAGDI_TraceWin.h
 * @brief uVision "Trace Data" 窗口的上报通道（TDI）。
 * 记录模型是块链 + 紧凑记录，查询时展开并给出合成的周期号。
 * ITM 与 ETM 是两套互不相干的流与计数器，不能互相顶替。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

// ---- 会话 / 注册 ----

// 把 TDI 回调表注册给 uVision，并描述窗口的列与过滤器；幂等。
// 没有 BOM 时返回 false，Trace 窗口不可用，其余调试功能不受影响。
bool TraceWinInit();

// 会话结束/断开：清记录、注销注册状态；下次 Connect 重新注册。
void TraceWinShutdown();

// 把当前配置串注册进 uVision 的项目（UV_NEW_TARG_SETTINGS）。
void TraceWinPublishSettings();

// 置/清 bom::bTrcDisp（“trace 数据展示已启用”）。
// uVision 靠它决定 Trace Data 窗口是否取记录，必须落在它读的那个 BOM 实例上。
// 返回置位后的实际状态；where 只用于日志。
bool TraceWinAssertTraceDisplay(bool setBit, const char* where);

// 诊断：只读地把 UV4.exe 里取数门链的当前值打进日志（默认关闭）。
void TraceWinReadHostGates(const char* where);

// 重新下发 TDI 接口表与窗口配置。
// 窗口在 UV_TRACE_IF_INIT 之后才创建，紧跟的 WIN_CONFIG 可能被丢弃，
// 导致窗口没有列/过滤器、永远不来取记录。
void TraceWinReconfigure();

// 窗口已注册且 BOM 可用。
bool TraceWinReady();

// "Update" 按钮的回调（返回 true = 拿到新数据）；传 nullptr 取消。
void TraceWinSetRefreshHook(bool (*fn)());

// ---- 记录（全在 uVision 线程上调用） ----

// ITM 软件激励：port 端口号，data 载荷（LSB 在前），len 1..4 字节。
void TraceWinItmStimulus(uint32_t port, const uint8_t* data, unsigned len);

// DWT PC 采样。
void TraceWinPcSample(uint32_t pc);

// DWT 事件计数包；mask 是 TR_EVT_* 位。
void TraceWinEvent(uint32_t mask);

// 异常 trace 包；packed 是包里的小端原值，窗口自己拆类型与编号。
void TraceWinExceptionTrace(uint32_t packed);

// ETM 分支；executed = 该指令是否真的执行（条件分支未走也要报一条）。
void TraceWinEtmInstruction(uint32_t addr, bool executed);

// 流控记录；flowType 取 TR_TYPE_FLOW_*。
void TraceWinFlow(uint32_t flowType);

// ---- 周期轴 ----
// 周期号即 uVision 的时间基准（time = cycle / TRACE_CLK），只能由一个来源推进。
// 两个候选是互不相同的计数器，同时接进来就会重复计数。
enum CycleSource : int {
    kCycleSrcNone = 0,
    kCycleSrcItm  = 1,   // ITM 时间戳包，按 tspresc 乘回核心周期
    kCycleSrcEtm  = 2,   // ETM 时间戳或周期计数（每个包只挑一个）
    kCycleSrcWall = 3,   // 墙钟兜底：采集至今的墙钟 × 核心时钟
};

// 轴的两种机制：数据量差几个数量级，ITM 追墙钟、ETM 每条 +1 保持稠密。
// 试过让 ETM 也追墙钟，会造出空洞并让 uVision 全量取数而崩，已回退。
enum CycleAxisMode : int {
    kAxisModeEtm = 0,
    kAxisModeItm = 1,
};

/// trace 启动时按实际选中的源设置；只影响轴要不要追墙钟。
void TraceWinSetAxisMode(int mode);
int  TraceWinAxisMode();

// ITM 时间戳增量（核心周期数）。实测推不动轴，现在只计数供日志。
void TraceWinAdvanceCycle(uint32_t delta);

// ETM 时间量（绝对值，内部做差）：第一次只建基准，回绕/复位时重建基准。
// what 只用于日志；COUNT_UNKNOWN 表示包没带值，会被忽略。
// 两个计数器不能混着喂。
void TraceWinEtmTime(uint64_t absolute, const char* what);

// 轴现场证据（一行，ASCII 锚点 cycle axis）：来源/总周期/记录数/各源贡献。
void TraceWinCycleAxisLog(const char* where);

// 记录缓冲规模：chunkMB 每块大小、limitMB 总上限、recMax 记录条数上限（0 = 不限）。
// 只在第一次 Connect 时有效（上限决定索引数组大小）；recMax 随时可改。
void TraceWinSetBuffer(unsigned chunkMB, unsigned limitMB,
                       unsigned long long recMax = 0);

// 是否显示时间列。默认 false：该字段同时是取数门的一环，打开可能一行都不取。
void TraceWinSetTimestampsVisible(bool on);

// 清空记录（ClearTrace / 会话开始）；内部先 Drain，避免块双重归属。
void TraceWinClear();

// 等后台 cache 写线程把在飞的块写完并还回空闲链；会等待但不持结构锁。
void TraceWinDrain();

// 让所有 trace 视图定位到 cycle（UV_TRACE_WIN_SYNC）。
// 不发它，uVision 可能一直不来取记录。
void TraceWinSyncViews(uint64_t cycle);

// trace 周期号所依据的时钟（Hz）。
void TraceWinSetCoreClock(uint32_t hz);

// 注册 CORE_CLK / TRACE_CLK 两个虚拟寄存器；窗口的时间轴要的正是 TRACE_CLK。
void TraceWinRegisterClocks(uint32_t coreHz, uint32_t traceHz);

// 当前记录条数（进日志）。
unsigned TraceWinRecordCount();

// 最后一条记录的周期号（空时 0）。
uint64_t TraceWinLastCycle();

// 对外报的可查范围左端 = 本段起点（不是“最旧可读”）。
// 三个出口必须同值，都走 FirstReportedCycle()。
uint64_t TraceWinFirstCycle();

// 让 uVision 显示指定周期区间（bExplicitRange = 1 + m_pRecordRange）。
// 不给显式区间时窗口只有列没有行。
void TraceWinShowRange(uint64_t firstCycle, uint64_t lastCycle);

// ---- 运行状态 ----

// bState = 正在采集，bUpdate = 让窗口刷新；只在状态变化时真的发通知。
void TraceWinSetRunning(bool bState, bool bUpdate);

// 当前是否采集/处理中（此时 uVision 不来要记录）。
bool TraceWinIsRunning();

// 把 CORE_CLK / TRACE_CLK 重新发布回 VTR。
// 要在事件点调（配置应用/启动/停止收尾），不要在时钟写回调里调；重入安全。
void RefreshTraceClocks();

// ---- Trace 选项（官方取值原样） ----
enum : uint32_t {
    kTraceEnable     = 0x0001, // TRACE_ENABLE      - 总开关
    kTraceTimestamp  = 0x0002, // TRACE_TIMESTAMP   - 时间戳
    kTracePcSample   = 0x0004, // TRACE_PCSAMPLE    - PC 采样
    kTracePcData     = 0x0008, // TRACE_PC_DATA     - PC + 数据访问
    kTraceExcTrc     = 0x0010, // TRACE_EXCTRC      - 异常事件
    kTraceCpi        = 0x0020, // TRACE_CPI         - CPI 事件
    kTraceExc        = 0x0040, // TRACE_EXC         - 异常事件
    kTraceSleep      = 0x0080, // TRACE_SLEEP       - 睡眠事件
    kTraceLsu        = 0x0100, // TRACE_LSU         - 加载/存储事件
    kTraceFold       = 0x0200, // TRACE_FOLD        - 折叠事件
    kTraceCycCnt     = 0x0400, // TRACE_CYCCNT      - CYCCNT 事件
    kTraceEventMask  = 0x07F0, // TRACE_EXCTRC .. TRACE_CYCCNT
    kPcSample        = 0x0800, // PC_SAMPLE         - 经 DWT_PCSR 采样
    kTpiuFormat      = 0x1000, // TPIU_FORMAT       - 使用 TPIU 格式化器
    kEtmTrace        = 0x2000, // ETM_TRACE         - 使用 ETM trace
    kUnlimitedTrace  = 0x8000, // UNLIMITED_TRACE   - 无限 trace
    kTraceUseCoreClk = 0x00010000, // TRACE_USE_CORECLK - 用系统时钟而非 TPIU 时钟
};

void TraceWinSetOptions(uint32_t opt);
uint32_t TraceWinOptions();

// ---- 状态栏上报 ----
// 走 pio->Notify(UV_RTAH_ERRMSG, buf)；msg = 0 表示清空。
enum : unsigned {
    kTMsgComErr  = 1, // Trace: Communication Error
    kTMsgHwBuf   = 2, // Trace: HW Buffer Overrun
    kTMsgSwBuf   = 3, // Trace: SW Buffer Overrun
    kTMsgNoSync  = 4, // Trace: No Synchronization
    kTMsgDataErr = 5, // Trace: Data Stream Error
    kTMsgDataOvf = 6, // Trace: Data Overflow
    kTMsgRun     = 7, // Trace: Running ...
};

// 显示 trace 状态；非 0 消息在安全扩展有效时带 Secure 前缀。
// 只排队不投递：Notify 跨线程到 GUI，抽数线程每轮等它会把周期拖长。
void TraceWinStatusMsg(unsigned msg);

// 显示任意一行状态文本（空串/ nullptr = 清空），同样只排队。
void TraceWinStatusText(const char* text);

// 把排队的消息投递出去；只能在非抽数线程上调。
void TraceWinStatusFlush();

// ---- 错误掩码与折叠 ----
enum : int {
    kTErrTdErr   = 0x0001, // TD Stream Error
    kTErrTdBuf   = 0x0002, // TD Buffer Overrun
    kTErrTpBuf   = 0x0004, // TP Buffer Overrun
    kTErrTrBuf   = 0x0008, // TR Buffer Overrun
    kTErrDataOvf = 0x0010, // Data Overflow
    kTErrNoSync  = 0x0020, // No Synchronization
    kTErrSwBuf   = 0x000E, // SW Buffer Overrun（Td/Tp/Tr 三位合一）
    kTErrHwBuf   = 0x0040, // HW Buffer Overrun
    kTErrHwCom   = 0x0080, // HW Communication Error
    kTErrDataErr = 0x0100, // Data Stream Error
};

// 错误自动恢复超时（毫秒）。
enum : int { kTRecover = 3000 };

// 置位错误掩码。
void TraceWinErrorSet(int mask);

// 折叠掩码并上报最高优先级那条；返回 0 表示已清空。
unsigned TraceWinTraceStatus();

// ---- 采集路径 ----

// DWT 同步抽头绕一圈的时间（ms）；0 = 看门狗不启动。
// 没有权威核心时钟时宁可不报，也不报假的“失去同步”。
void TraceWinSetSyncPeriod(unsigned ms);

// 重置错误状态与同步超时；不动窗口里的记录。
void TraceWinFlush(bool setRun);

// 折叠状态（timeMs 是本轮折叠时长，0 = 只折一次不空转）。
void TraceWinRead(unsigned timeMs, bool ts);

// 只推进自动恢复倒计时，给不空转的调用方用。
void TraceWinRecoverTick(unsigned elapsedMs);

} // namespace agdi
} // namespace orb
