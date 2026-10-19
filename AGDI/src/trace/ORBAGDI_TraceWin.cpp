/**
 * @file ORBAGDI_TraceWin.cpp
 * @brief uVision "Trace Data" 窗口的上报通道（Keil TDI 标准流程），Notify 统一走 BOM。
 */
#include "pch.h"
#include "ORBAGDI_TraceWin.h"
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_Etb.h"
#include "ORBAGDI_TraceFile.h"   // TraceFileMark（P2-1 的 cache 索引）
#include "ORBAGDI_Cover.h"       // 覆盖率 / 性能分析：每解出一条指令记一次

#include <stdio.h>
#include <string.h>
#include <process.h>   // _beginthreadex（退休队列的后台写线程）

// uVision 侧接口头：BOM.H 提供 struct bom 与全局 pio，TraceView/TraceData*.h 提供 TDI。
// BOM.H 会拉进 ComTyp.h，TDI 的类型照它写，所以必须排在前面。
#include "BOM.H"

// TraceDataTypes.h 用到 DOUBLE（TRACE_CLOCK_SAMPLE::m_dLastTime），
// 本仓库精简的 COMTYP.H 没有这个 typedef，补一个放在 Keil 头之前。
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

// 记录缓冲 = 块链 + 紧凑记录：4 MB 一块串成链表，块指针按 lastCycle 升序可二分；
// 记录存 varint 周期差（ETM 7 B/条），每块 256 检查点支持块内半路查询。
// 追加不取锁（先写字节流、最后发布 usedBytes），结构改动才取 storeLock。
const unsigned kChunkBytes   = 4u << 20;   // 每块 4 MB（§九：4–16 MB 里取下限）
// 内存窗口上限默认 256 MB（`trmem=` 或环境变量 `ORBAGDI_TRACE_MEM_MB` 可覆盖）。
//
// ⚠ 原来是"物理内存 1/4，封顶 1 GB"。2026-10-19 实测推翻了这个上限：抽/解分离之后
// 解码不再丢数据，ETM 会把分支之间的每条指令展开成记录 ≈ 550 万条/s，**30 秒就攒到
// 1.03 亿条 / 172 块 / 688 MB**。而 uVision 是 32 位进程，它在 RunStart 之后按这个
// 条数去取数（1.03 亿 × 16 B ≈ 1.6 GB），拿到数就消失了 —— 日志停在
// `TraceWin: running=1, 缓冲 103047588 条记录 / 172 块`，之后再无任何输出。
//
// 1 GB 的窗口它从来吃不下：设那么大，唯一的效果是让缓冲在崩掉之前先长到不可用的规模。
// 256 MB ≈ 3800 万条，uVision 侧约 600 MB，是保守的起点；调大前先用
// `ORBAGDI_TRACE_MEM_MB` 试，别直接改这个常量。
const unsigned kMemLimitMax  = 256u << 20;
const unsigned kMemLimitMin  = 16u << 20;  // 上限下限 16 MB
const unsigned kMemLimitFrac = 4;          // 上限 = 物理内存的 1/4
// 选项 trchunk= / trmem= 的合理范围（MB）。写错一个数量级不该把会话拖垮。
const unsigned kChunkMBMin   = 1;
const unsigned kChunkMBMax   = 64;
const unsigned kMemMBMax     = 4096;
// 每块的检查点条数（见上）。256 ⇒ 每次查询最多解 1/256 块。
const unsigned kCkptPerChunk = 256;

// ---- 记录条数上限（trmax=）----
// 默认 0 = 不限，只受字节上限约束。旧硬编码 1<<20 在 ETM 下只留 0.13 s 历史，
// 且就地裁剪不写 cache、不报警，架空了内存上限；现在只走整块退休并响亮报错。
const uint64_t kMaxRecsSlack = 4096;       // 超出这么多条才动一次手，摊薄解码成本


// ---- 退休队列：抽数 ↔ cache 的交接层 ----
// 官方靠 4 槽环解耦（逆向得 4），写满的块只把指针交给后台写线程；
// 原来在抽数线程同步 fwrite 4 MB，正是实测卡顿的来源。
const unsigned kRetireQ = 4;

// 队列满时抽数最多等多久（ms）。
// 官方无限等；这里必须设上限，否则抽数停住会让探针环溢出、整段缺数据。
// 等待发生在持 storeLock 的路径上，正常量级只是一次写盘（~40 ms）。
const DWORD kRetireWaitMs = 1000;

struct Chunk {
    Chunk*   prev;
    Chunk*   next;        // 空闲链上也用它
    uint64_t firstCycle;  // 第一条记录的周期号
    uint64_t lastCycle;   // 最后一条记录的周期号
        // 本块第一条记录的全局序号（块身份：单调、不复用，用于二分定位）。
        // 注意裁剪后“最旧”是 headRecSeq，不是 firstRecSeq。
    uint64_t firstRecSeq;
        // ---- 记录级裁剪 ----
        // 已改成整块退休，默认配置下 headOfs/deadRecs/headRecSeq 恒为 0，本机制休眠。
        // 字段保留：usedBytes - headOfs 的记账仍在用，将来做精确裁剪也要靠它。
    uint32_t headOfs;         // 可读区间起点（块内字节偏移）；必须落在记录边界上
    uint32_t deadRecs;        // headOfs 之前已裁掉的条数
    // `buf + headOfs` 处那条记录的**绝对**周期号，必须是某条真实记录的周期号
    // （**绝不能是 0**）：区间最旧周期号 / GetFirstCycle 直接报它。
    uint64_t headFirstCycle;
        // headOfs 处那条记录的全局序号，与 headFirstCycle 同步推进。
        // 单独存而不用 firstRecSeq + deadRecs：块被裁空后再写入会重定 firstRecSeq
        // 却不清 deadRecs，算式会偏大。
    uint64_t headRecSeq;
    unsigned usedBytes;   // 已写字节数，**最后**发布（读者靠它判断到哪是完整的）
    unsigned capBytes;
        // 当前可读条数（裁剪后递减），查询快路与日志都用它。
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
        // 已写满块的字节数与条数，比值即紧凑记录的实测平均大小（容量判据）。
    unsigned long long retiredBytes;
    unsigned long long retiredRecs;
        // 被记录级上限裁掉的条数，不能并入 retiredRecs（会破坏 avgRec 容量判据）。
        // 对外不变量：recSeq == count + retiredRecs + capTrimmedRecs。
    unsigned long long capTrimmedRecs;
    unsigned long long count;       // 常驻记录条数
        // 本段峰值，清空后仍保留：日志可能落在 Clear 之后，只有峰值能证明攒下多少。
    unsigned long long peakCount;
    unsigned         peakChunks;
    // 选项 trchunk= / trmem= 请求的规模（0 = 默认）。在 Ensure() 里应用一次。
    unsigned         reqChunkMB;
    unsigned         reqMemMB;
    // 选项 trmax= 请求的**常驻条数上限**（0 = 不限，**默认**）。见 kMaxRecsSlack 上面那段。
    // 与 reqMemMB 不同：它只是一个比较值（随时可改），不参与任何一次性分配。
    uint64_t         recCap;
        // 本段第一条记录的周期号（轴原点）。对外“可查范围左端”用它，不是最旧可读。
        // 报最旧可读会让 Navigation 条只画幸存的那一小段。
    uint64_t         segFirstCycle;
        // “本段起点已确定”的标志，不能用 segFirstCycle != 0 判断（0 是合法起点）。
    bool             segFirstCycleSet;
    uint64_t         cycle;         // 当前周期号
    // 上一条**记录**的周期号（P1-2：紧凑记录存的是相对它的差）。跨块连续 ——
    // 块的 firstCycle 是绝对值，所以差不必在块边界重置。
    uint64_t         lastRecCycle;

        // 全局记录序号：单调递增，只在清空时归零（摘块不回退）。
    uint64_t         recSeq;

        // 查询游标：uVision 按 nRecordOfs 翻页时接着上次位置走。
        // 没有游标时每页都从区间头重扫，实测放大 650×，UI 线程 35.7 s 无响应。
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

        // 结构改动的互斥。临界区可重入，公开函数互相调用不会自锁。
        // 命名 storeLock 说明它保护整个 Store 结构，而不是类型名。
    CRITICAL_SECTION storeLock;

