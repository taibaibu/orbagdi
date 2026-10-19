/**
 * @file ORBAGDI_TraceWin.cpp
 * @brief uVision "Trace Data" 窗口的上报通道（Keil TDI 标准流程）。
 *
 * 参照 Keil 参考驱动 `DbgCM/TraceWinConnect.cpp`：
 *   - `InitTraceInterface()`  -> 本文件 TraceWinInit() 的注册部分
 *   - `ConfigureTraceWin()`   -> 本文件 Configure()（列 / 过滤器 / 异常表照搬）
 *   - `TraceStatusUpdate()`   -> 本文件 StatusUpdate()
 *   - TDI 回调                -> 本文件补全（参考驱动里全是 TODO 桩，
 *                                DBGCM_FEATURE_TRCDATA_WIN 为 0）
 *
 * 与参考驱动的差异（都是"把桩补成实现"）：
 *   - 记录缓冲是**块链**（P1-1：双向链表 + 空闲链 + 按周期的块索引，见下），
 *     `GetTraceRecords64` / `GetFiltTraceRecords64` 按周期区间真的查、真的拷；
 *   - 周期号是合成的（每条记录 +1，见头文件），所以 `bHideTimestamps = 1`；
 *   - `UpdateTraceBuffer` 通过 AGDI 层给的 refresh 钩子重新拉一次数据
 *     （ETB 重读 / orbtrace 排空）。
 *
 * 所有 Notify 都走 BOM（`pio`）：uVision 通过 AG_CB_GETBOMPTR 把 `struct bom*`
 * 交给我们，orb::pdsc 一直存着它，这里直接借用。
 */
#include "pch.h"
#include "ORBAGDI_TraceWin.h"
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_Etb.h"
#include "ORBAGDI_TraceFile.h"   // TraceFileMark（P2-1 的落盘索引）

#include <stdio.h>
#include <string.h>
#include <process.h>   // _beginthreadex（退休队列的后台写线程）

// uVision 自己的接口。BOM.H 拥有 struct bom（AG_CB_GETBOMPTR 返回的 "ioc"）、
// UV_NOTIFY 请求码和全局 pio；TraceView/TraceData*.h 拥有 TDI 的结构体与回调
// 表。BOM.H 会拉进 ComTyp.h，TDI 的类型正是照它写的，所以必须排在前面。
#include "BOM.H"

// TraceDataTypes.h 用 DOUBLE（TRACE_CLOCK_SAMPLE::m_dLastTime）。完整 MDK 安装
// 的 ComTyp.h 里有这个 typedef，本仓库里这份精简的 COMTYP.H 没有，所以补一个。
// 放在 Keil 的头之前，且只在没有别处定义过时才生效。
#ifndef DOUBLE
typedef double DOUBLE;
#endif

#include "TraceView/TraceDataTypes.h"
#include "TraceView/TraceDataIf.h"

// 本驱动是 ANSI 构建（build.ps1 不定义 UNICODE），LPCTSTR == const char*。
// 用这个宏把意图写清楚，同时不必依赖 tchar.h。
#define TRCSTR(s) s

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// 记录缓冲：块链（P1-1）+ 紧凑记录（P1-2）
// ---------------------------------------------------------------------------
// 单块环形缓冲装不下多少时间：131072 条 × 22 B = 2.9 MB，实测只覆盖 1,549,863 个
// 周期 —— 48 MHz 下 **10 ms**（Todo.md §四 P1-1 的量化理由）。而全指令 trace 下
// 记录速率 = 执行指令率（上限 48 M/s），1.5 GB 也只够 3.5–6 s。⇒ 内存缓冲的定位
// 本来就该是"秒级滚动窗口"，但 10 ms 连一帧都不够。
//
// **两层**改动，各对应一条判据：
//
//   ① P1-1 块链 —— 块之间双向链表相连：尾部追加、头部摘最旧、空闲链复用。
//      追加只在"当前块满了"时才要新块（空闲链 → 新分配 → 摘最旧复用），所以
//      **分配失败只表现为"少一块"，不会整体失败**（每次只要 4 MB，而 32 位进程里
//      "一次要 1 GB 连续内存"必然失败）。`idx` 是按 lastCycle 升序的块指针数组，
//      二分定位（1 GB / 4 MB = 256 块 ⇒ ≤ 8 次比较，数组连续、缓存友好）。
//      ⚠ 对原方案"hash 映射（cycle → 块）"的**修正：不用 hash**。周期轴是稀疏的
//      （ETM 真实周期跳变、ITM 时间戳粒度粗），"cycle → 桶"要么大量空桶要么互相
//      撞；块数上限只有几百，二分更快也更简单，还不依赖"周期分布均匀"这个假设。
//
//   ② P1-2 紧凑记录 —— **不存 22 B 的 `TRACE_RECORD_64`**，存"标志 + 载荷"：
//
//        字节 0 : nType（TR_TYPE_*）
//        字节 1 : flags（与 `TRACE_RECORD_64` 的位域**同布局**：bTS/bPC/bAddr/
//                 nDataS/bExec/bTD/bOvf —— 展开时逐位拷回，不丢信息）
//        之后按 flags 依次出现：
//          bPC    → PC（4 B）
//          bAddr  → 地址（4 B；**ITM 记录是 1 B 的端口号**，同官方 PacketSize()）
//          nDataS → 数据（0 / 1 / 2 / 4 B，小端）
//          周期差 → varint（7 位/字节，最高位 = 还有后续；典型值 1 ⇒ 1 B）
//
//      周期**不存绝对值**，存相对上一条的差；查询时从块的 firstCycle 累加还原。
//      一条 ETM 指令记录 = 2 + 4 + 1 = **7 B**（旧表示 22 B ⇒ **3.1×**），
//      ITM 激励 5–8 B、事件 4 B、异常 5 B。这正是官方 `PacketSize()`
//      （`DbgCM/TraceRec.cpp:423`）的思路（标志(1) + 地址(4) + 时间戳差(4)），
//      只是它把差固定成 4 B —— 这里用 varint，典型差就是 1。
//
//      变长编码的代价：**想从块中间开始查就得从头解**（记录自定长，没有下标）。
//      所以每块带一张**检查点表**（`kCkptPerChunk` 条 `{字节偏移, 周期号}`），
//      查询二分到最近的检查点再解码 —— 每次最多解 1/256 块（4 MB 块 ≈ 16 KB ≈
//      2300 条），代价可忽略。
//
// 锁：**结构改动**（挂块 / 摘块 / 清空 / 换块）取 `Store::storeLock`，查询期间整段持有；
// **逐条追加不取锁** —— 那是热路径（实测 3.96–7.80 M 条/s，一条一取锁要吃掉一个核）。
// 发布顺序保证并发读者看不到半条记录：先写完字节流，**最后**加 `usedBytes`。
const unsigned kChunkBytes   = 4u << 20;   // 每块 4 MB（§九：4–16 MB 里取下限）
// 内存窗口上限：**物理内存的 1/4，封顶 1 GB**。
//
// ⚠⚠ **这个值不要缩小。** 2026-10-14 我曾以"和查询那段的注释矛盾"为由把它收到 64 MB
//    —— 那是一次**破坏性的**改动，不是优化：
//
//   P1-1 / P1-2 / P2-2 的整套容量账都是对着"1.5 GB / 1 GB 上限"算的
//   （Todo.md §二 的表：紧凑记录 5–9 B/条、3.96–7.80 M 条/s ⇒ 1.5 GB 装 21–76 s；
//    容量公式 `时长 = 内存上限 ÷ (记录速率 × 每条字节数)`）。砍到 64 MB ⇒ 那张表
//   全部作废（变成 ~1.7–5 s），P1-2"同容量装 4.4 倍"的收益、块链存在的理由、
//   P2-2 的定位**一起失效**。
//
//   而且块链的设计目的就是**尽量长地延伸**（用户 2026-10-14 原话：**"用块链，1M 一个块，
//   无限延伸，以达到无限采样的目的"**）—— 上限的作用只是"别把 32 位进程的地址空间
//   吃光"，**不是**"限制能回看多久"。
//
//   要缩短是**用户**用 `trmem=` 按项目自己调（Record 页 / 选项串）；默认值不动。
//
//   附带说明：当时的"卡顿"根因**不在上限**，而在 QueryRange 的内层循环过了 `hi` 只
//   `continue` 不 `break` ⇒ 每次查询把 `lo` 之后的记录全解一遍（已修，见那一段）。
//   拿上限去治那个症状，是治错了地方。
const unsigned kMemLimitMax  = 1u << 30;   // 上限封顶 1 GB
const unsigned kMemLimitMin  = 16u << 20;  // 上限下限 16 MB
const unsigned kMemLimitFrac = 4;          // 上限 = 物理内存的 1/4
// 选项 trchunk= / trmem= 的合理范围（MB）。写错一个数量级不该把会话拖垮。
const unsigned kChunkMBMin   = 1;
const unsigned kChunkMBMax   = 64;
const unsigned kMemMBMax     = 4096;
// 每块的检查点条数（见上）。256 ⇒ 每次查询最多解 1/256 块。
const unsigned kCkptPerChunk = 256;

// ---- 记录条数上限（`trmax=`，第 1 号）----------------------------------------
// ⚠⚠ 2026-10-15 **默认关掉**（`trmax=0` ⇒ 窗口只受**字节**上限 `trmem=` 约束）。
//
// 这里原来是硬编码的 `1u << 20`（对齐官方 `0x100F0A8C cmp 0x100000`），它在真机上
// 做出来的事是**静默吃掉整段历史**：ETM 一条紧凑记录 5–9 B ⇒ 1.05 M 条 ≈ **0.13 s**，
// 于是"10 s 采集"只留最后 0.13 s。现场（用户 2026-10-15 报告）停止时那行：
//     `… 持有=552 MB 上限=1024 MB … 上限裁掉=81608143 条 累计写入(本段)=82658740 条`
// 三个数一算就明白：**内存只用了 552 MB / 1024 MB，整段 10 s 本来装得下**
// （82.66 M 条 ≈ 552 MB），是这条条数上限把**内存上限架空了**；而它动手的位置
// （`EnforceRecCap` 的就地裁剪）**不落盘、不报警、不置 `T_ERR_DATA_OVF`** ——
// `摘块=0 丢历史=0 丢记录=0`，一个信号都没有。定性、日志锚点与修法见 `Todo.md §4.3`。
//
// 它与本文件的既有裁定直接冲突（见上面 kMemLimitMax 那段）：上限的作用只是
// "别把 32 位地址空间吃光"，**不是**"限制能回看多久"。
//
// 要控窗口大小请用 `trmem=`（字节，精确）；`trmax=` 只在确实要"卡条数"时用，
// 而且现在**走整块退休**（落盘 / 响亮报错），不再静默裁字节。
const uint64_t kMaxRecsSlack = 4096;       // 超出这么多条才动一次手，摊薄解码成本


// ---- 退休队列（P3-1）：抽数 ↔ 落盘之间的交接层 ------------------------------
//
// 官方 ULP2CM3.dll 把"抽数（快、实时）"与"落盘（慢、I/O）"解耦靠的是一个**环**：
// 生产者推进写指针时 `and eax, 3`（`0x100EBAAAD`）⇒ 环里正好 **4 个槽**；
// 初始化点（`0x100EA468..0x100EA48B`）也把两个平行数组各写了 4 项，与数组间距
// `0x10`（16 B = 4 × 4 B）完全吻合。**这个 4 是逆向出来的，不是拍的。**
//
// 本驱动原来没有这一层：`NewWriteChunk()` 在**抽数线程上同步 `fwrite`** 一整块
// （4 MB），那一瞬间抽数就停了 —— 用户实测反馈的"卡顿"正是它。现在改成：写满的块
// 只把**指针**交给后台写线程，抽数立刻拿下一块继续。
const unsigned kRetireQ = 4;

// 队列满时抽数最多等多久（毫秒）。
//
// 官方在这里是**无限等**（`0x100EBAC8` 压 `0x12bc7a14` 调 `[0x10218428]`，没有超时）。
// 这里给个上限：等不到就报错并丢，**绝不能让抽数线程永远挂着** —— 探针侧的 RDDI
// 环只有那么大，抽数停住的损失（环溢出、整段缺数据）比丢一块历史大得多。
//
// ⚠ 这个等待发生在 `NewWriteChunk()` 里，而它**持着结构锁 `storeLock`** ⇒ 并发查询
//   （`GetTraceRecords64` 整段持 `storeLock`）会被一起卡住。正常情况根本等不到：写线程
//   **弹出即释放空槽**，所以等待量级是"写完上一块"的时间（4 MB / 100 MB/s ≈ 40 ms）。
//   1 s 已经是 25 倍余量；真等到 1 s 说明磁盘卡死了，那本来就是错误路径。
const DWORD kRetireWaitMs = 1000;

struct Chunk {
    Chunk*   prev;
    Chunk*   next;        // 空闲链上也用它
    uint64_t firstCycle;  // 第一条记录的周期号
    uint64_t lastCycle;   // 最后一条记录的周期号
    // 本块**第一条记录**的全局序号（`Store::recSeq`）。块内序号连续，所以本块覆盖过的
    // 序号区间是 `[firstRecSeq, firstRecSeq + deadRecs + nRecs - 1]`。
    // ⚠ 裁剪（`kMaxRecs`）之后**可读**区间收窄成 `[headRecSeq, headRecSeq + nRecs - 1]`。
    //   `firstRecSeq` 只是块的**身份**（单调、不复用、二分定位用），**别拿它当"最旧"**。
    // 用途（P4-1）：uVision 的 `nRecordOfs` 是**区间内的记录序号**，有了全局序号就能
    // 二分定位、直接跳到目标附近，不必从区间头一条条跳过（那是卡顿的根因）。
    // 它同时是块的**身份**：序号单调递增且不复用 ⇒ 游标靠它判断"那块还在不在"。
    uint64_t firstRecSeq;
    // ---- 记录级裁剪（`trmax=`，第 1 号）----------------------------------------
    // ⚠ 2026-10-15：`EnforceRecCap` 已改成**整块退休**（落盘 / 响亮报错），不再就地
    //   裁字节 ⇒ 默认配置（`trmax=0`）下 `headOfs` / `deadRecs` / `headRecSeq` **恒为 0**，
    //   下面这套"死前缀"机制进入**休眠**。字段保留：`usedBytes - headOfs` 的记账还在用，
    //   而且将来若要做**精确**的条数裁剪（而不是块粒度的 retirement）还得靠它。
    //   原本的语义是：`headOfs` 之前的前缀是**已裁掉**的记录，只有
    //   buf[headOfs, usedBytes) 是活的；裁剪**只推进游标、不搬字节**（搬 4 MB 会把热
    //   路径打死），所以废弃字节留在原地直到整块被淘汰或复用 —— 现场 `持有=552 MB`
    //   里绝大部分就是这种死前缀（而可读数只有 6 MB），定性见 `Todo.md §4.3`。
    uint32_t headOfs;         // 可读区间起点（块内字节偏移）；必须落在记录边界上
    uint32_t deadRecs;        // headOfs 之前已裁掉的条数
    // `buf + headOfs` 处那条记录的**绝对**周期号，必须是某条真实记录的周期号
    // （**绝不能是 0**）：区间最旧周期号 / GetFirstCycle 直接报它。
    uint64_t headFirstCycle;
    // `headOfs` 处那条记录的**全局序号**（`Store::recSeq`）。与 `headFirstCycle` 平行，
    // 两者必须同步推进。
    //   为什么不写 `firstRecSeq + deadRecs`：那条算式把正确性押在"`deadRecs` 永远是有效
    //   中间量"上，而有一条已声明不可达、但**确实可以为错**的路径 —— 块被裁空后若再次
    //   收到记录，首条写入分支会重定 `firstRecSeq` 却**不清 `deadRecs`** ⇒ 算式偏大
    //   `deadRecs`。存字段就与那条路径解耦了。
    uint64_t headRecSeq;
    unsigned usedBytes;   // 已写字节数，**最后**发布（读者靠它判断到哪是完整的）
    unsigned capBytes;
    // 当前**可读**条数（裁剪后递减）。计数、日志与查询快路的 `n += c->nRecs` 都用它，
    // 所以它必须**唯一**地表示"现在能读到几条"。
    // 不变量：nRecs + deadRecs = 本块写入过的总条数。
    unsigned nRecs;
    unsigned nCkpt;       // 已填的检查点数
    unsigned nextCkptAt;  // 下一个检查点的字节阈值
    uint32_t ckptOfs[kCkptPerChunk];   // 该处记录的字节偏移
    uint64_t ckptCyc[kCkptPerChunk];   // 该处记录的**绝对**周期号
    uint64_t ckptRec[kCkptPerChunk];   // 该处记录的**全局序号**（P4-1）
    uint8_t* buf;                      // 紧凑记录字节流，紧跟块头
};

// 块头之后字节流的偏移。字节流是 `#pragma pack(1)` 的记录序列（对齐 1），所以
// 只需要把块头补齐到 8 字节。
const size_t kChunkHdr = (sizeof(Chunk) + 7u) & ~static_cast<size_t>(7u);

// 一块的字节数（分配与记账都用它）。`capBytes` 是**字节流**的容量。
size_t ChunkBytes(unsigned capBytes)
{
    return kChunkHdr + static_cast<size_t>(capBytes);
}

struct Store {
    // ---- 块链（P1-1）-------------------------------------------------------
    Chunk*   head;       // 最旧的一块
    Chunk*   tail;       // 最新的一块
    Chunk*   freeList;   // 空闲链：清空 / 摘块后挂这里，**不还给系统**
    Chunk**  idx;        // 常驻块指针，按 lastCycle 升序（= 链的顺序）
    unsigned idxCount;
    unsigned idxCap;
    unsigned nChunks;    // 已分配块数（含空闲链上的）
    unsigned maxChunks;  // 上限 / 每块字节
    unsigned chunkCapBytes; // 每块字节流容量（P1-2 起单位是**字节**，不是条）
    unsigned long long memLimit;    // 字节上限
    unsigned long long memHeld;     // 已分配字节（含空闲块）
    unsigned long long nDropped;    // 因到上限而摘掉的块数
    unsigned long long nAllocFail;  // 分配失败次数
    unsigned long long nDegraded;   // "只有一块可用"的退化次数
    unsigned long long nAppendDrop; // 因缓冲不可用而丢掉的记录数
    // 已**写满**的块（摘掉 / 退化的那些）的字节数与条数。它们的比值就是紧凑记录的
    // 真实平均大小 —— 也就是 P1-2 的容量判据（"同容量能装多少条"）的实测值。
    unsigned long long retiredBytes;
    unsigned long long retiredRecs;
    // ★ 被**记录级上限**（kMaxRecs / EnforceRecCap）裁掉的条数。**不能并入 retiredRecs**：
    //   那会破坏 `avgRec = retiredBytes / retiredRecs` —— 分子已经用 `usedBytes - headOfs`
    //   把被裁掉的字节排除在外，分母再加被裁条数 ⇒ "满块均 B/条" 偏小、容量判据失真。
    //   对外不变量：`recSeq == count + retiredRecs + capTrimmedRecs`（**无条件**成立：
    //   追加 / 摘块 / 上限裁剪各自成对增减，段界只在 `TraceWinClear()` 处五项同清）。
    unsigned long long capTrimmedRecs;
    unsigned long long count;       // 常驻记录条数
    // 本段采集的峰值。**清空之后仍然有效** —— `buf(...)` 那行日志可能落在
    // `TraceWinClear()` 之后（实测：采集停止时先清、后打），只有峰值能证明
    // "这一段到底攒下多少、挂了几块"。见 TraceWinSetRunning()。
    unsigned long long peakCount;
    unsigned         peakChunks;
    // 选项 trchunk= / trmem= 请求的规模（0 = 默认）。在 Ensure() 里应用一次。
    unsigned         reqChunkMB;
    unsigned         reqMemMB;
    // 选项 trmax= 请求的**常驻条数上限**（0 = 不限，**默认**）。见 kMaxRecsSlack 上面那段。
    // 与 reqMemMB 不同：它只是一个比较值（随时可改），不参与任何一次性分配。
    uint64_t         recCap;
    // 本段**第一条记录**的周期号（= 轴原点那一刻；现场锚点 = `TraceWin: ETM cycle
    // count base = …` 那条日志，它自己**不推进轴**，只是记录"本段从这一刻开始"）。
    // 对外报"可查范围左端"用的是它，不是"最旧一条可读记录"（见 FirstReportedCycle）。
    // ⚠ 用户 2026-10-15 裁定：内存窗口之外的历史一旦被丢掉（条数上限 / 内存到顶而记录
    //   日志没开），报"最旧可读"会让 uVision 的 Navigation 条**只画幸存的那一小段**
    //   （现场：10 s 采集只画最后 0.13 s）。`Todo.md §4.3`。
    uint64_t         segFirstCycle;
    // "本段起点**已确定**"的显式标志。
    // ⚠ 不能用 `segFirstCycle != 0` 当判据（原来是那么写的）：第一条记录的周期号
    //   **理论上可以是 0** —— `AppendRec` 的 `advance` 是入参（将来有人传 0 就中），
    //   ITM 轴模式下 `SyncAxisToWall()` 也可能把 `s.cycle` 留在 0。那时哨兵式判据会
    //   把"本段起点"当成未设 ⇒ `FirstReportedCycle()` 静默退回"最旧可读" ⇒
    //   **正是这次要修的症状**。用标志位判，值与判据彻底解耦。
    bool             segFirstCycleSet;
    uint64_t         cycle;         // 当前周期号
    // 上一条**记录**的周期号（P1-2：紧凑记录存的是相对它的差）。跨块连续 ——
    // 块的 firstCycle 是绝对值，所以差不必在块边界重置。
    uint64_t         lastRecCycle;

    // ---- 全局记录序号（P4-1）-------------------------------------------------
    // **单调递增，只在清空时归零**（摘块不回退 —— 它是"这条记录是第几条"的身份，
    // 不是"当前还留着几条"）。块头记 `firstRecSeq`、检查点记 `ckptRec`，把 uVision
    // 的 `nRecordOfs`（区间内记录序号）从"线性跳过 k 条"变成"二分定位"。
    uint64_t         recSeq;

    // ---- 查询游标（P4-2）：uVision 按 `nRecordOfs` 翻页时不重扫 -----------------
    // 现场（2026-10-14 15:20 日志）：uVision 固定一个周期区间，按记录序号一页 4096 条
    // 地取完它 —— 同一个区间连打几十次。没有游标时**每页都从区间头重新走一遍**，
    // 实测 424 次查询解了 11.36 亿条记录，只为交付 174 万条（放大 650×），
    // 35.7 s 全花在 uVision 的 UI 线程上 ⇒ 界面无响应。
    // 游标记"上次停在哪一条"，下一页从那里接着走（O(返回条数) 而不是 O(区间)）。
    struct QueryCursor {
        bool     valid;
        uint64_t lo, hi;      // 区间：必须与本次一致才敢续
        uint64_t chunkSeq;    // 所在块的 firstRecSeq —— 块的**身份**（不复用）
        unsigned ofs;         // 块内字节偏移（正好是一条记录的开头）
        uint64_t cycle;       // 该处的周期号
        uint64_t matches;     // 该处**之前**已匹配的条数（= 下次的 nRecordOfs）
        bool     fNull;       // 过滤器指纹（三个字段一起比）
        uint32_t fType;
        uint32_t fExt;
    } qcur;

    // 结构改动的互斥（见文件上方说明）。Windows 临界区**可重入**，所以加了锁的
    // 公开函数之间互相调用不会自锁。
    //
    // 命名：`storeLock` 说的是**它保护什么**（整个 Store 的结构），而不是它是什么类型
    // （`CRITICAL_SECTION` 在声明处一眼可见）。不用 `cs` 是因为它是个到处都有的缩写
    // （code segment / chip select / C#），而且本文件里真有过一个**不是锁**的局部
    // 变量叫 `cs`（`TRACE_CLOCK_SAMPLE&`，已改名 `clockSample`）。
    CRITICAL_SECTION storeLock;

    bool             registered; // TDI 回调表已交给 uVision
    bool             running;    // 正在采集
    bool             (*refresh)();

    // realAxis：周期号已有真实来源（ITM 时间戳包或 ETM 周期计数），新记录的
    //   bTS 置 1。只影响记录级时间列，与窗口级的 showTs 不是一回事。
    bool             realAxis;

    // ---- 周期轴的来源与账目（P0-F′）---------------------------------------
    // ITM 时间戳包与 ETM 周期计数是两个不同的计数器，同时接进同一条轴会重复
    // 计数，所以轴只有一个所有者（CycleSource）。
    int              cycleSrc;      // kCycleSrc*
    // 自上次真实推进以来，**记录自身**推进掉的周期数（Append 的 advance 累加）。
    // 真实增量要减掉它，否则 3 秒 1 千多万条 ETM 记录会把轴拉长 8%~17%。
    uint64_t         synthCycles;
    bool             etmHaveAbs;    // ETM 绝对周期计数的基准是否已建立
    uint64_t         etmLastAbs;    // 上一次的 ETM 绝对周期计数
    uint64_t         etmRebases;    // 计数器回绕/复位导致重建基准的次数
    uint64_t         itmCycles;     // ITM 时间戳贡献到轴上的周期数（日志用）
    uint64_t         etmCycles;     // ETM 周期计数贡献到轴上的周期数（日志用）
    bool             itmIgnored;    // "ITM 时间戳被忽略"只报一次
    // showTs：是否把 bHideTimestamps 报成 0（让 uVision 显示时间列）。
    // ⚠ 默认隐藏。它**不是**纯显示开关：`TRACE_STATUS.bHideTimestamps` 同时是 uVision
    //   取数门的一环（§10.20.1，UV4.exe `0x45D410` 的 `[0x10C]==0` 分支）：
    //     bProcessing != 0                        -> 不取数
    //     (app[0xafc] & 8) == 0                   -> 不取数   ← bom::bTrcDisp
    //     (app[0x14] & 0x2000) && app[0xbc] == 0  -> 不取数   ← app[0xbc] 就是本字段
    //   三条是**与**关系且逐条短路：`bTrcDisp` 关着时后两条根本到不了。实测
    //   （§10.20.3）`app[0x14]&0x2000 = 1`，所以在那一项成立、且 `bTrcDisp` 为 1 时，
    //   把本字段翻成 0 会命中第三条 ⇒ 窗口一行都不取；而 `0x2000` 那一位为 0 时
    //   `cmp [eax+0xbc],0` 整条被跳过，本字段对取数无影响。故由选项 tswin 驱动，默认 0。
    bool             showTs;

    // ---- 周期轴：机制 + 记录时间戳的**墙钟兜底**（2026-10-14）------------------
    // `axisMode` 决定"轴要不要追墙钟"，按 **trace 源**分两套（用户裁定，见头文件
    // `CycleAxisMode`）：ITM/SWO 数据量小 ⇒ 追墙钟；ETM 数据量太大 ⇒ 不追，只按
    // 解码推进。**默认 ETM**。
    // ⚠⚠ 2026-10-14 **撤销这道区分试过一次，已回退**：ETM 也追墙钟会让轴出现空洞 +
    //   让 uVision 按真实时间全量取数 ⇒ 1.4 亿条记录把 uVision 打崩。
    //   细节见 `SyncAxisToWall()` 上面那段。别再无脑撤它。
    //
    // 下面两个时刻只服务于**追墙钟**那条路（ITM/SWO），用来给记录打时间戳
    // （Time 列 / bTS / 区间定位）—— 它**不参与** uVision 的取数区间，那个由
    // `StatusUpdate()` 的 `m_pRecordRange` 给死。
    //   segStartTick - 本段开始（`TraceWinSetRunning(true)` / `TraceWinClear`）
    //   segStopTick  - 本段停止那一刻；**0 = 仍在采集**。停止之后必须冻住：墙钟
    //                  继续走会让时间轴在停止后自己变长（Navigation 条越拉越长），
    //                  而 uVision 恰恰在停止后才来问 `GetLastCycle`。
    // axisRejects/axisWarned 是旧的"合理性预算"闸门留下的账 —— 闸门随
    // `AdvanceFrom()` 一起退役，字段保留给日志。
    int              axisMode;      // kAxisMode*（按 trace 源设，默认 ETM）
    uint64_t         segStartTick;  // 本段采集开始的墙钟毫秒（GetTickCount64）
    uint64_t         segStopTick;   // 本段停止的墙钟毫秒；0 = 仍在采集
    uint64_t         axisRejects;   // 被拒的不可能增量个数（日志用）
    bool             axisWarned;    // "拒绝"只 WARN 一次

    // ---- 退休队列（P3-1）：抽数 ↔ 落盘的交接层 ------------------------------
    // 写满的块不再在抽数线程上同步 `fwrite`，而是挂到这里由后台线程写（见 kRetireQ）。
    // 队列**有自己的锁**（`retireLock`），**不**共用 `storeLock` —— 这是避免死锁的关键：
    //   抽数在持 `storeLock` 的情况下等空槽，而写线程"腾出空槽"只需要 `retireLock`
    //   （弹出一个就立刻 SetEvent），不需要 `storeLock`。若共用一把锁，抽数持锁等待、写线程等着取
    //   同一把锁去还块 ⇒ 互等。分开之后"还块"可以晚一点（它不影响空槽）。
    Chunk*           rq[kRetireQ];
    unsigned         rqHead;        // 队头（最旧，先落盘）
    unsigned         rqTail;
    unsigned         rqCount;
    bool             rqBusy;        // 写线程手上正拿着一块（用于 Drain 判"真闲"）
    CRITICAL_SECTION retireLock;
    HANDLE           rqWork;        // 自动复位：有块待写
    HANDLE           rqSpace;       // 自动复位：有空槽（弹出即置位）
    HANDLE           rqThread;
    bool             rqStarted;
    // 现场证据（日志用）
    unsigned long long rqWritten;     // 已落盘块数
    unsigned long long rqWaits;       // 抽数为等空槽停下来的次数
    unsigned long long rqLostChunks;  // 没落盘就丢掉的块数（落盘没开/超时）
    unsigned long long rqLostBytes;
    bool               rqLostWarned;  // "丢历史"只报一次（每段）
    bool               capLostWarned; // "条数上限到顶且没落盘"只报一次（每段）

