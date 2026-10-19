/**
 * @file ORBAGDI_TraceWin.h
 * @brief uVision "Trace Data" 窗口的上报通道（Keil TDI 标准流程）。
 *
 * Keil 的调试驱动把 trace 交给 IDE 有两条路：
 *
 *   1. **ITM / printf 视图** - `AG_Serial(AG_ITMBOUT, ...)` -> `AG_CB_ITMDATA`。
 *      已在 ORBAGDI_Itm.cpp 里实现（软件激励包）。
 *   2. **Trace Data 窗口** - 走 Trace Data Interface（TDI）：驱动把自己的回调
 *      表通过 BOM 的 `Notify(UV_TRACE_IF_INIT, TRACE_INTERFACE_INIT_64*)` 注册给
 *      uVision，uVision 之后按**周期区间**向驱动要记录
 *      （`GetTraceRecords64` / `GetFiltTraceRecords64`），再用
 *      `UV_TRACE_WIN_CONFIG` 描述列与过滤器。**本模块实现的就是这一条。**
 *
 * 参照实现是 Keil 参考驱动 `DbgCM/TraceWinConnect.cpp`（`InitTraceInterface` /
 * `ConfigureTraceWin` / `TraceStatusUpdate` / TDI 回调骨架）。注意那份代码里
 * TDI 回调全是 TODO 桩（`DBGCM_FEATURE_TRCDATA_WIN` 为 0），本模块把它补全。
 *
 * 记录模型：**块链 + 紧凑记录**（P1-1 + P1-2）—— 块用双向链表串起来，尾部追加、
 * 头部摘最旧、空闲链复用，按周期定位走块索引二分；块里存的是**紧凑记录字节流**
 * （标志 + 载荷 + varint 周期差，约 5–7 B/条，而不是 22 B 的 `TRACE_RECORD_64`），
 * 查询时展开（完整说明见 `ORBAGDI_TraceWin.cpp` 上方的"记录缓冲：块链 + 紧凑记录"），
 * 加上单调递增的"周期"号。
 * ⚠ **ITM 与 ETM 无任何关系**：两者的时间计数器互不相干，不能互相顶替（见下面
 * `CycleSource` 的实测）。实测两边都**给不出**机器周期 ⇒ 周期号就是**合成的**。
 *
 * 周期号是**合成的**（每条记录 +1），并且记录里
 * `bTS` 保持 0、状态里 `bHideTimestamps = 1` —— 这正是 Keil 说的 "fake
 * timestamps"：窗口不显示时间列，但周期区间查询照样成立，且省掉大量解码。
 *
 * 归属：**AGDI 层**。RDDI 层不做 trace 语义（见 COMPAT_ANALYSIS.md §8）。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

// ---------------------------------------------------------------------------
// 会话 / 注册
// ---------------------------------------------------------------------------

// 把 TDI 回调表注册给 uVision，并描述 Trace Data 窗口的列与过滤器。
// 幂等。uVision 没给 BOM（或 BOM 没有 Notify）时返回 false，Trace 窗口保持
// 不可用，其余调试功能不受影响。
bool TraceWinInit();

// 会话结束 / 断开：清记录、注销注册状态。下次 Connect 会重新注册。
void TraceWinShutdown();

// 把当前配置串注册进 uVision 的项目（UV_NEW_TARG_SETTINGS = 0x831）。
//
// 官方 UpdateRegVals()（DbgCM/AGDI.CPP:753）在设置变更后这么做，ULP2CM3 也一样
// （机器码 0x1009600F，qdll{key,value}）。本驱动此前只靠设置页的返回值让 uVision
// 存串，从不主动注册。
void TraceWinPublishSettings();

// 声明"trace 数据展示已启用"：即 `struct bom::bTrcDisp`。ULP2CM3 在注册 TDI 接口
// 之前置它（机器码 0x100A3EB4 `or dword [pio+0xafc], 8`），官方参考驱动同样如此
// （DbgCM/Trace.cpp:1435）。uVision 侧读它来决定 Trace Data 窗口是否取记录。
//
// 关键前提：必须落在 uVision 读的那个 `struct bom` 实例上（AG_CB_GETBOMPTR 的
// 返回值）。详见 ORBAGDI_Pdsc.cpp 的 SetBomAlt()。
//
// 返回置位后的实际状态。`where` 只用于日志。
bool TraceWinAssertTraceDisplay(bool setBit, const char* where);

// 诊断工具（编译期开关 `ORBAGDI_GATE_PROBE`，默认 0）：只读地把 UV4.exe 里
// Trace Data 取数门链的当前值打进日志，用于"窗口有列有过滤器、却不出数据"时
// 直接定位是哪一道门。不写任何宿主内存。门链与逆向结论见 COMPAT_ANALYSIS.md。
void TraceWinReadHostGates(const char* where);

// 重新下发 TDI 接口表与窗口配置。
//
// 注册只在 Connect 里做一次，而 uVision 的 Trace Data 窗口是在收到
// UV_TRACE_IF_INIT 之后才创建的 —— 紧跟其后的 UV_TRACE_WIN_CONFIG 有落在
// "窗口还不存在" 那一侧的风险，被丢弃后窗口就没有列/过滤器，也就永远不会
// 来取记录（现象是：接口绑上了、ClearTrace 能调到、之后一片安静）。
// 在 trace 真正启动时再发一次，把顺序这个变量排除掉。
void TraceWinReconfigure();

// uVision 的 Trace Data 窗口是否已注册（且 BOM 可用）。
bool TraceWinReady();

// uVision 点 "Update"（或 ETB 模式下刷新）时回调，用于重新拉一次数据。
// `fn` 返回 true 表示拿到了新数据。传 nullptr 取消。
void TraceWinSetRefreshHook(bool (*fn)());

// ---------------------------------------------------------------------------
// 记录（全部在 uVision 自己的线程上调用）
// ---------------------------------------------------------------------------

// ITM 软件激励（DWT/printf 之外，Trace Data 窗口也要一份）。
//   port  - 激励端口号
//   data  - 载荷（1/2/4 字节），LSB 在前
//   len   - 字节数（1..4，其它值按 1 处理）
void TraceWinItmStimulus(uint32_t port, const uint8_t* data, unsigned len);

// DWT PC 采样。
void TraceWinPcSample(uint32_t pc);

// DWT 事件计数包。`mask` 是包里带的 `TR_EVT_*` 位掩码（CPI / EXC / SLEEP /
// LSU / FOLD），对应 Trace 页的 "Trace Events" 勾选。
void TraceWinEvent(uint32_t mask);

// 异常 trace 包（`AG_F_EXCTRC`）。`packed` 是包里的小端原值，窗口按
// `TR_EXC_TYP_POS` / `TR_EXC_NUM_MSK` 自己拆类型和编号 —— 与 Keil 一致。
void TraceWinExceptionTrace(uint32_t packed);

// ETM 指令 trace 的一条分支。`executed` 对应 TDI 的 bExec（该指令是否真的
// 执行了；条件分支未走时也要给 uVision 一条记录）。
void TraceWinEtmInstruction(uint32_t addr, bool executed);

// 流控记录。`flowType` 取 TR_TYPE_FLOW_* 子类型（见 TraceView/TraceDataTypes.h）。
void TraceWinFlow(uint32_t flowType);

// ---------------------------------------------------------------------------
// 周期轴（Todo.md 的 P0-F / P0-F′）
// ---------------------------------------------------------------------------
// 轴上的周期号就是 uVision 的时间基准（`time = cycle / TRACE_CLK`），而它只能由
// **一个**来源推进。本驱动有两个候选，它们是两个**互不相同**的计数器，同时接进
// 同一条轴会重复计数：
//
//   kCycleSrcItm - ITM 时间戳包（`ORBAGDI_Itm.cpp` 的 `OnTimestamp`）。值按
//                  `tspresc` 乘回核心周期，粒度由时间戳分频决定。
//   kCycleSrcEtm - ETM 自己的时间来源（`ORBAGDI_Etm.cpp`）：**ETM 时间戳包**
//                  （`EV_CH_TSTAMP`，ETMCR[28] + ETMCCER[22]，§7.7）或**周期计数包**
//                  （`EV_CH_CYCLECOUNT`，ETMCR[12]）—— 两者是**不同的计数器**，
//                  ETM 层只挑一个喂进来，不会混着推。ETM 一出现就**接管**这条轴，
//                  ITM 时间戳之后只被忽略（不是两者相加）。
//
// ⚠⚠ **ITM 与 ETM 无任何关系**（2026-10-14 用户指出；我此前把两者当成了"可以互相
//   顶替"的时间来源，是错的）。它们是两条**互不相干**的流、两套**互不相干**的计数器：
//
//     * ITM（tag 1，软件激励 / 事件 / PC 采样）—— 时间戳包由 `ITM_TCR.TSENA` +
//       `TSPRESC` 控制，走 ITM 自己的计数器；
//     * ETM（tag 2，指令 trace）—— `ETMCR[28]` 时间戳 / `ETMCR[12]` 周期计数，
//       是另一套计数器，与 ITM 的毫无关系。
//
//   ⇒ **一侧没有数据，不代表另一侧能当它的来源或回退。** 判据只能来自实测。
//
//   实测（2026-10-14，18:29–18:30 那次会话，`tcr=0x0001030F dwt=0x403F1201`）：
//     * ITM 时间戳**是开着的**（TSENA=1、TSPRESC=3 ⇒ 64）却**推不动周期轴**；
//     * ETM 侧给的是**非单调的小整数**（`base = 13` → 下一个值 `3`）；
//     * 结果：轴 = 记录条数 **1:1**（Δ周期 9670 == Δ记录 9670 / 0.813 s）
//       ⇒ 轴速率 = 记录速率 = **11.9 kHz**，而基准声称它是机器周期（48 MHz）。
//   ⇒ **两者都不是"实际机器周期"。** 谁都不能拿来当基准。
//
// 仲裁与账目都在本模块里，调用方只报"我这个来源推进了多少周期"。
enum CycleSource : int {
    kCycleSrcNone = 0,
    kCycleSrcItm  = 1,
    kCycleSrcEtm  = 2,
    // kCycleSrcWall - **墙钟兜底**（本段采集至今的墙钟毫秒 × 核心时钟）。2026-10-14
    //   起轴由它推进，**只用来给记录打时间戳**（Time 列 / bTS / 区间定位）。
    //
    // ⚠⚠ 它**不是** `TDRV_ERR_BOUNDS` 的修法。曾经这里写着"轴必须按墙钟走、否则
    //   uVision 请求会落在轴外"，**那个结论是错的**，本轮被官方二进制推翻：
    //   官方在 `UV_TRACE_IF_STATUS(0x901)` 里**从不给 `m_pClockSample`**，而是用
    //   `bExplicitRange = 1` + `m_pRecordRange = {最旧记录, 最新记录}` 把区间**报死**，
    //   所以 uVision 的请求永远落在区间内。详见 ORBAGDI_TraceWin.cpp 的
    //   `StatusUpdate()` 与 `SyncAxisToWall()`，以及 Todo §十四。
    //
    // ⚠ 上面那两个计数器在本板确实都不可用（ITM 流是花的、ETM 周期计数不实现），
    //   所以墙钟是**过渡**手段。正解是官方那条：轴用**真实周期计数器**
    //   （`DbgCM` 读 `DWT_CYCCNT` 累加，`ULP2CM3` 用 trace 时间戳累加）——
    //   本板 `DWT_CYCCNT` 开着却从没被读过，Todo §14.6。
    kCycleSrcWall = 3,
};

// ---------------------------------------------------------------------------
// 周期轴的**两种机制**（2026-10-14 用户裁定：按 trace 源分开）
// ---------------------------------------------------------------------------
// 用户原话：**"轴追墙钟是 ITM 的机制，ITM 可以灵活地追踪时间，但是 ETM 数据量太大，
// 不好做。所以把机制分成两个部分，ETM 和 ITM（SWO），区分。"**
//
// 两个源的数据量差着几个数量级，时间轴的做法必须分开：
//
//   kAxisModeItm - **ITM / SWO**（SWO 引脚，或 orbtrace 的 SWO 档 `trswo>0`）。
//                  数据量小 ⇒ 轴**追墙钟**：`SyncAxisToWall()` 每 64 条记录把轴抬到
//                  `墙钟 ms × 核心时钟`，时间轴能灵活地跟着真实时间走。
//   kAxisModeEtm - **ETM**（并口 TPIU `trwidth`、ETB，或 orbtrace 的并行档）。
//                  数据量太大（实测 4.9 M 条/s），按墙钟追不起 ⇒ 轴只按**解码**
//                  推进（每条记录 +1，外加 ETM 自己的周期计数如果可用）。
//
// ⚠⚠ 2026-10-14 试过"ETM 也追墙钟"，**真机把 uVision 打崩，已回退**。原因两条：
//   ① 抬轴会让轴上每隔 64 条出现一段"没有记录"的空洞 ⇒ `QueryRange()` 回 BOUNDS；
//   ② 实轴 = 真实时间 ⇒ uVision 按 2 秒窗口全量取数，现场有 1.4 亿条 / 5.3 秒
//      ⇒ 单次查询回 1 亿条（1453 ms，跑在它的 UI 线程上）⇒ 崩。
//   结论：**ETM 的轴必须保持每条 +1 的稠密单调性**。细节见 `.cpp` 的 `SyncAxisToWall()`。
//
// ⚠ 两种模式下 uVision 的取数区间都由 `StatusUpdate()` 的 `m_pRecordRange` 给死
//   （官方做法）⇒ **`BOUNDS` 与用哪种机制无关**，别把两件事混在一起。
// ⚠ 默认 **ETM**（不追墙钟）—— 宁可少做，不要在数据量大的那条路上做错。
enum CycleAxisMode : int {
    kAxisModeEtm = 0,
    kAxisModeItm = 1,
};

/// 由 AGDI 层在 trace 启动时按**实际选中的源**设置（见 ORBAGDI_Agdi.cpp）。
/// 只影响"轴要不要追墙钟"，不影响区间上报。
void TraceWinSetAxisMode(int mode);
int  TraceWinAxisMode();

// ITM 时间戳包报来的增量（`delta` = 核心周期数）。
//
// ⚠ **ITM 与 ETM 无任何关系** —— 这是 ITM 那套计数器，不是 ETM 的，也不能当 ETM
//   的替代/回退（见上面 CycleSource 那段实测）。ITM 时间戳实测**推不动**这条轴。
//
// ⚠⚠ **2026-10-14 起不再推进周期轴** —— 轴目前由**墙钟兜底**拥有
//   （`kCycleSrcWall`，见 ORBAGDI_TraceWin.cpp 的 SyncAxisToWall()），只用于给记录
//   打时间戳。本函数现在只**计数**，供 `cycle axis(...)` 那行日志看
//   "ITM 时间戳一共报了多少周期"。
//
// ⚠ 别把这条读成"墙钟是正解"：`TDRV_ERR_BOUNDS` 的修法是 `StatusUpdate()` 里
//   报 `m_pRecordRange`（官方做法），与轴用谁**无关**。轴的正解是**真实周期计数器**
//   （Todo §14.6），届时这个入口要接回去。
void TraceWinAdvanceCycle(uint32_t delta);

// ETM 的时间量（**绝对值**）。ETMv3.5 与 ETMv4 报出来的 `cycleCount` 都是单调递增的
// 周期总数（`traceDec/traceDecoder_etm35.cpp` 直接赋值、`traceDecoder_etm4.cpp:828`
// 累加增量）；ETM 时间戳包（`cpu->ts`）同样是单调递增的绝对时间值（§7.7.4，压缩
// 编码已在解码器里还原）。所以这里统一收绝对值、自己做差。
//
// `what` 只用于日志（`"ETM timestamp"` / `"ETM cycle count"`）。**两个计数器不能混着
// 喂** —— 那是两个不同的时基；`ORBAGDI_Etm.cpp` 保证一段采集里只挑一个。
// 传 `COUNT_UNKNOWN`（0xFFFF…FF）表示"这个包没带值"，会被忽略。
//
// 本模块自己做三件事：① 第一次收到只**建基准**（不推进轴 —— 计数器的起点不是 0）；
// ② 之后取相邻两次的**差**当增量；③ 计数器回绕/复位（新值 < 旧值）时重建基准。
//
// **这是 ETM 模式下唯一的时间来源** —— 纯 ETM 流里没有 ITM 时间戳包。
void TraceWinEtmTime(uint64_t absolute, const char* what);

// 周期轴的现场证据（一行）：来源 / 轴上的总周期 / 记录条数 / 各来源的贡献 /
// 重建基准次数。采集段结束时打一次。判据"纯 ETM 流也有真实时间轴"看这一行
// 加上 `bTS=1` 那一行。ASCII 锚点：`cycle axis`。
void TraceWinCycleAxisLog(const char* where);

// 记录缓冲（块链）的规模 —— 选项 `trchunk=` / `trmem=`（MB，0 = 用默认值）。
//
// 默认：每块 4 MB；上限 = 物理内存 1/4，封顶 1 GB（Todo.md §九）。
//
// ⚠ **只在第一次 Connect 时有效**：上限决定索引数组的大小，它在 `TraceWinInit()`
// 里一次性分配。之后调用只会记一条告警。
//
// 存在的理由：默认上限下"到上限摘最旧块"这条路要攒够 1 GB 才走得到，那是"到不了
// 就等于没写"的代码 —— `trchunk=1 trmem=2` 让它在几秒内跑到（假 BOM 宿主的验收
// 就是这么做的），顺带也给了"这台机器上该开多大"一个可调旋钮。
//
// `recMax`（`trmax=`，第 1 号）：常驻**记录条数**上限，**0 = 不限（默认）**。
// ⚠ 与前面两个不同，它**随时可改**（只是一个比较值，不参与任何一次性分配）。
//   默认必须是不限：硬编码 1,048,576 条在 ETM 下 = **0.13 s**，现场静默吃掉
//   81,608,143 条（定性见 `Todo.md §4.3`）。它现在只在用户显式要"卡条数"时生效，
//   且触顶走整块退休（落盘 / 响亮报错），不再静默裁字节。
void TraceWinSetBuffer(unsigned chunkMB, unsigned limitMB,
                       unsigned long long recMax = 0);

// 是否让 uVision 显示时间列（TRACE_STATUS.bHideTimestamps）。
// ⚠ 默认 false：该字段同时是取数门的一环，打开可能让窗口一行都不取
// （§10.20.1）。由选项 tswin 驱动。
void TraceWinSetTimestampsVisible(bool on);

// 清空记录（TDI 的 ClearTrace，以及会话开始）。
//
// ⚠ 内部会先 `TraceWinDrain()`：写线程手上可能还拿着块（在飞的历史）。不等它把块
// 还回来就把整条链搬进空闲链，会出现"一块同时属于空闲链和写线程"的双重归属。
void TraceWinClear();

// 等后台落盘线程把"在飞的块"全部写完并还回空闲链。
//
// 调用点：`TraceWinClear()` 内部，以及 AGDI 的 Disconnect（在
// `TraceFileClose()` **之前** —— 文件一关，队列里还没写的块就只能丢）。
// 会等待，但**不持结构锁**（写线程要取结构锁才能还块，持锁等就是死锁）。
void TraceWinDrain();

// 让 uVision 把所有 trace 相关视图定位到 `cycle`（UV_TRACE_WIN_SYNC）。
//
// ULP2CM3 在目标停止后主动发这一条：m_nTypes = TWS_TYPE_CYCLE(1)、
// m_nViews = 0xF（DATA|NAV|HLL|DASM）、m_nCycle = 当前周期
// （机器码 0x100ED652/0x100ED659/0x100ED666）。
//
// 这条通知之所以可能是"窗口不来取记录"的最后一环：要定位到某个周期，
// uVision 必须先拿到该周期对应的记录 —— 于是它会去调 GetFirstCycle /
// GetTraceRecords64。本驱动此前从不发它。
void TraceWinSyncViews(uint64_t cycle);

// trace 周期号所依据的时钟（Hz）。uVision 通过 AG_TRACESETUP_REQ 交下来的
// TRACE_CONFIG 里只有这一个字段，TDI 的 Set/GetTraceConfig 也用同一个值。
void TraceWinSetCoreClock(uint32_t hz);

// 把 CORE_CLK / TRACE_CLK 两个虚拟寄存器注册给 uVision。
//
// 官方 RegisterClock() / RegisterTraceClock()（DbgCM/AGDI.CPP:4165 / :4251）：
//   p_create("CLOCK", bt_ulong) -> 改名 "CORE_CLK" -> p_putval -> w_create
//   p_create("TRACE_CLK", ...)  -> 同名            -> p_putval -> w_create
// ULP2CM3 的机器码逐指令一致（0x1008C401 / 0x1008C50B）。
//
// Trace Data 窗口的时间轴要的就是 TRACE_CLK：没有它，窗口没有时间基准。
// 幂等：VTR 只创建一次，后续调用只更新数值。
void TraceWinRegisterClocks(uint32_t coreHz, uint32_t traceHz);

// 当前持有的记录条数（进日志）。
unsigned TraceWinRecordCount();

// 最后一条记录的周期号（缓冲为空时返回 0）。用于停止后的视图同步。
uint64_t TraceWinLastCycle();

// 对外报的"可查范围左端"（缓冲为空时返回 0）= **本段起点**（`Store::segFirstCycle`），
// **不是**"最旧一条可读记录"。
//
// ⚠ 2026-10-15 第二轮（用户裁定，`Todo.md §4.3`）：左端 = `TraceWin: ETM cycle count
//   base = …` 那一刻的位置。原先那条"必须报最旧**可读**"的规矩，在条数上限硬编码
//   `1<<20` 的年代是对的（窗口外的历史真的没了，报宽了 uVision 就去请求我们没有数据的
//   区间 ⇒ `TDI_ERR_BOUNDS`，症状是 Navigation 条中间段拖不出数据、只有最尾巴有）；
//   现在条数上限默认关、一段 10 s 的采集**整段**都留在内存里，报"最旧可读"反而把
//   Navigation 条缩成最后 0.13 s —— 现场就是这么被投诉的。真丢了历史（内存到顶而
//   记录日志没开）时本函数会报得更早，但那一刻状态栏已经有 `Trace: Data Overflow` 在提示。
// ★ 三个出口（本函数 / `StatusUpdate()` 的 `m_pRecordRange` / TDI `GetFirstCycle()`）
//   **必须同一个值**，都走 .cpp 里的 `FirstReportedCycle()`。
uint64_t TraceWinFirstCycle();

// 让 uVision 显示指定的周期区间（TRACE_STATUS.bExplicitRange = 1 +
// m_pRecordRange）。TRACE_STATUS 里这个字段的定义就是 "Show specific cycle
// range"，而要显示一段区间，uVision 必须先向驱动取这段的记录 ——
// 本驱动此前一直发 bExplicitRange = 0，窗口于是只有列、没有行。
void TraceWinShowRange(uint64_t firstCycle, uint64_t lastCycle);

// ---------------------------------------------------------------------------
// 运行状态
// ---------------------------------------------------------------------------

// bState = 正在采集 trace；bUpdate = 让 uVision 刷新窗口内容。
// 只在整体状态发生变化时才会真的发通知（与 DbgCM 的 SetTraceRun* 一致）。
void TraceWinSetRunning(bool bState, bool bUpdate);

// 当前是否处于"采集/处理中"（uVision 在此时不会来要记录）。
bool TraceWinIsRunning();

// 把当前生效的 CORE_CLK / TRACE_CLK 经 p_putval 重新发布回 VTR，对齐官方唯一的重发
// helper `0x1008B7C0`。**必须在"事件"处调**（conf 应用 / trace 启动 / 停止-复位收尾），
// 不要在时钟写回调里调 —— 官方 `wrCoreClk 0x1008C340` 根本不调它。
// ⚠ **唯一例外**：`wrTraceClk` 的"脱管纠偏"分支（host 写了只读的 TRACE_CLK，必须把
//   VTR 掰回权威值，而官方那三个事件点都不在场）—— 那里必须调，见该处注释。
// 重入安全：函数入口带静态重入守卫（`p_putval` 若同步派发回写钩子会重入调用者）。
// 幂等、null-safe（BOM 或 VTR 不在时静默返回）。
void RefreshTraceClocks();

// ---------------------------------------------------------------------------
// Trace 选项（官方 `DbgCM/Trace.h:295-310`，取值原样）
// ---------------------------------------------------------------------------
// 官方把 trace 的开关和内容选择放在一个位掩码里（`TraceConf.Opt`，生效副本是
// `TraceOpt`），运行控制、状态上报和解码路径都按它分支。本模块保留同一套取值，
// 由 AGDI 层在 trace 源启动/停止时置位。
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
    // "Trace Events" 那一组（Trace 页的 6 个复选框 + 没有复选框的 CYCCNT）。
    // 这几个位一起并进 TraceOpt，窗口据此给记录分类。
    kTraceEventMask  = 0x07F0, // TRACE_EXCTRC .. TRACE_CYCCNT
    kPcSample        = 0x0800, // PC_SAMPLE         - 经 DWT_PCSR 采样
    kTpiuFormat      = 0x1000, // TPIU_FORMAT       - 使用 TPIU 格式化器
    kEtmTrace        = 0x2000, // ETM_TRACE         - 使用 ETM trace
    kUnlimitedTrace  = 0x8000, // UNLIMITED_TRACE   - 无限 trace
    kTraceUseCoreClk = 0x00010000, // TRACE_USE_CORECLK - 用系统时钟而非 TPIU 时钟
};

// 设置 / 读取当前生效的 trace 选项（官方 `TraceOpt`）。
void TraceWinSetOptions(uint32_t opt);
uint32_t TraceWinOptions();

// ---------------------------------------------------------------------------
// 状态栏上报（官方 `OutTraceMsg` / `OutMsg`，DbgCM/AGDI.CPP:702-743）
// ---------------------------------------------------------------------------
// 官方把 trace 的运行状态和错误直接写进 uVision 的状态栏：
//
//     pio->Notify(UV_RTAH_ERRMSG, (void *)buf);
//
// `pio` 就是 AG_CB_GETBOMPTR 交给我们的那个 BOM 对象，`UV_RTAH_ERRMSG` 是它的
// 一个请求码（BOM.H:2382 = 0x827）——和 Trace Data 窗口用的 UV_TRACE_IF_* 走同
// 一条通道，所以这条路是通的。
//
// 消息码取官方 `Trace.h:129-136`；`msg = 0` 写入空串，等于清空该栏。
enum : unsigned {
    kTMsgComErr  = 1, // Trace: Communication Error
    kTMsgHwBuf   = 2, // Trace: HW Buffer Overrun
    kTMsgSwBuf   = 3, // Trace: SW Buffer Overrun
    kTMsgNoSync  = 4, // Trace: No Synchronization
    kTMsgDataErr = 5, // Trace: Data Stream Error
    kTMsgDataOvf = 6, // Trace: Data Overflow
    kTMsgRun     = 7, // Trace: Running ...
};

// 显示 trace 状态（官方 `OutTraceMsg`）。非 0 的消息在安全扩展有效时会带上
// "Secure" / "Non-Secure" 前缀——v7-M 上 `bSecureExt` 恒为 0，该分支不进入。
//
// ⚠ **只排队，不投递**（2026-10-14）。抽数 worker 每一轮都会走一遍错误折叠，
//   而 `bom->Notify(UV_RTAH_ERRMSG, ...)` 是跨线程投递到 uVision 的 GUI 线程 ——
//   让抽数线程每轮等它一次，等于把 GUI 的响应时间乘进抽数周期（实测 21 ms/轮，
//   设计 2 ms）。真正的投递交给 `TraceWinStatusFlush()`，由**调试线程**调用。
void TraceWinStatusMsg(unsigned msg);

// 显示任意一行状态文本（官方 `OutMsg`）。传 nullptr 或空串清空（也是排队）。
void TraceWinStatusText(const char* text);

// 把排队中的状态栏消息投递出去（见 `TraceWinStatusMsg` 的说明）。没有待发布
// 时立即返回（一次 `InterlockedExchange`），所以可以放在轮询循环里每轮调用。
//
// ⚠ 只能在**非抽数线程**上调用（调试线程的轮询循环、状态变化的收尾）。
void TraceWinStatusFlush();

// ---------------------------------------------------------------------------
// Trace 错误掩码与状态折叠（官方 `DbgCM/Trace.h:140-151` + `ETB.cpp:136-164`）
// ---------------------------------------------------------------------------
// 解码路径只负责置位 `T_Err`，由 `ETB_TraceStatus()` 把掩码折叠成一条状态栏
// 消息——同一时刻只显示优先级最高的那一条，全部清空则清掉状态栏。官方在每次
// ETB 处理完后调用它（`ETB.cpp:959`）。
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

// 官方 `T_RECOVER`：错误自动恢复超时（毫秒）。
enum : int { kTRecover = 3000 };

// 置位错误掩码（官方 `T_Err |= ...`）。解码路径在遇到相应问题时调用。
void TraceWinErrorSet(int mask);

// 折叠错误掩码并上报（官方 `ETB_TraceStatus()`）。返回当前仍待显示的最高优先级
// 消息码，0 表示已清空。
unsigned TraceWinTraceStatus();

// ---------------------------------------------------------------------------
// 采集路径（官方 `Trace_Flush` / `Trace_Read`，DbgCM/Trace.cpp:1777-2129）
// ---------------------------------------------------------------------------

// 官方 `SyncPeriod`（Trace.cpp:132）——DWT 同步抽头计数绕一圈所需的时间，由 trace
// 建立时按核心时钟算出（Trace.cpp:1504-1514）：
//
//     SyncPeriod = 1000 * 2^tap / Clk + 100   [ms]
//
// 本驱动没有权威的核心时钟（见 bug.md §R6），所以这个值保持 0 时，`TraceWinRead`
// 的"失去同步"看门狗不启动——宁可不报，也不报一个假的"失去同步"。
void TraceWinSetSyncPeriod(unsigned ms);

// 官方 `Trace_Flush`（Trace.cpp:1777-1808）。`setRun` 为真表示这是"让目标跑起来"
// 的一部分。重置错误状态与同步超时；**不动** Trace Data 窗口的记录——官方重置的
// 是解码缓冲（TD_/TP_/TR_），窗口内容要留给用户。
void TraceWinFlush(bool setRun);

// 官方 `Trace_Read`（Trace.cpp:2003-2129）的状态折叠那一半。
//
// 官方的取数在 `SWV_Read(time)` 里 —— 那是个**阻塞**读，所以它外面那层
// `do { SWV_Read(time); ... } while (GetTickCount() - tick < time)` 不空转。
// 本驱动的取数由调用方驱动（`ItmPump` / `OrbTracePump` / `EtbProcess`），这里
// 只剩折叠，于是同一个 `timeMs` 就变成了**忙等时长**。⇒ 从抽数 worker 里调用
// 时传 **0**，恢复倒计时改用 `TraceWinRecoverTick()`。
//   timeMs - 本轮折叠循环的时长（官方传 1；0 = 只折一次，不空转）
//   ts     - 本轮是否带时间戳（官方运行时传 FALSE，停机采样传 TRUE）
void TraceWinRead(unsigned timeMs, bool ts);

// 只推进 `T_Recover` 自动恢复倒计时（官方在 `Trace_Read` 末尾按 `time` 递减）。
// 给"不空转"的调用方用：`TraceWinRead(0, ts)` + `TraceWinRecoverTick(elapsed)`。
void TraceWinRecoverTick(unsigned elapsedMs);

} // namespace agdi
} // namespace orb