    bool             registered; // TDI 回调表已交给 uVision
    bool             running;    // 正在采集
    bool             (*refresh)();

        // realAxis：周期号有真实来源，新记录 bTS 置 1。
    bool             realAxis;

        // ---- 周期轴的来源：只允许一个所有者，两个计数器同时接进来会重复计数 ----
    int              cycleSrc;      // kCycleSrc*
        // 记录自身推进掉的周期数，真实增量要减掉它，否则轴会被拉长 8%~17%。
    uint64_t         synthCycles;
    bool             etmHaveAbs;    // ETM 绝对周期计数的基准是否已建立
    uint64_t         etmLastAbs;    // 上一次的 ETM 绝对周期计数
    uint64_t         etmRebases;    // 计数器回绕/复位导致重建基准的次数
    uint64_t         itmCycles;     // ITM 时间戳贡献到轴上的周期数（日志用）
    uint64_t         etmCycles;     // ETM 周期计数贡献到轴上的周期数（日志用）
    bool             itmIgnored;    // "ITM 时间戳被忽略"只报一次
        // showTs：是否让 uVision 显示时间列，默认隐藏。
        // 它同时是 UV4 取数门的一环（bHideTimestamps），翻成 0 可能一行都不取。
    bool             showTs;

        // ---- 轴机制：ITM/SWO 追墙钟，ETM 只按解码推进 ----
        // ETM 追墙钟会造出空洞并让 uVision 全量取数，曾把 uVision 打崩，已回退。
        // segStopTick 为 0 表示仍在采集；停止后必须冻住，否则时间轴自己变长。
    int              axisMode;      // kAxisMode*（按 trace 源设，默认 ETM）
    uint64_t         segStartTick;  // 本段采集开始的墙钟毫秒（GetTickCount64）
    uint64_t         segStopTick;   // 本段停止的墙钟毫秒；0 = 仍在采集
    uint64_t         axisRejects;   // 被拒的不可能增量个数（日志用）
    bool             axisWarned;    // "拒绝"只 WARN 一次

        // ---- 退休队列 ----
        // 队列有自己的锁 retireLock：写线程腾空槽只需它，不需要 storeLock，避免互等。
    Chunk*           rq[kRetireQ];
    unsigned         rqHead;        // 队头（最旧，先写 cache）
    unsigned         rqTail;
    unsigned         rqCount;
    bool             rqBusy;        // 写线程手上正拿着一块（用于 Drain 判"真闲"）
    CRITICAL_SECTION retireLock;
    HANDLE           rqWork;        // 自动复位：有块待写
    HANDLE           rqSpace;       // 自动复位：有空槽（弹出即置位）
    HANDLE           rqThread;
    bool             rqStarted;
    // 现场证据（日志用）
    unsigned long long rqWritten;     // 已写 cache 块数
    unsigned long long rqWaits;       // 抽数为等空槽停下来的次数
    unsigned long long rqLostChunks;  // 没写 cache 就丢掉的块数（cache 没开/超时）
    unsigned long long rqLostBytes;
    bool               rqLostWarned;  // "丢历史"只报一次（每段）
    bool               capLostWarned; // "条数上限到顶且没写 cache"只报一次（每段）

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

// 结构锁的 RAII 守卫：查询有多条返回路径，漏一次 Leave 就是死锁。
// 取第一条可读记录所在的块：刚挂上还没写的新块与被裁空的块也可能是 head，
// 它们都不是“最旧”，凡对外报最旧都必须过这里。
const Chunk* FirstReadable(const Store& s)
{
    const Chunk* c = s.head;
    while (c && c->nRecs == 0) c = c->next;
    return c;
}

// 对外报的“可查范围左端”取本段起点，不是最旧可读记录。
// 三个出口（GetFirstCycle / m_pRecordRange / TraceWinFirstCycle）必须同值，
// 不一致时 uVision 会请求没有数据的区间。默认配置下两者相等。
uint64_t FirstReportedCycle(const Store& s)
{
    const Chunk* c = FirstReadable(s);
    uint64_t first = c ? c->headFirstCycle : 0;
        // 判据是标志位：0 是合法起点。内存里已无可读记录时也只能报本段起点。
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

// 把块内起始偏移夹到裁剪游标 headOfs 之后。
// from 必须落在记录边界（0 / ckptOfs[i] / headOfs），否则之后条条皆偏。
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

// ---- 退休队列：RetireStart 建线程、RetirePush 交块、RetireDrain 收尾 ----
// 写线程是进程级的，起一次不停；Store 故意泄漏，会话反复 Connect 不重建线程。
// 队列锁与结构锁分开：写线程腾空槽不碰 storeLock，不会互等。
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

                // 空槽在这一刻释放，不等写完：抽数最多领先 4 块，无需等磁盘 I/O。
        SetEvent(s.rqSpace);

                // 先记索引再原样追加字节流：索引的 ofs 必须是记录在文件里的位置。
                // 只写活的区间，索引周期取首条可读记录，否则回放会多出已裁掉的历史。
        if (c->usedBytes > c->headOfs) {
            TraceFileMarkRecords(c->headFirstCycle);
            TraceFileWriteRecords(c->buf + c->headOfs, c->usedBytes - c->headOfs);
        }

        // 还块：要结构锁。抽数此刻若正持锁，只会挡住这一小段（不影响空槽）。
        {
            StoreGuard g;
                        // 归还前清裁剪游标，保证空闲链上的块不带裁剪状态。
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
        ORB_LOG_WARN("TraceWin: 退休队列的事件建不出来 —— cache 写退回同步");
        if (s.rqWork)  { CloseHandle(s.rqWork);  s.rqWork  = nullptr; }
        if (s.rqSpace) { CloseHandle(s.rqSpace); s.rqSpace = nullptr; }
        return;                      // rqStarted 保持 false ⇒ 调用方走同步兜底
    }

    unsigned tid = 0;
    s.rqThread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, RetireBody, nullptr, 0, &tid));
    if (!s.rqThread) {
        ORB_LOG_WARN("TraceWin: cache 写线程创建失败 —— 退回同步写");
        CloseHandle(s.rqWork);  s.rqWork  = nullptr;
        CloseHandle(s.rqSpace); s.rqSpace = nullptr;
        return;
    }
    s.rqStarted = true;
    ORB_LOG_INFO("TraceWin: cache 写线程已启动（交接队列 %u 槽 / 每块 %u MB）"
                 "—— 抽数不再等磁盘 I/O",
                 kRetireQ, static_cast<unsigned>(s.chunkCapBytes >> 20));
}

// 把写满的块交给写线程。false = 没交出去，调用方必须原地复用（那段历史就没了）。
bool RetirePush(Chunk* c)
{
    Store& s = S();

        // 装不下又写不进 cache ⇒ 历史真的丢了，置 T_ERR_DATA_OVF 上报，不能静默。
    if (!TraceFileRecordsActive()) {
        s.rqLostChunks++;
        s.rqLostBytes += c->usedBytes - c->headOfs;
        if (!s.rqLostWarned) {
            s.rqLostWarned = true;
            ORB_LOG_ERROR("TraceWin: 内存缓冲已满（上限 %u 块 / %llu MB）而"
                          "**记录 cache 没开或已到上限（trrecmb=）** —— 最旧的历史"
                          "正在被丢弃。要留住它：选项串加 `trrec=1`、调大 "
                          "trrecmb=，或调大 trmem=",
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

                // 队列满就等写线程腾空槽；官方无限等，这里给上限（kRetireWaitMs）。
        if (!t0) { t0 = GetTickCount64(); ++s.rqWaits; }
        const uint64_t waited = GetTickCount64() - t0;
        if (waited >= kRetireWaitMs) {
            s.rqLostChunks++;
            s.rqLostBytes += c->usedBytes - c->headOfs;
            if (!s.rqLostWarned) {
                s.rqLostWarned = true;
                ORB_LOG_ERROR("TraceWin: cache 跟不上（队列 %u 块全满，等了 %u ms）"
                              "—— 丢弃最旧的一块以保住抽数（磁盘太慢？"
                              "trrecmb= 到顶后 cache 会停，检查那一条）",
                              kRetireQ, kRetireWaitMs);
                TraceWinErrorSet(kTErrDataOvf);
            }
            return false;
        }
        WaitForSingleObject(s.rqSpace, static_cast<DWORD>(kRetireWaitMs - waited));
    }
}

// 等在飞的块落完并还回空闲链。不持结构锁：写线程还块要结构锁，持锁等就死锁。
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
            ORB_LOG_WARN("TraceWin: 等 cache 写线程超时（队列还剩 %u 块）—— 不再等",
                         left);
            return;
        }
        Sleep(1);
    }
}