    Store()
        : head(nullptr), tail(nullptr), freeList(nullptr),
          idx(nullptr), idxCount(0), idxCap(0),
          nChunks(0), maxChunks(0), chunkCapBytes(0),
          memLimit(0), memHeld(0),
          nDropped(0), nAllocFail(0), nDegraded(0), nAppendDrop(0),
          retiredBytes(0), retiredRecs(0), capTrimmedRecs(0),
          count(0), peakCount(0), peakChunks(0),
          reqChunkMB(0), reqMemMB(0), recCap(0), segFirstCycle(0),
          segFirstCycleSet(false),
          cycle(0), lastRecCycle(0), recSeq(0),
          registered(false), running(false), refresh(nullptr),
          realAxis(false),
          cycleSrc(kCycleSrcNone), synthCycles(0),
          etmHaveAbs(false), etmLastAbs(0), etmRebases(0),
          itmCycles(0), etmCycles(0), itmIgnored(false),
          showTs(false),
          axisMode(kAxisModeEtm),
          segStartTick(GetTickCount64()), segStopTick(0),
          axisRejects(0), axisWarned(false),
          rqHead(0), rqTail(0), rqCount(0), rqBusy(false),
          rqWork(nullptr), rqSpace(nullptr), rqThread(nullptr), rqStarted(false),
          rqWritten(0), rqWaits(0), rqLostChunks(0), rqLostBytes(0), rqLostWarned(false),
          capLostWarned(false)
    {
        InitializeCriticalSection(&storeLock);
        InitializeCriticalSection(&retireLock);
        memset(rq, 0, sizeof(rq));
        memset(&qcur, 0, sizeof(qcur));
    }
};

Store& S()
{
    // 和 AGDI 的 Context 一样故意泄漏：uVision 在 CRT 拆静态对象期间仍可能
    // 回调 TDI 接口。
    static Store* s = new Store();
    return *s;
}

// uVision 会长期持有这个指针，所以放静态存储期。
TRACE_INTERFACE_INIT_64 g_init;

// 结构锁的 RAII 守卫。查询有好几条返回路径，漏一次 Leave 就是死锁。
// 临界区可重入，所以守卫套守卫（同一线程）不会自锁。
// 取"第一条**可读**记录所在的块"。
// 有两种块会处在 `nRecs == 0` 的空态，而它们**都可能**是 `s.head`：
//   ① 刚 `LinkTail` 挂上、还没写第一条的新块；
//   ② 被 `EnforceRecCap` 裁空的块 —— 它只把 `nRecs` 归 0，**不动 `s.head`**。
// 空块的 `firstCycle` / `headFirstCycle` 都不是"当前最旧记录的周期号" ⇒ 凡是要对外
// 报"最旧"的地方都必须过这里，否则会把一个更早（甚至 0）的周期号报给 uVision。
// 空块至多一个（裁剪一次最多裁空一块，之后就退出），所以循环实际 O(1)。
const Chunk* FirstReadable(const Store& s)
{
    const Chunk* c = s.head;
    while (c && c->nRecs == 0) c = c->next;
    return c;
}

// 对外报的"可查范围左端" —— 取**本段起点**，不是"最旧一条可读记录"。
// ⚠ 用户 2026-10-15 裁定（定性见 `Todo.md §4.3`）：现场一次 10 s 采集写进 82,658,740
//   条、只剩 1,050,597 条可读（条数上限裁掉 81,608,143 条），此时若报"最旧可读"，
//   uVision 的 Navigation 条就只画最后 0.13 s。三个出口 —— TDI `GetFirstCycle()`、
//   `StatusUpdate()` 的 `m_pRecordRange`、`TraceWinFirstCycle()`（停止时的显示区间）
//   —— **必须是同一个值**：两边不一致时 uVision 会按宽的那份去请求我们没有数据的
//   区间（症状与理由见 `TraceWinFirstCycle()` 里那段 BOUNDS 记录）。
// ✅ 默认配置（`trmax=0`）下不存在任何裁剪 ⇒ `segFirstCycle` 与 `FirstReadable` 相等
//   ⇒ 本函数与旧行为**完全一致**；只有真丢了历史时它才报得更早。
uint64_t FirstReportedCycle(const Store& s)
{
    const Chunk* c = FirstReadable(s);
    uint64_t first = c ? c->headFirstCycle : 0;
    // ★ 判据是**标志位**，不是 `segFirstCycle != 0`（等于 0 的合法起点不能当"未设"）。
    //   本段起点一旦确定（本段第一条记录写进去那一刻）就永远参与取小 ⇒ 无论后面
    //   怎么裁/退休/摘块，报出去的都是本段起点，绝不晚于它。
    //   `!first` 那一支是"内存里已无可读记录"（块都被退休/裁空）时的兜底：那时更
    //   只能报本段起点，报 0 会让 uVision 拿到一个空区间。
    if (s.segFirstCycleSet && (!first || s.segFirstCycle < first)) first = s.segFirstCycle;
    return first;
}

// 同上，给"要**改**那一块"的调用方（裁剪）用。
Chunk* FirstReadable(Store& s)
{
    Chunk* c = s.head;
    while (c && c->nRecs == 0) c = c->next;
    return c;
}

// 把"块内起始偏移"夹到裁剪游标 `headOfs` 之后。
//
// ★ `from` **必须落在记录边界**上，否则 `RecIter` 会从记录中段起步、**之后条条皆偏**。
//   合法边界只有三种：`0`、`c->ckptOfs[i]`（写入时在 `AppendRec` 里记的）、
//   `c->headOfs`（由 `recStart` 推进）。三者取 max 仍是边界 ⇒ 本函数安全。
// ★ `recAt` 非空时一并改：本函数**只在 `from < headOfs` 时触发**，而触发时把 `from` 置成
//   `headOfs`（取 max，不会落在记录中间）⇒ 夹过之后那个位置**就是**首条可读记录，其全局
//   序号 = 块头存的 `headRecSeq`（与 `headFirstCycle` 平行维护，不再手算式）。
//   不改它，`LocateSeq` 的 `seqOut` 会从错误的序号起步。
void ClampToHead(const Chunk& c, unsigned& from, uint64_t& cycAt, uint64_t* recAt)
{
    if (from >= c.headOfs) return;
    from  = c.headOfs;
    cycAt = c.headFirstCycle;
    if (recAt) *recAt = c.headRecSeq;
}

struct StoreGuard {
    StoreGuard()  { EnterCriticalSection(&S().storeLock); }
    ~StoreGuard() { LeaveCriticalSection(&S().storeLock); }
    StoreGuard(const StoreGuard&) = delete;
    StoreGuard& operator=(const StoreGuard&) = delete;
};

// ---------------------------------------------------------------------------
// 退休队列（P3-1）：抽数 → 落盘的交接层
// ---------------------------------------------------------------------------
// 三个函数的分工：
//   RetireStart() —— 惰性建队列 + 起写线程（第一次有块要退休时）
//   RetirePush()  —— 抽数侧：把写满的块交出去（队列满就等）
//   RetireDrain() —— 收尾侧：等"在飞的块"全部落完并还回空闲链
//
// **写线程是进程级的**：起一次就不停。Store 本身也是故意泄漏的（见 S()），
// 收尾只 Drain、不销毁线程 —— 会话反复 Connect/Disconnect 不该反复起线程。
//
// 死锁分析（为什么队列有**自己的锁** `retireLock`，不共用 `storeLock`）：
//   抽数在**持 `storeLock`** 的情况下等空槽（NewWriteChunk 里有 StoreGuard）。写线程要
//   "腾出空槽"只需 `retireLock`（弹出一个立刻 SetEvent(rqSpace)），**不需要 `storeLock`**；
//   它只在"把块还回空闲链"时才要 `storeLock`，而那件事**不影响空槽**。所以两者不会互等。
//   如果共用一把锁，抽数持锁等、写线程等着同一把锁去还块 ⇒ 死锁。
unsigned __stdcall RetireBody(void*)
{
    Store& s = S();

    for (;;) {
        WaitForSingleObject(s.rqWork, INFINITE);

        Chunk* c = nullptr;
        EnterCriticalSection(&s.retireLock);
        if (s.rqCount) {
            c = s.rq[s.rqHead];
            s.rqHead = (s.rqHead + 1u) % kRetireQ;
            --s.rqCount;
            s.rqBusy = true;
        }
        LeaveCriticalSection(&s.retireLock);
        if (!c) continue;

        // **空槽在这一刻就释放**，不等整块写完 —— 这正是官方 4 槽环的语义：
        // 生产者最多领先消费者 4 块，而"领先"的门槛是**消费者取走**，不是"写完"。
        // 抽数因此永远不必等一次磁盘 I/O（除非队列真的满了）。
        SetEvent(s.rqSpace);

        // 落盘（P2-2）：先记索引（指向这块的**第一条**记录），再把字节流**原样**
        // 追加。顺序不能反 —— 索引里的 `ofs` 必须是"这条记录在文件里的位置"。
        // ★ 只写"活的区间"，索引周期号取首条**可读**记录 —— 否则回放会凭空多出
        //   已被裁剪掉的历史（写进文件后不可回收，是最严重的一类错）。
        // ★ 空块守卫：被 EnforceRecCap 裁空的块 `headOfs == usedBytes` ⇒ 不写 0 长度段，
        //   也不打一个**没有记录**的空 mark（回放侧会因此多出一个空段/伪时间点）。
        if (c->usedBytes > c->headOfs) {
            TraceFileMarkRecords(c->headFirstCycle);
            TraceFileWriteRecords(c->buf + c->headOfs, c->usedBytes - c->headOfs);
        }

        // 还块：要结构锁。抽数此刻若正持锁，只会挡住这一小段（不影响空槽）。
        {
            StoreGuard g;
            // 归还前清掉裁剪游标：让"空闲链上的块不带裁剪状态"成为**真的**不变量，
            // 而不是靠下游复用点记得复位。
            c->headOfs        = 0;
            c->deadRecs       = 0;
            c->headFirstCycle = 0;
            c->headRecSeq     = 0;
            c->next = s.freeList;
            s.freeList = c;
        }

        EnterCriticalSection(&s.retireLock);
        s.rqBusy = false;
        ++s.rqWritten;
        LeaveCriticalSection(&s.retireLock);
    }
    return 0;
}

void RetireStart()
{
    Store& s = S();
    if (s.rqStarted) return;

    // 自动复位事件：一次 Push 一次 SetEvent，语义正好是"有多少块就有多少次唤醒"。
    s.rqWork  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.rqSpace = CreateEventW(nullptr, FALSE, TRUE,  nullptr);   // 初始 4 个空槽
    if (!s.rqWork || !s.rqSpace) {
        ORB_LOG_WARN("TraceWin: 退休队列的事件建不出来 —— 落盘退回同步写");
        if (s.rqWork)  { CloseHandle(s.rqWork);  s.rqWork  = nullptr; }
        if (s.rqSpace) { CloseHandle(s.rqSpace); s.rqSpace = nullptr; }
        return;                      // rqStarted 保持 false ⇒ 调用方走同步兜底
    }

    unsigned tid = 0;
    s.rqThread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, RetireBody, nullptr, 0, &tid));
    if (!s.rqThread) {
        ORB_LOG_WARN("TraceWin: 落盘写线程创建失败 —— 退回同步写");
        CloseHandle(s.rqWork);  s.rqWork  = nullptr;
        CloseHandle(s.rqSpace); s.rqSpace = nullptr;
        return;
    }
    s.rqStarted = true;
    ORB_LOG_INFO("TraceWin: 落盘写线程已启动（交接队列 %u 槽 / 每块 %u MB）"
                 "—— 抽数不再等磁盘 I/O",
                 kRetireQ, static_cast<unsigned>(s.chunkCapBytes >> 20));
}

// 把一块写满的历史交给写线程。true = 交出去了（写线程负责落盘 + 还块）；
// false = **没交出去**，调用方必须自己处置（原地复用，那段历史就没了）。
bool RetirePush(Chunk* c)
{
    Store& s = S();

    // 装不下、又落不了盘 ⇒ 这段历史**真的没了**。
    // 官方在这里的动作是置错误位（`or dword ptr [0x12bd1f44], 0x200`，落盘文件建
    // 不出来时置的）；本驱动的对应物是 `T_ERR_DATA_OVF` ⇒ 状态栏
    // "Trace: Data Overflow"。**静默丢历史不可接受**（用户 2026-10-14 的要求）。
    if (!TraceFileRecordsActive()) {
        s.rqLostChunks++;
        s.rqLostBytes += c->usedBytes - c->headOfs;
        if (!s.rqLostWarned) {
            s.rqLostWarned = true;
            ORB_LOG_ERROR("TraceWin: 内存缓冲已满（上限 %u 块 / %llu MB）而"
                          "**记录日志没开** —— 最旧的历史正在被丢弃。"
                          "要留住它：选项串加 `trrec=1`（落 %TEMP%），"
                          "或调大 trmem=",
                          s.maxChunks, s.memLimit >> 20);
            TraceWinErrorSet(kTErrDataOvf);
        }
        return false;
    }

    RetireStart();
    if (!s.rqStarted) return false;          // 线程/事件没起来 ⇒ 调用方同步兜底

    uint64_t t0 = 0;
    for (;;) {
        EnterCriticalSection(&s.retireLock);
        if (s.rqCount < kRetireQ) {
            s.rq[s.rqTail] = c;
            s.rqTail = (s.rqTail + 1u) % kRetireQ;
            ++s.rqCount;
            LeaveCriticalSection(&s.retireLock);
            SetEvent(s.rqWork);
            return true;
        }
        LeaveCriticalSection(&s.retireLock);

        // 队列满 ⇒ 等写线程腾出空槽。**官方在这里就是阻塞等待**
        // （`0x100EBAC8` 压 `0x12bc7a14` 调 `[0x10218428]`，没有超时），
        // 只是官方无限等，这里给个上限（见 kRetireWaitMs）。
        if (!t0) { t0 = GetTickCount64(); ++s.rqWaits; }
        const uint64_t waited = GetTickCount64() - t0;
        if (waited >= kRetireWaitMs) {
            s.rqLostChunks++;
            s.rqLostBytes += c->usedBytes - c->headOfs;
            if (!s.rqLostWarned) {
                s.rqLostWarned = true;
                ORB_LOG_ERROR("TraceWin: 落盘跟不上（队列 %u 块全满，等了 %u ms）"
                              "—— 丢弃最旧的一块以保住抽数（磁盘太慢？"
                              "trrecmb= 到顶后落盘会停，检查那一条）",
                              kRetireQ, kRetireWaitMs);
                TraceWinErrorSet(kTErrDataOvf);
            }
            return false;
        }
        WaitForSingleObject(s.rqSpace, static_cast<DWORD>(kRetireWaitMs - waited));
    }
}

// 等"在飞的块"全部落完并还回空闲链。
// ⚠ **不持结构锁**：写线程写完要取结构锁才能还块，持锁等它 = 死锁。
void RetireDrain()
{
    Store& s = S();
    if (!s.rqStarted) return;

    const uint64_t t0 = GetTickCount64();
    for (;;) {
        EnterCriticalSection(&s.retireLock);
        const bool idle = (s.rqCount == 0) && !s.rqBusy;
        const unsigned left = s.rqCount;
        LeaveCriticalSection(&s.retireLock);
        if (idle) return;

        if (GetTickCount64() - t0 > (kRetireWaitMs * 4)) {
            ORB_LOG_WARN("TraceWin: 等落盘写线程超时（队列还剩 %u 块）—— 不再等",
                         left);
            return;
        }
        Sleep(1);
    }
}

// 分配一块（块头 + 字节流，**一次**分配）。失败返回 nullptr。
//
// **不清零字节流**：查询只走 `usedBytes` 之内的字节，而每条记录都是"写满才发布"。
Chunk* AllocChunk(unsigned capBytes)
{
    const size_t bytes = ChunkBytes(capBytes);
    void* p = ::operator new(bytes, std::nothrow);
    if (!p) return nullptr;

    Chunk* c = new (p) Chunk();
    c->prev = c->next = nullptr;
    c->firstCycle = c->lastCycle = 0;
    c->firstRecSeq = 0;
    c->headOfs        = 0;
    c->deadRecs       = 0;
    c->headFirstCycle = 0;
    c->headRecSeq     = 0;
    c->usedBytes = 0;
    c->capBytes  = capBytes;
    c->nRecs     = 0;
    c->nCkpt     = 0;
    c->nextCkptAt = capBytes / kCkptPerChunk;
    c->buf       = reinterpret_cast<uint8_t*>(static_cast<char*>(p) + kChunkHdr);
    return c;
}

bool Ensure()
{
    Store& s = S();
    if (s.idx) return true;

    // 块大小与上限。默认（Todo.md §九）：每块 4 MB，上限 = 物理内存 1/4 封顶
    // 1 GB；选项 trchunk= / trmem= 可覆盖（见 TraceWinSetBuffer）。
    // ⚠ 上限**不要缩小** —— 见 kMemLimitMax 上面的说明。
    // **惰性分配**：这里只算账 + 申请索引数组，第一块等第一条记录到达才要 ——
    // 所以"没开 trace 的会话"一个字节的缓冲都不占。
    //
    // P1-2 起 `chunkCapBytes` 是**字节**（紧凑记录是变长的，没有"条数容量"）。
    const unsigned chunkMB = s.reqChunkMB ? s.reqChunkMB : (kChunkBytes >> 20);
    s.chunkCapBytes = chunkMB << 20;
    if (s.chunkCapBytes < 4096) s.chunkCapBytes = 4096;

    unsigned long long limit;
    if (s.reqMemMB) {
        limit = static_cast<unsigned long long>(s.reqMemMB) << 20;
    } else {
        limit = kMemLimitMax;
        MEMORYSTATUSEX ms;
        memset(&ms, 0, sizeof(ms));
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys) {
            const unsigned long long quarter = ms.ullTotalPhys / kMemLimitFrac;
            if (quarter < limit) limit = quarter;
        }
        if (limit < kMemLimitMin) limit = kMemLimitMin;
    }
    const size_t per = ChunkBytes(s.chunkCapBytes);
    if (limit < per) limit = per;          // 至少要装得下一块
    s.memLimit = limit;

    unsigned long long maxC = limit / per;
    if (maxC < 1)     maxC = 1;
    if (maxC > 0xFFFF) maxC = 0xFFFF;      // idx 计数是 unsigned，给个天花板
    s.maxChunks = static_cast<unsigned>(maxC);
    s.idxCap    = s.maxChunks;

    s.idx = new (std::nothrow) Chunk*[s.idxCap];
    if (!s.idx) {
        ORB_LOG_ERROR("TraceWin: 块索引分配失败（%u 块），Trace 窗口没有缓冲",
                      s.idxCap);
        return false;
    }
    ORB_LOG_INFO("TraceWin: 记录缓冲 = 块链 %u MB/块（%u 字节流 + %u 检查点），"
                 "上限 %u 块 / %llu MB（物理内存 1/4 封顶 1 GB，惰性分配）；"
                 "落盘交接队列 %u 槽 —— 在飞的块另计，最多再占 %u MB",
                 chunkMB, s.chunkCapBytes, kCkptPerChunk,
                 s.maxChunks, s.memLimit >> 20,
                 kRetireQ, (kRetireQ + 1u) * chunkMB);
    return true;
}

// 把块挂到链尾（= 时间序最新）并进索引。
void LinkTail(Chunk* c)
{
    Store& s = S();
    c->prev = s.tail;
    c->next = nullptr;
    if (s.tail) s.tail->next = c; else s.head = c;
    s.tail = c;
    if (s.idxCount < s.idxCap) s.idx[s.idxCount++] = c;
}

// 把块**接回链头**（= 时间序最旧）并插回索引首位。
// 只给"摘出来又交不出去"那条路用（`EnforceRecCap`：`DropOldest` 之后 `RetirePush`
// 失败 ⇒ 块**必须**回链，绝不能就这么从链上消失）。
// 摘的时候传了 `markFile = false`（没记 OFLOW 索引），所以这里不必回滚索引；
// 但 `DropOldest` 已经动过的账要**逐项回滚** —— 对外不变量
// `recSeq == count + retiredRecs + capTrimmedRecs` 必须无条件成立
// （见 Store::capTrimmedRecs 的说明）。
void LinkHead(Chunk* c)
{
    Store& s = S();
    s.count        += c->nRecs;
    s.retiredBytes -= c->usedBytes - c->headOfs;
    s.retiredRecs  -= c->nRecs;
    if (s.nDropped) --s.nDropped;

    c->prev = nullptr;
    c->next = s.head;
    if (s.head) s.head->prev = c; else s.tail = c;
    s.head = c;
    if (s.idxCount < s.idxCap) {
        memmove(s.idx + 1, s.idx, s.idxCount * sizeof(Chunk*));
        s.idx[0] = c;
        ++s.idxCount;
    }
}

// 摘掉最旧的一块。**调用方必须保证 idxCount >= 2**，否则会把链摘空。
//
// `markFile`：是否顺手给 OFLOW 文件（`trfile=`）记一条"这段历史离开内存"的索引。
// ⚠ 只有块**真的**离开内存时才该记。`EnforceRecCap` 是先摘、再问 `RetirePush`
//   （交不出去就把块接回链头，那时它还在内存里）⇒ 它传 `false`，成功后才自己补记。
Chunk* DropOldest(bool markFile = true)
{
    Store& s = S();
    Chunk* c = s.head;
    if (!c) return nullptr;

    s.head = c->next;
    if (s.head) s.head->prev = nullptr; else s.tail = nullptr;
    c->prev = c->next = nullptr;

    s.count -= c->nRecs;
    // 摘掉的块一定是**写满**的（只有满块才会要新块）⇒ 它的比值就是实测的
    // "每条多少字节"（P1-2 的容量判据）。
    // ★ 减去已裁掉的前缀 `headOfs`：那些记录早已从 count 销账，若这里仍按 usedBytes
    //   记字节，"平均每条字节数"会随裁剪次数单调变大（把丢掉的历史算成每条更胖）。
    s.retiredBytes += c->usedBytes - c->headOfs;
    s.retiredRecs  += c->nRecs;
    // 落盘索引（P2-1）：这段历史**刚刚离开内存**，只剩文件里那一份 —— 正是
    // 记下"它落在文件哪个偏移之前"的时刻。这一句只往内存索引里追加，不碰磁盘。
    if (markFile) TraceFileMark(c->lastCycle);
    // 记录日志（P2-2）**不在这里写**：调用方把块交给退休队列（RetirePush），由后台
    // 写线程去 `TraceFileMarkRecords` + `TraceFileWriteRecords`。
    // ⚠ 原来这两句就在此处**同步**执行 —— 那是抽数卡顿的直接原因（见 kRetireQ）。
    if (s.idxCount) {
        memmove(s.idx, s.idx + 1, (s.idxCount - 1u) * sizeof(Chunk*));
        --s.idxCount;
    }
    ++s.nDropped;
    return c;
}

// 要一块**可写**的块。来源依次：
//   ① 空闲链（**写线程落完盘还回来的**）→ ② 新分配（链长未到上限）
//   ③ 链已到上限 ⇒ 把最旧的一块**退休给写线程**（异步落盘），自己立刻拿下一块
//   ④ 退无可退 ⇒ 原地复用最旧的一块（**唯一还同步写盘的地方**）
// 返回 nullptr 表示缓冲根本不可用（索引没建起来）。
Chunk* NewWriteChunk()
{
    Store& s = S();
    StoreGuard g;

    Chunk* c    = nullptr;
    bool   link = true;

    // ① 空闲链（写线程还回来的 + 清空/摘块留下的）。内存不还给系统：反复
    //    Connect/Disconnect 不该反复分配 4 MB 的块。
    if (s.freeList) {
        c = s.freeList;
        s.freeList = c->next;
        c->next = nullptr;
    }

    // ② 未到上限：新分配一块。
    //
    // ⚠ 判据从 `nChunks < maxChunks` 改成 `idxCount < maxChunks`：`nChunks` 是
    //   **分配过的块数**，而交给写线程的块在落完盘之前**仍然占着内存**。按
    //   `nChunks` 判，每写一块就少一块窗口深度（在飞的块白白吃掉容量）。
    //   按**链长**判之后，在飞的块最多 kRetireQ + 1 块（队列 4 槽 + 写线程手上 1 块）
    //   ⇒ 内存上界 = (maxChunks + kRetireQ + 1) × 每块字节
    //   （默认 4 MB × 5 = **20 MB 的额外占用**）。
    if (!c && s.idxCount < s.maxChunks) {
        c = AllocChunk(s.chunkCapBytes);
        if (c) {
            ++s.nChunks;
            s.memHeld += ChunkBytes(s.chunkCapBytes);
        } else if (s.nAllocFail++ == 0) {
            // 32 位进程地址空间不够是**正常**结局，不是错误：缓冲停止增长而已。
            ORB_LOG_WARN("TraceWin: 块分配失败（%u MB/块，已持有 %llu MB）—— "
                         "缓冲停止增长，改用摘最旧块复用",
                         static_cast<unsigned>(ChunkBytes(s.chunkCapBytes) >> 20),
                         s.memHeld >> 20);
        }
    }

    // ③ 链到上限：把**最旧的一块**退休给写线程（异步落盘），自己立刻拿下一块继续。
    //
    //    官方就是这个结构：缓冲装不下 ⇒ 落盘，而落盘是**消费者**的事，抽数只推
    //    写指针（`0x100EBABB`）再和读指针比一下（`0x100EBAC0`）。本驱动原来在这
    //    里**同步 `fwrite` 一整块 4 MB**，那一瞬间抽数停了 —— 用户实测的卡顿。
    if (!c && s.idxCount >= 2) {
        Chunk* old = DropOldest();           // 摘出链（不动字节流），idxCount 已减
        if (old) {
            if (RetirePush(old)) {
                // 交出去了 ⇒ 必须另取一块。此刻空闲链必然是空的（写线程要取结构锁
                // 才能还块，而锁在我们手上）⇒ 直接新分配；分配不出来是"地址空间
                // 到顶"的病态，落到 ④ 的原地复用。
                c = AllocChunk(s.chunkCapBytes);
                if (c) {
                    ++s.nChunks;
                    s.memHeld += ChunkBytes(s.chunkCapBytes);
                } else if (s.nAllocFail++ == 0) {
                    ORB_LOG_WARN("TraceWin: 块分配失败（%u MB/块，已持有 %llu MB）"
                                 "—— 退化为原地复用最旧块",
                                 static_cast<unsigned>(ChunkBytes(s.chunkCapBytes) >> 20),
                                 s.memHeld >> 20);
                }
            } else {
                // 没交出去（记录日志没开 / 写线程等超时）：**指针不能丢**，原地复用。
                // RetirePush 已经报过错（kTErrDataOvf），这里只负责把链补回来。
                c = old;
            }
        }
    }

    // ④ 退无可退：原地复用最旧的一块（丢这段历史）。
    //    走到这里只有两种情形：`trmem` 只够一块（`idxCount == 1`，头就是尾，等价于
    //    P1-1 之前的单块环形），或 ③ 里分配失败。`idxCount >= 2` 时 `head != tail`
    //    ⇒ 复用的一定是**已写满**的块，不会盖掉正在写的那一块。
    //    **这是唯一还同步写盘的地方**：块要原地复用，交不出去 —— 交出去就没得写了。
    if (!c && s.head) {
        c    = s.head;
        link = false;
        s.count -= c->nRecs;
        s.retiredBytes += c->usedBytes - c->headOfs;  // 同上，且要减掉已裁前缀
        s.retiredRecs  += c->nRecs;
        TraceFileMark(c->lastCycle);         // 落盘索引（P2-1），同上
        if (TraceFileRecordsActive()) {
            // ★ 空块守卫，同落盘线程：裁空的块不写 0 长度段、不打空 mark。
            if (c->usedBytes > c->headOfs) {
                TraceFileMarkRecords(c->headFirstCycle);              // 记录日志：先记索引
                TraceFileWriteRecords(c->buf + c->headOfs,
                                      c->usedBytes - c->headOfs);     // 再追加字节流
            }
        } else {
            s.rqLostChunks++;
            s.rqLostBytes += c->usedBytes - c->headOfs;
            if (!s.rqLostWarned) {
                s.rqLostWarned = true;
                ORB_LOG_ERROR("TraceWin: 只有一块缓冲、记录日志又没开 —— "
                              "历史被丢弃。要留住它：选项串加 `trrec=1`");
                TraceWinErrorSet(kTErrDataOvf);
            }
        }
        if (s.nDegraded++ == 0)
            ORB_LOG_WARN("TraceWin: 只有一块缓冲可用，退化为单块环形（历史被丢弃）");
    }

    if (!c) return nullptr;

    c->usedBytes = 0;
    c->nRecs     = 0;
    c->nCkpt     = 0;
    c->nextCkptAt = c->capBytes / kCkptPerChunk;
    c->firstCycle = c->lastCycle = 0;
    c->firstRecSeq = 0;              // 复用/新块都从"还没定身份"开始
    // ★ 这三行不能漏：否则 headOfs 会**跨块残留**，新块的前缀被当成已裁掉的记录。
    // headFirstCycle 归 0 只是"未定身份"态，真值由 AppendRec 在首条写入时补上。
    c->headOfs        = 0;
    c->deadRecs       = 0;
    c->headFirstCycle = 0;
    c->headRecSeq     = 0;
    if (link) {
        LinkTail(c);
        if (s.idxCount > s.peakChunks) s.peakChunks = s.idxCount;
    }
    return c;
}

// ---------------------------------------------------------------------------
// 紧凑记录的打包（P1-2）
// ---------------------------------------------------------------------------
// flags 的位与 `TRACE_RECORD_64` 的位域**同布局**（见该结构体：bTS/bPC/bAddr/
// nDataS(2 位)/bExec/bTD/bOvf），所以展开时逐位拷回即可，不丢信息。
enum : uint8_t {
    kFlTS    = 1u << 0,
    kFlPC    = 1u << 1,
    kFlAddr  = 1u << 2,
    // bits [4:3] = nDataS（0/1/2/3 → 0/1/2/4 字节）
    kFlExec  = 1u << 5,
    kFlTD    = 1u << 6,
    kFlOvf   = 1u << 7,
};

// nDataS 位（`idx` 是"字节数档位"：0→0 B、1→1 B、2→2 B、3→4 B）。
inline uint8_t DataSzBits(unsigned idx) { return static_cast<uint8_t>((idx & 3u) << 3); }

// nDataS 档位对应的载荷字节数（与官方 `PacketSize()` 的 `TB_DATA_SZ` 表一致）。
inline unsigned DataSzBytes(unsigned idx)
{
    return (idx == 1) ? 1u : (idx == 2) ? 2u : (idx == 3) ? 4u : 0u;
}

inline void PutU16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void PutU32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint16_t GetU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t GetU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// varint 周期差：7 位/字节，最高位 = 还有后续。返回写出的字节数。
unsigned PutVarint(uint8_t* p, uint64_t v)
{
    unsigned n = 0;
    for (;;) {
        const uint8_t b = static_cast<uint8_t>(v & 0x7Fu);
        v >>= 7;
        if (v) { p[n++] = static_cast<uint8_t>(b | 0x80u); }
        else   { p[n++] = b; break; }
    }
    return n;
}

// 读 varint。`avail` 是剩余字节数（防御：坏数据/被截断时不越界）。
// 返回消耗的字节数；`avail` 不足或编码超过 10 字节时返回 0（调用方应当停）。
unsigned GetVarint(const uint8_t* p, unsigned avail, uint64_t& v)
{
    v = 0;
    unsigned sh = 0, n = 0;
    while (n < avail && n < 10) {
        const uint8_t b = p[n++];
        v |= static_cast<uint64_t>(b & 0x7Fu) << sh;
        if (!(b & 0x80u)) return n;
        sh += 7;
    }
    return 0;
}

// 打包一条紧凑记录写进当前块。周期号前进 `advance`。
//
// 字段是否出现在字节流里由 `flags` 决定（同官方 `PacketSize()`）：
//   kFlPC   → pc  (4 B)        kFlAddr → addr (ITM 1 B / 其它 4 B)
//   nDataS  → data(0/1/2/4 B)  周期差  → varint
//
// 记录时间戳的**墙钟兜底**（定义在下面 SrcName() 之后）。AppendRec 是热路径，所以
// 只在这里前向声明 —— 正文（以及"⚠ 墙钟不是 BOUNDS 的修法"）写在轴那一节。
static void SyncAxisToWall(Store& s);

// 记录级硬上限的执行者：定义在 `RecIter` 之后（它要用 RecIter），而调用点在下面
// `AppendRec` 的尾上。⚠ 声明**必须**在 `AppendRec` 定义之前 —— 放在调用点之后
// 会直接 C3861（"找不到标识符"），而 IDE 诊断未必刷新，别靠 lint 判断这一段。
void EnforceRecCap(Store& s);

// 热路径（当前块还有空位）**不取锁**：实测 3.96–7.80 M 条/s，一条一取锁要吃掉
// 一个核。块满时才走 NewWriteChunk()（取锁）。
void AppendRec(uint8_t type, uint8_t flags, uint32_t pc, uint32_t addr,
               uint32_t data, uint32_t advance)
{
    Store& s = S();

    // ---- bTS：这条记录的周期号是不是**真实时间** -----------------------------
    //
    // 轴一旦有了真实来源（ITM 时间戳包 / ETM 周期计数），记录的周期号就锚在它上面，
    // uVision 的 **Time 列**要靠这一位才会填。
    //
    // ⚠ 这里原来**没有**这一句。`kFlTS` 有定义、解码端也照它还原 `rec.bTS`，但**没有
    //   任何调用点置过它** —— 于是每条记录都是 `bTS=0`，窗口的 Time 列有表头、没有值。
    //   注释里那句"新记录的 bTS 置 1"（见 Store::realAxis）只写在注释里，代码里没有。
    //   2026-10-14 从真机日志定位：`cycle axis ... bTS=1`（轴是真轴）而窗口时间列全空。
    //
    // 放在这里而不是各调用点：8 处调用都要这一位，漏一处就是"那一类记录没有时间"。
    //
    // ⚠ 2026-10-14 **试过无条件置位，已回退**（真机把 uVision 打崩，见 Todo §16.2 / §16.4）。
    //   当时的想法是"ETM 下 realAxis 恒 false ⇒ 时间列全空 ⇒ 无条件置位就有数了"。
    //   代价没算到：这一位置上以后，uVision 会把**每一条**记录的 nCycle 当时间戳用；
    //   而 ETM 现场是 1.4 亿条记录 / 5 秒，它一次全量取数就翻了。回退到"只有轴真的
    //   有来源时才置位"。
    if (s.realAxis) flags |= kFlTS;

    const unsigned dataIdx = (flags >> 3) & 3u;
    const unsigned dataLen = DataSzBytes(dataIdx);
    const unsigned addrLen = (flags & kFlAddr) ? ((type == TR_TYPE_ITM) ? 1u : 4u) : 0u;
    const unsigned pcLen   = (flags & kFlPC) ? 4u : 0u;
    // 字节数上界（varint 差最大 10 B；正常 1 B）。够大就整条写得下。
    const unsigned maxRec  = 2u + pcLen + addrLen + dataLen + 10u;

    Chunk* c = s.tail;
    // `idxCount == 0` = 缓冲刚被清空（tail 已摘、块都回了空闲链）：必须走慢路径
    // 重新挂块。少了这一条，清空与追加交错时会留下"count 涨了、链上却没有块"
    // 的状态 —— 那时 GetFirstCycle 报 EMPTY 而区间查询报 BOUNDS，窗口一片空白。
    if (!c || s.idxCount == 0 || (c->usedBytes + maxRec) > c->capBytes)
        c = NewWriteChunk();

    if (!c) {
        // 缓冲根本不可用（Ensure 失败）：丢掉这条记录，但周期轴照走。只在第一次报。
        if (s.nAppendDrop++ == 0)
            ORB_LOG_ERROR("TraceWin: 缓冲不可用，记录被丢弃（后续不再重复报）");
        s.cycle += advance;
        s.synthCycles += advance;
        s.lastRecCycle = s.cycle;
        return;
    }

    // 全局记录序号（P4-1）：**只在这条记录真的写进去之后**才递增 —— 上面那条
    // "缓冲不可用"的丢弃路径直接 return，不该占用序号。
    const uint64_t seq = ++s.recSeq;

    // ---- 记录的时间戳：抬到"墙钟位置"（2026-10-14，见 SyncAxisToWall()）-------
    // 只为让记录有个**单调的时间戳**。⚠ 它**不是** BOUNDS 的修法 —— 那个在
    // `StatusUpdate()` 里报 `m_pRecordRange`（见那里的说明）。
    // 热路径上不能每条都取墙钟，每 64 条抬一次：最多 63 条记录的周期号会略早于
    // 墙钟位置（毫秒以下，肉眼不可见）。
    if ((s.count & 63u) == 0) SyncAxisToWall(s);

    const uint64_t cyc = s.cycle + advance;
    s.cycle = cyc;
    // 本段**第一条**记录 ⇒ 记下"本段起点"（= 轴原点那一刻，现场锚点就是
    // `TraceWin: ETM cycle count base = …` 那条日志）。对外报"可查范围左端"用它，
    // 见 FirstReportedCycle()。
    // ⚠ 判据必须是 `seq == 1`（本段第一条），**不是**"块的第一条"：块会来来回回
    //   （LinkTail / 复用 / 退休），只有本段第一条才落在那条 base 日志的位置上。
    // ⚠ 必须**同时**置 `segFirstCycleSet`：`segFirstCycle == 0` 是合法起点，
    //   不能当"未设"（见该字段的说明）。
    if (seq == 1) {
        s.segFirstCycle    = cyc;
        s.segFirstCycleSet = true;
        // 一次一条：把"GetFirstCycle 之后会报的值"当场写进日志，上机可直接与
        // `[1] GetFirstCycle -> …` 对账（`Todo.md §4.3`）。
        ORB_LOG_INFO("TraceWin: 本段起点 = %llu 周期（本段第一条记录；此后 TDI "
                     "GetFirstCycle / StatusUpdate 的 m_pRecordRange / 显示区间"
                     "一律报它 —— 锚点即 `TraceWin: ETM cycle count base = …` 那条"
                     "日志的位置）", (unsigned long long)cyc);
    }
    // 这一份是**记录自己**推进的（不是目标真实经过的周期）：真实来源推进轴时
    // 要把它减掉，见 AdvanceFrom()。
    s.synthCycles += advance;

    // 周期差 = 相对上一条记录。块内第一条记 0（它的绝对值已经由 firstCycle 给出）。
    // ⚠ 用 **64 位**：周期轴偶发会跳到 1e10 量级（见 Todo.md §1 的待查项），单条差
    // 超过 4 G 时截成 32 位会让整段还原偏掉 —— varint 本身就能装 64 位。
    const uint64_t delta = (c->nRecs == 0) ? 0ull : (cyc - s.lastRecCycle);
    s.lastRecCycle = cyc;

    // ---- 检查点：跨过阈值就把"本条的位置 + 周期号"记下来（查询从这里起步）----
    if (c->nCkpt < kCkptPerChunk && c->usedBytes >= c->nextCkptAt) {
        c->ckptOfs[c->nCkpt] = c->usedBytes;
        c->ckptCyc[c->nCkpt] = cyc;
        c->ckptRec[c->nCkpt] = seq;      // P4-1：该处的全局序号（供序号直查）
        ++c->nCkpt;
        c->nextCkptAt = c->usedBytes + c->capBytes / kCkptPerChunk;
    }

    // ---- 打包 ----
    // 本条记录的起始偏移。它是**记录边界**的一种：裁剪游标 headOfs 只允许取边界值
    // （合法来源只有 0 / ckptOfs[i] / 本值），否则解码会从记录中段起步、之后条条皆偏。
    const unsigned recOfs = c->usedBytes;
    uint8_t* p = c->buf + c->usedBytes;
    *p++ = type;
    *p++ = flags;
    if (flags & kFlPC)   { PutU32(p, pc);   p += 4; }
    if (flags & kFlAddr) {
        if (addrLen == 1) { *p++ = static_cast<uint8_t>(addr); }
        else              { PutU32(p, addr); p += 4; }
    }
    switch (dataIdx) {
    case 1: *p++ = static_cast<uint8_t>(data); break;
    case 2: PutU16(p, static_cast<uint16_t>(data)); p += 2; break;
    case 3: PutU32(p, data); p += 4; break;
    default: break;
    }
    p += PutVarint(p, delta);

    // 发布顺序：先定 first/last，**最后**加 `usedBytes` —— 并发读者要么看不见这条
    // 记录，要么看见的是一条完整记录（字节流与周期号都定好了）。
    if (c->nRecs == 0) {
        c->firstCycle = cyc; c->firstRecSeq = seq;
        // ★ 首条**可读**记录的位置 —— 裁剪游标与"区间最旧周期号"都从这里起步。
        //   **绝不能留在 0**：GetFirstCycle / 区间最旧周期号会直接把它报给 UV4，
        //   而 0 就是"没有时钟"那一类症状的另一种形态。
        //   裁空之后本块若再收到记录（nRecs 回到 0 再自增），这里会重新赋真值。
        c->headOfs        = recOfs;
        c->headFirstCycle = cyc;
        c->headRecSeq     = seq;    // 与 headFirstCycle 同时定身份（见块头字段说明）
    }
    c->lastCycle = cyc;
    ++c->nRecs;
    c->usedBytes = static_cast<unsigned>(p - c->buf);

    ++s.count;
    // 峰值跟着走：停止之后 worker 还会把 RDDI 环里的积压排空（实测停止后又进了
    // 几千条），只在 SetRunning(false) 那一刻取样会低估。一次比较，分支几乎不取。
    if (s.count > s.peakCount) s.peakCount = s.count;

    // 条数上限（`trmax=`，第 1 号）：**默认关**（`recCap == 0`）—— 只在**超出余量**时
    // 才动手，未超额时只有一次比较。放在峰值取样**之后** ⇒ `peakCount` 仍是"在册条数
    // 真正达到过的值"，上限生效前最坏那个读数照样能从日志里看出来。
    // ⚠ 2026-10-15：这条上限以前是硬编码 `1<<20`，在 ETM 现场把"能回看多久"钉死在
    //   **0.13 s**（10 s 采集只留 1.05 M 条）。现在它只在用户显式要卡时生效，
    //   而且不再静默裁字节 —— 见 EnforceRecCap 与 `Todo.md §4.3`。
    if (s.recCap && s.count > s.recCap + kMaxRecsSlack) EnforceRecCap(s);
}

// ---------------------------------------------------------------------------
// 紧凑记录的解码（P1-2 的读侧）
// ---------------------------------------------------------------------------
// **按字节范围**解，所以内存块和"记录日志"（P2-2）用的是**同一段代码**：
//   - 内存块：`RecIter(c->buf + from, c->buf + c->usedBytes, cycleAtFrom)`
//   - 记录日志：按索引定位读一段，`RecIter(buf, buf + n, cycleCarry, primed)`
//
// `cycleAtFirst` = 该处**第一条记录**的绝对周期号（内存块用块头 firstCycle 或检查点
// 的 ckptCyc；记录日志用索引里记的周期号）。逐条解出 `TRACE_RECORD_64`（位域逐位
// 还原，不丢信息），周期号按 varint 差累加。尾部被截断就直接停（不越界）。
struct RecIter {
    const uint8_t*  p;
    const uint8_t*  end;
    // 当前这条记录在缓冲区里的**起始**位置（`Next()` 里记下，随后 `p` 就往后走了）。
    // 用途（P4-1/P4-2）：把"停在哪一条"变成可续扫的游标 —— 只要记下这个偏移 + 该处
    // 周期号，下次就能从这里接着解，不必从区间头重来。
    const uint8_t*  recStart;
    uint64_t        cycle;
    bool            primed;
    TRACE_RECORD_64 rec;

    RecIter(const uint8_t* first, const uint8_t* last, uint64_t cycleAtFirst)
        : p(first), end(last), recStart(first), cycle(cycleAtFirst), primed(false)
    {
        memset(&rec, 0, sizeof(rec));
    }

    // 续解（文件回放跨读段时用）：把上一条的周期号与"是否已起步"带过来。
    RecIter(const uint8_t* first, const uint8_t* last, uint64_t cycleCarry, bool primedCarry)
        : p(first), end(last), recStart(first), cycle(cycleCarry), primed(primedCarry)
    {
        memset(&rec, 0, sizeof(rec));
    }

    // 还没解的字节数（>0 说明尾部有一条被截断的记录，回放时要搬到下一段前面）。
    unsigned Left() const { return static_cast<unsigned>(end - p); }

    // 解下一条；false = 到头了（或尾部被截断）。
    bool Next()
    {
        unsigned left = static_cast<unsigned>(end - p);
        if (left < 2) return false;

        recStart = p;                              // 本条记录的起点（游标用）

        const uint8_t  type    = p[0];
        const uint8_t  flags   = p[1];
        const unsigned dataIdx = (flags >> 3) & 3u;
        const unsigned need    = 2u
            + ((flags & kFlPC)   ? 4u : 0u)
            + ((flags & kFlAddr) ? ((type == TR_TYPE_ITM) ? 1u : 4u) : 0u)
            + DataSzBytes(dataIdx)
            + 1u;                                  // varint 至少 1 字节
        if (left < need) return false;

        p    += 2;
        left -= 2;

        memset(&rec, 0, sizeof(rec));
        rec.nType  = type;
        rec.bTS    = (flags >> 0) & 1u;
        rec.bPC    = (flags >> 1) & 1u;
        rec.bAddr  = (flags >> 2) & 1u;
        rec.nDataS = (flags >> 3) & 3u;
        rec.bExec  = (flags >> 5) & 1u;
        rec.bTD    = (flags >> 6) & 1u;
        rec.bOvf   = (flags >> 7) & 1u;

        if (rec.bPC) { rec.nPC = GetU32(p); p += 4; left -= 4; }
        if (rec.bAddr) {
            if (type == TR_TYPE_ITM) { rec.nAddr = *p++;      left -= 1; }
            else                     { rec.nAddr = GetU32(p); p += 4; left -= 4; }
        }
        switch (rec.nDataS) {
        case 1: rec.nData = *p;        p += 1; left -= 1; break;
        case 2: rec.nData = GetU16(p); p += 2; left -= 2; break;
        case 3: rec.nData = GetU32(p); p += 4; left -= 4; break;
        default: break;
        }

        uint64_t       d    = 0;
        const unsigned used = GetVarint(p, left, d);
        if (used == 0) return false;               // 坏数据/被截断：停
        p    += used;
        left -= used;

        // 第一条的周期号由调用方给，之后累加差。
        if (primed) cycle += d; else primed = true;
        rec.nCycle = cycle;
        return true;
    }
};

// 记录日志回放的一段缓冲区（只有查询线程用，所以可以是静态的）。
// 记录条数上限（`trmax=`，第 1 号）的执行者。
//
// ⚠⚠ 2026-10-15 **重写**。原来这里是"**就地裁剪**"（只推进游标、不搬字节，裁到
//   `kMaxRecs = 1<<20`），它有三宗罪 —— 现场 81,608,143 条就是这样**一声不响**
//   消失的（定性、那行 `buf(...)` 原文见 `Todo.md §4.3`）：
//     ① **不落盘**：`TraceFileWriteRecords()` 只在"整块退休"那条路上被调用过，
//        所以这种丢法下**加 `trrec=1` 也一条都留不住**；
//     ② **不报警**：连"记录日志没开"都不判 ⇒ `摘块=0 丢历史=0 丢记录=0`，没有信号；
//     ③ **不置位**：`T_ERR_DATA_OVF` 也不置 ⇒ uVision 状态栏没有任何征兆。
//   （顺带：被裁的字节留在原地不回收 —— 现场 `持有=552 MB` 里绝大部分是这种死前缀。）
//
// 现在改成走**与"内存到顶"完全相同的那条路**：
//     摘最旧一块（`DropOldest`）→ 交给写线程（`RetirePush`）
//       · 记录日志开着 ⇒ 落盘（含索引），回放能接着读，**一条不丢**；
//       · 没开 ⇒ `RetirePush` 自己报 ERROR + 置 `kTErrDataOvf`，块接回链头、原地不动。
//
// 代价：淘汰粒度从"条"变成"块"（默认 4 MB ≈ 数十万条）⇒ `trmax=` 只能**粗调**；
// 要精确控窗口大小用 `trmem=`（字节）。**默认 `recCap == 0` ⇒ 本函数不会被调用。**
void EnforceRecCap(Store& s)
{
    while (s.recCap && s.count > s.recCap) {
        // 再摘就把链摘空（单块环：没有"更旧的历史"可淘汰）⇒ 停手。
        // 剩下的由内存上限兜底 —— 条数上限比内存上限还小，那是配置问题不是代码问题。
        if (s.idxCount < 2) break;

        // 摘之前先问"有没有人接"：记录日志没开 ⇒ 摘了就是真丢。这里**只报一次**
        // （每段），然后停手，让窗口回到"只受字节上限约束"的样子（那才是我们要的行为）
        // 直到用户把 `trmax=` 去掉。
        if (!TraceFileRecordsActive()) {
            if (!s.capLostWarned) {
                s.capLostWarned = true;
                ORB_LOG_ERROR("TraceWin: 记录条数上限（trmax=%llu）到顶，而**记录日志"
                              "没开** —— 最旧的历史会被丢弃（当前常驻 %llu 条）。"
                              "要留住它：去掉 trmax=（改用 trmem= 控窗口），"
                              "或加 `trrec=1`（落 %TEMP%）",
                              static_cast<unsigned long long>(s.recCap),
                              static_cast<unsigned long long>(s.count));
                // 与整块退休那条路**同一个**错误位：状态栏要看得见，绝不静默。
                TraceWinErrorSet(kTErrDataOvf);
            }
            break;
        }

        // `false`：交不出去还要接回链头（那时块仍在内存里），先别记 OFLOW 索引。
        Chunk* old = DropOldest(false);
        if (!old) break;
        // ⚠ 先取值再交块：`RetirePush` 成功后写线程会立刻把块收回空闲链，之后
        //   `old->lastCycle` 就是别人的数据（数据竞争）。
        const uint64_t oldLast = old->lastCycle;
        if (RetirePush(old)) {
            TraceFileMark(oldLast);          // 真离开了内存，此时才记索引
            continue;
        }
        // 交不出去（写线程起不来 / 队列超时）：`RetirePush` 已报错 + 置位，
        // 块**必须**接回链头 —— 绝不能让它就这么从链上消失。然后停手。
        LinkHead(old);
        break;
    }
}

const unsigned kReplayBuf = 64u * 1024;

// 从记录日志回放 [lo, hi]（P2-2）。`consider` 由调用方给，与内存路径共用同一套
// 命中/过滤/偏移/条数计数（见 QueryRange 里的 lambda）。
//
// ⚠ **必须按索引逐段解**：记录日志是各块字节流的**拼接**，而每块的第一条记录存的
// 是"差 = 0"（它的绝对周期号由索引给出）。线性跨块解会把这条当成"与上一条同周期"，
// 于是每过一个块边界就漂一点 —— 实测 407 个边界累积出 304 条的错位（分四段查询时
// 表现成"四段之和 != 全区间"）。所以段边界上把周期号**重新锚定**到索引里的值，
// 与内存路径"每块从 firstCycle 起步"完全一致。
template <typename Fn>
void ReplayRecordsFromFile(uint64_t lo, uint64_t hi, Fn consider)
{
    uint64_t ofs = 0, cycAt = 0;
    if (!TraceFileLocateRecords(lo, ofs, cycAt)) return;

    static uint8_t buf[kReplayBuf];

    for (;;) {
        // 这一段 = [ofs, nextMark) —— 索引就是在"追加这块之前"记的，所以段边界
        // 天然是记录边界。
        uint64_t nextOfs = 0, nextCyc = 0;
        const bool haveNext = TraceFileNextMark(ofs, nextOfs, nextCyc);

        unsigned have   = 0;
        uint64_t cur    = ofs;
        uint64_t cycle  = cycAt;
        bool     primed = false;
        bool     stop   = false;

        while (!haveNext || cur < nextOfs) {
            unsigned want = kReplayBuf - have;
            if (haveNext && (nextOfs - cur) < want) want = static_cast<unsigned>(nextOfs - cur);
            if (want == 0) break;

            const unsigned n = TraceFileReadRecords(cur, buf + have, want);
            if (n == 0) break;
            cur += n;
            const unsigned total = have + n;

            RecIter it(buf, buf + total, cycle, primed);
            while (it.Next()) {
                if (it.rec.nCycle > hi) { stop = true; break; }   // 过了上界：停
                if (it.rec.nCycle >= lo) consider(it.rec);
            }
            cycle  = it.cycle;
            primed = it.primed;

            if (stop) break;

            // 读段尾部若有一条被切断的记录，搬到下一段前面接着解（否则丢一条并错位）。
            const unsigned rest = it.Left();
            if (rest) memmove(buf, it.p, rest);
            have = rest;

            if (n < want) break;                 // 文件到头了
        }

        if (stop || !haveNext) return;
        ofs   = nextOfs;                          // 下一段：重新锚定
        cycAt = nextCyc;
    }
}

// ---------------------------------------------------------------------------
// 周期轴：来源仲裁 + 记账（P0-F′）
// ---------------------------------------------------------------------------
const char* SrcName(int src)
{
    switch (src) {
    case kCycleSrcWall: return "wall clock";
    case kCycleSrcItm:  return "ITM timestamps";
    case kCycleSrcEtm:  return "ETM cycle count";
    default:            return "none";
    }
}

// ---------------------------------------------------------------------------
// 周期轴的**墙钟兜底**（2026-10-14）
// ---------------------------------------------------------------------------
// ⚠⚠ **先说清楚它不是什么。** 这里原来写着"轴必须按墙钟走，这是 `BOUNDS` 的唯一
//   根因" —— **那个结论是错的**，本轮被官方二进制推翻（Todo §十四）。墙钟只是
//   **给记录打时间戳的兜底**，它**不参与** uVision 的取数区间。
//
// ---- BOUNDS 的真正根因（2026-10-14 逆向 `ULP2CM3.dll` 得到）--------------------
// 官方在 `UV_TRACE_IF_STATUS(0x901)` 里**从不给 `m_pClockSample`**（发送函数
// VA 0x100F2EB0 里它从头到尾没有被赋值，恒 NULL），而是每轮状态都重报
// "我手里只有 [最旧记录, 最新记录] 这一段"：
//     0x100F2F9F  or  dword ptr [ebp-0x60], 8    ; bExplicitRange = 1
//     0x100F2FB7  mov [ebp-0x55], ecx            ; m_pRecordRange = &range
// ⇒ uVision 只在 `[最旧, 最新]` 里取数，**越界从根上不存在**。
//
// 我们原来反着做：`bExplicitRange = 0` **加**一个 `m_pClockSample` 锚点 ⇒ uVision
// 拿锚点当起点、之后**按它自己的墙钟**以 `m_nClock` 外推：
//     cycle(time) = m_nLastCycle + (time - m_dLastTime) × m_nClock
// ⇒ 它请求的位置必然跑到我们轴外 ⇒ `TDRV_ERR_BOUNDS` ⇒ 窗口只有列、没有行。
//
// ⚠ **这解释了为什么"改轴的速率"怎么改都还是 2 倍** —— 它用的根本不是我们的轴。
//   实测（19:00–19:01，14 s 采集）：驱动范围 `[3024001, 330901065]`，它请求
//   `[661368994, ...]`（1.999 倍）；把轴从"记录条数"（约 105 kHz）改成墙钟
//   （48 MHz）之后**仍然**是 2 倍 —— 就是这条证据把方向定死的。
//
// ---- 那这里为什么还留着墙钟 ---------------------------------------------------
// 因为**记录自己要有时间戳**（Time 列、`bTS=1`、区间定位都靠它）。官方那两条真来源
// 在本板都不可用：
//   * ITM 时间戳：SWO 位时钟 = 核心时钟（48000000 Hz），驱动自己就警告过
//     "the TPIU cannot sustain that for NRZ, so expect a garbled ITM stream"，
//     实测载荷报出饱和值 0xFFFFFFFF（被预算闸门拒掉）；
//   * ETM 侧：时间戳包恒 0、周期计数不实现（`ETMCCR bit12 = 0`）。
// 墙钟是唯一"速率正确"的兜底（速率 = 核心时钟，与 uVision 的换算口径一致）。
//
// ⚠⚠ **正解还差一步**：官方轴是**真实周期计数器** —— `DbgCM` 读 `DWT_CYCCNT` 累加
//   （`DbgCM/AGDI.CPP:3565` `UpdateCycles()`），`ULP2CM3` 用 trace 流自己的时间戳
//   累加（全局 0x12bc7ad8 那个 64 位计数器）。本板 `DWT_CYCCNT` 是**开着的**
//   （`kDwtPcSample = 0x00001201` 含 `CYCCNTENA` bit0）却**从没被读过** ⇒ 下一步
//   就该读它（Todo §14.6）。
//
// ⚠ 与"ITM / ETM 两个计数器"的关系：那两个的解析与日志都保留，但**不再推进轴**
//   （见 TraceWinAdvanceCycle / TraceWinEtmTime）—— 它们的值在本板都不可信。
/// 本段采集至今**按墙钟**应该走到的周期号（毫秒 × 核心时钟 kHz）。
/// 核心时钟未知时返回 0（不判、不猜 —— 判据要能解释）。
static uint64_t ElapsedCycles(const Store& s)
{
    const int khz = orb::cfg::Get().cpuKhz;
    if (khz <= 0) return 0;
    // 停止之后墙钟必须冻在 `segStopTick`：否则停止后轴自己继续涨，时间轴越拉越长
    // （Navigation 条），而 uVision 恰恰在停止后才来问 `GetLastCycle`。
    const uint64_t now = s.segStopTick ? s.segStopTick : GetTickCount64();
    if (now <= s.segStartTick) return 0;
    // ms × kHz 就是周期数（kHz × 1000 Hz × ms / 1000 s = kHz × ms）。
    return static_cast<uint64_t>(khz) * (now - s.segStartTick);
}

/// 机制名（日志用）。
static const char* AxisModeName(int m)
{
    return (m == kAxisModeItm) ? "ITM/SWO（轴追墙钟）"
                               : "ETM（轴按解码推进，不追墙钟）";
}

// ⚠ `TraceWinSetAxisMode()` / `TraceWinAxisMode()` 是**对外**的（头文件里声明、
//   `ORBAGDI_Agdi.cpp` 调用），定义**必须放在这个匿名 namespace 外面** —— 放里面
//   会变成内部链接，链接期直接报
//   `error LNK2019: 无法解析的外部符号 "void __cdecl orb::agdi::TraceWinSetAxisMode(int)"`。
//   （本轮真踩过。）它们的定义在下面 `TraceWinSetCoreClock()` 旁边。