// 分配一块（块头 + 字节流一次分配）。不清零：查询只走 usedBytes 之内。
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

        // 块大小与上限（默认 4 MB/块，上限物理内存 1/4 封顶 256 MB）。
        // 惰性分配：没开 trace 的会话一个字节缓冲都不占。chunkCapBytes 是字节数。
    const unsigned chunkMB = s.reqChunkMB ? s.reqChunkMB : (kChunkBytes >> 20);
    s.chunkCapBytes = chunkMB << 20;
    if (s.chunkCapBytes < 4096) s.chunkCapBytes = 4096;

    unsigned long long limit;
    if (s.reqMemMB) {
        limit = static_cast<unsigned long long>(s.reqMemMB) << 20;
    } else {
        limit = kMemLimitMax;

        // 环境变量覆盖（16..2048 MB）：不改 uVision 的选项串也能二分"多大窗口不崩"。
        // 选项串 `trmem=` 仍然优先（走上面那一支）。
        {
            char  v[32] = {0};
            const DWORD n = GetEnvironmentVariableA("ORBAGDI_TRACE_MEM_MB", v, sizeof(v));
            if (n > 0 && n < sizeof(v)) {
                const int k = atoi(v);
                if (k >= 16 && k <= 2048) {
                    limit = static_cast<unsigned long long>(k) << 20;
                    ORB_LOG_INFO("TraceWin: 内存窗口上限被环境变量覆盖 = %d MB", k);
                }
            }
        }
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
                 "上限 %u 块 / %llu MB（物理内存 1/4 封顶 256 MB，惰性分配）；"
                 "cache 交接队列 %u 槽 —— 在飞的块另计，最多再占 %u MB",
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

// 把块接回链头并插回索引首位，只给“摘出来又交不出去”那条路用。
// DropOldest 动过的账要逐项回滚，保持 recSeq == count + retired + capTrimmed。
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

// 摘掉最旧的一块，调用方必须保证 idxCount >= 2。
// markFile：只有块真的离开内存时才记 OFLOW 索引。
Chunk* DropOldest(bool markFile = true)
{
    Store& s = S();
    Chunk* c = s.head;
    if (!c) return nullptr;

    s.head = c->next;
    if (s.head) s.head->prev = nullptr; else s.tail = nullptr;
    c->prev = c->next = nullptr;

    s.count -= c->nRecs;
        // 摘掉的块一定写满，比值即实测每条字节数；要减去已裁掉的前缀 headOfs。
    s.retiredBytes += c->usedBytes - c->headOfs;
    s.retiredRecs  += c->nRecs;
        // cache 索引：这段历史刚离开内存，记下它在文件里的位置。
    if (markFile) TraceFileMark(c->lastCycle);
        // 记录 cache 由写线程在 RetirePush 之后写；原来在这里同步写，正是卡顿原因。
    if (s.idxCount) {
        memmove(s.idx, s.idx + 1, (s.idxCount - 1u) * sizeof(Chunk*));
        --s.idxCount;
    }
    ++s.nDropped;
    return c;
}

// 要一块可写的块：空闲链 → 新分配 → 退休最旧的一块 → 原地复用。
// 返回 nullptr 表示缓冲不可用（索引没建起来）。
Chunk* NewWriteChunk()
{
    Store& s = S();
    StoreGuard g;

    Chunk* c    = nullptr;
    bool   link = true;

        // 空闲链的块不还给系统：反复 Connect/Disconnect 不该反复分配 4 MB。
    if (s.freeList) {
        c = s.freeList;
        s.freeList = c->next;
        c->next = nullptr;
    }

        // 未到上限：新分配。按链长而不是 nChunks 判，否则在飞的块白吃容量。
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

        // 链到上限：把最旧一块退休给写线程，自己立刻拿下一块继续。
    if (!c && s.idxCount >= 2) {
        Chunk* old = DropOldest();           // 摘出链（不动字节流），idxCount 已减
        if (old) {
            if (RetirePush(old)) {
                                // 交出去了 ⇒ 必须另取一块；此刻空闲链必空，直接新分配。
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
                                // 没交出去：指针不能丢，原地复用；错误已由 RetirePush 报过。
                c = old;
            }
        }
    }

        // 退无可退：原地复用最旧一块，丢这段历史。
        // 只有 trmem 只够一块或分配失败会走到这里。
    if (!c && s.head) {
        c    = s.head;
        link = false;
        s.count -= c->nRecs;
        s.retiredBytes += c->usedBytes - c->headOfs;  // 同上，且要减掉已裁前缀
        s.retiredRecs  += c->nRecs;
        TraceFileMark(c->lastCycle);         // cache 索引（P2-1），同上
        if (TraceFileRecordsActive()) {
            // ★ 空块守卫，同 cache 线程：裁空的块不写 0 长度段、不打空 mark。
            if (c->usedBytes > c->headOfs) {
                TraceFileMarkRecords(c->headFirstCycle);              // 记录 cache：先记索引
                TraceFileWriteRecords(c->buf + c->headOfs,
                                      c->usedBytes - c->headOfs);     // 再追加字节流
            }
        } else {
            s.rqLostChunks++;
            s.rqLostBytes += c->usedBytes - c->headOfs;
            if (!s.rqLostWarned) {
                s.rqLostWarned = true;
                ORB_LOG_ERROR("TraceWin: 只有一块缓冲、记录 cache 又没开 —— "
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
        // 这三行不能漏：否则 headOfs 会跨块残留，新块前缀被当成已裁掉的记录。
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

// ---- 紧凑记录的打包 ----
// flags 与 TRACE_RECORD_64 位域同布局，展开时逐位拷回。
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

// 读 varint；avail 不足或编码超 10 字节返回 0（调用方应当停）。
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

// 打包一条紧凑记录写入当前块，周期号前进 advance。
// 字段出现与否由 flags 决定（同官方 PacketSize()）。
static void SyncAxisToWall(Store& s);

// 记录级上限的执行者，声明必须在 AppendRec 之前（否则 C3861）。
void EnforceRecCap(Store& s);

// ⚠ 这里**必须**取 storeLock，别再退回"热路径不取锁"。
//
// 那条注释当年成立的前提是：追加只有两个入口（ITM 的 TraceWinItmByte、ETM 的
// TraceWinEtmInstruction），而**两者都在 ComLock 里**（worker 的 OrbTracePump 也在锁内），
// 所以追加实际上是被 ComLock 串行化的，AppendRec 自己不用锁。
//
// 抽 / 解拆成两条线程之后，这个前提没了：解码线程不持 ComLock，与调试线程的
// TryTraceGetRecord / GetFirstCycle 以及抽数线程的 ITM 追加同时动 s.tail、s.usedBytes、
// s.count、c->nRecs。实测后果是记录链被写坏 —— uVision 读到半条记录后直接崩，
// 日志停在采集开始后 10 s（`AG_GOFORBRK still waiting (10 s)` 之后一声不响）。
//
// 死锁方面的结论（都核对过）：
//   · NewWriteChunk() 里也取 StoreGuard —— 临界区可重入，同线程不会自锁；
//   · RetirePush() 等槽时持着本锁，但写线程"腾槽"只走 retireLock（SetEvent 在还块之前），
//     需要 storeLock 的只是"还块"，我们不等它还块 ⇒ 不互等（同 RetireDrain 的说明）；
//   · 锁序始终是 ComLock → storeLock，没有任何路径反向取 ⇒ 与调试线程无环。
//
// 代价：ETM 约 1.18 M 条/s，每条一次无竞争 Enter/Leave ≈ 2–4% 单核；真正要防的是
// "UI 线程读记录时把解码线程挡住"，那是锁竞争的固有代价，换掉的是一条必崩的路径。
void AppendRec(uint8_t type, uint8_t flags, uint32_t pc, uint32_t addr,
               uint32_t data, uint32_t advance)
{
    Store& s = S();
    StoreGuard g;

        // ---- bTS：这条记录的周期号是不是真实时间（Time 列要靠它填）----
        // 轴有真实来源时才置 1；放在这里是因为 8 处调用都要这一位。
        // 无条件置位试过，ETM 下 uVision 会把每条记录当时间戳全量取数而崩，已回退。
    if (s.realAxis) flags |= kFlTS;

    const unsigned dataIdx = (flags >> 3) & 3u;
    const unsigned dataLen = DataSzBytes(dataIdx);
    const unsigned addrLen = (flags & kFlAddr) ? ((type == TR_TYPE_ITM) ? 1u : 4u) : 0u;
    const unsigned pcLen   = (flags & kFlPC) ? 4u : 0u;
    // 字节数上界（varint 差最大 10 B；正常 1 B）。够大就整条写得下。
    const unsigned maxRec  = 2u + pcLen + addrLen + dataLen + 10u;

    Chunk* c = s.tail;
        // idxCount == 0 = 缓冲刚被清空，必须走慢路径重新挂块，否则链上没块而 count 已涨。
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

        // 全局序号只在记录真的写进去之后才递增。
    const uint64_t seq = ++s.recSeq;

        // 把记录时间戳抬到墙钟位置（每 64 条一次，热路径不能每条取墙钟）。
    if ((s.count & 63u) == 0) SyncAxisToWall(s);

    const uint64_t cyc = s.cycle + advance;
    s.cycle = cyc;
        // 本段第一条记录 ⇒ 记下本段起点，并置 segFirstCycleSet（0 是合法起点）。
        // 判据必须是 seq == 1，块会来回复用。
    if (seq == 1) {
        s.segFirstCycle    = cyc;
        s.segFirstCycleSet = true;
                // 当场把 GetFirstCycle 之后会报的值写进日志，便于与现场对账。
        ORB_LOG_INFO("TraceWin: 本段起点 = %llu 周期（本段第一条记录；此后 TDI "
                     "GetFirstCycle / StatusUpdate 的 m_pRecordRange / 显示区间"
                     "一律报它 —— 锚点即 `TraceWin: ETM cycle count base = …` 那条"
                     "日志的位置）", (unsigned long long)cyc);
    }
        // 记录自身推进的周期，真实来源推进轴时要减掉（见 AdvanceFrom）。
    s.synthCycles += advance;

        // 周期差相对上一条，块内首条记 0；用 64 位，周期轴偶发会跳到 1e10。
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

        // ---- 打包 ---- 本条记录的起始偏移（合法的裁剪边界之一）。
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

        // 发布顺序：先定 first/last，最后加 usedBytes，读者不会看到半条记录。
    if (c->nRecs == 0) {
        c->firstCycle = cyc; c->firstRecSeq = seq;
                // 首条可读记录的位置，绝不能留 0（会被当作“没有时钟”报给 UV4）。
        c->headOfs        = recOfs;
        c->headFirstCycle = cyc;
        c->headRecSeq     = seq;    // 与 headFirstCycle 同时定身份（见块头字段说明）
    }
    c->lastCycle = cyc;
    ++c->nRecs;
    c->usedBytes = static_cast<unsigned>(p - c->buf);

    ++s.count;
        // 峰值跟着走：停止后 worker 还会排空积压，只在 SetRunning(false) 取样会低估。
    if (s.count > s.peakCount) s.peakCount = s.count;

        // 条数上限默认关，只在超出余量时动手；放在峰值取样之后，读数仍可信。
    if (s.recCap && s.count > s.recCap + kMaxRecsSlack) EnforceRecCap(s);
}

// ---- 紧凑记录的解码（按字节范围，内存块与记录 cache 共用）----
// cycleAtFirst 是该处第一条记录的绝对周期号，逐条还原成 TRACE_RECORD_64。
struct RecIter {
    const uint8_t*  p;
    const uint8_t*  end;
        // 当前记录的起始偏移（Next() 里记下）。
        // 记下偏移 + 周期号就能续扫，不必从区间头重来。
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

// trmax 执行者：走整块退休而非就地裁剪（就地裁剪不写 cache 不报警，现场曾静默丢 8160 万条）。
// 记录 cache 开着就写 cache，没开就报 ERROR 并置 overflow；默认 recCap==0 不调用。
void EnforceRecCap(Store& s)
{
    while (s.recCap && s.count > s.recCap) {
                // 再摘就把链摘空（单块环，没有更旧的历史）⇒ 停手，交给内存上限兜底。
        if (s.idxCount < 2) break;

                // 摘之前先问有没有人接：记录 cache 没开 ⇒ 摘了就是真丢。只报一次然后停手。
        if (!TraceFileRecordsActive()) {
            if (!s.capLostWarned) {
                s.capLostWarned = true;
                ORB_LOG_ERROR("TraceWin: 记录条数上限（trmax=%llu）到顶，而**记录 cache"
                              "没开或已到上限（trrecmb=）** —— 最旧的历史会被丢弃"
                              "（当前常驻 %llu 条）。要留住它：去掉 trmax=（改用 "
                              "trmem= 控窗口），或加 `trrec=1` / 调大 trrecmb=",
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
                // 先取值再交块：RetirePush 成功后写线程会立刻收回块，old->lastCycle 已是别人的数据。
        const uint64_t oldLast = old->lastCycle;
        if (RetirePush(old)) {
            TraceFileMark(oldLast);          // 真离开了内存，此时才记索引
            continue;
        }
                // 交不出去（写线程起不来 / 队列超时）：RetirePush 已报错置位，块必须接回链头。
        LinkHead(old);
        break;
    }
}

const unsigned kReplayBuf = 64u * 1024;

// 从记录 cache 回放 [lo, hi]，与内存路径共用同一套命中/过滤/偏移计数。
// 必须按索引逐段解：每块首条记录存的是“差 = 0”，跨块线性解每个边界漂一点，
// 实测 407 个边界累积错位 304 条。段边界上要重新锚定周期号。
template <typename Fn>
void ReplayRecordsFromFile(uint64_t lo, uint64_t hi, Fn consider)
{
    uint64_t ofs = 0, cycAt = 0;
    if (!TraceFileLocateRecords(lo, ofs, cycAt)) return;

    static uint8_t buf[kReplayBuf];

    for (;;) {
                // 这一段 = [ofs, nextMark)；索引在追加该块之前记，段边界天然是记录边界。
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

// ---- 周期轴：来源仲裁 + 记账 ----
const char* SrcName(int src)
{
    switch (src) {
    case kCycleSrcWall: return "wall clock";
    case kCycleSrcItm:  return "ITM timestamps";
    case kCycleSrcEtm:  return "ETM cycle count";
    default:            return "none";
    }
}

// 墙钟兜底只给记录打时间戳，不参与取数区间（区间靠 StatusUpdate 的 m_pRecordRange 钉死，
// 挂 m_pClockSample 会让 uVision 外推越界回 BOUNDS）。本板 ITM/ETM 真来源都不可用。
/// 本段采集至今按墙钟应走到的周期号（毫秒 × 核心时钟 kHz）；核心时钟未知返回 0。
static uint64_t ElapsedCycles(const Store& s)
{
    const int khz = orb::cfg::Get().cpuKhz;
    if (khz <= 0) return 0;
        // 停止后墙钟必须冻在 segStopTick，否则时间轴自己继续涨，而 uVision 恰在停止后才来问。
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

// 对外函数（头文件声明、Agdi.cpp 调用）定义必须放在匿名 namespace 外面，
// 否则变成内部链接，链接期报 LNK2019。

/// 把周期轴抬到墙钟位置，只抬不降（轴必须单调）。只在 ITM/SWO 机制下生效：ETM 追墙钟
/// 会在空洞里造 BOUNDS 并让 uVision 全量取数把自己搞崩。不影响取数区间。
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
                // 这句里的 bTS=1 字面串被 test/trace_stream.ps1 的断言依赖，不能改。
        ORB_LOG_INFO("TraceWin: cycle axis = wall clock（**记录的时间戳**改用墙钟兜底，"
                     "此后新记录 bTS=1。⚠ 这不是 BOUNDS 的修法 —— 那个由 "
                     "StatusUpdate() 的 m_pRecordRange 负责，见那里的说明）。"
                     "此前来源=%s，轴=%llu 周期",
                     SrcName(prev), (unsigned long long)wall);
    }
}

// 轴增量上限余量：预算 = 墙钟 × 核心时钟 × 该倍数。
// 实测异常增量差三个数量级，4 倍够拦；长时间空档后的正常大增量仍在预算内。
const uint64_t kAxisBudgetSlack = 4;
// 预算下限（周期）：采集刚开始时墙钟几乎没走，没这个底会把头几个正常包拒掉。
const uint64_t kAxisBudgetFloor = 1000000000ull;

/// 这一段采集至今最多可能过去多少核心周期（ms × kHz）。
/// 核心时钟未知时返回“不设限”：不知道主频就不判。
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

// 一个真实来源报来 delta 个核心周期（当前无调用者，墙钟兜底在用）。
// 规则：先到者拥有轴（两个计数器同时接进来是重复计数）；真实增量要减掉
// 记录自己推进的 +1，否则时间轴会被拉长 8%~17%；认领那一刻就置 realAxis。
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

        // 合理性闸门：轴单位是核心周期，物理上限即"墙钟 × 核心时钟"；超限或累计越界一律拒绝
        // （夹取等于凭空编时间）。这是围堵不是根因修复：只保证垃圾值不毁掉整段轴。
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

// ---- 窗口描述（照搬 DbgCM/TraceWinConnect.cpp 的列 / 过滤器 / 异常表）----

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
        // 曾打开成 "Cycles"，随 bTS 回退一起关掉：列名会换掉 uVision 的列集，
        // 要周期数走分析，别在这里试。
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

// ---- 状态通知 ----
// Trace 周期号的时钟（Hz）：uVision 经 TRACE_CONFIG 交下来，也可由 TraceWinSetCoreClock() 改。
// 定义在这里是因为 StatusUpdate() 的时间锚点要用它。
static uint32_t g_coreClkHz = 0;

// 时间锚点的持久存储：uVision 在 Notify 返回后还会读它，不能是栈上的局部变量。
static TRACE_CLOCK_SAMPLE g_clockSample;

// 记录区间的持久存储，同一个坑：指向栈上会让 uVision 拿到垃圾区间而回 BOUNDS。
static TRACE_RECORD_RANGE g_recRange;

void StatusUpdate(bool processing, bool updateView)
{
    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    Store& s = S();

        // st 必须是持久存储：uVision 的 Notify 返回后还会读它（见 g_clockSample）。
        // memset 会把 m_pClockSample 清成 NULL，else 分支不必再显式清。
    static TRACE_STATUS s_st;

    TRACE_STATUS& st = s_st;
    memset(&st, 0, sizeof(st));
    st.bProcessing     = processing ? 1u : 0u;
    // 默认隐藏（=1）：这个字段同时是取数门的一环，见 Store::showTs 的说明。
    st.bHideTimestamps = s.showTs ? 0u : 1u;
    st.bSkipViewUpdate = updateView ? 0u : 1u;
        // 区间直接上报手里的 [最旧, 最新]：bExplicitRange=1 + m_pRecordRange，m_pClockSample 恒 NULL。
        // 挂锚点会让 uVision 按墙钟外推、请求越界回 BOUNDS ⇒ 窗口只有列没有行。
    st.bExplicitRange = 1;
    memset(&g_recRange, 0, sizeof(g_recRange));
    if (s.count && s.head && s.tail) {
                // 左端统一走 FirstReportedCycle()：内部先过 FirstReadable，再退回本段起点。
                // 三个出口（这里 / TDI GetFirstCycle / TraceWinFirstCycle）必须同值。
        g_recRange.nFirstCycle = FirstReportedCycle(s);
        g_recRange.nLastCycle  = s.tail->lastCycle;
        st.m_pRecordRange      = &g_recRange;
    }
        // m_pClockSample 保持 NULL 是官方行为，不是遗漏，别挂回去。

        // uVision 收到 UV_TRACE_IF_INIT 后会来调 ClearTrace；“上报了却不刷新”与
        // “没上报”在窗口上都是空白，只能靠日志分开。
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

        // 覆盖率自检：只在停止/挂起时打（processing=0 的那次）。窗口显示 0 时，
        // 先看这行 —— >0 说明执行位确实标上了，问题在 uVision 读的另一条路；
        // =0 说明解码没走 TraceWinEtmInstruction()（没有 ETM 源）。
    if (!processing) {
        ORB_LOG_INFO("Cover: %u 段 / %llu 个 DWORD 带执行位 / %llu 条指令计数",
                     orb::cover::SegmentCount(),
                     static_cast<unsigned long long>(orb::cover::MarkedWords()),
                     static_cast<unsigned long long>(orb::cover::ExecCountRange(0u, 0xFFFF0000u)));
    }
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

// ---- 过滤器匹配 ----
// TR_TYPE_xxx 是位掩码而不是位序号；流控记录（0x00）没有位，只能用扩展掩码判。
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

// TDI 回调没被调用时窗口是空的，而日志里“没被调”和“调了返回空”长得一样。
// 每个槽位单独计数：0=取记录、1=GetFirstCycle、2=GetLastCycle、3=ClearTrace、4=UpdateBuffer。
void LogTdi(int slot, const char* what)
{
    static int n[8] = {0};
    if (slot < 0 || slot >= 8) return;
        // 槽 0 是“uVision 有没有真来取数”的唯一信号，额度给大些；其余 4 条够用。
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
// 查询加速：把 nRecordOfs 翻页从 O(区间) 降到 O(返回条数)
// ---------------------------------------------------------------------------
// 原来每页都从区间头重扫，实测 424 次查询解 11.36 亿条只为交付 174 万条（放大 650×），
// 35.7 s 卡在 uVision 的 UI 线程。做法：给已有检查点补全局序号 Chunk::ckptRec ——
// 过滤器全通过时 skip 直接加在序号上二分跳转，否则记住上次位置续扫。
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
    // 采集进行中照样给数：本驱动是流式源（bUnlimitedTrace=1），运行期取数是正常用法，
    // 回 TDI_ERR_PROCESSING 只会白丢一次机会；环形缓冲读本身是安全的。
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

    // 内存为空**不再直接判 EMPTY**：记录 cache（P2-2）里可能还有整段历史。
    // `ReplayRecordsFromFile` 会自己判断"文件里有没有这一段"。
    const uint64_t lo    = *nFirstCycle;
    const uint64_t hi    = *nLastCycle;
    const uint64_t cap   = *nCount;                              // 0 = 只要数量
    const uint64_t limit = (cap == 0) ? (uint64_t)-1 : cap;
    uint64_t       skip  = nRecordOfs ? *nRecordOfs : 0;

    // 只问条数（pTrace64==NULL && *nCount==0）时在索引上数，不解整段：原来逐条解码，
    // 1.04 亿条要 1453 ms 卡在 UI 线程。整块落在区间内直接加 nRecs，只有两个边界块要
    // 从"周期号 <= lo 的最后一个检查点"起解。仅过滤器全通过且不在记录 cache 段时走此快路。
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
    // 是否真的落到记录 cache 上，从这里唯一能看出来 —— 否则 P2-2 只有假 BOM 宿主
    // 那一条证据（§0.3）。包一层，不动 consider 的语义。
    uint64_t fileHits = 0, memHits = 0;
    auto considerFile = [&](const TRACE_RECORD_64& r) { ++fileHits; consider(r); };
    auto considerMem  = [&](const TRACE_RECORD_64& r) { ++memHits;  consider(r); };

    // 起点定位（优先序）：⓪ 游标续扫（按 nRecordOfs 接着上次停处）→ ① 序号直查（过滤器
    // 全通过时二分跳到目标附近）→ ② 从头定位（二分块 + 二分检查点 + 顺序解到 >= lo）。
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
            // 游标存在即证明区间里有记录，直接认账。少了这句，翻到最后一页时一条都进不了 consider
            // ⇒ inRange 为假 ⇒ 错回 BOUNDS（正确答案是 EMPTY）；原实现每页重扫才没暴露。
            inRange    = true;
        }
    }

    // 内存窗口之前那段从记录 cache 回放：按周期号定位文件偏移，用同一套解码展开，顺序上必在
    // 内存之前，故先回放再走内存，nRecordOfs 语义与从头扫一致。游标续扫时必须跳过，否则重复交付。
    const Chunk*   rdHeadM = FirstReadable(s);
    const uint64_t memFirst = rdHeadM ? rdHeadM->headFirstCycle : 0;
    if (!resumed && (!s.count || lo < memFirst)) {
        const uint64_t fileHi = (s.count && hi >= memFirst) ? (memFirst - 1) : hi;
        if (fileHi >= lo) ReplayRecordsFromFile(lo, fileHi, considerFile);
    }

    if (s.count == 0) {
        if (!inRange) { LogQuery("EMPTY (内存 0 条，记录 cache 里也没有这一段)"); return TDI_ERR_EMPTY; }
        // 全在文件里 —— 后面内存那段自然一条都不出。
    }

    // ---- ② 内存窗口 ----------------------------------------------------------
    if (!started) {
        unsigned kk = 0, ff = 0;
        uint64_t cc = 0, ss = 0;
        if (LocateAligned(lo, kk, ff, cc, ss)) {
            kStart = kk; fromStart = ff; cycleStart = cc;
            started = true;

            // 序号直查：过滤器全通过时"区间内第 skip 条命中"就是"第 skip 条记录"，目标 = ss + skip0
            // 二分跳过去。⚠ 只有区间起点就在内存里（fileHits == 0）时成立，否则会错位。
            if (skip0 > 0 && fileHits == 0 && FilterPassAll(filter)) {
                unsigned jk = 0, jf = 0;
                uint64_t jc = 0, js = 0;
                if (LocateSeq(ss + skip0, jk, jf, jc, js)) {
                    kStart = jk; fromStart = jf; cycleStart = jc;
                    // 该检查点之前已匹配 js - ss 条。⚠ js < ss 时必须记 0：无符号减法会下溢成天文数字，
                    // 于是 matched <= skip0 恒成立、一条都交付不出来（检查点落在区间起点之前的情形）。
                    matched = (js > ss) ? (js - ss) : 0;
                }
            }
        }
    }

    for (unsigned k = kStart; started && k < s.idxCount; ++k) {
        const Chunk* c = s.idx[k];
        if (c->nRecs == 0) continue;
        if (c->firstCycle > hi) break;      // 后面的块只会更晚

        // 块内起点：首块用定位结果（检查点 / 游标 / 序号直查），其余块二分检查点取
        // "周期号 <= lo 的最后一条"。变长编码的代价：块中间查必须从头解，检查点限在 1/256 块。
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
            // ⚠ 过了 hi 必须 break 不能 continue：块内周期号升序，之后整块都不会再命中。
            // 原来 continue ⇒ 每块解到块尾、外层走到 hi，一次窄查询就解掉 lo 之后全部记录（880 万条），
            // 而这条查询跑在 uVision 的 UI 线程上 ⇒ 界面卡死。
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
        // 内存里一块都没有 —— 整段历史可能都在记录 cache 里（P2-2）。
        uint64_t f = 0, l = 0;
        if (!TraceFileRecordsRange(f, l)) return TDI_ERR_EMPTY;
        *nFirstCycle = f;
        return TDI_ERR_SUCCESS;
    }

    // 可查范围左端报本段起点（segFirstCycle），不是最旧可读：报晚了 Navigation 条只画内存那一小段。
    // 历史真被丢掉时也报起点（状态栏已有 Data Overflow），再与记录 cache 左端取更早者。
    uint64_t first = FirstReportedCycle(s);
    uint64_t f = 0, l = 0;
    if (TraceFileRecordsRange(f, l) && f < first) first = f;
    *nFirstCycle = first;

    // ⚠ 日志必须记下返回值而不只是"被调用"：真机上 uVision 反复请求的值正好是驱动上界的
    // 2.00 倍且 91 s 不动，不记数就永远分不清是返回值、g_coreClkHz 还是 uVision 的换算问题。
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

    // ⚠ 报缓冲里最新一条记录的真实 cycle，不是当前时刻：官方 GetLastCycle 就是记录边界，
    // 与 StatusUpdate 的 nLastCycle 是同一对值。报墙钟会让范围比数据长，拖到数据之后回 BOUNDS。
    const uint64_t last = s.tail->lastCycle;
    *nLastCycle = last;

    // 同 GetFirstCycle：这个值就是 uVision 算时间轴的右端，必须留证据。
    // 注意**没有**把 `TraceFileRecordsRange()` 的上界并进来（P2-2 的 cache 段在内存窗口
    // 之后）—— 若 uVision 的时间轴右端大于这里报的值，就是它自己在算，不是读我们的。
    uint64_t ff = 0, fl = 0;
    const bool haveFile = TraceFileRecordsRange(ff, fl);
    ORB_LOG_INFO("TraceWin: [2] GetLastCycle -> %llu（周期时钟 %u Hz，内存 %llu 条；"
                 "cache 段 %s%s%llu）",
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
    // 采集进行中照样清（流式源的正常用法）：原回 TDI_ERR_PROCESSING 让 uVision 把 clear
    // 按钮置灰。安全性靠 TraceWinClear 先 RetireDrain 再取结构锁，追加不取该锁 ——
    // 最坏是清完瞬间又被追加一条，正是流式语义。
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
// 官方参考留 NULL（头文件标 Obsolete），但它的 trace 窗口本来没移植，不能据此断定
// uVision 不用 —— 这是驱动唯一可能收到"host 改了 trace 配置"的口子，先接上并记日志。

INT32 SetTraceConfig(TRACE_CONFIG* p)
{
    LogTdi(5, "SetTraceConfig");
    if (!p) return TDI_ERR_INVALID_PARAM;

    // ★ 不是内核时钟的来源：nClock 只是"host 改了配置"的通知（官方 TraceConf.Clk 初值为 0）。
    // 原 `if (p->nClock) g_coreClkHz = p->nClock;` 正是 8 M 灌进内核时钟的入口 —— 已删。
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
// 周期轴机制：ITM/SWO 追墙钟，ETM 不追
// ---------------------------------------------------------------------------
// 定义必须放在匿名 namespace 外，否则变内部链接 ⇒ 链接期 LNK2019（真踩过）。机制名表留在那边。
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
// Trace Data 窗口的时间轴要的就是 TRACE_CLK，此前一个 VTR 都没注册，于是"接口全通却永远
// 不来取记录"。
static VTR* g_coreClk  = nullptr;
static VTR* g_traceClk = nullptr;
static WATCH g_wCoreClk;
static WATCH g_wTraceClk;

// 官方 wrCoreClk / wrTraceClk：uVision Trace 页改写这两个 VTReg 的回调，核心时钟真源
// 在这里而不是 cpu= 选项。官方语义：运行中不许改（回写旧值），停止态写入 TraceConf.Clk
// （0 归 1），TRACE_USE_CORECLK 时把 TRACE_CLK 镜像成生效值。签名是 WATCH 的 wf。
void wrCoreClk(DWORD adr, AGSICB_REASON r)
{
    (void)adr; (void)r;
    if (!g_coreClk) return;

    // ★ 运行中守卫：采集进行中不接受时间基变更，一律回写权威值 TraceConf.Clk —— 已入库记录
    // 按旧时钟换算，中途换基会前后不自洽。停止态照样无条件落到 g_coreClkHz。
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
    // ★ 不能在这里 return（原 `if (khz == c.cpuKhz) return;` 是错的）：会连带跳过下面的
    // TRACE_CLK 镜像回写；changed 只决定日志/重算/落配置，不决定要不要镜像。
    const bool changed = (khz != c.cpuKhz);
    if (changed) {
        ORB_LOG_INFO("TraceWin: uVision Trace 页把 CORE_CLK 改为 %u Hz（原 %d kHz）"
                     "—— TPIU 分频按新值重算", hz, c.cpuKhz);
        c.cpuKhz = khz;                   // ★ 顺序敏感：必须在下面那句**之前**落配置

        // ★ 无条件更新内核时间基 —— 这是本 bug 的**真修复半边**。原来它只在"跟随核心
        //   时钟"那一支里调，于是 uVision 填的 Core Clock 在不跟随时对记录时间轴毫无影响。
        TraceWinSetCoreClock(hz);
    }

    // 同官方 AGDI.CPP:4149：TRACE_USE_CORECLK 时把 TRACE_CLK 框回写成生效值。★ 直写字段、
    // 不用 putval —— putval 会冲掉用户显式填的 TPIU 时钟。重发统一走 RefreshTraceClocks。
    if ((c.useCoreClock || c.traceClockKhz == 0) && g_traceClk) {
        g_traceClk->v.ul = hz;
    }
}

void wrTraceClk(DWORD adr, AGSICB_REASON r)
{
    (void)adr; (void)r;
    if (!g_traceClk) return;

    orb::cfg::Config& c = orb::cfg::Get();

    // ★ 与 wrCoreClk 同款运行中守卫：采集进行中拒绝改写并回写权威值。⚠ 回写的不是 TPIU_Clk
    // 而是生效 TPIU 时钟（跟随核心时钟时即 Clk），即 `follow ? clk : traceClockKhz*1000`，
    // 三处必须一起改。
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

    // "Use Core Clock"（tclkc）时 TRACE_CLK 只读：回 EU49 提示并把框回写成生效值。
    // ⚠ 两处有意偏离：用 INFO 日志代替用户可见警告（且无 ReInit 门控）；额外经 p_putval 纠偏。
    if (c.useCoreClock) {
        const uint32_t written = g_traceClk->v.ul;
        const uint32_t core    = static_cast<uint32_t>(c.cpuKhz) * 1000u;
        ORB_LOG_INFO("TraceWin: TRACE_CLK 只读（Use Core Clock 勾着）—— 忽略 "
                     "uVision 写的 %u Hz，保持核心时钟 %u Hz", written, core);
        // ★ 有意偏离官方：官方只直写本地 VTR 字段，我们改走 p_putval —— 直写只改本地镜像，
        // uVision 那边未必跟着变，正是"轴是新的、窗口里是旧的"的成因。这是"时钟写回调里不调
        // 重发"的唯一例外（脱管纠偏），重入由 RefreshTraceClocks 的静态守卫兜住。
        RefreshTraceClocks();
        return;
    }

    const uint32_t hz = g_traceClk->v.ul;
    if (hz < 1000u) return;
    //   ⚠ **有意偏离**：官方 `AGDI.CPP:4236-4237` 是 `if (==0) =1`（不丢值、0 钳成 1 Hz），
    //     我们 <1000 一律拒。口径与 `wrCoreClk` 那边一致，见那处注释。

    const int khz = static_cast<int>(hz / 1000u);
    // ★ 早退与 wrCoreClk 形状同、性质不同：后面只有一条日志和 c.traceClockKhz = khz；官方那一支
    // 末尾还有 UpdateClockSWO 所以不能早退。将来把 SWO 下发挪进本回调时必须删掉这个早退。
    if (khz == c.traceClockKhz) return;

    ORB_LOG_INFO("TraceWin: uVision Trace 页把 TRACE_CLK 改为 %u Hz（原 %d kHz）",
                 hz, c.traceClockKhz);
    c.traceClockKhz = khz;
    // ★ 删掉原来的 `TraceWinSetCoreClock(hz);` —— TRACE_CLK **不是**内核时间基，把它写进
    //   `g_coreClkHz` 正是"6 倍时间污染"的另一半（8 M 那条是 SetTraceConfig）。
    //   VTR 此刻已持有新值，无需重发。
}

// 对齐官方唯一的重发 helper：CORE_CLK ← TraceConf.Clk（无条件）；TRACE_CLK ←
// TRACE_USE_CORECLK ? Clk : TPIU_Clk。⚠ 它不在 wrCoreClk 链上，只能在事件点（conf 应用 /
// trace 启动 / 停止收尾）调用，别在时钟写回调里调。
void RefreshTraceClocks()
{
    // ★ 重入守卫：p_putval 是否同步回声到 VTR 写钩子无法证实，若同步就是
    // wrTraceClk → Refresh → p_putval → wrTraceClk 无限递归（栈溢出）。官方为此专门设了重入
    // 字节，这里用 inFlight 等价。⚠ static 无锁：只保证重入安全，不保证跨线程正确。
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

    // ★ 每次都重新 p_create，不缓存 VTR*：uVision 的 VTReg 表按会话重建，缓存会让第二次会话
    // 的 p_putval 打在野指针上 ⇒ 没有 clock ⇒ 算不出时间跨度 ⇒ 窗口永远不来取记录。
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

// TDI 接口表的 ABI 编译期断言：nVersionUV 在 +0x28（UV4 0x45C98D 回写），整表
// rep movsd ecx=0x20 ⇒ 0x80 字节。结构成员全是 4 字节宽，pack(1) 与默认布局相同，
// 别因为看到 pack 就删这两条断言。
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
    // ⚠ nVersion 必须精确等于 2：>2 会让 UV4 整张表拒装（窗口直接消失），<2 走旧 v1 安装器。
    // 要扩字段只能用 nReserved[19]。
    g_init.nVersion               = TDI_VERSION;   // TDI_VERSION == 2（TraceDataTypes.h:32）
    // 这两个槽官方参考留 NULL（"Obsolete"），但那是因为它的窗口没移植。官方另有 conf 应用路径
    // 做反向同步，我们没有 ⇒ 这很可能是唯一能收到"host 改了配置"的口子，保留注册。
    g_init.pSetTraceConfig        = &SetTraceConfig;
    g_init.pGetTraceConfig        = &GetTraceConfig;
    g_init.pGetFirstCycle         = &GetFirstCycle;
    g_init.pGetLastCycle          = &GetLastCycle;
    g_init.pClearTrace            = &ClearTrace;
    g_init.pGetTraceRecords64     = &GetTraceRecords64;
    g_init.pGetFiltTraceRecords64 = &GetFiltTraceRecords64;
    g_init.pUpdateTraceBuffer     = &UpdateTraceBuffer;
    // bUnlimitedTrace：置 0 时 uVision 把驱动当自备缓冲的 ETB 类，只问范围不取记录；置 1 才是
    // 按需取数的无限 trace 视图。orbtrace 是流式源，恒置 0 正是"有列没有行"的直接原因。
    g_init.bUnlimitedTrace        = 1;
    g_init.bEmbeddedTraceBuffer   = 0;   // ETB 模式下由 AGDI 层另行置位

    // nVersionUV 正常会话必为 2（UV4 唯一写入点 0x45C98D，清表路径 EnumUvARM7 从没被调用）。
    // 打 0 = 表没被 UV4 接走 ⇒ 去查 Notify(UV_TRACE_IF_INIT) 的 rc 与 BOM，别按"v1 降级"猜。
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
// 声明"trace 数据展示已启用"（struct bom::bTrcDisp，标志位 bit3）
// ---------------------------------------------------------------------------
// 这是 Trace Data 窗口取记录的开关位，官方在注册 TDI 之前置它，uVision 侧
// `test byte [app+0xafc], 8` 读。
// ⚠ 必须置在 uVision 真正读的那个 bom 实例上：AG_CB_GETBOMPTR 返回的才是，不是 pDbgX->pBom。
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
// 诊断工具：只读地把 UV4.exe 取数门链的当前值打进日志（编译期开关，默认关）
// ---------------------------------------------------------------------------
// 窗口有列有过滤器却一行不出时，用它指出是哪道门关着，不必反汇编猜。门链：
// [0x10C] / bProcessing / bom::bTrcDisp / bHideTimestamps，最后才到 FetchRecords。
// ⚠ app=[0xca2194] 与 CTraceWin 属主=[0xca2198] 是两个不同基准，读错会得到一堆垃圾值。
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

    // ⚠⚠ 必须过 FirstReadable（第一条可读记录所在的块），不能直接返回 s.head->firstCycle：
    // 后者是"本块写入过的第一条"，记录级裁剪不动它，卡在条数上限时它早已被裁掉且只会越错越远。
    // 真机曾因此把区间报宽 7.32 M 周期 ⇒ Navigation 拖中间段全落空回 BOUNDS。
    // ★ 左端统一用 FirstReportedCycle()（= 本段起点），三个出口必须报同一个值。
    return FirstReportedCycle(s);
}

void TraceWinShowRange(uint64_t firstCycle, uint64_t lastCycle)
{
    Store& s = S();
    if (!s.registered) return;

    struct bom* bom = orb::pdsc::Bom();
    if (!bom || !bom->Notify) return;

    // bExplicitRange + m_pRecordRange 是唯一定名区间的字段，不指名 uVision 只问范围不取记录。
    // ⚠ 两者必须指向持久存储：Notify 返回后 uVision 还会读，指栈上局部变量会让它拿到垃圾区间
    // 并回 BOUNDS（窗口只有列没有行，真机踩过）。只在 uVision 线程访问，不必加锁。
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

    // 时间锚点一起带上（与 StatusUpdate 共用持久存储），Time 列才有值：报 (0, 0.0) 即
    // "周期 0 = 0 秒"。⚠ 它只负责周期换算成秒，防 BOUNDS 的是 bExplicitRange + m_pRecordRange。
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
        // 覆盖率 / 性能分析共用这一处：解码出的每条指令就是"执行过的地址"。
        // ⚠ 放在 TraceWinReady() 判断**之前**：覆盖率不依赖记录缓冲是否就绪
        // （Trace 窗口没开也要能统计），否则关了 Trace 窗口覆盖率就是空的。
        // 内部稳态无锁，只在跨 64 kB 段时取一次锁。
    orb::cover::OnInstruction(addr, executed);

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
    // ⚠ 不再推进周期轴：轴现在归墙钟兜底（只打时间戳），而本板 ITM 时间戳不可信（SWO 位时钟 =
    // 核心时钟，实测载荷饱和 0xFFFFFFFF 被预算闸门拒掉）。这里只留计数供日志看。
    S().itmCycles += delta;
}

// ETM 报来 0 的次数（= "unknown"，见下面 TraceWinEtmTime 的说明）。只用于那条
// 一次性 WARN 的限流；跨线程竞争只会多打一行日志，不值得为它加锁。
static unsigned long long s_etmZeroVals = 0;

void TraceWinEtmTime(uint64_t absolute, const char* what)
{
    if (absolute == 0xFFFFFFFFFFFFFFFFull) return;   // COUNT_UNKNOWN：这个包没带值

    // ⚠⚠ 0 也是"没有值"（IHI0014Q §7.7.4：zero = unknown），不是一次合法采样：本板 ETM 时间戳包
    // 恒 0、周期计数不实现，解出来也是 0。原来把 0 当绝对值建基准并认领轴 ⇒ 增量恒 0 ⇒ 轴不再动、
    // 总长 ≈ 记录条数（7661 条 = 4.1 ms）⇒ Navigation / 时间列全错。修法：0 一律不建基准、不认领、不推进。
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
        // 第一次收到只建基准：ETM 计数器自由运行，起点不为 0，直接加上去会让时间从巨大偏移开始。
        // ⚠⚠ 这里不认领周期轴：收到首个包 ≠ 这个来源会推进，本板 ETM 周期计数不实现、包值恒 0，
        // 认领后轴就不动了 ⇒ 认领交给 AdvanceFrom 在第一次非零增量时做。
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

    // 日志报实际分配量：报峰值而非当前值（本函数可能落在 Clear 之后，当前值那时恒 0）。
    // `持有` 是会话内分配过的字节（含空闲链，只增不减）；`满块均=N B/条` 是容量判据实测值。
    const unsigned long long avgRec = s.retiredRecs
                                    ? (s.retiredBytes / s.retiredRecs) : 0;
    ORB_LOG_INFO("TraceWin: buf(%s) 块峰值=%u/%u（每块 %u MB）记录峰值=%llu "
                 "满块均=%llu B/条（%llu 块）持有=%llu MB 上限=%llu MB "
                 "条数上限=%llu 摘块=%llu 分配失败=%llu 退化=%llu 丢记录=%llu "
                 "cache=%llu 块 等 cache=%llu 次 丢历史=%llu 块/%llu MB "
                 // `条数上限` = trmax= 的值，0 才正常；非 0 时 `上限裁掉` > 0 说明条数上限真动过手。
                 // `本段起点` 就是 GetFirstCycle 报的位置；`累计写入(本段)` = recSeq，与三项相加应相等。
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

    // trmax= 只是比较值、不参与一次性分配 ⇒ 随时可改，故放在"索引已分配"那个早退之前。
    // ⚠ 默认 0 = 不限；原来硬编码 1<<20 把回看钉死在 0.13 s，且静默丢掉 8160 万条。
    if (s.recCap != recMax) {
        s.recCap = recMax;
        ORB_LOG_INFO("TraceWin: 记录条数上限 = %llu 条（0 = 不限：窗口只受 %u MB 字节"
                     "上限约束；非 0 时触顶走整块退休 —— 记录 cache 开着就写 cache，"
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

    // ⚠ **先等在飞的块写完 cache**：写线程手上可能还拿着块，不等它把块还回来就把整条链
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
    // 本段起点是段属性，与 recSeq 同处清零、下一条记录进来时重赋。⚠ 别挪到 SetRunning(true)：
    // stop → start 也会走到，会把起点抹掉、FirstReportedCycle 退回"最旧可读"。
    s.segFirstCycle    = 0;
    s.segFirstCycleSet = false;
    // ★ 不变量 recSeq == count + retiredRecs + capTrimmedRecs 的五个量必须同处清零，少清一个
    // 就报错账。放在清空处而不是 SetRunning(true)：只 Clear / 只 start 两种顺序都得成立。
    s.capTrimmedRecs = 0;
    s.retiredBytes   = 0;
    s.retiredRecs    = 0;
    // 游标指向的块已经不在链上了 —— 必须作废，否则下一次查询会按一个失效的
    // `chunkSeq` 去定位（那会解出垃圾）。P4-2。
    s.qcur.valid = false;

    // ⚠ 这里不清峰值/诊断类账目（nDropped / peakCount 等）：buf() 日志落在本函数之后，清掉就
    // 只剩 0；它们在每段采集开始时清。★ 相反，不变量账目一律在本函数清，段界就是"清空"。
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

    // cache（P2-2）：记录 cache 里"内存窗口之前那一段"也一起清掉 —— 否则清空窗口之后
    // 还能把旧记录翻出来（TDI 的 ClearTrace 语义是"清空"）。
    TraceFileClearRecords();

    // ⚠ 清完必须通知 uVision 刷新，否则窗口留着上一次的行（用户实测"清不掉"）。正解走
    // StatusUpdate：强制 bSkipViewUpdate = 0、bExplicitRange 保持 0。⚠⚠ 不能用
    // TraceWinShowRange(0,0) —— 那会把区间钉死成 [0,0]，数据再也出不来（第一版就错在这）。
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
        // 新的一段采集：按段诊断账目从零开始。⚠ retiredBytes/retiredRecs 是不变量账目，已挪到
        // TraceWinClear 与 recSeq 共用段界；峰值类语义是"清空后仍有效"，留在这里。
        s.nDropped = 0;
        s.nAllocFail = 0;
        s.nDegraded = 0;
        s.nAppendDrop = 0;
        s.peakCount = s.count;
        s.peakChunks = s.idxCount;

        // 退休队列（P3-1）的账目同样按段清：这几项是"交接层到底有没有起作用"
        // 的判据 —— cache 块数 > 0 说明异步写 cache 真的跑了；丢历史块数 > 0 说明
        // 内存/磁盘都没接住（那时状态栏同时会有 Data Overflow）。
        s.rqWritten    = 0;
        s.rqWaits      = 0;
        s.rqLostChunks = 0;
        s.rqLostBytes  = 0;
        s.rqLostWarned = false;
        // "条数上限到顶且没写 cache"同样每段只吼一次（判据见 EnforceRecCap）。
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
// 状态栏：排队与投递分开
// ---------------------------------------------------------------------------
// bom->Notify 跨线程投递到 GUI 线程，抽数 worker 每轮折叠都要等在门口，实测把 2 ms 的
// 循环拖到 21 ms。修法：StatusMsg 只写 g_pendMsg（Interlocked，读即清），StatusFlush
// 由调试线程投递。语义不变，最多晚一个轮询周期。
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

// 官方 OutTraceMsg：把消息码渲染成文本并投递。差异仅在用 memcpy/Append 代替 strncpy_s +
// sprintf（本工程把不安全 CRT 标为错误，sprintf_s 截断会终止宿主），逻辑与分支一致。
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

// 官方 ETB_TraceStatus：按优先级折叠，只把 DATA_ERR/DATA_OVF 变成消息，且仅当消息码更大时
// 覆盖显示。照抄的行为：折叠时清位，若它们是仅有的置位项，末尾会立刻把状态栏清空。
// 返回值：官方返回 0，这里返回当前消息码便于日志。
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