/// 把周期轴抬到墙钟位置。**只抬不降**（轴必须单调 —— 区间查询靠它定位）。
///
/// ⚠⚠ **只在 ITM/SWO 机制下生效**（`kAxisModeItm`）。用户裁定：追墙钟是 **ITM** 的
///   机制（数据量小、可以灵活按时间追），而 **ETM 数据量太大**（实测 4.9 M 条/s）
///   追不起 ⇒ ETM 下这个函数**直接返回**，轴只按解码推进（`AppendRec` 的 +1，
///   外加 ETM 自己的周期计数如果可用）。见头文件 `CycleAxisMode`。
///
/// ⚠ 2026-10-14 **这道闸门被撤过一次，已回退**（真机把 uVision 打崩）。撤销之后
///   ETM 现场的两个后果都实测到了：
///     ① 轴被抬到墙钟位置 ⇒ **轴上的周期推进不再等于记录推进**。每 64 条抬一次，
///        平均抬升约 50 个周期（4.9 M 条/s 下），于是轴上每隔 64 条就出现一段
///        **没有任何记录的空洞**。`QueryRange()` 的 `LocateAligned(lo)` 一旦定位到
///        首条记录已 `> hi` 的块就 `break`，`inRange` 保持 false ⇒ 回 `BOUNDS`。
///        uVision 要窄窗口（16 个周期那种）时约一半落进洞里 ⇒ 日志里
///        `[33314144, 33314159]` 落在驱动范围 `[128001, 238393979]` 内却回 BOUNDS。
///     ② 轴被 uVision 当成真实时间轴 ⇒ 它按 2 秒的窗口反复全量取数，而现场有
///        **1.41 亿条 / 5.3 秒**的记录 ⇒ 单次查询回 1 亿条、1453 ms（跑在它的 UI
///        线程上）⇒ uVision 崩。日志 `22:43:31` 之后戛然而止就是那一下。
///   ⇒ 结论：ETM 的轴**必须**保持"每条 +1"的稠密单调性。要时间列有值，得换别的
///     办法（例如读 `DWT_CYCCNT` 让每条记录自己往前走，见 Todo §16.4 第 4 条 / §14.6）。
///
/// ⚠ 它只影响**记录的时间戳**，**不影响** uVision 的取数区间（那个由
///   `StatusUpdate()` 的 `m_pRecordRange` 给死）。别再把"BOUNDS 消失"寄托在这里。
static void SyncAxisToWall(Store& s)
{
    if (s.axisMode != kAxisModeItm) return;     // ETM：不追墙钟
    const uint64_t wall = ElapsedCycles(s);
    if (wall <= s.cycle) return;

    const int prev = s.cycleSrc;
    s.cycle       = wall;
    s.synthCycles = 0;              // 那堆 +1 已被墙钟吞掉，不再参与抵扣

    if (prev != kCycleSrcWall) {
        s.cycleSrc = kCycleSrcWall;
        s.realAxis = true;          // 此后新记录的 bTS=1（Time 列有值）
        // ⚠ 这句里**必须**留着 `bTS=1` 这个字面串：`test/trace_stream.ps1` 的 P0-F
        //   断言就是靠它判断"轴变成真的了"（原来那句在 `AdvanceFrom()` 里，已退役）。
        ORB_LOG_INFO("TraceWin: cycle axis = wall clock（**记录的时间戳**改用墙钟兜底，"
                     "此后新记录 bTS=1。⚠ 这不是 BOUNDS 的修法 —— 那个由 "
                     "StatusUpdate() 的 m_pRecordRange 负责，见那里的说明）。"
                     "此前来源=%s，轴=%llu 周期",
                     SrcName(prev), (unsigned long long)wall);
    }
}

// 轴增量的上限余量：预算 = 墙钟 × 核心时钟，再乘这个倍数。实测那次异常是 2.0e11
// 周期而同一段只有几秒（按 48 MHz 折算预算约 3e8）—— 差三个数量级，4 倍余量拦得住，
// 同时不会误杀"长时间空档之后的一次大增量"（那种增量仍在墙钟预算之内）。
const uint64_t kAxisBudgetSlack = 4;
// 预算的下限（周期）。采集刚开始时墙钟几乎没走，不留这个底会把头几个正常包也拒掉。
// 1e9 周期在任何真实主频下都远大于"一秒之内可能过去的周期数"的下限。
const uint64_t kAxisBudgetFloor = 1000000000ull;

/// 这一段采集至今**最多**可能过去多少个核心周期。
///
/// ms × kHz 就是周期数（kHz × 1000 Hz × ms / 1000 s = kHz × ms）。核心时钟未知时
/// 返回"不设限"：判据要能解释，不能靠猜 —— 不知道主频就不判。
uint64_t AxisBudget(const Store& s)
{
    const int khz = orb::cfg::Get().cpuKhz;
    if (khz <= 0) return ~0ull;

    // +1000 ms 是起步余量：探针建流、目标从复位跑起来都在这段时间里。
    const uint64_t ms = (GetTickCount64() - s.segStartTick) + 1000;
    uint64_t b = static_cast<uint64_t>(khz) * ms;
    if (b < kAxisBudgetFloor) b = kAxisBudgetFloor;
    return b * kAxisBudgetSlack;
}

// 一个真实来源报来 `delta` 个核心周期。
//
// ⚠⚠ **2026-10-14 起已退役，没有调用者** —— 轴目前由**墙钟兜底**拥有
//   （`SyncAxisToWall()` / `kCycleSrcWall`），只用来给记录打时间戳。
//
// ⚠ 但墙钟只是**过渡**：官方轴是**真实周期计数器**（`DbgCM` 读 `DWT_CYCCNT` 累加，
//   `ULP2CM3` 用 trace 时间戳累加），Todo §14.6 就是要把本板那个开着却从没读过的
//   `DWT_CYCCNT` 读起来 —— **接回来的时候用的就是本函数**（它的仲裁与预算规则
//   仍然有效）。下面"三条规则"是退役前的语义。
//
// 三条规则：
//   ① 轴的所有者：先到者拥有。ETM 周期计数是周期精确的，它一出现就**接管**
//      （ITM 时间戳之后只被忽略）—— 两个计数器同时接进一条轴是重复计数。
//   ② 记账：这段区间里每条记录已经自己推进了 +1（Append），真实增量要减掉
//      它们。不减的话，3 秒 1 千多万条 ETM 记录会把时间轴整体拉长 8%~17%。
//      真实分辨率比记录速率还细时（`delta <= synth`）增量为 0 —— 那时轴已经
//      被记录推得够远，不补才对。
//   ③ `realAxis` 在**认领那一刻**置起，而不是等第一次非零增量：认领本身就
//      意味着"此后轴由真实计数器锚定"，记录的 `bTS` 该置 1 了。
void AdvanceFrom(int src, uint64_t delta)
{
    if (delta == 0) return;

    Store& s = S();

    if (s.cycleSrc == kCycleSrcNone) {
        s.cycleSrc = src;
        s.realAxis = true;
        ORB_LOG_INFO("TraceWin: cycle axis source = %s（周期轴来源；此后新记录 bTS=1）",
                     SrcName(src));
    } else if (s.cycleSrc != src) {
        if (src == kCycleSrcEtm) {
            s.cycleSrc = src;
            s.realAxis = true;
            ORB_LOG_INFO("TraceWin: cycle axis -> ETM cycle count（周期精确，接管 "
                         "ITM 时间戳；后者从此只被忽略）");
        } else {
            if (!s.itmIgnored) {
                s.itmIgnored = true;
                ORB_LOG_INFO("TraceWin: cycle axis: ITM timestamps ignored（周期轴已由 "
                             "ETM 周期计数拥有 —— 两个计数器同时接会重复计数）");
            }
            return;
        }
    }

    // ---- 合理性：不可能的大增量一律拒绝（2026-10-14）----------------------
    //
    // 轴的单位是核心周期，而这一段采集只持续了几秒 ⇒ 它有物理上限：这一段至今
    // 最多只可能过去 `墙钟 × 核心时钟` 个周期（AxisBudget）。解错的流（TPIU 错位、
    // 或解码器吐出的坏值）会报出 1e10~1e11 量级的"绝对时间"，加进去就把轴**永久**
    // 推到一个荒谬的位置 —— uVision 的时间列与 Navigation 条跟着荒谬
    // （Todo 的"周期轴偶发跳到 1e10"就是它）。
    //
    // ⚠ **两个都要卡**：只看 `adv` 不够。一串"各自勉强合格"的坏增量累起来同样能
    //   把轴推走（每条都小于当时的预算，和却大得多），所以还要卡**加完之后的位置**。
    //
    // **拒绝而不是夹取**：夹到上限等于凭空编一段时间，而拒绝只是"这一段轴没走" ——
    // 真实增量下一秒就会到（ETM 那条的基准已在调用方更新过，下一包从新值起算）。
    // 拒绝时**不动** `synthCycles`：那些 +1 是记录自己推的，下一条真增量还要拿它抵扣。
    //
    // ⚠ 这是**围堵**不是根因修复：根因在喂进来的流（`trwidth` 与固件不同档时约
    //   80% 的载荷是错位的，见 Todo 的 tag 那条）。围堵要做的只是别让垃圾值把
    //   整段采集的时间轴毁掉，并且**报出来**让人能去查根因。
    const uint64_t synth  = s.synthCycles;
    const uint64_t adv    = (delta > synth) ? (delta - synth) : 0;
    const uint64_t budget = AxisBudget(s);
    if (adv > budget || s.cycle > (budget - adv)) {
        ++s.axisRejects;
        if (!s.axisWarned) {
            s.axisWarned = true;
            ORB_LOG_WARN("TraceWin: 周期轴拒绝一个不可能的值 —— %s 报增量 %llu 周期，"
                         "加完是 %llu，而这一段采集至今最多只可能过去 %llu 周期"
                         "（按 cpu=%d kHz 与墙钟算）。这是**解码出来的垃圾值**，不是时间，"
                         "轴不动。先查流有没有错位（探针 trwidth 与固件同档？）",
                         SrcName(src), (unsigned long long)adv,
                         (unsigned long long)(s.cycle + adv),
                         (unsigned long long)budget, orb::cfg::Get().cpuKhz);
        } else if ((s.axisRejects & 0xFFu) == 0) {
            ORB_LOG_INFO("TraceWin: 周期轴已拒绝 %llu 个不可能值（最新增量 %llu，"
                         "轴在 %llu，预算 %llu）", (unsigned long long)s.axisRejects,
                         (unsigned long long)adv, (unsigned long long)s.cycle,
                         (unsigned long long)budget);
        }
        return;
    }

    s.synthCycles = 0;

    if (adv == 0) return;                       // 规则 ②：记录已经推够了

    s.cycle += adv;
    if (src == kCycleSrcEtm) s.etmCycles += adv;
    else                     s.itmCycles += adv;
}

// ---------------------------------------------------------------------------
// 窗口描述（照搬 DbgCM/TraceWinConnect.cpp 的列 / 过滤器 / 异常表）
// ---------------------------------------------------------------------------

// 记录类型名：按**掩码位序号**索引（TR_TYPE_ETM=0x01 是 bit0）。
static LPCTSTR s_typeNames[8] = {
    TRCSTR("ETM"),           // bit0  TR_TYPE_ETM
    TRCSTR("ITM"),           // bit1  TR_TYPE_ITM
    TRCSTR("Counter Event"), // bit2  TR_TYPE_EVT
    TRCSTR("Exception"),     // bit3  TR_TYPE_EXCTRC
    TRCSTR("PC Sample"),     // bit4  TR_TYPE_PC_SAMPLE
    TRCSTR("Data Read"),     // bit5  TR_TYPE_DATA_READ
    TRCSTR("Data Write"),    // bit6  TR_TYPE_DATA_WRITE
    TRCSTR("ETM"),           // bit7  TR_TYPE_ETM_HLL
};

static LPCTSTR s_colNames[TRACE_COL_MAX] = {
    TRCSTR(/*Unused*/ ""),             // TRACE_COL_INDEX
    TRCSTR("Time"),                    // TRACE_COL_TIME
    // 2026-10-14 曾打开成 "Cycles"（显示 nCycle 原值），随 bTS 那次回退一起关掉：
    // 列的名字会换掉 uVision 的列集，而这个现场本来就因为 1.4 亿条记录在崩，先把
    // 窗口的变量减到最少。要周期数请走 §10.20.1 的分析，别在这里试。
    TRCSTR(/*Unused*/ ""),             // TRACE_COL_CYCLES
    TRCSTR("Address / Port"),          // TRACE_COL_ADDR_PORT
    TRCSTR(/*Unused*/ ""),             // TRACE_COL_OPCODE
    TRCSTR("Instruction / Data"),      // TRACE_COL_INSTR_DATA
    TRCSTR("Src Code / Trigger Addr"), // TRACE_COL_SRC_TRGADDR
    TRCSTR("Function"),                // TRACE_COL_FUNCTION
};

static INT32 s_colWidth[TRACE_COL_MAX] = {
    75,  // Index
    135, // Time
    130, // Cycles
    95,  // Address
    70,  // Opcode
    230, // Instruction
    200, // src code
    150, // function
};

const uint32_t kColBitsSupported =
    TRACE_COL_BIT_TIME | TRACE_COL_BIT_ADDR_PORT | TRACE_COL_BIT_INSTR_DATA |
    TRACE_COL_BIT_SRC_TRGADDR | TRACE_COL_BIT_FUNCTION;

// name, type mask, column mask, extended type mask
static TRACE_DISPLAY_FILTER s_filters[] = {
    { TRCSTR("All"),                    0xFF, kColBitsSupported, 0x1 },
    { TRCSTR("ETM - Code Exec"),        (TR_TYPE_ETM | TR_TYPE_ETM_HLL), kColBitsSupported, 0x1 },
    { TRCSTR("ETM - Code Exec HLL"),    TR_TYPE_ETM_HLL, kColBitsSupported, 0x1 },
    { TRCSTR("ITM - All"),
      (TR_TYPE_ITM | TR_TYPE_EVT | TR_TYPE_EXCTRC | TR_TYPE_PC_SAMPLE |
       TR_TYPE_DATA_READ | TR_TYPE_DATA_WRITE), kColBitsSupported, 0x0 },
    { TRCSTR("ITM - SW Stimuli"),       TR_TYPE_ITM, kColBitsSupported, 0x0 },
    { TRCSTR("ITM - Event Counters"),   TR_TYPE_EVT, kColBitsSupported, 0x0 },
    { TRCSTR("ITM - Exceptions"),       TR_TYPE_EXCTRC, kColBitsSupported, 0x0 },
    { TRCSTR("ITM - PC Samples"),       TR_TYPE_PC_SAMPLE, kColBitsSupported, 0x0 },
    { TRCSTR("ITM - Data Read"),        TR_TYPE_DATA_READ, kColBitsSupported, 0x0 },
    { TRCSTR("ITM - Data Write"),       TR_TYPE_DATA_WRITE, kColBitsSupported, 0x0 },
    { TRCSTR("ITM - Data Read/Write"),  (TR_TYPE_DATA_READ | TR_TYPE_DATA_WRITE), kColBitsSupported, 0x0 },
};
const UINT32 kFilterCount = (UINT32)(sizeof(s_filters) / sizeof(s_filters[0]));

static LPCTSTR s_vecNames[16] = {
    TRCSTR(""), TRCSTR("Reset"), TRCSTR("NMI"), TRCSTR("HardFault"),
    TRCSTR("MemManage"), TRCSTR("BusFault"), TRCSTR("UsageFault"),
    TRCSTR(""), TRCSTR(""), TRCSTR(""), TRCSTR(""),
    TRCSTR("SVCall"), TRCSTR("DbgMon"), TRCSTR(""),
    TRCSTR("PendSV"), TRCSTR("SysTick"),
};

// ---------------------------------------------------------------------------
// 状态通知
// ---------------------------------------------------------------------------
// Trace 周期号的时钟（Hz）：uVision 经 `AG_TRACESETUP_REQ` 的 `TRACE_CONFIG`
// 交下来，TDI 的 `Set/GetTraceConfig` 用同一个值，`TraceWinSetCoreClock()` 也能改。
// 定义放这里是因为 `StatusUpdate()` 的时间锚点要用它。
static uint32_t g_coreClkHz = 0;

// 时间锚点（`TRACE_CLOCK_SAMPLE`）的持久存储。`StatusUpdate()` 与
// `TraceWinShowRange()` 共用一份 —— 见下面各自的说明：**uVision 在 Notify 返回后
// 还会读它**，所以不能是栈上的局部变量。
static TRACE_CLOCK_SAMPLE g_clockSample;

// 记录区间（`TRACE_RECORD_RANGE`）的持久存储。**和上面那个锚点同一个坑**：uVision
// 在 `Notify` 返回之后还会读它，指向栈上的局部变量 ⇒ 它拿到垃圾区间 ⇒ 请求一个
// 根本不存在的区间 ⇒ 驱动回 `BOUNDS`（2026-10-13 真机实测过这一条）。
static TRACE_RECORD_RANGE g_recRange;

void StatusUpdate(bool processing, bool updateView)
{
    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    Store& s = S();

    // ⚠ 与 `TraceWinShowRange()` 同一个坑：`st` 必须是**持久存储** ——
    //   uVision 的 `Notify` 返回之后还会读它（`m_pClockSample` 就指向文件级的
    //   `g_clockSample`）。放栈上 ⇒ 一出作用域即失效 ⇒ uVision 拿到垃圾时间锚点。
    //   `memset` 会把 `m_pClockSample` 清成 NULL，所以不需要在 else 分支里显式清。
    static TRACE_STATUS s_st;

    TRACE_STATUS& st = s_st;
    memset(&st, 0, sizeof(st));
    st.bProcessing     = processing ? 1u : 0u;
    // 默认隐藏（=1）：这个字段同时是取数门的一环，见 Store::showTs 的说明。
    st.bHideTimestamps = s.showTs ? 0u : 1u;
    st.bSkipViewUpdate = updateView ? 0u : 1u;
    // ---- 区间：**直接告诉 uVision 我们手里有什么**（官方做法，2026-10-14）--------
    //
    // 反编译官方 `ULP2CM3.dll` 得到的原文（`UV_TRACE_IF_STATUS` 的发送函数
    // VA 0x100F2EB0）：
    //     0x100F2F9F  or  dword ptr [ebp-0x60], 8    ; bExplicitRange = 1
    //     0x100F2FB7  mov [ebp-0x55], ecx            ; m_pRecordRange = &range
    //     0x100F2FA3  mov [ebp-0x34], eax            ; nFirstCycle ← 0x12bda058（最旧记录）
    //     0x100F2FB1  mov [ebp-0x2c], edx            ; nLastCycle  ← 0x12bda060（最新记录）
    // 而 **`m_pClockSample` 在那个函数里从头到尾没有被赋值，一直是 NULL** ——
    // 官方**不让 uVision 去外推时间轴**，而是每次状态更新都报"我手里只有
    // [最旧记录, 最新记录] 这一段"。那两个 cycle 值就是它环形缓冲的真实边界
    // （`GetFirstCycle` 0x100F20A0 / `GetLastCycle` 0x100F2100 返回同一对）。
    //
    // ⚠ 这正是我们一直搞错的地方。原来这里是 `bExplicitRange = 0` **加上**一个
    //   `m_pClockSample` 锚点 ⇒ uVision 拿它当起点、之后**按它自己的墙钟**外推：
    //       cycle(time) = m_nLastCycle + (time - m_dLastTime) × m_nClock
    //   ⇒ 它请求的位置一路跑在驱动能给的区间之外 ⇒ `TDRV_ERR_BOUNDS` ⇒
    //     窗口只有列、没有行。
    //   2026-10-14 实测（F407，14 s 采集）：驱动范围 `[3024001, 330901065]`，
    //   它请求 `[661368994, ...]` —— **正好 2 倍**，而且改快改慢都是 2 倍
    //   （因为它用的根本不是我们的轴，是它自己的墙钟）。
    //
    // ⇒ 改成官方做法：**只报区间，不挂锚点**。uVision 只会在 [最旧, 最新] 里取数，
    //   越界从根上不存在。`TraceWinShowRange()` 早就是这么做（`bExplicitRange=1`
    //   + `m_pRecordRange`），只有这条**主**路径漏了。
    st.bExplicitRange = 1;
    memset(&g_recRange, 0, sizeof(g_recRange));
    if (s.count && s.head && s.tail) {
        // ★ 左端统一走 FirstReportedCycle()：它内部先过 FirstReadable（`s.head` 可能是
        //   空块：刚挂上 / 被裁空），再退回**本段起点**。三个出口（这里 / TDI
        //   `GetFirstCycle` / `TraceWinFirstCycle`）**必须同一个值**，理由见那里的说明。
        //   右端仍是"最新的那条记录"。
        g_recRange.nFirstCycle = FirstReportedCycle(s);
        g_recRange.nLastCycle  = s.tail->lastCycle;
        st.m_pRecordRange      = &g_recRange;
    }
    // ⚠ `m_pClockSample` **保持 NULL**（上面 `memset` 已经清过）—— 这是官方行为，
    //   不是遗漏。别"顺手"再挂回去：挂上就等于把时间轴交给 uVision 自己外推。
    //   时间列走 `TRACE_CLK` VTR：`time = cycle / TRACE_CLK`（官方模型）。

    // uVision 认这条通道是确定的（它收到 UV_TRACE_IF_INIT 之后会来调 ClearTrace），
    // 但"上报了却不去刷新窗口"和"根本没上报"在窗口上都是空白，只能靠日志分开。
    const DWORD rc = bom->Notify(UV_TRACE_IF_STATUS, &st);
    ORB_LOG_INFO("TraceWin: 上报状态 processing=%u skipViewUpdate=%u "
                 "hideTS=%u 真实周期轴=%u 机制=%s 来源=%s 时钟=%u Hz 区间=%s%llu..%llu"
                 " -> rc=%u",
                 st.bProcessing, st.bSkipViewUpdate, st.bHideTimestamps,
                 s.realAxis ? 1u : 0u, AxisModeName(s.axisMode), SrcName(s.cycleSrc),
                 g_coreClkHz,
                 st.m_pRecordRange ? "" : "无",
                 (unsigned long long)(st.m_pRecordRange ? g_recRange.nFirstCycle : 0),
                 (unsigned long long)(st.m_pRecordRange ? g_recRange.nLastCycle : 0),
                 (unsigned)rc);
}

void Configure(struct bom* bom)
{
    TRACE_REC_TYPE_NAMES names;
    memset(&names, 0, sizeof(names));
    names.m_nSize  = 8;
    names.m_pNames = s_typeNames;

    TRACE_COLUMN_INFO cols;
    memset(&cols, 0, sizeof(cols));
    cols.m_nColumns        = TRACE_COL_MAX;
    cols.m_pColumnNames    = s_colNames;
    cols.m_pColumnWidthDef = s_colWidth;

    TRACE_DISPLAY_FILTER_CONFIG filt;
    memset(&filt, 0, sizeof(filt));
    filt.m_nFilters = kFilterCount;
    filt.m_pFilters = s_filters;

    TRACE_EXCEPTION_CONFIG exc;
    memset(&exc, 0, sizeof(exc));
    exc.m_ExceptionMasks.m_nExcNumPos   = 0;
    exc.m_ExceptionMasks.m_nExcNumMask  = TR_EXC_NUM_MSK;
    exc.m_ExceptionMasks.m_nExcTypePos  = TR_EXC_TYP_POS;
    exc.m_ExceptionMasks.m_nExcTypeMask = TR_EXC_TYP_MSK;
    exc.m_nFixedVectors                 = 16;
    exc.m_nFixedVectorOffset            = 0;
    exc.m_pFixedVectorNames             = s_vecNames;
    exc.m_nGenericVectorOffset          = 16;
    exc.m_pGenericeVectorPrefix         = TRCSTR("ExtIRQ ");

    TRACE_WIN_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.m_pTraceRecTypeNames    = &names;
    cfg.m_pTraceColumnInfo      = &cols;
    cfg.m_pDisplayFiltConf      = &filt;
    cfg.m_pTraceExceptionConfig = &exc;
    cfg.m_bSuppressAddrType     = 0;

    // uVision 在这次调用里把内容拷走（参考驱动同样用局部变量）。
    const DWORD rc = bom->Notify(UV_TRACE_WIN_CONFIG, &cfg);
    ORB_LOG_INFO("TraceWin: 下发窗口配置 -> rc=%u", (unsigned)rc);
}

// ---------------------------------------------------------------------------
// 过滤器匹配
// ---------------------------------------------------------------------------
// TR_TYPE_xxx 是**位掩码**而不是位序号：ETM=0x01、ITM=0x02、EVT=0x04 …
// 流控记录（TR_TYPE_FLOW = 0x00）没有位，只能用扩展掩码判。
bool Matches(const TRACE_RECORD_64& r, const TRACE_FILTER_CONF* f)
{
    if (!f) return true;

    if (r.nType == TR_TYPE_FLOW) {
        if (f->nExtTypeMask == 0) return true;
        return (f->nExtTypeMask & TR_TYPE_EXT_FLOW) != 0;
    }

    if (f->nTypeMask == 0) return true;
    return (f->nTypeMask & r.nType) != 0;
}

// uVision 的 TDI 回调一个都不被调用时，窗口就是空的，而日志上什么都看不到 ——
// "回调没被调" 和 "回调被调但返回空" 在窗口上长得一模一样，只能靠日志分开。
// 每个槽位单独计数，否则第一个回调就把额度用光，后面的看不见。
//   0 = 取记录（GetTraceRecords64 / GetFiltTraceRecords64）
//   1 = GetFirstCycle   2 = GetLastCycle
//   3 = ClearTrace      4 = UpdateTraceBuffer
void LogTdi(int slot, const char* what)
{
    static int n[8] = {0};
    if (slot < 0 || slot >= 8) return;
    // 槽 0 是"取记录"，是判断 uVision 有没有真正来取数的唯一信号，额度给大些；
    // 其余槽位只在最开始几条有意义，4 条够用。
    const int quota = (slot == 0) ? 64 : 4;
    if (n[slot] >= quota) return;
    ++n[slot];
    ORB_LOG_INFO("TraceWin: [%d] %s", slot, what);
}

void LogQuery(const char* what)
{
    LogTdi(0, what);
}

// ---------------------------------------------------------------------------
// 查询加速（P4-1 / P4-2）：把 uVision 的 nRecordOfs 翻页从 O(区间) 降到 O(返回条数)
// ---------------------------------------------------------------------------
// 现场（2026-10-14 15:20 日志）：uVision 固定一个周期区间，按记录序号一页 4096 条地
// 取完它（同一个区间连打几十次，命中数每次正好少 4096）。原来每页都从区间头重新走，
// 实测 **424 次查询解了 11.36 亿条记录**，只为交付 174 万条（放大 650×），
// 35.7 s 全花在 uVision 的 UI 线程上 ⇒ 界面无响应。
//
// 官方 ULP2CM3.dll 的做法（逆向：`0x100F2810` / `0x100F21A0` / `0x100F1B60`）：
// 按**记录序号**建一张 1M 槽的环形索引（`and 0xfffff`），周期→序号用**二分**
// （`mid = lo + (hi-lo)/2`），序号→位置**直接寻址**。所以它的 `nRecordOfs` 是 O(1)。
//
// 这里不建新索引，而是给**已有的检查点**补一个全局序号字段（`Chunk::ckptRec`）：
//   ① 序号直查：过滤器全通过时，"区间内第 k 条命中" = "第 k 条记录" ⇒
//      `skip` 直接加在全局序号上，二分跳到目标附近（残余 ≤ 1/256 块 ≈ 3300 条）。
//   ② 游标续扫：过滤器不是全通过时（或跳不准时），记住上次停在哪一条，下次接着走。
bool FilterPassAll(const TRACE_FILTER_CONF* f)
{
    if (!f) return true;
    // 八个类型位全在（`TR_TYPE_FLOW` = 0x00 没有位，只能靠扩展掩码判）。
    const UINT32 allTypes = TR_TYPE_ETM | TR_TYPE_ITM | TR_TYPE_EVT | TR_TYPE_EXCTRC |
                            TR_TYPE_PC_SAMPLE | TR_TYPE_DATA_READ | TR_TYPE_DATA_WRITE |
                            TR_TYPE_ETM_HLL;
    if ((f->nTypeMask & allTypes) != allTypes) return false;
    // `Matches()`：FLOW 记录在 nExtTypeMask == 0 时直接通过，否则要看 EXT_FLOW 位。
    if (f->nExtTypeMask == 0) return true;
    return (f->nExtTypeMask & TR_TYPE_EXT_FLOW) != 0;
}

// 过滤器指纹（游标续扫的前提：过滤条件必须**一模一样**）。
void FilterFingerprint(Store::QueryCursor& q, const TRACE_FILTER_CONF* f)
{
    q.fNull = (f == nullptr);
    q.fType = f ? f->nTypeMask    : 0u;
    q.fExt  = f ? f->nExtTypeMask : 0u;
}

bool FilterSame(const Store::QueryCursor& q, const TRACE_FILTER_CONF* f)
{
    if (q.fNull) return (f == nullptr);
    if (!f)      return false;
    return q.fType == f->nTypeMask && q.fExt == f->nExtTypeMask;
}

// 定位"内存段里第一条周期号 >= lo 的记录"。
// 返回 false = 内存段里没有这样的记录（lo 晚于所有记录）。
// 输出：块下标、块内字节偏移、该处周期号、该记录的**全局序号**。
bool LocateAligned(uint64_t lo, unsigned& kOut, unsigned& ofsOut,
                   uint64_t& cycleOut, uint64_t& seqOut)
{
    Store& s = S();

    unsigned a = 0, b = s.idxCount;
    while (a < b) {
        const unsigned mid = a + ((b - a) >> 1);
        if (s.idx[mid]->lastCycle < lo) a = mid + 1; else b = mid;
    }

    for (unsigned k = a; k < s.idxCount; ++k) {
        const Chunk* c = s.idx[k];
        if (c->nRecs == 0) continue;

        // 块内检查点二分（与主循环同规则："周期号 <= lo 的最后一条"）。
        unsigned from  = c->headOfs;                  // ★ 不是 0：前缀已被裁掉
        uint64_t cycAt = c->headFirstCycle;
        uint64_t recAt = c->headRecSeq;
        {
            unsigned ca = 0, cb = c->nCkpt;
            while (ca < cb) {
                const unsigned mid = ca + ((cb - ca) >> 1);
                if (c->ckptCyc[mid] <= lo) ca = mid + 1; else cb = mid;
            }
            if (ca > 0) {
                from  = c->ckptOfs[ca - 1];
                cycAt = c->ckptCyc[ca - 1];
                recAt = c->ckptRec[ca - 1];
            }
            // ★ 选中的检查点可能落在裁剪游标**之前**（它对应的记录已被裁掉）⇒ 夹回来。
            ClampToHead(*c, from, cycAt, &recAt);
        }

        uint64_t seq = recAt;                 // 该偏移处那条记录的全局序号
        for (RecIter it(c->buf + from, c->buf + c->usedBytes, cycAt); it.Next(); ++seq) {
            if (it.rec.nCycle >= lo) {
                kOut     = k;
                ofsOut   = static_cast<unsigned>(it.recStart - c->buf);
                cycleOut = it.rec.nCycle;
                seqOut   = seq;
                return true;
            }
        }
    }
    return false;
}

// 按**全局序号**定位到"不晚于 seq 的最后一个检查点"。
// 返回 false = seq 还没到链上任何检查点（调用方应当从区间头顺序走）。
// 输出位置处那条记录的序号是 `seqOut`，调用方从那里**向前**走 `seq - seqOut` 条即可。
bool LocateSeq(uint64_t seq, unsigned& kOut, unsigned& ofsOut,
               uint64_t& cycleOut, uint64_t& seqOut)
{
    Store& s = S();

    // 块按 firstRecSeq 升序（链 = 时间序 = 序号序）⇒ 二分找"firstRecSeq <= seq 的最后一块"。
    unsigned a = 0, b = s.idxCount;
    while (a < b) {
        const unsigned mid = a + ((b - a) >> 1);
        if (s.idx[mid]->firstRecSeq <= seq) a = mid + 1; else b = mid;
    }
    if (a == 0) return false;                 // 连第一块都比 seq 晚

    const unsigned k = a - 1;
    const Chunk*   c = s.idx[k];
    if (c->nRecs == 0) return false;

    unsigned from  = c->headOfs;                  // ★ 不是 0：前缀已被裁掉
    uint64_t cycAt = c->headFirstCycle;
    uint64_t recAt = c->headRecSeq;
    {
        unsigned ca = 0, cb = c->nCkpt;
        while (ca < cb) {
            const unsigned mid = ca + ((cb - ca) >> 1);
            if (c->ckptRec[mid] <= seq) ca = mid + 1; else cb = mid;
        }
        if (ca > 0) {
            from  = c->ckptOfs[ca - 1];
            cycAt = c->ckptCyc[ca - 1];
            recAt = c->ckptRec[ca - 1];
        }
        // ★ 选中的检查点可能已被裁掉 ⇒ 夹回来（连同 recAt，见 ClampToHead 注释）。
        ClampToHead(*c, from, cycAt, &recAt);
    }

    kOut = k; ofsOut = from; cycleOut = cycAt; seqOut = recAt;
    return true;
}

// GetTraceRecords64 / GetFiltTraceRecords64 的共同实现。
//   nRecordOfs != nullptr 时先按记录序号跳过前若干条命中；
//   filter     != nullptr 时按掩码过滤。
INT32 QueryRange(UINT64* nFirstCycle, UINT64* nLastCycle, UINT64* nCount,
                 const UINT64* nRecordOfs, const TRACE_FILTER_CONF* filter,
                 TRACE_RECORD_64* out)
{
    if (!TraceWinReady())    { LogQuery("NOT_SUPPORTED (接口未注册)"); return TDI_ERR_NOT_SUPPORTED; }
    // 采集进行中**照样给数**。本驱动是流式源（bUnlimitedTrace=1），uVision 在
    // 目标运行期间来取数是这种模式的正常用法；照 TDI 文档回 TDI_ERR_PROCESSING
    // 只会把一个机会直接丢掉。缓冲是环形、读操作本身安全，最多读到"此刻为止"的
    // 部分数据，正是无限 trace 视图想要的语义。
    if (TraceWinIsRunning()) LogQuery("采集进行中，照常返回当前已有记录");
    if (!nFirstCycle || !nLastCycle || !nCount) return TDI_ERR_INVALID_PARAM;
    if (out && *nCount == 0) return TDI_ERR_INVALID_PARAM;   // 有缓冲却没有容量

    Store& s = S();

    // 块链的结构（idx / head / tail）在遍历期间不能变 —— 摘块会 memmove 索引
    // 数组，读到一半就是野指针。逐条追加**不取**这把锁，所以这里只挡住"换块"
    // （每块一次：190,650 条记录才一次），不挡数据流。
    StoreGuard guard;

    // 计时。**这条函数跑在 uVision 的 UI 线程上** —— 它慢多少，界面就卡多少。
    // 拖动时间轴会连打几十次查询，所以这里必须有可量化的证据（见收尾处的 WARN）。
    const uint64_t t0 = GetTickCount64();

    // 内存为空**不再直接判 EMPTY**：记录日志（P2-2）里可能还有整段历史。
    // `ReplayRecordsFromFile` 会自己判断"文件里有没有这一段"。
    const uint64_t lo    = *nFirstCycle;
    const uint64_t hi    = *nLastCycle;
    const uint64_t cap   = *nCount;                              // 0 = 只要数量
    const uint64_t limit = (cap == 0) ? (uint64_t)-1 : cap;
    uint64_t       skip  = nRecordOfs ? *nRecordOfs : 0;

    // ---- 只问条数：**在索引上数，不解整段**（P7-2，2026-10-14 崩机现场）--------
    //
    // 契约（`TraceDataIf.h:193-198`）：`pTrace64 == NULL && *nCount == 0` 要的是
    // "区间内**全部**记录的条数"，uVision 拿它去**分配缓冲**。原实现逐条走一遍
    // 把它算出来 ⇒ 1.04 亿条就是 1.04 亿次解码、**1453 ms 卡在 uVision 的 UI 线程
    // 上**，然后它拿着一亿这个数去要 2.3 GB（22 B × 1.04 亿）。
    //
    // 但条数在只用到**块索引 + 检查点**时是可以直接数出来的：
    //   · 整块落在 [lo, hi] 里 ⇒ 加 `c->nRecs`，一条记录都不用解；
    //   · 只有**两个边界块**要解，而且起点从"周期号 <= lo 的最后一个检查点"起
    //     （1/256 块 ≈ 2300 条，与一次正常窄查询同量级）。
    //   ⇒ 从 O(区间内全部记录) 降到 O(log 块数 + 1 个块)。
    //
    // 只在这个模式下开快路，且**过滤器必须全通过**：带过滤器时"第 k 条命中"要逐条
    // 比对，数不出来，那就仍走下面那条慢路（那种调用 uVision 很少发）。
    //
    // 区间**落在记录日志那一段**时也不开快路：文件里的条数只能扫文件。
    if (!out && cap == 0 && skip == 0 && FilterPassAll(filter) && s.count && s.head) {
        const Chunk*   rdHead0 = FirstReadable(s);
        const uint64_t first0 = rdHead0 ? rdHead0->headFirstCycle : 0;
        const uint64_t last0  = s.tail->lastCycle;
        if (lo >= first0 && lo <= last0) {
            uint64_t n = 0, fh = 0, lh = 0;
            bool     any = false;

            for (unsigned k = 0; k < s.idxCount; ++k) {
                const Chunk* c = s.idx[k];
                if (c->nRecs == 0)     continue;
                if (c->lastCycle < lo) continue;      // 整块在区间之前
                if (c->firstCycle > hi) break;        // 后面的块只会更晚（按 lastCycle 排序）

                // 整块在区间内：`nRecs` 就是条数，`firstCycle`/`lastCycle` 就是首末
                // 那条记录的周期号（块头维护的，见 `AppendRec`）。
                if (c->firstCycle >= lo && c->lastCycle <= hi) {
                    n += c->nRecs;
                    // ★ 报给 UV4 的"区间首周期号"必须是**可读**首条，不是被裁掉的那条
                    if (!any) { fh = c->headFirstCycle; any = true; }
                    lh = c->lastCycle;
                    continue;
                }

                // 边界块：解。起点 = 周期号 <= lo 的最后一个检查点。
                unsigned from  = c->headOfs;              // ★ 不是 0：前缀已被裁掉
                uint64_t cycAt = c->headFirstCycle;
                if (c->headFirstCycle < lo) {
                    unsigned a = 0, b = c->nCkpt;
                    while (a < b) {
                        const unsigned mid = a + ((b - a) >> 1);
                        if (c->ckptCyc[mid] <= lo) a = mid + 1; else b = mid;
                    }
                    if (a > 0) { from = c->ckptOfs[a - 1]; cycAt = c->ckptCyc[a - 1]; }
                }
                // ★ 检查点可能落在裁剪游标之前 ⇒ 夹（三者都是记录边界，取 max 仍安全）。
                ClampToHead(*c, from, cycAt, nullptr);
                for (RecIter it(c->buf + from, c->buf + c->usedBytes, cycAt); it.Next(); ) {
                    const uint64_t cy = it.rec.nCycle;
                    if (cy < lo) continue;
                    if (cy > hi) break;
                    ++n;
                    if (!any) { fh = cy; any = true; }
                    lh = cy;
                }
            }

            // 区间在缓冲跨度内却一条都没有（轴上的空洞）⇒ 契约上是 `EMPTY`。
            // ⚠ 慢路在这里回的是 `BOUNDS`（`consider` 一次没进 ⇒ `inRange` 为假），
            //   而真机日志里那一串"落在范围内却回 BOUNDS"就是它。快路按契约回 EMPTY。
            if (n == 0) {
                *nCount = 0;
                LogQuery("EMPTY (只问条数：索引快路，区间内无记录)");
                return TDI_ERR_EMPTY;
            }

            *nFirstCycle = fh;
            *nLastCycle  = lh;
            *nCount      = n;

            // 证明快路真的生效（限流：uVision 每 0.6 s 会来问一次）。
            static unsigned s_fastShown = 0;
            if (s_fastShown++ % 16 == 0) {
                ORB_LOG_INFO("TraceWin: [0] 只问条数 —— 索引快路 %u ms，区间 [%llu, %llu] "
                             "⇒ %llu 条（块索引 %u 块，内存 %llu 条；同一条数走老路要"
                             "逐条解整段）",
                             (unsigned)(GetTickCount64() - t0),
                             (unsigned long long)lo, (unsigned long long)hi,
                             (unsigned long long)n, s.idxCount,
                             (unsigned long long)s.count);
            }
            LogQuery("OK (只问条数，索引快路)");
            return TDI_ERR_SUCCESS;
        }
    }

    uint64_t found    = 0;      // 本次返回的条数（日志/收尾的 *nCount 用）
    uint64_t firstHit = 0;
    uint64_t lastHit  = 0;
    bool     haveHit  = false;
    bool     inRange  = false;  // 区间内至少有一条记录（不论是否被过滤掉）

    // 本次要跳过的命中数（**绝对值**：区间内第几条起开始交付）。
    // ⓪ 游标续扫 / ① 序号直查 会配合它把起点挪到目标附近。
    const uint64_t skip0 = skip;
    // 区间内**已经匹配**的条数（含被跳过的）。它是**绝对计数**，所以无论从哪儿
    // 起步都要先把它补到"起点之前已匹配了多少条"—— 游标记的就是这个数（P4-2）。
    uint64_t matched = 0;

    // uVision 给的缓冲装满了。**要能中断整条查询** —— 只是从 lambda 里 return
    // 不够（外层还会把剩下的块全解一遍），见下面的 `overflow` 判断。
    bool overflow = false;

    // 一条命中的记录该做什么 —— **内存路径与文件回放共用这一段**，否则两条路的
    // 过滤/偏移/条数语义迟早会走偏。
    auto consider = [&](const TRACE_RECORD_64& r) {
        inRange = true;
        if (!Matches(r, filter)) return;
        ++matched;
        if (matched <= skip0) return;              // 还在"跳过"的那一段里
        const uint64_t nth = matched - skip0;      // 本次结果里的第几条（1 起）
        if (nth > limit) { overflow = true; return; }
        if (!haveHit) { firstHit = r.nCycle; haveHit = true; }
        lastHit = r.nCycle;
        if (out) out[nth - 1] = r;
        found = nth;
    };

    // 记录来源计数（只用于日志）。真 uVision 里"拖 Navigation 条回到内存窗口之前"
    // 是否真的落到记录日志上，从这里唯一能看出来 —— 否则 P2-2 只有假 BOM 宿主
    // 那一条证据（§0.3）。包一层，不动 consider 的语义。
    uint64_t fileHits = 0, memHits = 0;
    auto considerFile = [&](const TRACE_RECORD_64& r) { ++fileHits; consider(r); };
    auto considerMem  = [&](const TRACE_RECORD_64& r) { ++memHits;  consider(r); };

    // ---- 起点定位（P4-1 / P4-2）-----------------------------------------------
    // 内存段的起点 = "块下标 + 块内字节偏移 + 该处周期号"。三条来源，按优先级：
    //   ⓪ 游标续扫：uVision 按 nRecordOfs 翻页 —— 接着上次停下的那一条继续（不重扫）
    //   ① 序号直查：过滤器全通过时，skip 直接加在全局序号上，二分跳到目标附近
    //   ② 从头定位：二分块（按 lastCycle）+ 二分检查点 + 顺序解到第一条 >= lo
    unsigned kStart     = 0;
    unsigned fromStart  = 0;
    uint64_t cycleStart = 0;
    bool     started    = false;
    bool     resumed    = false;        // ⓪ 生效 ⇒ 连文件回放也要跳过

    if (s.qcur.valid && s.qcur.lo == lo && s.qcur.hi == hi && skip0 >= s.qcur.matches &&
        FilterSame(s.qcur, filter)) {
        // 按 firstRecSeq 找游标那一块（序号单调 ⇒ 块按序号升序；块被摘掉就找不到）
        unsigned a = 0, b = s.idxCount;
        while (a < b) {
            const unsigned mid = a + ((b - a) >> 1);
            if (s.idx[mid]->firstRecSeq < s.qcur.chunkSeq) a = mid + 1; else b = mid;
        }
        if (a < s.idxCount && s.idx[a]->firstRecSeq == s.qcur.chunkSeq &&
            s.qcur.ofs < s.idx[a]->usedBytes) {
            kStart     = a;
            fromStart  = s.qcur.ofs;
            cycleStart = s.qcur.cycle;
            started    = true;
            resumed    = true;
            // 起点之前已经匹配掉 `qcur.matches` 条 —— 把绝对计数补上，这样
            // `consider` 里的 `matched - skip0` 仍然算的是"本次结果里的第几条"。
            matched    = s.qcur.matches;
            // 游标存在本身就证明"这个区间里有记录" ⇒ 直接认账。
            // ⚠ 少了这一句，翻到**最后一页**（区间已被取完、走两步就 `> hi`）时
            //   一条都不会进 `consider`，于是 `inRange` 为假 ⇒ 返回 **BOUNDS**，
            //   而正确答案是 EMPTY。原实现每页都从区间头重走，所以没暴露这个坑。
            inRange    = true;
        }
    }

    // ---- ① 内存窗口**之前**的那一段：从记录日志回放（P2-2）--------------------
    // 内存块链只装得下秒级窗口（见 §二），更早的记录在 `trrec=` 的记录日志里。
    // 按周期号定位到文件偏移、读一段、用**同一段解码代码**展开。顺序上它一定在
    // 内存窗口之前，所以先回放、再走内存 —— `nRecordOfs`（区间内偏移）的语义
    // 因此与"从头扫一遍"完全一致。
    //
    // ⚠ 游标续扫时**必须跳过**：游标之前的部分上次已经交付过了，再走一遍就会重复。
    const Chunk*   rdHeadM = FirstReadable(s);
    const uint64_t memFirst = rdHeadM ? rdHeadM->headFirstCycle : 0;
    if (!resumed && (!s.count || lo < memFirst)) {
        const uint64_t fileHi = (s.count && hi >= memFirst) ? (memFirst - 1) : hi;
        if (fileHi >= lo) ReplayRecordsFromFile(lo, fileHi, considerFile);
    }

    if (s.count == 0) {
        if (!inRange) { LogQuery("EMPTY (内存 0 条，记录日志里也没有这一段)"); return TDI_ERR_EMPTY; }
        // 全在文件里 —— 后面内存那段自然一条都不出。
    }

    // ---- ② 内存窗口 ----------------------------------------------------------
    if (!started) {
        unsigned kk = 0, ff = 0;
        uint64_t cc = 0, ss = 0;
        if (LocateAligned(lo, kk, ff, cc, ss)) {
            kStart = kk; fromStart = ff; cycleStart = cc;
            started = true;

            // ① 序号直查：过滤器**全通过**时，"区间内第 skip 条命中" 就是
            //    "区间起点往后第 skip 条记录" ⇒ 目标序号 = ss + skip0，二分跳过去。
            //    过滤器不全通过时不能这么做（"第 k 条命中" ≠ "第 k 条记录"），
            //    那时靠 ⓪ 的游标兜底。
            //
            //    ⚠ 只有**区间起点就在内存里**时才成立：`ss` 是内存段第一条 >= lo 的
            //      记录的序号，而 `skip0` 是从**区间起点**（可能在记录日志里）算起的。
            //      文件回放出过记录时（`fileHits > 0`）跳不了 —— 少跳会让结果错位。
            if (skip0 > 0 && fileHits == 0 && FilterPassAll(filter)) {
                unsigned jk = 0, jf = 0;
                uint64_t jc = 0, js = 0;
                if (LocateSeq(ss + skip0, jk, jf, jc, js)) {
                    kStart = jk; fromStart = jf; cycleStart = jc;
                    // 该检查点那条记录是区间起点之后第 `js - ss` 条 ⇒ 它之前已经
                    // 匹配掉这么多条（过滤器全通过，记录数就是命中数）。
                    //
                    // ⚠ `js < ss` 时必须记 0，**不能写成 `js - ss`** —— 那是无符号
                    //   减法，会下溢成一个天文数字，于是 `matched <= skip0` 永远成立、
                    //   一条都交付不出来。这种情况是"检查点落在区间起点之前"（起点在
                    //   块中间），从那个检查点顺序走即可，此时它之前的记录都 < lo，
                    //   会被循环里的 `r.nCycle < lo` 滤掉，不该计入命中。
                    matched = (js > ss) ? (js - ss) : 0;
                }
            }
        }
    }

    for (unsigned k = kStart; started && k < s.idxCount; ++k) {
        const Chunk* c = s.idx[k];
        if (c->nRecs == 0) continue;
        if (c->firstCycle > hi) break;      // 后面的块只会更晚

        // 块内起点：第一块用定位给出的位置（检查点 / 游标 / 序号直查），其余块二分
        // 检查点（按周期号升序，取"周期号 <= lo 的最后一条"）。
        // 这就是变长编码的代价所在：**从块中间查必须从头解**，检查点把每次解码
        // 限制在 1/256 块（4 MB 块 ≈ 16 KB ≈ 2300 条）以内。
        unsigned from        = c->headOfs;            // ★ 不是 0：前缀已被裁掉
        uint64_t cycleAtFrom = c->headFirstCycle;
        if (k == kStart) {
            from        = fromStart;
            cycleAtFrom = cycleStart;
        } else {
            unsigned a = 0, b = c->nCkpt;
            while (a < b) {
                const unsigned mid = a + ((b - a) >> 1);
                if (c->ckptCyc[mid] <= lo) a = mid + 1; else b = mid;
            }
            if (a > 0) { from = c->ckptOfs[a - 1]; cycleAtFrom = c->ckptCyc[a - 1]; }
        }
        // ★ 三条来源（游标续扫 / 检查点二分 / 首条可读）都可能落在裁剪游标之前 ⇒ 夹。
        ClampToHead(*c, from, cycleAtFrom, nullptr);

        for (RecIter it(c->buf + from, c->buf + c->usedBytes, cycleAtFrom); it.Next(); ) {
            const TRACE_RECORD_64& r = it.rec;
            if (r.nCycle < lo) continue;
            // ⚠ **过了 `hi` 必须 break，不能 continue。**
            //
            // 块内记录按周期号**升序**（链本身单调递增，索引也是按 lastCycle 排的），
            // 所以"第一条 > hi"之后整块都不会再命中。
            //
            // 原来这里是 `continue` ⇒ 每个块都要**解到块尾**（4 MB ≈ 60 万条），而
            // 外层还会把所有 `k` 走到 `hi` 为止 ⇒ 拖一下时间轴（请求一个窄区间）实际
            // 解掉了 `lo` 之后的**全部**记录（2026-10-14 实测内存里 880 万条）。
            // 这条查询跑在 **uVision 的 UI 线程**上 ⇒ 界面直接卡死。
            if (r.nCycle > hi) break;
            considerMem(r);

            // ③ 缓冲满了**立刻停**（P4-3）。原来只在外层按块检查 ⇒ 置位之后当前那
            //    一块还要解到块尾（4 MB ≈ 60 万条）。
            //    停之前把游标记下来：指向**这一条**（它还没被交付），下次从这里接着走。
            if (overflow) {
                s.qcur.valid    = true;
                s.qcur.lo       = lo;
                s.qcur.hi       = hi;
                s.qcur.chunkSeq = c->firstRecSeq;
                s.qcur.ofs      = static_cast<unsigned>(it.recStart - c->buf);
                s.qcur.cycle    = r.nCycle;
                s.qcur.matches  = matched - 1;   // 这条之前已匹配的条数（绝对）
                FilterFingerprint(s.qcur, filter);
                break;
            }
        }
        if (overflow) break;        // uVision 的缓冲满了，剩下的块不用解
    }

    if (!inRange) {
        *nCount = 0;
        // 带上"请求的区间"与"驱动手里的范围"—— BOUNDS 的原因全在这两个数上。
        // 2026-10-13 真机就是靠这一条定位的：uVision 请求的区间整个在
        // [1, 13431761] 之外（它手里的范围是 0），于是窗口一直空。
        const Chunk*   rdHeadR = FirstReadable(s);
        const uint64_t f = rdHeadR ? rdHeadR->headFirstCycle : 0;
        const uint64_t t = (s.count && s.tail) ? s.tail->lastCycle  : 0;
        ORB_LOG_WARN("TraceWin: [0] BOUNDS —— 请求 [%llu, %llu]，驱动范围 [%llu, %llu]"
                     "（内存 %llu 条）",
                     (unsigned long long)lo, (unsigned long long)hi,
                     (unsigned long long)f, (unsigned long long)t,
                     (unsigned long long)s.count);
        return TDI_ERR_BOUNDS;   // 请求的周期区间整个在采集范围之外
    }
    if (!haveHit) {
        *nCount = 0;
        LogQuery("EMPTY (区间内有记录，但都被过滤器滤掉了)");
        return TDI_ERR_EMPTY;    // 区间里有记录，但都被过滤器滤掉了
    }

    // 来源证据（限流：拖 Navigation 条会连打几十次查询，每次都打会刷屏）。
    // 判据：拖到内存窗口**之前** → `文件段 > 0`；拖回内存窗口内 → `文件段 = 0`。
    if (fileHits) {
        static unsigned s_shown = 0;
        if (s_shown++ % 64 == 0)
            ORB_LOG_INFO("TraceWin: [0] 记录来源 —— 文件段 %llu 条 / 内存段 %llu 条"
                         "（内存窗口自周期 %llu 起，本次请求 [%llu, %llu]）",
                         fileHits, memHits, memFirst, lo, hi);
    }

    *nFirstCycle = firstHit;
    *nLastCycle  = lastHit;
    *nCount      = (found > limit) ? limit : found;

    // 慢查询告警。阈值 50 ms：拖动时间轴时 uVision 会连打几十次，一次 50 ms 就是
    // 肉眼可见的顿；而正常一次（窄区间、检查点定位）应该在个位数毫秒。
    const uint64_t ms = GetTickCount64() - t0;
    if (ms >= 50u) {
        // ⚠ `命中` 的含义自 P4-3 起变了：缓冲一满就**立刻停**（不再把当前块解完），
        //   所以满的时候它是"至少这么多"（打印成 `4096+`），不再是区间内的真实总数。
        //   真实总数只有"只问数量"（上限 0）那次才数得出来。
        ORB_LOG_WARN("TraceWin: [0] 查询 [%llu, %llu] 用了 %u ms（命中 %llu%s / 上限 %llu，"
                     "内存 %llu 条，文件段 %llu 内存段 %llu）—— 这条跑在 uVision 的 UI "
                     "线程上，慢多少界面就卡多少",
                     (unsigned long long)lo, (unsigned long long)hi, (unsigned)ms,
                     (unsigned long long)found, overflow ? "+" : "",
                     (unsigned long long)cap,
                     (unsigned long long)s.count,
                     (unsigned long long)fileHits, (unsigned long long)memHits);
    }
    LogQuery("OK (返回了记录)");
    return TDI_ERR_SUCCESS;
}

// ---------------------------------------------------------------------------
// TDI 回调（uVision 调我们）
// ---------------------------------------------------------------------------
INT32 GetFirstCycle(UINT64* nFirstCycle)
{
    LogTdi(1, "GetFirstCycle");
    if (!TraceWinReady())    return TDI_ERR_NOT_SUPPORTED;
    if (!nFirstCycle)        return TDI_ERR_INVALID_PARAM;
    if (TraceWinIsRunning()) return TDI_ERR_PROCESSING;

    const Store& s = S();
    StoreGuard guard;
    if (s.count == 0 || !s.head) {
        // 内存里一块都没有 —— 整段历史可能都在记录日志里（P2-2）。
        uint64_t f = 0, l = 0;
        if (!TraceFileRecordsRange(f, l)) return TDI_ERR_EMPTY;
        *nFirstCycle = f;
        return TDI_ERR_SUCCESS;
    }

    // 可查范围左端 = **本段起点**（见 FirstReportedCycle）：
    //   ① 内存窗口之前的区间在 P2-2 之后也取得到，报晚了会让 uVision 的 Navigation
    //      条只画内存那一小段；
    //   ② 就算那段历史真被丢掉了（条数上限 / 内存到顶而记录日志没开），也要报**本段
    //      起点**（= `TraceWin: ETM cycle count base = …` 那一刻的位置），而不是
    //      "最旧一条可读记录" —— 用户 2026-10-15 的裁定（`Todo.md §4.3`）。
    // 再与记录日志的左端取更早的那个。
    uint64_t first = FirstReportedCycle(s);
    uint64_t f = 0, l = 0;
    if (TraceFileRecordsRange(f, l) && f < first) first = f;
    *nFirstCycle = first;

    // ⚠ 记下**返回值**，不是只记"被调用了"。
    //
    // 2026-10-14 真机实测：uVision 反复请求 `781231624`，而驱动这条轴的上界是
    // `390751888` —— **正好 2.00 倍**，且 91 秒里一动不动（不是"当前时刻"，是个
    // 固定值）。驱动这条轴本身是**对的**：9462 条 P1-0 WRN 挤在 26 ms 里、累计计数
    // 涨了 127 k ⇒ 约 4.9 M 条/s，总量 38,745,374 条 ⇒ 采集 ≈7.9 s，而轴
    // 390,751,888 / 48 MHz = **8.14 s**，吻合。
    // ⇒ "2 倍"要么出在这个返回值/`g_coreClkHz` 上，要么出在 uVision 拿它们换算时间
    //   的方式上。**不记返回值就永远分不清**，所以这两条日志必须带上数。
    ORB_LOG_INFO("TraceWin: [1] GetFirstCycle -> %llu（周期时钟 %u Hz，内存 %llu 条）",
                 (unsigned long long)first, g_coreClkHz, (unsigned long long)s.count);
    return TDI_ERR_SUCCESS;
}

INT32 GetLastCycle(UINT64* nLastCycle)
{
    LogTdi(2, "GetLastCycle");
    if (!TraceWinReady())    return TDI_ERR_NOT_SUPPORTED;
    if (!nLastCycle)         return TDI_ERR_INVALID_PARAM;
    if (TraceWinIsRunning()) return TDI_ERR_PROCESSING;

    Store& s = S();
    StoreGuard guard;
    if (s.count == 0 || !s.tail) return TDI_ERR_EMPTY;

    // ⚠ **报"缓冲里最新一条记录的真实 cycle"，不是"当前时刻"**（2026-10-14 修回）。
    //
    // 反编译官方 `ULP2CM3.dll` 的 `GetLastCycle`（VA 0x100F2100）：
    //     0x100F212A  mov ecx,[0x12bda060]   ; 最新一条记录的 cycle（低 32 位）
    //     0x100F2132  and edx,[0x12bda064]   ; 高 32 位
    // ⇒ 官方报的就是**记录的边界**，没有任何墙钟参与（它的 `GetTickCount` /
    //   `QueryPerformanceCounter` 只用在 USB 轮询超时和 CRT cookie 上）。
    //   这和 `StatusUpdate()` 报的 `m_pRecordRange.nLastCycle` 是同一对值 —— 两边
    //   必须一致，否则 uVision 拿到的"可显示范围"和它来问的区间会对不上。
    //   （本轮早些时候这里曾返回"墙钟位置"，那是错的：范围比真实数据长，于是它拖到
    //     数据之后的位置照样回 BOUNDS。）
    const uint64_t last = s.tail->lastCycle;
    *nLastCycle = last;

    // 同 GetFirstCycle：这个值就是 uVision 算时间轴的右端，必须留证据。
    // 注意**没有**把 `TraceFileRecordsRange()` 的上界并进来（P2-2 的落盘段在内存窗口
    // 之后）—— 若 uVision 的时间轴右端大于这里报的值，就是它自己在算，不是读我们的。
    uint64_t ff = 0, fl = 0;
    const bool haveFile = TraceFileRecordsRange(ff, fl);
    ORB_LOG_INFO("TraceWin: [2] GetLastCycle -> %llu（周期时钟 %u Hz，内存 %llu 条；"
                 "落盘段 %s%s%llu）",
                 (unsigned long long)last, g_coreClkHz,
                 (unsigned long long)s.count,
                 haveFile ? "" : "无", haveFile ? "上界 " : "",
                 (unsigned long long)(haveFile ? fl : 0));
    return TDI_ERR_SUCCESS;
}

INT32 ClearTrace()
{
    LogTdi(3, "ClearTrace");
    if (!TraceWinReady())    return TDI_ERR_NOT_SUPPORTED;
    // 采集进行中**照样清**。
    //
    // 2026-10-14 用户实测："Trace Data 没有 clear 功能（按钮灰色）"。根因就是这一行
    // 原来的 `return TDI_ERR_PROCESSING`：uVision 在目标运行期间来清空是**流式源的
    // 正常用法**（同 `QueryRange()` 里那句"采集进行中照样给数"的道理），回 PROCESSING
    // 等于告诉 uVision"本驱动不支持运行中清空"，按钮于是常灰。
    //
    // 安全性：`TraceWinClear()` 自己先 `RetireDrain()` 等在飞的块落完盘、再取结构锁；
    // 抽数线程**追加记录不取**那把锁（见 Append 的说明），所以不存在"清到一半被追加"
    // 的错位 —— 最坏是刚清完那一瞬又被追加一条新记录，那正是流式语义。
    if (TraceWinIsRunning()) LogTdi(3, "ClearTrace：采集进行中，照常清空");

    // 官方的 ETB_Clear 挂在窗口清空这一动作上：清空意味着缓冲区已被消费，允许
    // ETB 再产生一次更新（DbgCM/ETB.cpp:1377）。
    EtbClear();
    TraceWinClear();
    return TDI_ERR_SUCCESS;
}

INT32 GetTraceRecords64(UINT64* nFirstCycle, UINT64* nLastCycle, UINT64* nCount,
                        TRACE_RECORD_64* pTrace64)
{
    return QueryRange(nFirstCycle, nLastCycle, nCount, nullptr, nullptr, pTrace64);
}

INT32 GetFiltTraceRecords64(UINT64* nFirstCycle, UINT64* nLastCycle, UINT64* nCount,
                            UINT64* nRecordOfs, TRACE_FILTER_CONF* pTraceFilter,
                            TRACE_RECORD_64* pTrace64)
{
    // 官方同样要求 nRecordOfs 非空（DbgCM/TraceWinConnect.cpp:647），但那条
    // 路径是**静默**返回的：日志里看不出是"uVision 没来要"还是"来了被拒"。
    if (!nRecordOfs) {
        LogTdi(0, "GetFiltTraceRecords64 -> INVALID_PARAM (nRecordOfs == NULL)");
        return TDI_ERR_INVALID_PARAM;
    }
    return QueryRange(nFirstCycle, nLastCycle, nCount, nRecordOfs, pTraceFilter,
                      pTrace64);
}

INT32 UpdateTraceBuffer(UINT64* /*nFirstCycle*/, UINT64* /*nLastCycle*/)
{
    LogTdi(4, "UpdateTraceBuffer");
    if (!TraceWinReady()) return TDI_ERR_NOT_SUPPORTED;

    // ETB 模式下 uVision 用它重新拉一次目标缓冲；orbtrace 模式下就是再排空
    // 一次端点。两者都由 AGDI 层装的 refresh 钩子完成。
    bool got = false;
    if (S().refresh) got = S().refresh();

    if (!got && S().count == 0) return TDI_ERR_EMPTY;
    return TDI_ERR_SUCCESS;
}

} // namespace

// ===========================================================================
// 公开接口
// ===========================================================================

// ---------------------------------------------------------------------------
// TDI：trace 配置交换（SetTraceConfig / GetTraceConfig）
// ---------------------------------------------------------------------------
// 官方参考实现把这两个指针留成 NULL（头文件注释写着 "Obsolete, do not use"），
// 但那份实现的 trace 窗口本来就没移植（DBGCM_FEATURE_TRCDATA_WIN == 0）——
// "官方不设"并不等于"uVision 不会用"。它们是本驱动在 TDI 里唯一留空的入口：
// uVision 若通过它们交换 trace 配置，NULL 会让它无从下手。先接上并记录调用，
// 用来判断 uVision 到底走不走这条路（调用日志比推测可靠）。
//
// `g_coreClkHz` 的定义在文件上方（StatusUpdate 的时间锚点要用它）。

INT32 SetTraceConfig(TRACE_CONFIG* p)
{
    LogTdi(5, "SetTraceConfig");
    if (!p) return TDI_ERR_INVALID_PARAM;

    // ★ 这里**不是**内核时钟的来源：官方把 nClock 写进 `TraceConf.Clk`（0x10096280），
    //   而 Clk 的初值是 0（0x100EB474-497 批量清零，DLL 内根本没有 8 M / 48 M 常量）
    //   ⇒ 本通道是"host 改了 trace 配置"的**通知**，不是"设置时钟"。
    //   原 `if (p->nClock) g_coreClkHz = p->nClock;` 正是 8 M 灌进内核时钟的入口 —— 删掉。
    ORB_LOG_INFO("TraceWin: SetTraceConfig 收到 nClock=%u Hz（仅记录，不采纳为内核时钟）",
                 p->nClock);

    RefreshTraceClocks();   // 对齐官方 conf 应用后的反向同步（0x1008B806-83B）
    return TDI_ERR_SUCCESS;
}

INT32 GetTraceConfig(TRACE_CONFIG* p)
{
    LogTdi(6, "GetTraceConfig");
    if (!p) return TDI_ERR_INVALID_PARAM;
    memset(p, 0, sizeof(*p));
    // 回的是**内核时钟**（`g_coreClkHz`）；conf 的 Clk 不由本通道表达。
    p->nClock = g_coreClkHz;
    ORB_LOG_INFO("TraceWin: GetTraceConfig -> nClock=%u Hz（内核时钟）", g_coreClkHz);
    return TDI_ERR_SUCCESS;
}

void TraceWinSetCoreClock(uint32_t hz)
{
    if (hz) g_coreClkHz = hz;
}

// ---------------------------------------------------------------------------
// 周期轴机制：ITM/SWO 追墙钟，ETM 不追（2026-10-14 用户裁定）
// ---------------------------------------------------------------------------
// 定义放这里而不是上面轴那一节：那边在**匿名 namespace** 里面（第 53 行开、2141 行
// 关），对外函数放进去会变成内部链接 ⇒ 链接期
// `error LNK2019: 无法解析的外部符号 "...TraceWinSetAxisMode..."`（本轮真踩过）。
// 机制名表 `AxisModeName()` 留在那边（只有本文件用）。
void TraceWinSetAxisMode(int mode)
{
    Store& s = S();
    if (mode != kAxisModeItm) mode = kAxisModeEtm;
    if (s.axisMode == mode) return;
    s.axisMode = mode;
    ORB_LOG_INFO("TraceWin: 周期轴机制 = %s（按 trace 源选：ITM/SWO 数据量小、可以"
                 "追墙钟；ETM 数据量太大不能追。⚠ 两种机制下 uVision 的取数区间都由 "
                 "m_pRecordRange 给死，BOUNDS 与它无关）", AxisModeName(mode));
}

int TraceWinAxisMode() { return S().axisMode; }

// ---------------------------------------------------------------------------
// 虚拟寄存器：CORE_CLK / TRACE_CLK
// ---------------------------------------------------------------------------
// 官方 RegisterClock() / RegisterTraceClock()（DbgCM/AGDI.CPP:4165 / :4251）。
// ULP2CM3 的实现逐指令一致：p_create -> 改名 -> p_putval -> w_create。
//
// Trace Data 窗口的时间轴要的就是 TRACE_CLK。本驱动此前一个 VTR 都没注册 ——
// 现象正是"窗口在、接口绑上了、ClearTrace 也能调到，却永远不来取记录"。
static VTR* g_coreClk  = nullptr;
static VTR* g_traceClk = nullptr;
static WATCH g_wCoreClk;
static WATCH g_wTraceClk;

// 官方 wrCoreClk / wrTraceClk（DbgCM/AGDI.CPP:4122 / :4213）：uVision 的 Trace 页改写
// 这两个 VTReg 时的回调。**核心时钟的真源就在这里**，不是本驱动的 `cpu=` 选项 ——
// 原来这两个函数只打一行日志、把值扔掉，于是 uVision 里填的 Core Clock 对 TPIU 分频
// 毫无影响（COMPAT_ANALYSIS.md §10.28 清单第 3 条）。
//
// 官方的写法：
//     if (iRun) { pCoreClk->v.ul = TraceConf.Clk; return; }   // 运行中不许改，回写旧值
//     TraceConf.Clk = pCoreClk->v.ul;
//     if (TraceConf.Clk == 0) TraceConf.Clk = 1;
//     if (TraceConf.Opt & TRACE_USE_CORECLK) { pTraceClk->v.ul = TraceConf.Clk; ... }
//
// 签名必须是 WATCH 的 wf 成员：void (*)(DWORD adr, AGSICB_REASON r)。
void wrCoreClk(DWORD adr, AGSICB_REASON r)
{
    (void)adr; (void)r;
    if (!g_coreClk) return;

    // ★ 官方的运行中守卫（就是上面抄的伪码**第 1 行**）：采集**进行中**不接受时间基
    //   变更，一律回写权威值 `TraceConf.Clk`。理由：已入库记录是按旧时钟换算时间轴的，
    //   中途换基会让同一段里前后记录不自洽（`AxisBudget()` 也按 `cpuKhz` 判，可能开始
    //   拒值 ⇒ `axisRejects` 上涨）。官方如此，照抄。
    //   ⚠ **与 A4 不冲突**：A4 修的是"跟随分支之外的写入被整个丢弃"，守卫管的是"何时
    //     允许写"—— 停止态下的写入照样无条件落到 `g_coreClkHz`（下面 `TraceWinSetCoreClock(hz)` 那行）。
    //   ⚠ **不会挡掉本驱动自己的重发**：`RefreshTraceClocks()` 的唯一事件入口
    //     `TraceSetRunning()` 在函数**最前**就调它（`Agdi.cpp:430`），而 `TraceWinSetRunning()`
    //     到 `:507` 才置 `s.running` ⇒ 启动时本守卫尚未生效；停止时虽已生效，但重发写的
    //     就是 `cpuKhz*1000`（≙ 本守卫回写的同一个 `TraceConf.Clk`），挡不挡同值。
    if (TraceWinIsRunning()) {
        const uint32_t conf = static_cast<uint32_t>(orb::cfg::Get().cpuKhz) * 1000u; // ≙ TraceConf.Clk
        if (g_coreClk->v.ul != conf) {
            ORB_LOG_WARN("TraceWin: 采集进行中，忽略 uVision 写的 CORE_CLK %u Hz，"
                         "回写并保持 %u Hz（官方 iRun 守卫）",
                         g_coreClk->v.ul, conf);
        }
        g_coreClk->v.ul = conf;
        return;
    }

    const uint32_t hz = g_coreClk->v.ul;
    if (hz < 1000u) return;               // 0 / 异常值：uVision 建表时会读一次
    //   ⚠ **有意偏离**：官方是 `if (==0) 值=1`（`AGDI.CPP:4131-4132`）—— 它**不丢值**、
    //     0 钳成 1 Hz；我们 <1000 一律拒（更保守：1 Hz 会污染 TPIU 分频探测）。别按官方
    //     "修"回去。同样口径在 `wrTraceClk` 那边也有（对应原文 `:4236-4237`）。

    orb::cfg::Config& c = orb::cfg::Get();
    const int khz = static_cast<int>(hz / 1000u);
    // ★ **不能在这里 `return`**（原来写的 `if (khz == c.cpuKhz) return;` 是错的）：
    //   官方 `AGDI.CPP:4122-4154` 停止态**没有"值未变就返回"**，它一路走到 `:4149-4153`
    //   的镜像回写。早退会**连带跳过下面的 TRACE_CLK 镜像** ⇒ 构造场景：勾着
    //   Use Core Clock、host 写 CORE_CLK = 当前值、而 TRACE_CLK 框里是旧值 ⇒ 官方会把
    //   TRACE_CLK 掰回核心时钟，我们什么都不做（框里留旧值）。
    //   `changed` 只决定"要不要记日志 + 重算 + 落配置"，**不决定要不要镜像**。
    const bool changed = (khz != c.cpuKhz);
    if (changed) {
        ORB_LOG_INFO("TraceWin: uVision Trace 页把 CORE_CLK 改为 %u Hz（原 %d kHz）"
                     "—— TPIU 分频按新值重算", hz, c.cpuKhz);
        c.cpuKhz = khz;                   // ★ 顺序敏感：必须在下面那句**之前**落配置

        // ★ 无条件更新内核时间基 —— 这是本 bug 的**真修复半边**。原来它只在"跟随核心
        //   时钟"那一支里调，于是 uVision 填的 Core Clock 在不跟随时对记录时间轴毫无影响。
        TraceWinSetCoreClock(hz);
    }

    // 官方 `AGDI.CPP:4149-4153` 同款：TRACE_USE_CORECLK 时把 TRACE_CLK 框里回写成生效值
    // （原文：`pTraceClk->v.ul = TraceConf.Clk;` + `TPIU_Clock = TraceConf.Clk;`）。
    // ★ 官方这里**直写字段、不用 putval**，原文行内自己给了理由（`:4150`）：
    //   "Don't use putval, otherwise TraceConf.TPIU_Clk gets overwritten" —— putval 会把
    //   用户显式填的 TPIU 时钟冲掉。这与 `wrTraceClk` 守卫里那处"回写 `TPIU_Clock`"的口径
    //   正好互补（一个保住显式值，一个保住生效值）。别"顺手改成 putval"。
    // 本驱动里 "跟随核心时钟" 有两个来源：Trace 页的 "Use Core Clock"（`tclkc`）与
    // `traceClockKhz == 0` 这个"没填"的表示（见 cfg::Effective）。
    // ⚠ **此处不发 VTR**：官方 `wrCoreClk 0x1008C340` 根本不是重发 helper
    //   `0x1008B7C0` 的调用者（它的 3 个调用点全在 conf 应用 / trace 启动 / 停止复位）
    //   ⇒ 重发统一走 RefreshTraceClocks()，别把 p_putval 挪回这里。
    if ((c.useCoreClock || c.traceClockKhz == 0) && g_traceClk) {
        g_traceClk->v.ul = hz;
    }
}

void wrTraceClk(DWORD adr, AGSICB_REASON r)
{
    (void)adr; (void)r;
    if (!g_traceClk) return;

    orb::cfg::Config& c = orb::cfg::Get();

    // ★ 官方 `wrTraceClk`（VA `0x1008C460`）与 `wrCoreClk` **同款**运行中守卫：
    //   `cmp byte[0x102A3DAD],0 / je 正常路径` —— 采集进行中一律拒绝改写，回写权威值后 ret。
    //   ⚠ 回写的**不是** `TraceConf.TPIU_Clk`（照抄 `wrCoreClk` 会错）：官方回写全局
    //     `TPIU_Clock`（`[0x10282578]`），而该全局在 TRACE_USE_CORECLK 下被维护成
    //     `TraceConf.Clk` ⇒ **一个表达式覆盖两种模式**。我们这边同一个表达式就是
    //     `RefreshTraceClocks()` 里那句 `val.ul = follow ? clk : traceClockKhz * 1000u`
    //     （其中 `follow = useCoreClock || traceClockKhz == 0`）。**这三处必须一起改**
    //     （另一处在 `wrCoreClk` 的本地镜像分支）—— 按上面的表达式找，别按行号找。
    //   ⚠ 顺序按官方：守卫在下面 `useCoreClock` 只读分支**之前** —— 运行中不再走
    //     "脱管纠偏 + RefreshTraceClocks" 那条路（此时权威值本来就是要写回的值）。
    if (TraceWinIsRunning()) {
        const uint32_t tail = (c.useCoreClock || c.traceClockKhz == 0)
                                  ? static_cast<uint32_t>(c.cpuKhz) * 1000u
                                  : static_cast<uint32_t>(c.traceClockKhz) * 1000u; // ≙ TPIU_Clock
        if (g_traceClk->v.ul != tail) {
            ORB_LOG_WARN("TraceWin: 采集进行中，忽略 uVision 写的 TRACE_CLK %u Hz，"
                         "回写并保持 %u Hz（官方 iRun 守卫）",
                         g_traceClk->v.ul, tail);
        }
        g_traceClk->v.ul = tail;
        return;
    }

    // "Use Core Clock"（`tclkc`）：TRACE_CLK 变成只读 —— 官方在这一支回
    // `EU49 "Read-only from command line in this case"` 并把框里的值回写成生效值
    // （`AGDI.CPP:4227-4232`）。跟随核心时钟时那个生效值就是 CORE_CLK。
    //   ⚠ 原文 `:4228` 写的是 `pTraceClk->v.ul = TPIU_Clock`（**有效 TPIU 时钟**，与上面 iRun
    //     守卫同一表达式），不是 CORE_CLK —— 我们 `RefreshTraceClocks()` 发布的正是同一个值。
    //   ⚠ 两处**有意偏离**：① 官方此处是 `warntxtout(EU49)` 用户可见警告、且由
    //     `ReInitInProgress` 门控（重初始化期静默，`:4229-4231`），我们是**无条件 INFO
    //     日志、无门控** —— 语义更弱但不制造噪声，裁定可接受；
    //     ② 官方只直写字段，我们额外经 `p_putval` 纠偏（**有意**：正是"轴是新的、窗口里
    //     是旧的"那个症状的修法，理由见 `RefreshTraceClocks()` 上方）。
    if (c.useCoreClock) {
        const uint32_t written = g_traceClk->v.ul;
        const uint32_t core    = static_cast<uint32_t>(c.cpuKhz) * 1000u;
        ORB_LOG_INFO("TraceWin: TRACE_CLK 只读（Use Core Clock 勾着）—— 忽略 "
                     "uVision 写的 %u Hz，保持核心时钟 %u Hz", written, core);
        // ★ **有意偏离官方**：官方此处**只** `pTraceClk->v.ul = TPIU_Clock`（直写本地 VTR
        //   字段，`AGDI.CPP:4228`）—— 没有反向同步。我们替掉"直接写字段"、改走
        //   `p_putval`（即官方重发 helper 里的同步段，VA `0x1008B847-871`）是因为：直写字段
        //   只改本地镜像，VTR 在 uVision 侧未必跟着变 —— 那正是"轴是新的、窗口里是旧的"的成因。
        //   ⚠ **这是"不要在时钟写回调里调"的唯一例外**（规则见头文件声明处）：官方不在
        //     `wrCoreClk` 里重发，是因为那条路上值本来就权威；本分支是**脱管纠偏** ——
        //     host 写了只读的 TRACE_CLK，必须掰回权威值，而那三个事件点都不在场。
        //   重入安全由 `RefreshTraceClocks()` 自带的静态守卫兜住（`p_putval` 若同步派发
        //   回写钩子会重入本分支）。
        RefreshTraceClocks();
        return;
    }

    const uint32_t hz = g_traceClk->v.ul;
    if (hz < 1000u) return;
    //   ⚠ **有意偏离**：官方 `AGDI.CPP:4236-4237` 是 `if (==0) =1`（不丢值、0 钳成 1 Hz），
    //     我们 <1000 一律拒。口径与 `wrCoreClk` 那边一致，见那处注释。

    const int khz = static_cast<int>(hz / 1000u);
    // ★ 这里的早退与 `wrCoreClk` 那处**形状相同、性质不同**，别"为了一致"把它也挪到后面：
    //   `wrCoreClk` 后面有 TRACE_CLK 镜像回写，所以不能早退；本支后面只有"一条日志 +
    //   `c.traceClockKhz = khz`"，跳过的只是**日志**。
    //   ⚠ 官方这一支**末尾还有 `UpdateClockSWO()`**（`AGDI.CPP:4239`，已复读）⇒ 官方
    //     **不可能**加这种早退。我们不在写回调里下发 SWO 分频（在 trace 启动时写目标
    //     `TPIU_ACPR`），所以这里确实无可跳过的副作用 —— **但将来若把 SWO 下发挪进本
    //     回调，必须同时删掉这个早退**。
    if (khz == c.traceClockKhz) return;

    ORB_LOG_INFO("TraceWin: uVision Trace 页把 TRACE_CLK 改为 %u Hz（原 %d kHz）",
                 hz, c.traceClockKhz);
    c.traceClockKhz = khz;
    // ★ 删掉原来的 `TraceWinSetCoreClock(hz);` —— TRACE_CLK **不是**内核时间基，把它写进
    //   `g_coreClkHz` 正是"6 倍时间污染"的另一半（8 M 那条是 SetTraceConfig）。
    //   VTR 此刻已持有新值，无需重发。
}

// 对齐官方**唯一**的重发 helper `0x1008B7C0`（3 个调用点：conf 应用 0x10096161 /
// trace 启动 0x1009622B / 停止-复位收尾 0x10095102）。
//   CORE_CLK  ← [TraceConf.Clk]                        无条件（0x1008B820 / 0x1008B833）
//   TRACE_CLK ← TRACE_USE_CORECLK ? Clk : TPIU_Clk     （0x1008B847 / 0x1008B871）
//
// ⚠ 它**不**在 `wrCoreClk` 的调用链上（0x1008C340 不调 0x1008B7C0）—— 所以调用点必须
//   落在上面那三类**事件**处，不要在时钟写回调里调。
// 幂等且 null-safe：BOM 或 VTR 不在时静默返回，代价只有一条日志。
void RefreshTraceClocks()
{
    // ★ 重入守卫：本函数**唯一**在"VTR 写回调内部"被调的地方是 `wrTraceClk` 的脱管
    //   纠偏分支，而 `p_putval` 会不会**同步**把写事件派发回 VTR 的写钩子，取决于
    //   `struct bom` 的实现 —— 它在工作区外的 ABI 头里，本仓库**无法证实**。若同步，
    //   路径 `wrTraceClk → RefreshTraceClocks → p_putval → wrTraceClk → …` 就是**无限
    //   递归**（栈溢出，不是死循环，现场更难查）。
    //   ⇒ **官方有等价物，这是"p_putval 很可能同步回声"的旁证**：官方重发 helper
    //     `0x1008B7C0` 在 `0x1008B80E` 置 `byte [0x102A3CCE] = 1`、出口 `0x1008B8D4` 清 0，
    //     入口 `0x1008B7EC` 用 `cmp byte [0x102A3CCE],0 / jne` 早退 —— 官方为一个"把
    //     `TraceConf` 反向 `p_putval` 回 VTR"的动作**专门设了一个重入字节**，说明它自己
    //     也不信任 `p_putval` 不会回声。下面这个 `inFlight` 就是那个字节的等价物。
    //     （官方另有两个标志 `[0x102A3CCF]` = conf-dirty、`[0x102A3DAC]` = 运行中延迟应用，
    //      我们没有对应物 —— 因为我们的发布发生在**事件点**、没有"配置应用期"这个概念。）
    //   守卫只挡重入：外层那次写的就是同一个值，内层直接返回**语义不变**。
    //   ⚠ `static` 无锁 ⇒ 本守卫只保证**重入**安全，**不保证跨线程正确性**：两个线程
    //     同时进来时，后到的那一次会**整段静默跳过**（不只是挡住重入）。本驱动其余时钟
    //     状态同样不跨线程保证 —— 这里不引入新的并发假设，也不假装已经解决。
    static bool inFlight = false;
    if (inFlight) return;
    struct ReentryGuard {
        bool& f;
        explicit ReentryGuard(bool& b) : f(b) { f = true; }
        ~ReentryGuard() { f = false; }
    } guard(inFlight);

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->p_putval) return;             // 未注册 ⇒ 静默（与注册块口径一致）

    orb::cfg::Config& c = orb::cfg::Get();
    const uint32_t clk    = static_cast<uint32_t>(c.cpuKhz) * 1000u;   // ≙ TraceConf.Clk
    const bool     follow = (c.useCoreClock || c.traceClockKhz == 0);  // 与 wrCoreClk 同口径

    union v val;
    if (g_coreClk) {
        val.ul = clk;
        bom->p_putval(g_coreClk, &val);
    }
    if (g_traceClk) {
        val.ul = follow ? clk : static_cast<uint32_t>(c.traceClockKhz) * 1000u;
        bom->p_putval(g_traceClk, &val);
        g_traceClk->v.ul = val.ul;                  // 本地缓存同步（官方 0x1008C38F 同款）
    }
    ORB_LOG_INFO("TraceWin: 重发时钟 VTR CORE_CLK=%u Hz TRACE_CLK=%u Hz 跟随=%d",
                 clk, g_traceClk ? g_traceClk->v.ul : 0u, static_cast<int>(follow));
}

// 把一条 WATCH 挂在 VTReg 指针上（官方 wCoreClk/wTraceClk 的字面初始化：
// { NULL, NULL, fn, (DWORD)&pXxxClk, ATRX_WRITE, 0xFFFF }）。
void MakeClockWatch(WATCH& w, void (*fn)(DWORD, AGSICB_REASON), VTR** ppVtr)
{
    memset(&w, 0, sizeof(w));
    w.v.wf     = fn;
    w.adr      = reinterpret_cast<DWORD>(ppVtr);
    w.acc_type = 0x04;     // ATRX_WRITE (ORBAGDI_Break.h)
    w.acc_size = 0xFFFF;   // 0xFFFF == VTREG access
}

void TraceWinRegisterClocks(uint32_t coreHz, uint32_t traceHz)
{
    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->p_create || !bom->p_putval || !bom->w_create) {
        ORB_LOG_WARN("TraceWin: BOM 没有 p_create/p_putval/w_create，VTR 无法注册");
        return;
    }

    // ① 先把所有已有 VTReg 上的 Clock 标志清掉，保证全表只有一个是"时钟"。
    //    官方 RegisterClock()（AGDI.CPP:4172-4176）就是这么做的。
    if (bom->pVTA) {
        VTR* pV = bom->pVTA;
        for (DWORD n = 0; n < bom->nVT; ++n, ++pV) {
            if (pV->Clock) pV->Clock = 0;
        }
    }

    union v val;

    // ② **每次都要重新 p_create**，不能缓存 VTR*。
    //
    //    官方两处注册（AGDI.CPP:4180 / :4257）都是无条件 p_create，没有守卫。
    //    原因是 uVision 的 VTReg 表（pio->pVTA/nVT）是**按会话重建**的：本驱动
    //    此前用 `if (!g_coreClk)` 把创建缓存成进程内一次，而 TraceWinShutdown()
    //    又不重置它，于是**第二次及以后的会话里 p_create 被跳过、p_putval 打在
    //    野指针上** —— uVision 那边根本没有 clock。
    //
    //    后果是整条时间链断掉：没有 clock 就算不出 trace 时间跨度，状态栏恒显示
    //    `t1: 0.00000000 sec`、Trace Navigation 两端都是 `0 s`，窗口没有可渲染的
    //    区间，于是**永远不来调 GetTraceRecords64** —— 正是"接口全通、列名都对、
    //    就是不取数"这个现象。
    g_coreClk = bom->p_create(const_cast<char*>("CLOCK"), bt_ulong);
    if (g_coreClk) {
        g_coreClk->name = const_cast<char*>("CORE_CLK");
        val.ul          = coreHz;
        bom->p_putval(g_coreClk, &val);
        MakeClockWatch(g_wCoreClk, &wrCoreClk, &g_coreClk);
        bom->w_create(&g_wCoreClk);
    }

    g_traceClk = bom->p_create(const_cast<char*>("TRACE_CLK"), bt_ulong);
    if (g_traceClk) {
        g_traceClk->name = const_cast<char*>("TRACE_CLK");
        val.ul           = traceHz;
        bom->p_putval(g_traceClk, &val);
        MakeClockWatch(g_wTraceClk, &wrTraceClk, &g_traceClk);
        bom->w_create(&g_wTraceClk);
    }

    ORB_LOG_INFO("TraceWin: VTR 注册 CORE_CLK=%u Hz TRACE_CLK=%u Hz "
                 "(core=%p trace=%p)",
                 coreHz, traceHz, (void*)g_coreClk, (void*)g_traceClk);
}

// TDI 接口表的 ABI 断言（零运行代价，把 UV4 侧的硬编码偏移固化成编译期检查）：
//   * UV4 在 UV_TRACE_IF_INIT 处理体里 `0x71CECA cmp [t+0x00],2` 读 nVersion；
//     `0x45C98D mov dword [edx+0x28],2`（edx = &g_init）**回写 nVersionUV** ——
//     位置与 +0x28 完全吻合，且与副本那条 `0x45C986 mov dword [host+0x6d],2`
//     （host+0x45 是副本基址，+0x28 = host+0x6d）互为交叉验证。
//   * 整表用 `0x45C97C rep movsd ecx=0x20` 拷贝 ⇒ 大小必须是 0x20 dwords = 0x80 字节。
//   * ⚠ 别看到 `TraceDataTypes.h:38` 的 `#pragma pack(push, 1)` 就来"修正"上面两条断言：
//     该结构**所有**成员都是 4 字节宽（含 `+0x30` 那个位域 `bUnlimitedTrace:1 |
//     bEmbeddedTraceBuffer:1 | :30` 的底层 DWORD）⇒ pack(1) 与默认收紧产出**相同布局**，
//     `+0x28` 与 `sizeof == 0x80` 在任何 pack 设置下都成立。
// 这两条一旦不成立，Window 侧读到的 nVersionUV / 槽位全是错的 —— 那才是真 ABI 隐患，
// 所以别把断言删掉当作"多此一举"。
static_assert(offsetof(TraceInterfaceInit64_t, nVersionUV) == 0x28,
              "TDI ABI: nVersionUV 必须在 +0x28（UV4.exe 0x45C98D 按此偏移回写 2）");
static_assert(sizeof(TraceInterfaceInit64_t) == 0x80,
              "TDI ABI: 接口表必须是 0x80 字节（UV4.exe 0x45C97C rep movsd ecx=0x20）");

bool TraceWinInit()
{
    Store& s = S();
    if (s.registered) return true;

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) {
        ORB_LOG_WARN("TraceWin: 没有 BOM，Trace Data 窗口不可用");
        return false;
    }
    if (!Ensure()) return false;

    // bTrcDisp 是 uVision 侧"这个驱动支持 Trace Data 窗口"的开关。Keil 参考
    // 驱动在 Trace_Setup 里同样先置位再注册（Trace.cpp:1435）。
    bom->bTrcDisp = 1;

    memset(&g_init, 0, sizeof(g_init));
    // ⚠ `nVersion` 必须**精确等于** 2，不能"加字段就顺手上调"：
    //   UV4 `0x45CECA cmp [t+0x00],2` ⇒ `>2` 走 `0x45C95F return 1`，**整张表被拒装**
    //   （Trace Data 窗口直接消失，不是降级）；`<2` 走旧 v1 安装器 `0x45C750`。
    //   要扩字段只能用 `nReserved[19]`（+0x34..+0x80），**别动这里**。
    g_init.nVersion               = TDI_VERSION;   // TDI_VERSION == 2（TraceDataTypes.h:32）
    // 这两个在官方参考里是 NULL（"Obsolete"），但参考的窗口没移植，不能据此
    // 断定 uVision 不用。接上才能判断。
    // 官方参考实现把这两个槽留 NULL，但官方**另有** conf 应用路径（0x10096280 →
    // TraceConf.Clk）**和**反向同步（0x1008B806-83B：应用后把 Clk 重新 p_putval 回
    // CORE_CLK VTR）。我们没有那条独立路径（0x10096280 全库 0 引用）⇒ 这两个槽很可能
    // 是驱动**唯一**能收到"host 改了 trace 配置"的通知口 ⇒ **保留注册**（置 NULL 是
    // 信息损失）。职责已收窄为"通知 + 重发"，见 SetTraceConfig / RefreshTraceClocks。
    g_init.pSetTraceConfig        = &SetTraceConfig;
    g_init.pGetTraceConfig        = &GetTraceConfig;
    g_init.pGetFirstCycle         = &GetFirstCycle;
    g_init.pGetLastCycle          = &GetLastCycle;
    g_init.pClearTrace            = &ClearTrace;
    g_init.pGetTraceRecords64     = &GetTraceRecords64;
    g_init.pGetFiltTraceRecords64 = &GetFiltTraceRecords64;
    g_init.pUpdateTraceBuffer     = &UpdateTraceBuffer;
    // bUnlimitedTrace：ULP2CM3 在"无限 trace"选项打开时置 1（机器码
    // 0x100F3477 `test [0x12bc7758], 0x8000` + 0x100F34CD `mov ebx,1`）。
    //
    // 这一位决定 uVision 怎么看待这个窗口：置 0 时它把驱动当成"自备窗口缓冲"
    // 的 ETB 类驱动，只问范围（GetFirstCycle/GetLastCycle）就不再取记录，
    // 窗口的 Update 按钮也是灰的；置 1 时窗口才是"按需向驱动取"的无限 trace
    // 视图。orbtrace 是**流式**源，与 ULINKpro 同类，此前恒置 0 正是
    // "窗口有列、没有行"的直接原因。
    g_init.bUnlimitedTrace        = 1;
    g_init.bEmbeddedTraceBuffer   = 0;   // ETB 模式下由 AGDI 层另行置位

    // nVersionUV 由 uVision 在初始化时回填，**正常会话里它必然是 2**：
    //   UV4 唯一的写入点是 `0x45C98D mov dword [edx+0x28],2`；唯一会把它清回 0 的
    //   路径是 `EnumUvARM7(h,2)` 清表，而 UV4 **从不调它**（`uv4-fetch` 穷尽枚举了
    //   UV4.exe 的 111 个 `GetProcAddress` 调用点：名字 `EnumUvARM7` 在映像里**完全
    //   不存在**，序号路亦断；5 处 `DllUv3Cap` 实参恒 `arg2=0`，而 teardown 门恰是
    //   `arg2==2`）⇒ **下面是诊断而不是描述**：打 0 = **表没被 UV4 接走**（不是"按 v1
    //   用"），直接查 `Notify(UV_TRACE_IF_INIT)` 为什么没被接（rc / 有没有 BOM）。
    //   DLL 侧独立复核（`re-tracepath`）：`0x100F52F0`（`EnumUvARM7`）在本 DLL 内**零
    //   E8/E9/jcc/imm32 引用** ⇒ 它在 DLL 内的唯一入口是导出表 —— 与上面"UV4 拿不到
    //   入口"合起来是**两条互相独立的路都断**；且该入口即便被调用，`arg1=2` 会在
    //   `[arg1+0x514]` 处立刻崩 ⇒ 它只有 `(合法对象指针, 2)` 一种可用形态。
    //   ⚠ **别用"序号 14"去认它**：`EnumUvARM7` 的导出序恰是 14，而 UV4 里唯一带序号
    //   的 `GetProcAddress` 点 `0x7F70F3` 也压 `0xE`(=14) —— **同序数不同模块**（那个
    //   是 `hhctrl.ocx`），单看序号必然误判成"UV4 调了它"。
    const DWORD rc = bom->Notify(UV_TRACE_IF_INIT, &g_init);
    Configure(bom);

    s.registered = true;
    // 构建时间戳：uVision 会在整个进程生命周期里缓存已加载的 DLL，会话重开不会
    // 重新读盘。没有这一行，"改了却没生效"和"根本没加载新 DLL"在日志上分不开。
    ORB_LOG_INFO("TraceWin: Trace Data 窗口接口已注册 (TDI v%d, 块链 %u MB/块, "
                 "上限 %u 块) Notify(UV_TRACE_IF_INIT) rc=%u, nVersionUV=%d, "
                 "unlimited=%u etb=%u [build %s %s]",
                 TDI_VERSION, S().chunkCapBytes >> 20, S().maxChunks,
                 (unsigned)rc, (int)g_init.nVersionUV,
                 (unsigned)g_init.bUnlimitedTrace,
                 (unsigned)g_init.bEmbeddedTraceBuffer,
                 __DATE__, __TIME__);
    return true;
}

// ---------------------------------------------------------------------------
// 声明"trace 数据展示已启用"（struct bom::bTrcDisp）
// ---------------------------------------------------------------------------
// 这是 Trace Data 窗口取记录的开关位。ULP2CM3 在注册 TDI 接口**之前**置它：
//
//   0x100A3EAF  mov eax, [0x102a3dd0]          ; pio
//   0x100A3EB4  or dword ptr [eax + 0xafc], 8  ; pio->bTrcDisp = 1
//   0x100A3EBB  call 0x100f3410                ; -> TDI 注册
//   0x100A3EC0  call 0x100f03a0                ; -> 窗口配置
//
// 官方参考驱动同样如此（DbgCM/Trace.cpp:1435，包在 DBGCM_FEATURE_TRCDATA_WIN 里）。
// `bTrcDisp` 是 BOM.H 里那组标志位的 bit3：
//
//     DWORD bSleepSim : 1; DWORD bSockBp : 1; DWORD eBpCmd : 1;
//     DWORD bTrcDisp  : 1;  // Sxxx supports Trace-Display-Window  ← bit3
//
// uVision 侧读它的地方是 UV4.exe 0x45D65D `test byte ptr [eax + 0xafc], 8`
// （完整门链见 bug.md「Trace Data 窗口不出数」）。
//
// **前提是这个位必须落在 uVision 读的那个 `struct bom` 实例上**：uVision 会把
// 同一个 ioc 交给驱动两次，但 `dbgblk::pDbgX->pBom` 与 `AG_CB_GETBOMPTR` 的
// 返回值实测**不是同一个对象**，只有后者是 uVision 自己读的那个。见
// ORBAGDI_Agdi.cpp 的 AG_INITCALLBACK 与 ORBAGDI_Pdsc.cpp 的 SetBomAlt()。
bool TraceWinAssertTraceDisplay(bool setBit, const char* where)
{
    struct bom* bom = orb::pdsc::Bom();
    if (!bom) {
        ORB_LOG_WARN("TraceWin: 没有 BOM，无法声明 trace 展示状态");
        return false;
    }

    const bool was = (bom->bTrcDisp != 0);

    // 两个已知实例都写（主实例是 ioc，见上面的说明）。
    orb::pdsc::SetTraceDisplayFlag(setBit || was);

    ORB_LOG_INFO("TraceWin: trace 展示状态 @%s: bom=%p bTrcDisp %u -> %u",
                 where ? where : "?", (void*)bom,
                 (unsigned)was, (unsigned)bom->bTrcDisp);
    return bom->bTrcDisp != 0;
}

// ---------------------------------------------------------------------------
// 诊断工具：只读地把 UV4.exe 里 Trace Data 取数门链的当前值打进日志
// ---------------------------------------------------------------------------
// **编译期开关，默认关**：需要排查时把下面这个宏改成 1 重新编译。
// **只读，不写任何宿主内存。** 用途：窗口有列有过滤器、却一行数据都不出时，
// 直接指出是哪一道门关着，不必再靠反汇编猜。逆向结论见 COMPAT_ANALYSIS.md。
//
// 门链（反汇编自 UV4.exe 2025-08-20 版；CTraceWin::UpdateStatus 0x45D410 的
// [0x10C]==0 分支）：
//   [0x10C] != 0                          -> 只存状态，不取数
//   bProcessing != 0                      -> 不取数
//   (app[0xafc] & 8) == 0                 -> 不取数   ← 就是 bom::bTrcDisp
//   (app[0x14] & 0x2000) && app[0xbc]==0  -> 不取数   (app[0xbc] = bHideTimestamps)
//   -> CTraceWin::FetchRecords (0x45DB10)
//      [0xD8] != 0 (= bSkipViewUpdate)    -> 直接退出
//
// 全局量：app = [0xca2194]，CTraceWin 属主 = [0xca2198]（**两者不同**，读错基准
// 会得到一堆垃圾值 —— 这是排查时踩过的坑，所以两个都单独取名）。
#define ORBAGDI_GATE_PROBE 0

void TraceWinReadHostGates(const char* where)
{
#if ORBAGDI_GATE_PROBE
    const uintptr_t kAppPtrRva   = 0x8A2194;   // 0x00CA2194 - 0x00400000
    const uintptr_t kOwnerPtrRva = 0x8A2198;   // 0x00CA2198 - 0x00400000
    const uintptr_t kTraceWin    = 0x97468;

    HMODULE host = ::GetModuleHandleA("UV4.exe");
    if (!host) {
        ORB_LOG_WARN("TraceWin: 宿主不是 UV4.exe，跳过门链读取");
        return;
    }

    __try {
        const uintptr_t base  = reinterpret_cast<uintptr_t>(host);
        const uintptr_t app   = *reinterpret_cast<const uintptr_t*>(base + kAppPtrRva);
        const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(base + kOwnerPtrRva);
        if (!app || !owner) {
            ORB_LOG_WARN("TraceWin: UV4 全局指针为空 (app=%p owner=%p)",
                         reinterpret_cast<void*>(app), reinterpret_cast<void*>(owner));
            return;
        }

        const uintptr_t tw  = owner + kTraceWin;
        const DWORD     f14 = *reinterpret_cast<const DWORD*>(app + 0x14);

        // 同一行并排打出"驱动手里的 bom"与"取数门读的 app"：两者必须相同，
        // 否则 bTrcDisp 写不到 uVision 读的那个对象上（本模块曾经的问题）。
        struct bom* bom = orb::pdsc::Bom();
        ORB_LOG_INFO("TraceWin: [门链只读 @%s] bom=%p app=%p 同=%d tw=%p",
                     where ? where : "?",
                     reinterpret_cast<void*>(bom), reinterpret_cast<void*>(app),
                     (bom == reinterpret_cast<struct bom*>(app)) ? 1 : 0,
                     reinterpret_cast<void*>(tw));
        ORB_LOG_INFO("TraceWin: [门链只读] app[0xafc]&8=%u  app[0x14]&0x2000=%u  app[0xbc]=%u",
                     (unsigned)((*reinterpret_cast<const BYTE*>(app + 0xafc) >> 3) & 1),
                     (unsigned)((f14 >> 13) & 1),
                     (unsigned)*reinterpret_cast<const BYTE*>(app + 0xbc));
        ORB_LOG_INFO("TraceWin: [门链只读] CTraceWin[0xCC]=%u [0x10C]=%u [0xD6]=%u "
                     "[0xD8]=%u [0xD9]=%u [0x20]=%d",
                     (unsigned)*reinterpret_cast<const BYTE*>(tw + 0xCC),
                     (unsigned)*reinterpret_cast<const BYTE*>(tw + 0x10C),
                     (unsigned)*reinterpret_cast<const BYTE*>(tw + 0xD6),
                     (unsigned)*reinterpret_cast<const BYTE*>(tw + 0xD8),
                     (unsigned)*reinterpret_cast<const BYTE*>(tw + 0xD9),
                     (int)*reinterpret_cast<const DWORD*>(tw + 0x20));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ORB_LOG_WARN("TraceWin: 读取 UV4 内部字段时异常（版本可能不匹配），已忽略");
    }
#else
    (void)where;
#endif
}

void TraceWinPublishSettings()
{
    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    // key 用驱动 DLL 的名字（官方 szKey = "DBGCM"，即 DLL 的裸名）。
    static char key[64]   = "ORB_AGDI";
    static char value[orb::cfg::kArgsMax];
    orb::cfg::Format(value, sizeof(value));

    QDLL q;
    memset(&q, 0, sizeof(q));
    q.key     = key;
    q.value   = value;
    q.ValSize = static_cast<int>(sizeof(value));

    const DWORD rc = bom->Notify(UV_NEW_TARG_SETTINGS, &q);
    ORB_LOG_INFO("TraceWin: 注册驱动设置给 uVision (key=%s, %u 字节) -> rc=%u",
                 key, (unsigned)strlen(value), (unsigned)rc);
}

void TraceWinReconfigure()
{
    Store& s = S();
    if (!s.registered) return;

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    // 与注册时同一份表、同一份配置：这里要排除的只是"发得太早"这一个变量。
    bom->Notify(UV_TRACE_IF_INIT, &g_init);
    Configure(bom);
    ORB_LOG_INFO("TraceWin: 重新下发 TDI 接口 + 窗口配置");
}

void TraceWinSyncViews(uint64_t cycle)
{
    Store& s = S();
    if (!s.registered) return;

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    // 与 ULP2CM3 逐字段一致：TWS_TYPE_CYCLE + 全部四个视图 + 当前周期。
    TRACE_WIN_SYNC sync;
    memset(&sync, 0, sizeof(sync));
    sync.m_nTypes = TWS_TYPE_CYCLE;                              // 1
    sync.m_nViews = TWS_VIEW_DATA | TWS_VIEW_NAV |
                    TWS_VIEW_HLL  | TWS_VIEW_DASM;               // 0xF
    sync.m_nCycle = static_cast<INT64>(cycle);

    const DWORD rc = bom->Notify(UV_TRACE_WIN_SYNC, &sync);
    ORB_LOG_INFO("TraceWin: 视图同步到周期 %llu -> rc=%u",
                 (unsigned long long)cycle, (unsigned)rc);
}

void TraceWinShutdown()
{
    Store& s = S();
    s.registered = false;
    s.running    = false;
    s.refresh    = nullptr;

    // VTR 指针必须一起清掉：uVision 的 VTReg 表是按会话重建的，留着上一会话的
    // 指针会让下一次 TraceWinRegisterClocks() 跳过 p_create（见该函数里的说明）。
    g_coreClk  = nullptr;
    g_traceClk = nullptr;

    TraceWinClear();
}

bool TraceWinReady()
{
    const Store& s = S();
    // 缓冲就绪 = 块索引已建好（第一块是惰性分配的，见 Ensure）。
    return s.registered && s.idx != nullptr;
}

void TraceWinSetRefreshHook(bool (*fn)())
{
    S().refresh = fn;
}

unsigned TraceWinRecordCount()
{
    StoreGuard guard;
    return static_cast<unsigned>(S().count);
}

uint64_t TraceWinLastCycle()
{
    const Store& s = S();
    StoreGuard guard;
    if (s.count == 0 || !s.tail) return 0;
    return s.tail->lastCycle;
}

uint64_t TraceWinFirstCycle()
{
    const Store& s = S();
    StoreGuard guard;
    if (s.count == 0 || !s.head) return 0;

    // ⚠⚠ 必须过 `FirstReadable`（= 第一条**可读**记录所在的块），**不能**直接返回
    //    `s.head->firstCycle`。那个字段是"本块**写入过**的第一条记录"，而
    //    `EnforceRecCap` 的记录级裁剪（`kMaxRecs = 1<<20`）只推进 `headOfs` /
    //    `headFirstCycle` / `headRecSeq` / `count`，**根本不动它** ⇒ 缓冲一卡在条数
    //    上限上（实测 `count = 1051790`），它就是一个**早已被裁掉**的周期号，而且
    //    随着裁剪继续推进只会越错越远、自己永不更新。
    //
    //    2026-10-15 真机（用户报告"导航条中间段无数据、拖到最尾巴才有"）：
    //      本函数报 43,240,000（头块的第一条，已被裁掉），
    //      真实最旧记录 50,556,981（`[1] GetFirstCycle -> …` / `[0] BOUNDS …` 两行都是它）
    //    ⇒ 停止时 `TraceWinShowRange()` 把它当"显示区间"交给 uVision，区间宽出
    //      **7.32 M 周期**（其中 8 成以上我们已经没有数据）⇒ uVision 的 Navigation
    //      条按这个宽区间铺开 ⇒ 拖中间段发出的请求 `[44200000, 44280000]` /
    //      `[43240000, 43320000]` 整个落在驱动范围之外 ⇒ 取记录回调回
    //      `TDI_ERR_BOUNDS`（就是日志里那条 `[0] BOUNDS —— 请求 …`），只有最右边
    //      那 1.05 M 周期拖得出数。
    //
    //    同一个数值在 `StatusUpdate()`（`g_recRange.nFirstCycle`）与 TDI
    //    `GetFirstCycle()` 里**本来就是对的** —— 两处都过了 `FirstReadable`，
    //    只有这里漏了。`FirstReadable` 上面那段注释写的正是这条规矩。
    //
    // ⚠⚠ 2026-10-15 第二轮（用户裁定，见 `Todo.md §4.3`）：左端**统一**改成
    //    `FirstReportedCycle()` = **本段起点**（`Store::segFirstCycle`，锚点就是
    //    `TraceWin: ETM cycle count base = …` 那条日志）。上面那条"要报最旧**可读**"
    //    在**条数上限还开着**的年代是对的（那时窗口外的历史真的没了，报宽了就去请求
    //    没数据的区间 ⇒ BOUNDS）；现在条数上限默认关、整段 10 s 都留在内存里 ——
    //    正因如此要报**本段起点**，否则 Navigation 条只画得出最后 0.13 s。
    //    真丢了历史时本函数会报得更早，这与上面那段教训不冲突：那一刻状态栏已经有
    //    `Trace: Data Overflow` 在提示（`EnforceRecCap` / `RetirePush` 置的位）。
    //    ★ 三个出口（这里 / `StatusUpdate()` / TDI `GetFirstCycle()`）**必须同一个值**。
    return FirstReportedCycle(s);
}

void TraceWinShowRange(uint64_t firstCycle, uint64_t lastCycle)
{
    Store& s = S();
    if (!s.registered) return;

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    // bExplicitRange + m_pRecordRange 是 TRACE_STATUS 里唯一"指名要哪一段"的
    // 手段（TraceDataTypes.h:429/433）。不指名的刷新只让 uVision 去问范围
    // （GetFirstCycle/GetLastCycle 会到），但它不会进一步取记录。
    //
    // ⚠ **`m_pRecordRange` 与 `st` 都必须指向持久存储。** uVision 的 `Notify`
    //   返回之后**还会**读它们（不是立即拷走 —— 与 bug.md 里"BOM 实例取错"同一类
    //   问题）。指向栈上的局部变量 ⇒ 一出作用域就失效 ⇒ uVision 拿到垃圾区间 ⇒
    //   请求一个根本不存在的区间 ⇒ 驱动回 `BOUNDS` ⇒ **窗口只有列、没有行**。
    //   2026-10-13 真机实测正是这个：范围报对了（`显示区间 [1, 13431761] -> rc=0`）、
    //   Navigation 条却是 `0 s → 0 s`、Trace Data 全空。
    //
    // 只被 TDI 回调与停止流程访问（都在 uVision 的线程上），所以不必加锁。
    static TRACE_RECORD_RANGE s_range;
    static TRACE_STATUS       s_status;

    memset(&s_range, 0, sizeof(s_range));
    s_range.nFirstCycle = firstCycle;
    s_range.nLastCycle  = lastCycle;

    memset(&s_status, 0, sizeof(s_status));
    s_status.bProcessing     = 0;
    s_status.bHideTimestamps = S().showTs ? 0u : 1u;   // 见 Store::showTs（取数门）
    s_status.bSkipViewUpdate = 0;
    s_status.bExplicitRange  = 1;
    s_status.m_pRecordRange  = &s_range;

    // **时间锚点一起带上**（与 `StatusUpdate()` 共用一份持久存储）：告诉 uVision
    // `time(cycle) = cycle / clock`（报 `(0, 0.0)` ⇒ "周期 0 就是 0 秒"），Time 列
    // 才有值。
    //
    // ⚠ 别把它当成"防 BOUNDS 的手段" —— 真正防 BOUNDS 的是上面那句
    //   `bExplicitRange = 1` + `m_pRecordRange`（官方做法，见 `StatusUpdate()` 的
    //   说明）。锚点只负责"周期号怎么换算成秒"。
    //   2026-10-13 那次 `请求 [4679999, 5144790]` vs `驱动 [1, 3429861]` 的越界，
    //   根因是 `bExplicitRange = 0`（**没告诉它区间**），不是"缺锚点"。
    if (g_coreClkHz) {
        memset(&g_clockSample, 0, sizeof(g_clockSample));
        g_clockSample.m_nClock     = g_coreClkHz;
        g_clockSample.m_nLastCycle = 0;
        g_clockSample.m_dLastTime  = 0.0;
        s_status.m_pClockSample    = &g_clockSample;
    }

    const DWORD rc = bom->Notify(UV_TRACE_IF_STATUS, &s_status);
    ORB_LOG_INFO("TraceWin: 显示区间 [%llu, %llu] -> rc=%u",
                 (unsigned long long)firstCycle,
                 (unsigned long long)lastCycle, (unsigned)rc);
}

void TraceWinItmStimulus(uint32_t port, const uint8_t* data, unsigned len)
{
    if (!TraceWinReady() || !data || len == 0) return;
    if (len > 4) len = 4;

    uint32_t v = 0;
    for (unsigned i = 0; i < len; ++i) {
        v |= (uint32_t)data[i] << (8 * i);          // LSB 在前
    }
    // nDataS：1/2/4 字节三档（3 字节也按 4 存，高字节为 0 —— 与旧表示一致）。
    const unsigned sz = (len == 1) ? 1u : (len == 2) ? 2u : 3u;
    AppendRec(TR_TYPE_ITM, static_cast<uint8_t>(kFlAddr | DataSzBits(sz)),
              0, port, v, 1);
}

void TraceWinPcSample(uint32_t pc)
{
    if (!TraceWinReady()) return;

    AppendRec(TR_TYPE_PC_SAMPLE, kFlPC, pc, 0, 0, 1);
}

void TraceWinEvent(uint32_t mask)
{
    if (!TraceWinReady()) return;

    // Keil masks CYCEVT out of the record (DbgCM/Trace.cpp:666): the cycle
    // event is reported through the timestamp, not as a counter of its own.
    // nDataS = 1：事件计数器的载荷恒为 1 字节（TraceDataTypes.h 的 "Always '1'"）。
    AppendRec(TR_TYPE_EVT, DataSzBits(1), 0, 0,
              mask & ~static_cast<uint32_t>(TR_EVT_CYC), 1);
}

void TraceWinExceptionTrace(uint32_t packed)
{
    if (!TraceWinReady()) return;

    // nDataS = 2：异常 trace 的载荷恒为 2 字节（"Always '2'"）。
    AppendRec(TR_TYPE_EXCTRC, DataSzBits(2), 0, 0, packed, 1);
}

void TraceWinEtmInstruction(uint32_t addr, bool executed)
{
    if (!TraceWinReady()) return;

    AppendRec(TR_TYPE_ETM,
              static_cast<uint8_t>(kFlPC | (executed ? kFlExec : 0u)),
              addr, 0, 0, 1);
}

void TraceWinFlow(uint32_t flowType)
{
    if (!TraceWinReady()) return;

    // 流控子类型（TR_TYPE_FLOW_*）放在 nData（4 字节）。
    AppendRec(TR_TYPE_FLOW, DataSzBits(3), 0, 0, flowType, 1);
}

void TraceWinAdvanceCycle(uint32_t delta)
{
    // ⚠ **不再推进周期轴**（2026-10-14）。两个理由：
    //   ① 轴目前由**墙钟兜底**拥有（SyncAxisToWall()），只用于给记录打时间戳。
    //      ⚠ 别把这条读成"墙钟是正解" —— `BOUNDS` 的修法是 `StatusUpdate()` 报
    //      `m_pRecordRange`，与轴用谁**无关**（见那里与 SyncAxisToWall() 的说明）；
    //   ② ITM 时间戳在本板本来就不可信：SWO 位时钟 = 核心时钟（48000000 Hz），
    //      驱动自己就警告过 "the TPIU cannot sustain that for NRZ, so expect a
    //      garbled ITM stream"，实测载荷报出饱和值 0xFFFFFFFF（被预算闸门拒掉）。
    // 只留一个计数，供 `cycle axis(...)` 那行日志看"ITM 时间戳一共报了多少周期"。
    S().itmCycles += delta;
}

// ETM 报来 0 的次数（= "unknown"，见下面 TraceWinEtmTime 的说明）。只用于那条
// 一次性 WARN 的限流；跨线程竞争只会多打一行日志，不值得为它加锁。
static unsigned long long s_etmZeroVals = 0;

void TraceWinEtmTime(uint64_t absolute, const char* what)
{
    if (absolute == 0xFFFFFFFFFFFFFFFFull) return;   // COUNT_UNKNOWN：这个包没带值

    // ⚠⚠ **0 也是"没有值"，不是"值等于零"**（2026-10-14 修，Todo §10.36 的遗留）。
    //
    // 手册 IHI0014Q §7.7.4 原文：
    //     "A value of zero indicates that the timestamp is unknown"
    // ⇒ 0 是"unknown"的**编码**，不是一次合法采样。
    //
    // 本板两条来源都会给 0：
    //   * ETM 时间戳包（§10.36 ①）：**每个值都是 0**（器件侧不给有效计数值）；
    //   * ETM 周期计数（§10.36 ③）：`ETMCR[12]` RAZ/WI、`ETMCCR bit12=0` ⇒
    //     这颗 ETM **不实现**周期计数 ⇒ 解出来同样是 0。
    //
    // 原来把 0 当合法绝对值用：**建基准（base = 0）并把周期轴认领走** ⇒ 此后每个包
    // 的增量都是 0（`AdvanceFrom()` 对 0 直接 return）⇒ **轴再也不动**，只剩记录自身
    // 的 +1 在推 ⇒ 轴的总长度 ≈ 记录条数：
    //
    //     会话 18:14:30–18:14:58（28 s）而 `GetLastCycle -> 196885` ≈ 4.1 ms；
    //     `GetLastCycle -> 7661` 恰好等于"内存 7661 条"。
    //
    // ⇒ uVision 的 Navigation 条 / 时间列全错（**总长度异常**），请求落在"轴上没有
    //   记录"的地方只能回 `BOUNDS`（**无法选取某个时间点**）—— 用户报的两个现象。
    //
    // 修法：0 一律当 unknown 丢掉 —— **不建基准、不认领、不推进**。代价为零：这条
    // 通路只用**差值**，而 0 之后的第一个非零值自然成为基准（`!s.etmHaveAbs` 那一支）。
    if (absolute == 0) {
        if (s_etmZeroVals++ == 0) {
            ORB_LOG_WARN("TraceWin: ETM %s 的值是 **0** —— 按手册 §7.7.4 那是 "
                         "\"timestamp unknown\"，不是一次采样。既不建周期轴基准、也不"
                         "认领/推进轴（本板 ETM 时间戳恒 0、周期计数不实现）。轴留给"
                         "真正在推进的来源（本板是 ITM 时间戳）。本条只报一次，"
                         "后续 0 值只计数。",
                         what ? what : "?");
        }
        return;
    }

    Store& s = S();

    if (!s.etmHaveAbs) {
        // 第一次收到只建基准：ETM 的计数器是自由运行的，起点不是 0，直接把
        // 它加到轴上会让时间从一个巨大的偏移开始。
        //
        // ⚠⚠ **这里不认领周期轴**（2026-10-14 修）。原来在建立基准时就认领，而
        //   "收到第一个包" **不等于** "这个来源会推进"：
        //
        //   实测本板（STM32F407）ETM 的周期计数**不实现**（`ETMCCR bit12 = 0`，
        //   `ETMCR[12]` 是 RAZ/WI，见 COMPAT_ANALYSIS.md §10.36）⇒ 解出来的周期
        //   计数包值恒 **0**。基准建完就把轴认领走，而此后每个包的增量都是 0
        //   （`AdvanceFrom()` 对 0 直接 return）⇒ **轴再也不动**，只剩记录自身的
        //   +1 在推 ⇒ **轴的总长度 ≈ 记录条数**：
        //
        //     会话 18:14:30–18:14:58（28 s）而 `GetLastCycle -> 196885`
        //     ≈ 4.1 ms；且 `GetLastCycle -> 7661` 恰好等于"内存 7661 条"。
        //
        //   ⇒ uVision 的 Navigation 条 / 时间列全错（**总长度异常**），请求落在
        //     "轴上没有记录"的地方就只能回 `BOUNDS`（**选不中某个时间点**）。
        //
        //   ⇒ 认领改由 `AdvanceFrom()` 在**第一次非零增量**时做（它本来就负责认领，
        //     见那里的规则 ①）。这样"不推进的来源"永远拿不到轴，轴留给真正在推进
        //     的那一条（本板就是 ITM 时间戳，§10.36 的结论）。
        s.etmHaveAbs  = true;
        s.etmLastAbs  = absolute;
        s.synthCycles = 0;
        ORB_LOG_INFO("TraceWin: ETM %s base = %llu（建立周期轴基准，不推进轴；"
                     "**不认领**轴 —— 认领要等第一次非零增量）",
                     what ? what : "?", (unsigned long long)absolute);
        return;
    }

    if (absolute < s.etmLastAbs) {
        // 计数器回绕或目标复位：差值没有意义，重建基准（**不**推进轴 —— 猜一个
        // 增量只会把时间轴推到一个假的位置）。
        ++s.etmRebases;
        s.etmLastAbs  = absolute;
        s.synthCycles = 0;

        // **限流**：正常只有回绕/复位才会走到这里（一次采集段几次），但**解码出错
        // 的时间戳流会反复变小** —— 那就变成每个包一行日志（实测 3 s 2.4 万个包）。
        // 首条 WARN + 之后每 4096 条一条 INFO：异常一眼可见，日志不被刷爆。
        if (s.etmRebases == 1) {
            ORB_LOG_WARN("TraceWin: cycle axis rebase #1（ETM %s 变小：新值 %llu < 旧值，"
                         "重建基准）—— 若这个计数持续增长，说明该来源的值不是单调的"
                         "（回绕/复位，或**解码有错**）",
                         what ? what : "?", (unsigned long long)absolute);
        } else if ((s.etmRebases & 0xFFFu) == 0) {
            ORB_LOG_INFO("TraceWin: cycle axis rebase #%llu（ETM %s，最新值 %llu）",
                         (unsigned long long)s.etmRebases, what ? what : "?",
                         (unsigned long long)absolute);
        }
        return;
    }

    const uint64_t delta = absolute - s.etmLastAbs;
    s.etmLastAbs = absolute;
    // 同上：**只计数，不推轴**（轴已由墙钟拥有）。本板这条恒 0 —— ETM 周期计数
    // 不实现、ETM 时间戳包恒 0，见上面的说明。
    s.etmCycles += delta;
}

void TraceWinCycleAxisLog(const char* where)
{
    const Store& s = S();
    StoreGuard guard;
    ORB_LOG_INFO("TraceWin: cycle axis(%s) source=%s cycles=%llu records=%llu "
                 "itm=%llu etm=%llu rebase=%llu reject=%llu bTS=%u",
                 where ? where : "?",
                 SrcName(s.cycleSrc),
                 (unsigned long long)s.cycle,
                 (unsigned long long)s.count,
                 (unsigned long long)s.itmCycles,
                 (unsigned long long)s.etmCycles,
                 (unsigned long long)s.etmRebases,
                 (unsigned long long)s.axisRejects,
                 s.realAxis ? 1u : 0u);

    // P1-1 的判据：日志报**实际分配量**。报的是**峰值**而不是当前值 —— 本函数
    // 可能落在 TraceWinClear() 之后（实测：采集停止时先清、后打），当前值那时
    // 恒为 0。`持有` 是会话内分配过的字节（含空闲链上的块：那部分不还给系统，
    // 见 NewWriteChunk ①），它天然只增不减，也是"实际分配量"的直接证据。
    // `满块均=N B/条` 是 P1-2 的容量判据实测值：已写满的块里"每条记录平均占多少
    // 字节"（旧表示固定 22 B/条）。`记录峰值` 与它的比值就是"同容量能装多少条"。
    const unsigned long long avgRec = s.retiredRecs
                                    ? (s.retiredBytes / s.retiredRecs) : 0;
    ORB_LOG_INFO("TraceWin: buf(%s) 块峰值=%u/%u（每块 %u MB）记录峰值=%llu "
                 "满块均=%llu B/条（%llu 块）持有=%llu MB 上限=%llu MB "
                 "条数上限=%llu 摘块=%llu 分配失败=%llu 退化=%llu 丢记录=%llu "
                 "落盘=%llu 块 等盘=%llu 次 丢历史=%llu 块/%llu MB "
                 // `条数上限` = `trmax=` 的值：**0 才是正常**（窗口只受字节上限 `trmem=`
                 // 约束）。非 0 ⇒ 选项串里还留着 `trmax=`，那时才可能看到 `上限裁掉` > 0
                 // （>0 = 条数上限真的动过手；`Todo.md §4.3` 那次现场它是 81,608,143，
                 // 而内存只用了 552 MB/1024 MB —— 整段本来装得下）。
                 // `本段起点` = `segFirstCycle`：它就是 TDI `GetFirstCycle` 现在报的位置
                 // （见 FirstReportedCycle），本段第一条记录进来时赋、清空时归零。
                 // `累计写入(本段)` = recSeq，与 count/retiredRecs/capTrimmedRecs 构成
                 // 不变量的锚点，三项相加应等于它。
                 "上限裁掉=%llu 条 本段起点=%llu 累计写入(本段)=%llu 条",
                 where ? where : "?",
                 s.peakChunks, s.maxChunks,
                 static_cast<unsigned>(s.chunkCapBytes >> 20),
                 s.peakCount,
                 avgRec, s.nDropped + s.nDegraded,
                 s.memHeld >> 20, s.memLimit >> 20,
                 static_cast<unsigned long long>(s.recCap),
                 s.nDropped, s.nAllocFail, s.nDegraded, s.nAppendDrop,
                 s.rqWritten, s.rqWaits,
                 s.rqLostChunks, s.rqLostBytes >> 20,
                 s.capTrimmedRecs, s.segFirstCycle, s.recSeq);
}

void TraceWinSetBuffer(unsigned chunkMB, unsigned limitMB, unsigned long long recMax)
{
    Store& s = S();

    // `trmax=`（第 1 号）只是一个**比较值**：不参与任何一次性分配 ⇒ **随时可改**，
    // 所以放在最前面，也放在下面"索引已分配就忽略"那个早退**之前**。
    // ⚠ 默认 0 = **不限**（窗口只受字节上限 `trmem=` 约束）。原来硬编码的 `1<<20` 在
    //   ETM 现场把"能回看多久"钉死在 **0.13 s**，静默吃掉 81,608,143 条
    //   （定性见 `Todo.md §4.3`）—— 所以它必须由用户显式打开，不能再是默认。
    if (s.recCap != recMax) {
        s.recCap = recMax;
        ORB_LOG_INFO("TraceWin: 记录条数上限 = %llu 条（0 = 不限：窗口只受 %u MB 字节"
                     "上限约束；非 0 时触顶走整块退休 —— 记录日志开着就落盘，"
                     "没开就报 Data Overflow）",
                     recMax, limitMB);
    }

    // 没设就什么都不做：正常会话里这两条选项都是 0，不该在每次 Connect 刷日志。
    if (!chunkMB && !limitMB) return;

    StoreGuard g;
    // 索引数组按上限**一次性**分配（Ensure），所以这两个值只在第一次 Connect
    // （建索引之前）有效。之后调用只记一条日志，不偷偷改变量纲。
    if (s.idx) {
        ORB_LOG_WARN("TraceWin: trchunk/trmem 只在第一次 Connect 时生效"
                     "（索引已按 %u 块分配），本次忽略（每块 %u MB / 上限 %u MB）",
                     s.idxCap, chunkMB, limitMB);
        return;
    }
    // 合理性夹取：这两个值来自手写的选项串，写错一个数量级不该把会话拖垮。
    // 0 表示"用默认值"，不参与夹取。
    if (chunkMB && chunkMB < kChunkMBMin) chunkMB = kChunkMBMin;
    if (chunkMB > kChunkMBMax)            chunkMB = kChunkMBMax;
    if (limitMB > kMemMBMax)              limitMB = kMemMBMax;

    s.reqChunkMB = chunkMB;
    s.reqMemMB   = limitMB;
    ORB_LOG_INFO("TraceWin: 缓冲规模由选项指定 —— 每块 %u MB、上限 %u MB"
                 "（0 表示用默认值）", chunkMB, limitMB);
}

void TraceWinSetTimestampsVisible(bool on)
{
    Store& s = S();
    if (s.showTs == on) return;
    s.showTs = on;
    if (on) {
        ORB_LOG_WARN("TraceWin: 时间列已打开（bHideTimestamps=0）—— 它同时是取数门"
                     "的一环（§10.20.1），窗口若变空白就把 tswin 关回去");
    } else {
        ORB_LOG_INFO("TraceWin: 时间列已隐藏（bHideTimestamps=1，取数门保持打开）");
    }
}

void TraceWinClear()
{
    Store& s = S();

    // ⚠ **先等在飞的块落完盘**：写线程手上可能还拿着块，不等它把块还回来就把整条链
    //   搬进空闲链，会出现"一块同时属于空闲链和写线程"的双重归属。
    //   必须在取结构锁**之前**做 —— 写线程要取结构锁才能还块（见 RetireDrain）。
    RetireDrain();

    StoreGuard guard;

    // 所有块回到空闲链。**内存不还给系统**：反复 Connect/Disconnect 不该反复
    // 分配 4 MB 的块（`memHeld` 是会话间的高水位，`nChunks` 不会重复增长）。
    // 记录内容不用清：查询只走 `used` 之内的槽位，而 Append 自己会清零每条记录。
    while (s.head) {
        Chunk* c = s.head;
        s.head = c->next;
        c->prev = c->next = nullptr;
        c->usedBytes = 0;
        c->nRecs     = 0;
        c->nCkpt     = 0;
        c->firstCycle = c->lastCycle = 0;
        c->headOfs        = 0;    // 进空闲链前清干净，别把裁剪游标留给下一个用户
        c->deadRecs       = 0;
        c->headFirstCycle = 0;
        c->headRecSeq     = 0;
        c->firstRecSeq    = 0;    // ★ 与另外三处复位（Chunk 分配 / NewWriteChunk / 归还
                                  //   空闲链）保持**同一集合**，免得只读本函数的人以为
                                  //   这里漏了（那两处确有兜底，这里是纯一致性）
        c->next = s.freeList;
        s.freeList = c;
    }
    s.tail = nullptr;
    s.idxCount = 0;
    s.count = 0;
    s.cycle = 0;
    // 序号是"这一段里的第几条"，随清空归零（P4-1）。
    s.recSeq = 0;
    // "本段起点"（= 轴原点那一刻 / `ETM cycle count base` 那条日志的位置）同样是
    // **段属性** ⇒ 与 `recSeq` 同一个段界、同处清零（下一条记录进来时重赋）。
    // ⚠ 别挪到 `TraceWinSetRunning(true)`：那里是"只 start 不清空"（stop → 再 start）
    //   也会走到的，单方面清零会把"本段起点"抹掉，`FirstReportedCycle()` 就退回
    //   "最旧可读" —— 正是这次要修的症状。
    s.segFirstCycle    = 0;
    s.segFirstCycleSet = false;
    // ★ 不变量 `recSeq == count + retiredRecs + capTrimmedRecs` 的**五个量必须同处
    //   清零**（`count`/`recSeq` 就在上面几行）。段界是"清空"（见上："序号是这一段里的
    //   第几条，随清空归零"），少清一个就报错账：`count=0` 而 `retiredRecs` 还挂着上一段
    //   的 T ⇒ `0 == 0 + T + 0` 为假。原 `retiredBytes/retiredRecs` 只清在
    //   `TraceWinSetRunning(true)`，那是**可达的**自相矛盾：① 只 Clear（TDI `pClearTrace`
    //   随时可来）⇒ 它不清；② 只 start 不清空（stop → 再 start）⇒ 在那里单方面清零。
    //   挪到这里后三种顺序（Clear+start / 只 start / 只 Clear）全部成立。
    s.capTrimmedRecs = 0;
    s.retiredBytes   = 0;
    s.retiredRecs    = 0;
    // 游标指向的块已经不在链上了 —— 必须作废，否则下一次查询会按一个失效的
    // `chunkSeq` 去定位（那会解出垃圾）。P4-2。
    s.qcur.valid = false;

    // ⚠ 这里**不**清"峰值 / 诊断"类账目（nDropped / nAllocFail / nDegraded /
    // nAppendDrop / peakCount / peakChunks）：`buf(...)` 那行日志会落在本函数之后
    // （实测：采集停止时先清、后打），清掉就只剩 0。那几项在**每段采集开始时**清，
    // 见 TraceWinSetRunning()。
    // ★ 与之相反，**不变量账目**（recSeq / count / capTrimmedRecs / retiredBytes /
    //   retiredRecs）**一律在本函数清**（见上面那个清零块）—— 因为段界就是"清空"。
    // 新的一段采集从"还没有真实时间轴"开始，等第一个时间戳包/周期计数包到达再
    // 置起来。不清掉的话，这一段里时间戳还没到的那些记录会被标成 bTS=1（谎报时间）。
    s.realAxis = false;

    // 周期轴的所有者与账目同样按采集段清零：上一段的来源不该继续拥有这一段，
    // ETM 的绝对计数器基准也必须重建（目标可能已经复位、计数器从别处开始）。
    s.cycleSrc     = kCycleSrcNone;
    s.synthCycles  = 0;
    s.etmHaveAbs   = false;
    s.etmLastAbs   = 0;
    s.etmRebases   = 0;
    s.itmCycles    = 0;
    s.etmCycles    = 0;
    s.itmIgnored   = false;

    // 轴的墙钟起点：ClearTrace 之后算新的一段（轴从 0 重新开始走）。
    // ⚠ 被拒的计数**不在这里清** —— `cycle axis(...)` 那行日志可能落在本函数之后
    //   （与 `buf(...)` 同一个坑），清了就只剩 0。它在 TraceWinSetRunning() 里清。
    s.segStartTick = GetTickCount64();
    s.segStopTick  = 0;             // 新的一段：墙钟重新开始走

    // 落盘（P2-2）：记录日志里"内存窗口之前那一段"也一起清掉 —— 否则清空窗口之后
    // 还能把旧记录翻出来（TDI 的 ClearTrace 语义是"清空"）。
    TraceFileClearRecords();

    // ⚠ **清完要通知 uVision 刷新，否则窗口不归零**（2026-10-14 用户实测：
    //   "窗口没有归零，还是有数据清不掉"）。原来只把自己这边清干净、一个通知都不发
    //   ⇒ uVision 不重新取数 ⇒ 窗口留着上一次的行。
    //
    // 依据是 uVision 的取数门链（本文件上方反汇编记录，来自 UV4.exe
    // `CTraceWin::UpdateStatus 0x45D410` / `FetchRecords 0x45DB10`）：
    //       bProcessing != 0                  -> 不取数
    //       [0xD8] != 0 (= bSkipViewUpdate)   -> 直接退出，不重新取数
    //
    // ⚠⚠ **不能用 `TraceWinShowRange(0, 0)`**：那条通道会把 `bExplicitRange` 置 1
    //    并**把区间钉死成 [0,0]** ⇒ uVision 之后一直按空区间取数 ⇒ **数据再也出不来**
    //    （2026-10-14 用户实测截图：窗口全空、Navigation 停在旧轴）。我第一版就是这么
    //    写错的，别再犯。
    //
    // 正解：走 `StatusUpdate()` —— 它只置 `bSkipViewUpdate = 0`（**强制刷新**），
    // `bExplicitRange` 保持 0（**不钉区间**，让 uVision 自己按当前轴取数）。
    // `processing` 照实报（采集中的话 uVision 本来就不取数，那是它的门，不是我们的）。
    //
    // ⚠ 本函数也被 `TraceWinUninit()` 与会话建立时调（后者在 `TraceWinInit()` 之前）：
    //   那两条路上 `registered` 已经/仍是 false，`StatusUpdate()` 自己会 early-return
    //   （它开头就查 `bom`），所以不会往正在关闭或还没开的窗口发通知。
    StatusUpdate(TraceWinIsRunning(), /*updateView=*/true);
}

void TraceWinDrain()
{
    RetireDrain();
}

void TraceWinSetRunning(bool bState, bool bUpdate)
{
    Store& s = S();
    if (!s.registered) return;
    if (s.running == bState) return;   // 只在整体状态变化时通知（同 DbgCM）

    s.running = bState;
    // 墙钟窗口必须在 `StatusUpdate()` **之前**定下来：那里面会调 SyncAxisToWall()
    // 给 uVision 报"这一刻"的周期号，若还用上一段的起点，报出去的"现在"就是假的。
    if (bState) { s.segStartTick = GetTickCount64(); s.segStopTick = 0; }
    else        { s.segStopTick  = GetTickCount64(); }

    StatusUpdate(bState, bUpdate);

    if (bState) {
        // 新的一段采集：**按段诊断**账目从零开始（丢记录 / 分配失败 / 退化 / 交接层计数）。
        // ⚠ `retiredBytes`/`retiredRecs` 已从这里**挪走**：它们是不变量账目，必须与
        //   `recSeq` 共用同一个段界（= 清空），见 `TraceWinClear()`。留在这里会有
        //   "只 start 不清空"（stop → 再 start）时单方面清零 ⇒ 反而把
        //   `recSeq == count + retiredRecs + capTrimmedRecs` 打破。
        //   峰值类（`peakCount`/`peakChunks`）语义是"清空后仍有效"，留在这里是对的。
        s.nDropped = 0;
        s.nAllocFail = 0;
        s.nDegraded = 0;
        s.nAppendDrop = 0;
        s.peakCount = s.count;
        s.peakChunks = s.idxCount;

        // 退休队列（P3-1）的账目同样按段清：这几项是"交接层到底有没有起作用"
        // 的判据 —— 落盘块数 > 0 说明异步落盘真的跑了；丢历史块数 > 0 说明
        // 内存/磁盘都没接住（那时状态栏同时会有 Data Overflow）。
        s.rqWritten    = 0;
        s.rqWaits      = 0;
        s.rqLostChunks = 0;
        s.rqLostBytes  = 0;
        s.rqLostWarned = false;
        // "条数上限到顶且没落盘"同样每段只吼一次（判据见 EnforceRecCap）。
        s.capLostWarned = false;

        // "被拒增量"计数按段清零（墙钟起点已在上面、`StatusUpdate()` 之前定好）。
        s.axisRejects  = 0;
        s.axisWarned   = false;
    } else {
        // 停止：把峰值定下来。此后 `buf(...)` 那行即使落在 TraceWinClear() 之后，
        // 报出来的也还是这一段真实攒下的量。
        if (s.count > s.peakCount) s.peakCount = s.count;
        if (s.idxCount > s.peakChunks) s.peakChunks = s.idxCount;
    }

    // 窗口空白时，第一个要回答的问题是"缓冲里到底有没有记录"：有，问题在
    // uVision 的查询或窗口配置；没有，问题在喂数据那一段。没有这一行两边
    // 看起来都是"什么都没发生"，只能靠猜。
    ORB_LOG_INFO("TraceWin: running=%d, 缓冲 %llu 条记录 / %u 块",
                 bState ? 1 : 0, (unsigned long long)s.count, s.idxCount);
}

bool TraceWinIsRunning()
{
    return S().running;
}

// ---------------------------------------------------------------------------
// Trace 选项与状态栏上报
// ---------------------------------------------------------------------------

// 官方 `DbgCM/Trace.cpp:45-54`，表原样（含 `\<clrr>` 红色前导）。索引 0 是空串，
// 供"清空状态栏"用。官方注释：优先级 High -> Low。
static const char* const kTraceMsg[] = {
    "",
    "\\<!clrr>Trace: Communication Error",
    "\\<!clrr>Trace: HW Buffer Overrun",
    "\\<!clrr>Trace: SW Buffer Overrun",
    "\\<!clrr>Trace: No Synchronization",
    "\\<!clrr>Trace: Data Stream Error",
    "\\<!clrr>Trace: Data Overflow",
    "Trace: Running ...",
};
static const unsigned kTraceMsgCount = sizeof(kTraceMsg) / sizeof(kTraceMsg[0]);

// 官方 `TraceOpt`——生效的 `TraceConf.Opt`（DbgCM/Trace.cpp:114）。
static uint32_t g_traceOpt = 0;

// 官方 `T_Secure`——最近一次上报的 trace 安全状态（DbgCM/Trace.cpp:116）。
static bool g_tSecure = false;

void TraceWinSetOptions(uint32_t opt) { g_traceOpt = opt; }
uint32_t TraceWinOptions() { return g_traceOpt; }

// ---------------------------------------------------------------------------
// 状态栏：**排队**与**投递**分开（2026-10-14）
// ---------------------------------------------------------------------------
// `bom->Notify(UV_RTAH_ERRMSG, ...)` 是跨线程投递到 uVision 的 GUI 线程的调用。
// 它有两个调用方线程：
//   - 调试线程：`TraceSetRunning`、`TraceWinTraceStatus`（ETB 路径）；
//   - **抽数 worker**：`TraceWinRead()` 的错误折叠、`TraceWinRecoverTick()`。
// worker 每一轮都要走一遍折叠，于是每一轮都可能站在 uVision 的 GUI 线程门口等
// 一次。实测那一轮的平均周期是 21 ms（设计 2 ms；`TracePump` 停止日志里
// 轮=3806 / 79.8 s 就是它）。状态栏那一行本来就只在**变化**时才需要发，把
// "决定显示什么"和"真的去投递"绑在同一个函数里，等于让抽数线程替 GUI 打工。
//
// 修法：
//   - `TraceWinStatusMsg()` / `TraceWinStatusText("")` 只把一个**消息码**写进
//     `g_pendMsg`（单个 `LONG`，`InterlockedExchange` 写、读即清，无锁无竞态）；
//   - `TraceWinStatusFlush()` 才是真正 `Notify` 的那一下，由**调试线程**调用
//     （它的轮询循环本来每轮都跑，那里调用等于零成本）。
// 语义不变：同一时刻只有一条待发布消息，晚到的覆盖早到的。与"就地发布"相比
// 最多晚一个调试线程轮询周期（运行中是毫秒量级）。
enum : LONG { kPendNone = -1 };
static volatile LONG g_pendMsg = kPendNone;   // -1 = 无；0 = 清空；1..7 = kTraceMsg 索引

// 官方 `OutMsg`（DbgCM/AGDI.CPP:702-707）：状态栏一栏文本。
void TraceWinStatusText(const char* text)
{
    if (text && *text) {
        // 没有调用方传非空文本（唯一用法是 `TraceWinTraceStatus()` 末尾的
        // `OutMsg("")`）。真有的话只能就地发布 —— 这条路径不能在 worker 上走。
        struct bom* bom = orb::pdsc::Bom();
        if (!bom || !bom->Notify || !bom->hwnd || !bom->hmsg) return;
        bom->Notify(UV_RTAH_ERRMSG, const_cast<char*>(text));
        return;
    }
    InterlockedExchange(&g_pendMsg, 0);          // 0 = 清空那一栏
}

// 官方 `OutTraceMsg`（DbgCM/AGDI.CPP:713-743）：把一条消息码渲染成文本并投递。
//
// 与官方唯一的形式差异：缓冲区拷贝用 `memcpy` / `orb::fmt::Append` 而不是
// `strncpy_s` + `sprintf`，因为本工程的编译选项把不安全 CRT 标为错误；
// 且 `sprintf_s` 在截断时会走 invalid parameter handler（/MT 下等于终止宿主）。
// 逻辑与分支完全一致。
static void PublishStatusMsg(unsigned msg)
{
    if (msg >= kTraceMsgCount) return;

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify || !bom->hwnd || !bom->hmsg) return;

    char   buf[128] = {0};
    size_t n      = 0;
    int    clrLen = 0;

    if (g_traceOpt & kTraceEnable) {
        if (bom->bSecureExt && msg != 0) {
            // 颜色前导 `\<clrX>` 共 8 个字符，先原样搬进缓冲区。
            char clr = 0;
            if (sscanf_s(kTraceMsg[msg], "\\<!clr%c>", &clr, 1) == 1) {
                clrLen = static_cast<int>(strlen("\\<!clrx>"));
                memcpy(&buf[n], kTraceMsg[msg], clrLen);
                n = static_cast<size_t>(clrLen);
            }
            if (bom->bSTrcEna) {
                orb::fmt::Append(buf, sizeof(buf), n, "Secure ");
                g_tSecure = true;
            } else {
                orb::fmt::Append(buf, sizeof(buf), n, "Non-Secure ");
                g_tSecure = false;
            }
        }
    }

    orb::fmt::Append(buf, sizeof(buf), n, "%s", kTraceMsg[msg] + clrLen);
    bom->Notify(UV_RTAH_ERRMSG, buf);
}

// 排队（见本段开头的说明）。抽数 worker 走的就是这里，所以它必须便宜。
void TraceWinStatusMsg(unsigned msg)
{
    if (msg >= kTraceMsgCount) return;
    InterlockedExchange(&g_pendMsg, static_cast<LONG>(msg));
}

// 投递（见本段开头的说明）。只能在调试线程上调用。
void TraceWinStatusFlush()
{
    const LONG code = InterlockedExchange(&g_pendMsg, kPendNone);
    if (code < 0) return;                        // 没有待发布的
    PublishStatusMsg(static_cast<unsigned>(code));
}

// ---------------------------------------------------------------------------
// Trace 错误掩码与状态折叠
// ---------------------------------------------------------------------------

// 官方 `T_Err` / `T_Msg`（DbgCM/Trace.cpp:56-57）。解码路径只置位掩码，由下面的
// 折叠函数决定状态栏显示哪一条。
static int      g_tErr = 0;
static unsigned g_tMsg = 0;

void TraceWinErrorSet(int mask) { g_tErr |= mask; }

// 官方 `ETB_TraceStatus()`（DbgCM/ETB.cpp:136-164）。
//
// 官方按**优先级**折叠：只把 `T_ERR_DATA_ERR` 和 `T_ERR_DATA_OVF` 变成消息，且
// 仅当新消息的优先级更高（消息码更大）时才覆盖当前显示。其余位只参与末尾的
// "还有错误吗"判断。
//
// 一处**照抄但值得留意**的行为：DATA_ERR / DATA_OVF 被折叠时会就地清位，于是
// 若它们原本是仅有的置位项，末尾的 `if (T_Err) ; else OutMsg("")` 会在刚显示
// 消息之后立刻把状态栏清空。官方代码如此，这里保持一致，没有"顺手修正"。
//
// 返回值：官方返回 0（该函数只为副作用而调用）；这里返回当前消息码，便于日志。
unsigned TraceWinTraceStatus()
{
    if (g_tErr & kTErrDataErr) {
        g_tErr &= ~kTErrDataErr;
        if (g_tMsg >= kTMsgDataErr) {
            if (g_tMsg != kTMsgDataErr) {
                g_tMsg = kTMsgDataErr;
                TraceWinStatusMsg(g_tMsg);
            }
        }
    }
    if (g_tErr & kTErrDataOvf) {
        g_tErr &= ~kTErrDataOvf;
        if (g_tMsg >= kTMsgDataOvf) {
            if (g_tMsg != kTMsgDataOvf) {
                g_tMsg = kTMsgDataOvf;
                TraceWinStatusMsg(g_tMsg);
            }
        }
    }

    if (g_tErr) {
        ;                                   // 还有错误待处理，保留当前消息
    } else {
        g_tMsg = 0;
        TraceWinStatusText("");             // 官方 OutMsg("")
    }

    // 上面两句都只是排队（见 TraceWinStatusMsg 的说明），这里补上投递。
    // 本函数只在 ETB 路径（调试线程）上被调用，所以这里投递是安全的。
    TraceWinStatusFlush();

    return g_tMsg;
}

// ---------------------------------------------------------------------------
// 采集路径（官方 Trace_Flush / Trace_Read）
// ---------------------------------------------------------------------------

// 官方 `SyncPeriod` / `SyncTimeout`（Trace.cpp:132-133）与 `T_Recover`（:58）。
static int g_syncPeriod  = 0;
static int g_syncTimeout = 0;
static int g_tRecover    = 0;

void TraceWinSetSyncPeriod(unsigned ms)
{
    g_syncPeriod  = static_cast<int>(ms);
    g_syncTimeout = g_syncPeriod;
}

// 官方 `Trace_Read` 里反复出现五遍的同一段折叠逻辑（Trace.cpp:2017-2038、
// 2065-2097），抽成一个函数以免抄五遍。与 `ETB_TraceStatus` 的差别是这里会置
// `T_Recover` 倒计时。
static void FoldErr(int errBit, unsigned msg)
{
    if (g_tErr & errBit) {
        g_tErr &= ~errBit;
        if (g_tMsg >= msg) {
            g_tRecover = kTRecover;
            if (g_tMsg != msg) {
                g_tMsg = msg;
                TraceWinStatusMsg(g_tMsg);
            }
        }
    }
}

// 官方 `Trace_Flush`（DbgCM/Trace.cpp:1777-1808）。
void TraceWinFlush(bool setRun)
{
    // 官方的 setRun 只影响它自己那次 DWT CYCCNT 基准读取（:1781-1789），本驱动
    // 的 DWT 周期计数由 ETM/ITM 路径各自管理，所以这里不需要它。
    (void)setRun;

    // 官方重置的是**解码缓冲**（TD_/TP_/TR_ 的 head/tail）与错误状态，不是 Trace
    // Data 窗口的内容——窗口要留给用户看。本驱动对应的解码同步点在各自的源启动
    // 时重置（OrbTraceStart / EtbProcess 里的 ItmResyncDecoder），不在这里重复。
    g_tErr        = 0;
    g_tRecover    = 0;
    g_syncTimeout = g_syncPeriod;
}

// 官方 `Trace_Read`（DbgCM/Trace.cpp:2003-2129）的状态折叠那一半。
void TraceWinRead(unsigned timeMs, bool ts)
{
    if (!(g_traceOpt & kTraceEnable)) return;

    if (timeMs) {
        const uint64_t tick = GetTickCount64();
        do {
            // 官方在取数循环里折叠通信错误与硬件缓冲溢出。
            FoldErr(kTErrHwCom, kTMsgComErr);
            FoldErr(kTErrHwBuf, kTMsgHwBuf);
        } while ((GetTickCount64() - tick) < timeMs);

        // 失去同步看门狗（:2040-2046）。官方无条件跑；本驱动在 SyncPeriod 未知时
        // （没有权威核心时钟，bug.md §R6）跳过——否则第一轮就会报一个假的
        // "No Synchronization"。
        if (ts && g_syncPeriod > 0) {
            g_syncTimeout -= static_cast<int>(timeMs);
            if (g_syncTimeout <= 0) {
                g_syncTimeout = g_syncPeriod;
                g_tErr |= kTErrNoSync;
            }
        }
    }

    // 官方在 TR_Read / TR_Process 之后折叠这几类（:2065-2097）。
    FoldErr(kTErrSwBuf,   kTMsgSwBuf);
    FoldErr(kTErrNoSync,  kTMsgNoSync);
    FoldErr(kTErrDataOvf, kTMsgDataOvf);

    // 自动恢复倒计时（:2103-2111）：错误显示一段时间后回到 "Trace: Running ..."。
    TraceWinRecoverTick(timeMs);
}

// 官方把这段写在 `Trace_Read` 末尾、按 `time` 递减。抽数 worker 传 `timeMs = 0`
// （它不能空转，见头文件），所以倒计时得由调用方按真实经过时间推进。
void TraceWinRecoverTick(unsigned elapsedMs)
{
    if (!g_tRecover) return;

    g_tRecover -= static_cast<int>(elapsedMs);
    if (g_tRecover <= 0) {
        g_tRecover = 0;
        g_tMsg     = kTMsgRun;
        TraceWinStatusMsg(g_tMsg);
    }
}

} // namespace agdi
} // namespace orb
