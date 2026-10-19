/**
 * @file ORBAGDI_TracePump.cpp
 * @brief 抽数 / 解码两条线程（结构见 ORBAGDI_TracePump.h）。
 *
 * 原本抽、解在同一个 worker 里串行：实测每轮 57 ms，其中 54 ms 是解码（P1-0 补全），
 * 而探针侧以 ~3.1 MB/s 灌进 RDDI 的 32 MB 环 —— 一次 63 s 的会话丢了 161 MB（82%），
 * 缺口之后解出的地址还全是假的。
 *
 * 现在拆成两条：
 *   ① 抽数线程：只拿探针锁搬字节（每片 2 ms），放进 48 × 256 KB 的有界队列。
 *      探针锁的持有时间不再受解码影响，环被排空的速度只取决于 memcpy。
 *   ② 解码线程：从队列取块，锁外解码（cache / 解帧 / P1-0 补全）。
 *
 * 队列满时**不抽**：数据留在探针环里，本层只记一笔并请求 ETM 重同步。这并不比原来丢得少，
 * 但丢的位置从"RDDI 内部环（看不见）"变成"本层有界队列（能记、能标记缺口）"，解码器不会
 * 拿着断掉的字节流继续推相对地址。真正的吞吐仍要靠解码提速（见 InstrScan 的缓存）。
 */
#include "pch.h"
#include "ORBAGDI_TracePump.h"

#include <process.h>   // _beginthreadex
#include "ORBAGDI_Agdi.h"
#include "ORBAGDI_DebugAccess.h"
#include "ORBAGDI_Itm.h"
#include "ORBAGDI_OrbTrace.h"
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_TraceWin.h"

// uVision 的 BOM：`Notify`、`UV_DBG_ACCESS_LEVEL` 与 `VW_*` 窗口掩码都在这里
// （BOM.H:2390 / 2514-2530）。恢复之后必须经它告诉 uVision，见 MaybeRecoverAccess()。
#include "BOM.H"

namespace orb {
namespace agdi {
namespace {

// 一次循环里最多花多久在抽数上（官方 `Trace_Read(50, TRUE)` 的等价预算）。
const unsigned kBudgetMs = 20;
// 每个时间片：锁只持有这么久。
const unsigned kSliceMs  = 2;
// 轮询周期。官方 DSMonitor 线程的节拍就是 50 ms（`Trace_Read(50, TRUE)` 兼作
// 节拍）；这里取小一档，因为本驱动的源是 USB 环，等太久会让环先满。
const unsigned kPeriodMs = 2;

HANDLE          s_threadDrain  = nullptr;
HANDLE          s_threadDecode = nullptr;
HANDLE          s_wake   = nullptr;   // Stop 的打断事件
HANDLE          s_timer  = nullptr;   // 高精度可等待计时器，见 PumpSleep()
volatile LONG   s_stop   = 0;
volatile LONG   s_suspend = 0;

// 诊断计数（只在 stop 时打一次日志）。
volatile LONG   s_rounds = 0;
// "一轮抽数已完成"的序号（含 continue 那几条路径）。TracePumpDrainOnce() 靠它判断
// "worker 已经醒来跑完一轮"，而不是只看队列深度 —— 队列空不代表环已抽空。
volatile LONG   s_roundDone = 0;
volatile LONG   s_skipSuspend = 0;
volatile LONG   s_skipBusy = 0;
volatile LONG   s_skipNoSource = 0;
unsigned long long s_bytes = 0;              // 抽到的字节（抽数线程写）
unsigned long long s_bytesDecoded = 0;       // 交给解码器的字节（解码线程写）
unsigned long long s_decodeBlocks = 0;
unsigned long long s_tStart = 0;             // 启动时刻（算解码线程繁忙度）
unsigned long long s_qDepthSum = 0;          // 队列水位采样（抽数线程写）
unsigned long long s_qDepthSamples = 0;

// 每轮耗时的分解。用 QPC 而不是 `GetTickCount`：后者的 15.6 ms 粒度量不出
// "等待 / 排空 / 解码 / 状态折叠"这几段。停线程时打一行 —— 21 ms 的周期到底
// 花在哪，靠这一行定案，不靠推测。
unsigned long long s_tWait = 0;      // WaitForSingleObject() 的时间
unsigned long long s_tDrain = 0;     // TraceDrain()（**锁内**：只搬字节）的时间
unsigned long long s_tDecode = 0;    // TraceDecodeStaged()（**锁外**：cache/解帧/解码）
unsigned long long s_tStatus = 0;    // TraceWinRead/TraceWinRecoverTick 的时间

// 有源时段的总耗时：只有内层循环（真在抽/解）才累加。⚠ 它含 s_tSleep —— 拿不到锁时的让锁
// 也发生在内层，算数据率要减掉，否则把 Sleep(1) 当成劳动力（实测 601 次让锁睡 9.4 s）。
unsigned long long s_tFeed = 0;

// 内层循环里"拿不到锁就让一下"的累计耗时。单列的唯一原因是它**不是干活**：
// `Sleep(1)` 在没抬定时器精度时实际睡 15.6 ms，这一项曾经被读成"活跃 9.63 s"。
unsigned long long s_tSleep = 0;

unsigned long long s_tRound = 0;     // 整轮（含等待）
unsigned long long s_tRoundMax = 0;
unsigned long long s_roundsTimed = 0;

// 抽（锁内）与解（锁外）之间的交接：单生产者 / 单消费者的有界队列。
// 256 KB = 4 个 RDDI 传输块：拿满锁纯 memcpy 约 0.4 ms 就放，既不挡调试线程又不占内存。
// 48 槽 = 12 MB：以 3.1 MB/s 的供给算，能吸收约 4 s 的解码滞后。
// 为什么必须分开，见 ORBAGDI_OrbTrace.h 的"抽/解 分离"（那里记着一次丢 506 MB 的证据链）。
const unsigned kStageBytes = 256u * 1024u;

// 槽数**可配**（Todo §3.1 要点 2 要求）：环境变量 ORBAGDI_TRACE_QUEUE_SLOTS，8..256。
// 默认 64 槽 × 256 KB = 16 MB —— 以 3.1 MB/s 的供给算能吸收约 5 s 的解码滞后。
unsigned g_qSlots = 64;

uint8_t*        s_q     = nullptr;      // g_qSlots × kStageBytes
unsigned*       s_qLen  = nullptr;      // 每槽有效字节
volatile LONG   s_qWrite = 0;           // 生产者写
volatile LONG   s_qRead  = 0;           // 消费者写
volatile LONG   s_qDepth = 0;
volatile LONG   s_qMaxDepth = 0;
volatile LONG   s_qDrops = 0;           // 因队列满而丢弃的**片数**
unsigned long long s_qDropBytes = 0;    // 丢弃的字节数（抽数线程写）
volatile LONG   s_gapPending = 0;       // 队列满丢过 ⇒ 解码前要重同步
HANDLE          s_qEvent = nullptr;

bool            s_inlineDecode = false; // 队列建不起来：退回"抽完就地解"（老路径）
uint8_t         s_stage[kStageBytes];   // 仅供 TraceDrain/TraceDecodeStaged 的公开路径

unsigned long long QpcNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<unsigned long long>(t.QuadPart);
}

double QpcMs(unsigned long long d)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (f.QuadPart > 0)
         ? (static_cast<double>(d) * 1000.0 / static_cast<double>(f.QuadPart))
         : 0.0;
}

// ---------------------------------------------------------------------------
// 高精度等待
// 实测 45 s 会话：2255 轮、平均 20.19 ms/轮，其中等待 15.92 ms（79%），601 次 Sleep(1) 睡掉
// 9.4 s，真正搬字节 + 解码只有 50 ms ⇒ "处理不过来"是节拍问题，不是算力问题。
// 不用 timeBeginPeriod(1)（整机分辨率与功耗一起变），改用 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION，
// 老系统上创建失败回落普通计时器（行为与改造前一致）。
// ---------------------------------------------------------------------------
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

HANDLE CreatePumpTimer()
{
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr,
                 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    return t;
}

// 睡 ms 毫秒，Stop 时立刻返回。等价于原来的 `WaitForSingleObject(s_wake, ms)`。
void PumpSleep(unsigned ms)
{
    if (s_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;  // 负值 = 相对时长（100 ns）
        if (SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE h[2] = { s_timer, s_wake };
            WaitForMultipleObjects(2, h, FALSE, INFINITE);    // 超时，或被 Stop 打断
            return;
        }
    }
    WaitForSingleObject(s_wake, ms);        // 计时器没建起来：退回老路
}

// 整轮计时：`continue` 那条路也要计到，所以用析构而不是在循环末尾累加。
struct RoundTimer {
    unsigned long long t0;
    RoundTimer() : t0(QpcNow()) {}
    ~RoundTimer() {
        const unsigned long long d = QpcNow() - t0;
        s_tRound += d;
        if (d > s_tRoundMax) s_tRoundMax = d;
        ++s_roundsTimed;
        // 轮末信号：放在析构里，"挂起 / 无源 / 正常"三条路径都会走到。
        // 停机时 TracePumpDrainOnce() 就等它 —— 判据是"跑完一整轮"，
        // 不是"队列深度为 0"（后者在 worker 还没醒时恒为真）。
        InterlockedIncrement(&s_roundDone);
    }
};

// 现在有流式 trace 源在跑吗？没有的话 worker 连探针锁都不该碰（见线程体里的说明）。
bool TraceDrainWanted()
{
    return ItmActive() || OrbTraceActive();
}

// ---------------------------------------------------------------------------
// 队列：写指针只由抽数线程改，读指针只由解码线程改，因此不需要锁。
// ---------------------------------------------------------------------------
uint8_t* QueueBegin(unsigned& slot)
{
    if (!s_q) return nullptr;
    const LONG w = s_qWrite;                                   // 只有生产者写
    const LONG r = InterlockedCompareExchange(&s_qRead, 0, 0);
    if (((w + 1) % static_cast<LONG>(g_qSlots)) == r) return nullptr;   // 满
    slot = static_cast<unsigned>(w);
    return s_q + static_cast<size_t>(w) * kStageBytes;
}

void QueueCommit(unsigned slot, unsigned n)
{
    if (!n || !s_q) return;
    s_qLen[slot] = n;
    MemoryBarrier();                                           // 先写完数据再放写指针
    InterlockedExchange(&s_qWrite, static_cast<LONG>((slot + 1) % g_qSlots));

    const LONG d = InterlockedIncrement(&s_qDepth);
    if (d > s_qMaxDepth) s_qMaxDepth = d;
    SetEvent(s_qEvent);
}

const uint8_t* QueueFront(unsigned& n)
{
    if (!s_q) return nullptr;
    const LONG r = s_qRead;                                    // 只有消费者写
    const LONG w = InterlockedCompareExchange(&s_qWrite, 0, 0);
    if (r == w) return nullptr;
    n = s_qLen[r];
    return s_q + static_cast<size_t>(r) * kStageBytes;
}

void QueuePop()
{
    MemoryBarrier();
    InterlockedExchange(&s_qRead, static_cast<LONG>((s_qRead + 1) % g_qSlots));
    InterlockedDecrement(&s_qDepth);
}

// 停机收尾要看"队列是否解完"。⚠ 不能用"往队列里推一个屏障槽"的做法：那个槽得由
// 调试线程写，于是队列变成两个生产者，SPSC 的写指针假设当场失效。直接看深度就够了 ——
// 深度归零意味着最后一块也已经 OrbTraceDecodeRaw() 完（QueuePop 在解码之后才减）。
//
// s_qDepth 由 Commit / Pop 用 Interlocked 维护，读它即可。

// 状态折叠与 T_Recover 倒计时（官方在 Trace_Read 里做，而它从 DSMonitor 线程调）都在锁外。
// TraceWinStatusMsg() 只排队，真正 Notify 的是 TraceWinStatusFlush()，由调试线程轮询调用。
// timeMs 传 0：这边只剩折叠、没有阻塞读。
void FoldStatus()
{
    const unsigned long long st0 = QpcNow();
    TraceWinRead(0, false);
    TraceWinRecoverTick(kSliceMs);
    s_tStatus += QpcNow() - st0;

    // 队列水位采样：判据要"峰值 / 均值"，只看峰值会把一次突发当成常态。
    s_qDepthSum += static_cast<unsigned long long>(
        InterlockedCompareExchange(&s_qDepth, 0, 0));
    ++s_qDepthSamples;
}

// ---------------------------------------------------------------------------
// 后台访问监视 —— 参考 DSMonitor 的等价物
// 参考的 DSMonitor 在循环里调 DebugAccessRecovery()：恢复不发生在访问路径上，等 uVision
// 再来读时链路已经修好。本驱动没有独立监控线程，这个 worker 正好空转着，就由它干；周期 100 ms。
// ⚠ 只在 AccessSuspect() 为真或等级已经变坏时才动：健康会话一次探针都不碰，P0-A 的
// "无流式源就不拿探针锁"（skipBusy == 0）不受影响。
// ---------------------------------------------------------------------------
const DWORD kAccCheckMs = 100u;
uint64_t    s_lastAccCheck = 0;

void MaybeRecoverAccess()
{
    Context& c = Get();
    if (!c.connected) return;
    // 会话正在卸载：连锁都不碰，让开给卸载路径（见 Context::uninitReq）。
    if (InterlockedCompareExchange(&c.uninitReq, 0, 0) != 0) return;
    if (!c.dap.AccessSuspect() && dbgAccLevel == kAccNormal) return;

    const uint64_t now = GetTickCount64();
    if ((now - s_lastAccCheck) < kAccCheckMs) return;

    // 拿不到探针锁就让下一轮 —— 调试线程在用探针时本来也不该插进去。
    ComLock lock;
    if (!lock.Held()) return;
    s_lastAccCheck = now;

    const int before = dbgAccLevel;
    DebugAccessEnsure(c.dap, c.core, &c.bps);

    // 等级变了就告诉 uVision（参考 DSMonitor.cpp:374-384）：链路坏掉时 uVision 会挂起 Memory /
    // Watch / 逻辑分析仪窗口，而"已经修好了"只有 driver 知道 —— 不发通知，窗口仍然是空的。
    // 参考还调了 Invalidate()/UpdateAllDlg()，那是它自己窗口的内部函数，driver 侧的对应动作是
    // UV_UPDATE_SELECTIVE。
    if (dbgAccLevel == before) return;

    struct bom* pio = orb::pdsc::Bom();
    if (!pio || !pio->Notify) {
        ORB_LOG_INFO("DbgAcc: 访问等级 %d -> %d（无 BOM，无法通知 uVision）",
                     before, dbgAccLevel);
        return;
    }

    // 这里只发"等级变了"。窗口刷新（UV_UPDATE_SELECTIVE）不放这里 —— 它在 DebugAccessEnsure()
    // 里，那才是参考的位置；上面那次调用已经走过一遍，再来一次等于每周期刷两遍同样的窗口。
    ORB_LOG_INFO("DbgAcc: 访问等级 %d -> %d，通知 uVision", before, dbgAccLevel);
    pio->Notify(UV_DBG_ACCESS_LEVEL, (void*)(INT_PTR)dbgAccLevel);
}

// ① 抽数线程：只搬字节。
unsigned __stdcall DrainBody(void*)
{
    for (;;) {
        RoundTimer round;                        // 整轮计时（见 RoundTimer 的说明）
        if (InterlockedCompareExchange(&s_stop, 0, 0) != 0) break;

        // 可中断的等待：Stop 时置事件，不必等满一个周期。走 PumpSleep 而不是 WaitForSingleObject ——
        // 后者的 2 ms 会被系统定时器粒度取整成 15.6 ms（一轮就从 2 ms 变 20 ms）。
        {
            const unsigned long long w0 = QpcNow();
            PumpSleep(kPeriodMs);
            s_tWait += QpcNow() - w0;
        }

        if (InterlockedCompareExchange(&s_stop, 0, 0) != 0) break;
        ++s_rounds;

        if (TracePumpSuspended()) {
            ++s_skipSuspend;
            continue;
        }

        // 后台访问监视每轮都跑，与有没有 trace 源无关（参考的 DSMonitor 就是无条件执行）。
        // 原来挂在 !TraceDrainWanted() 分支下是 bug：只要开着流式 trace，访问监视就整个停摆 ——
        // 目标在 0x80000000 的 FMC 区读上卡死 AHB，DP 只回 WAIT/NO_ACK，24 秒无人施救。
        // 代价为零：MaybeRecoverAccess() 在健康会话里立刻返回，连锁都不碰。
        MaybeRecoverAccess();

        // 没有流式源时连锁都不碰：调试线程的 AG_AllReg / AG_DapAcc / AG_GoStep
        // 用非阻塞 ComLock（拿不到就报 probe busy），worker 白抢会让它们随机失败
        // （实测：无源的 probe 跑一遍，AG_DapAcc(ACCDAP_ABORT) 间歇返回 100）。
        if (!TraceDrainWanted()) {
            ++s_skipNoSource;
            continue;
        }

        const uint64_t t0 = GetTickCount64();
        const unsigned long long f0 = QpcNow();   // 有源时段起点（见 s_tFeed）
        for (;;) {
            if (InterlockedCompareExchange(&s_stop, 0, 0) != 0) break;
            if (TracePumpSuspended()) break;
            if ((GetTickCount64() - t0) >= kBudgetMs) break;        // 预算用完

            const bool wantOrb = OrbTraceActive();

            // ---- 先占一个队列槽 -------------------------------------------------
            // 队列满 = 解码积压。**照样抽**（抽到 s_stage 后丢弃），不把数据留在探针环里：
            // 环一旦满，丢的是 RDDI 内部帧（不可控、会让 COBS 解帧错位、产生假地址，
            // 正是这次要消灭的那 84 次重同步）。丢在本层则字节数可精确记账，并且能用
            // gapPending 让解码器主动重同步。
            unsigned slot = 0;
            uint8_t* dst = (s_inlineDecode || !wantOrb) ? nullptr : QueueBegin(slot);
            const bool dropping = (!s_inlineDecode && wantOrb && !dst);

            // 非阻塞地拿探针锁；拿不到 = 调试线程在用。但不能把整轮扔掉：调试线程每 1 ms 采一次
            // IsHalted()，而 worker 一轮只试一次实测 41% 的轮次白扔（扔的是这一轮剩下的 20 ms 预算）。
            unsigned orbN = 0;
            unsigned itmN = 0;
            {
                ComLock lock;
                if (!lock.Held()) {
                    ++s_skipBusy;
                    // 这里原来 `Sleep(1)` 实际睡 15.6 ms —— 27% 的轮次就此搭进去
                    // （实测 601 次 = 9.4 s，比真正的搬运与解码多两个数量级）。
                    const unsigned long long sl0 = QpcNow();
                    PumpSleep(1);
                    s_tSleep += QpcNow() - sl0;
                    continue;
                }

                // 拿到锁后再确认一次挂起位：第一次检查可能在本函数拿到锁之前就通过了，而这段时间调试线程
                // 可能刚好挂起。少这一次确认，worker 会在 Flash 放开锁之后接着排空，和它撞在一起。
                if (TracePumpSuspended()) {
                    ++s_skipSuspend;
                    break;
                }

                Context& c = Get();
                const unsigned long long d0 = QpcNow();
                if (ItmActive()) itmN = ItmPump(c.dap, kSliceMs);
                // ← 锁内：**只搬字节**。解码交给下面那条独立线程（原来是同线程串行，
                //   一轮 57 ms 里 54 ms 在解码，探针环因此被灌满）。
                if (wantOrb) {
                    orbN = OrbTracePullRaw(c.dap, dst ? dst : s_stage, kStageBytes, kSliceMs);
                }
                s_tDrain += QpcNow() - d0;
            }
            if (dst) {
                QueueCommit(slot, orbN);
            } else if (dropping) {
                ++s_qDrops;
                s_qDropBytes += orbN;
                if (orbN) InterlockedExchange(&s_gapPending, 1);
                TraceWinErrorSet(kTErrDataOvf);
            }
            s_bytes += orbN + itmN;

            // 队列建不起来时的退路：抽完就地解码（慢，但至少不丢）。
            if (s_inlineDecode && orbN) {
                const unsigned long long dc0 = QpcNow();
                OrbTraceDecodeRaw(s_stage, orbN);
                s_tDecode += QpcNow() - dc0;
                s_bytesDecoded += orbN;
            }

            FoldStatus();

            if ((orbN + itmN) == 0) break;                       // 源排空了
        }
        s_tFeed += QpcNow() - f0;
    }
    return 0;
}

// ② 解码线程：从队列取块，锁外解释（cache / 解帧 / P1-0 补全）。
unsigned __stdcall DecodeBody(void*)
{
    for (;;) {
        const bool stopping = InterlockedCompareExchange(&s_stop, 0, 0) != 0;

        // 这里**不**看挂起位：挂起是为了让调试线程独占探针 / 让 Flash 放锁，而解码线程
        // 不碰探针。反过来，挂起后仍需把队列排空 —— 否则 TracePumpSync() 会一直等。

        unsigned n = 0;
        const uint8_t* p = QueueFront(n);
        if (!p) {
            if (stopping) break;                    // Stop 后把队列排空再退
            WaitForSingleObject(s_qEvent, 5);
            continue;
        }



        // 队列满时丢过块 ⇒ 字节流在这里断了。ETM 是相对地址，必须先重同步，否则接下来
        // 解出的地址全是假的（P0-D）。请求由 ProcessBytes 在持解码锁时执行。
        if (InterlockedExchange(&s_gapPending, 0) != 0) {
            OrbTraceRequestResync();
        }

        const unsigned long long d0 = QpcNow();
        OrbTraceDecodeRaw(p, n);
        s_tDecode += QpcNow() - d0;

        s_bytesDecoded += n;
        ++s_decodeBlocks;
        QueuePop();
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 单次排空
// ---------------------------------------------------------------------------

unsigned TraceDrain(unsigned timeMs)
{
    Context& c = Get();
    if (!c.connected) return 0;

    unsigned total = 0;

    // 三条来源互斥（见 Config 的取值顺序），两个 Pump 在源没开时各自立即返回，故直接都调。
    // ⚠ 本函数在探针锁里跑，只搬字节不解释内容：orbtrace 走 OrbTracePullRaw，cache/解帧/解码交给
    // 锁外的 TraceDecodeStaged()。SWO 暂时仍在锁内解（逐字节，代价低一个量级）。
    if (ItmActive())      total += ItmPump(c.dap, timeMs);
    if (OrbTraceActive()) total += OrbTracePullRaw(c.dap, s_stage, kStageBytes, timeMs);

    return total;
}

void TraceDecodeStaged(unsigned n)
{
    if (!n) return;
    if (n > kStageBytes) n = kStageBytes;   // 防御：调用方只该传 TraceDrain 的返回值
    OrbTraceDecodeRaw(s_stage, n);
}

// ---------------------------------------------------------------------------
// 挂起握手
// ---------------------------------------------------------------------------

void TracePumpSuspend() { InterlockedIncrement(&s_suspend); }
void TracePumpResume()  { if (InterlockedDecrement(&s_suspend) < 0) InterlockedExchange(&s_suspend, 0); }
bool TracePumpSuspended() { return InterlockedCompareExchange(&s_suspend, 0, 0) > 0; }

// 等解码线程把队列里的块全部处理完（深度归零）。返回 false = 超时 / 没有队列。
// ⚠ 稳态有源时队列可能永远不空 —— 那说明解码真跟不上，返回 false 就是事实。
bool TracePumpSync(unsigned timeMs)
{
    if (!s_threadDrain || s_inlineDecode || !s_q) return true;   // 没有队列：调用方走老路径

    const DWORD deadline = GetTickCount() + timeMs;
    for (;;) {
        if (InterlockedCompareExchange(&s_qDepth, 0, 0) <= 0) return true;
        if (static_cast<int>(GetTickCount() - deadline) >= 0) return false;
        Sleep(1);
    }
}

// 停机时的"最后一抽"：叫醒 worker，等它跑到**源排空**（内层循环抽到 0 字节才 break），
// 再等解码线程把块解完。
//
// 为什么需要它：TracePumpSync() 只看队列深度，worker 此刻通常正睡在 kPeriodMs 里，
// 队列是空的 ⇒ 立即返回 true，可探针环里还压着目标停机前最后一段字节。等它们被抽出来时，
// TraceSetRunning(false) 早已把"停止 + 区间"报给 uVision 了 —— 窗口拿到的是空区间或
// 缺尾部的一段；这些记录随后才进 store，uVision 不再来取（实测 +56333 条落在停止之后）。
//
// ⚠ 不能持探针锁调用：worker 抽数要那把锁，持锁等只会等到超时。
bool TracePumpDrainOnce(unsigned timeMs)
{
    if (!s_threadDrain) return false;

    if (s_wake) SetEvent(s_wake);                    // 打断 kPeriodMs 的等待，立刻开一轮

    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeMs);
    LONG lastRound = InterlockedCompareExchange(&s_roundDone, 0, 0);
    unsigned long long lastBytes = s_bytes;

    for (;;) {
        const LONG now = InterlockedCompareExchange(&s_roundDone, 0, 0);
        if (now != lastRound) {
            lastRound = now;
            // 这一整轮一个字节都没抽到 ⇒ 环真的空了（目标已停，不会再有新数据）。
            const unsigned long long bytes = s_bytes;
            if (bytes == lastBytes) break;
            lastBytes = bytes;                      // 抽到了：再等一轮确认抽空
        }
        if (static_cast<int>(GetTickCount() - deadline) >= 0) return false;
        Sleep(1);
    }

    return TracePumpSync(timeMs);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void TracePumpStart()
{
    if (s_threadDrain) return;

    if (!s_wake) {
        s_wake = CreateEvent(nullptr, FALSE, FALSE, nullptr);   // 自动复位
        if (!s_wake) {
            ORB_LOG_WARN("TracePump: 事件创建失败，抽数仍留在调试线程上");
            return;
        }
    }

    // 高精度计时器建不起来不算错：PumpSleep 会退回 `WaitForSingleObject`，
    // 也就是回到 15.6 ms 粒度的老行为（慢，但能跑）。
    if (!s_timer) s_timer = CreatePumpTimer();

    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_suspend, 0);
    s_rounds = s_skipSuspend = s_skipBusy = s_skipNoSource = 0;
    s_bytes  = 0;
    s_bytesDecoded = 0;
    s_decodeBlocks = 0;
    s_tWait = s_tDrain = s_tDecode = s_tStatus = s_tRound = s_tRoundMax = s_roundsTimed = 0;
    s_tFeed = s_tSleep = 0;
    s_qDepthSum = s_qDepthSamples = 0;
    s_qDropBytes = 0;
    s_tStart = GetTickCount64();

    // 槽数可配（诊断 / 内存受限时用）：ORBAGDI_TRACE_QUEUE_SLOTS，8..256。
    {
        char v[32] = {0};
        const DWORD n = GetEnvironmentVariableA("ORBAGDI_TRACE_QUEUE_SLOTS", v, sizeof(v));
        if (n > 0 && n < sizeof(v)) {
            const int k = atoi(v);
            if (k >= 8 && k <= 256) g_qSlots = static_cast<unsigned>(k);
        }
    }

    // 队列：一次性分配，失败就退回老路径（抽完就地解）。
    if (!s_q) {
        s_q    = static_cast<uint8_t*>(malloc(static_cast<size_t>(g_qSlots) * kStageBytes));
        s_qLen = static_cast<unsigned*>(malloc(sizeof(unsigned) * g_qSlots));
        if (!s_q || !s_qLen) {
            free(s_q);      s_q = nullptr;
            free(s_qLen);   s_qLen = nullptr;
            s_inlineDecode = true;
            ORB_LOG_WARN("TracePump: 队列分配失败（%u × %u KB）—— 退回抽完就地解码",
                         g_qSlots, kStageBytes / 1024);
        } else {
            s_inlineDecode = false;
        }
    }
    if (!s_qEvent) s_qEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    InterlockedExchange(&s_qWrite, 0);
    InterlockedExchange(&s_qRead, 0);
    InterlockedExchange(&s_qDepth, 0);
    InterlockedExchange(&s_qMaxDepth, 0);
    InterlockedExchange(&s_qDrops, 0);
    InterlockedExchange(&s_gapPending, 0);

    unsigned tid = 0;

    // 先起解码线程：它只在队列里有块时才动，先起不会有副作用；反过来（先起抽数）在
    // 解码线程创建失败时会积压一批没人取的块。
    if (!s_inlineDecode) {
        s_threadDecode = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, DecodeBody, nullptr, 0, &tid));
        if (!s_threadDecode) {
            ORB_LOG_WARN("TracePump: 解码线程创建失败 —— 退回抽完就地解码");
            s_inlineDecode = true;
        }
    }

    s_threadDrain = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, DrainBody, nullptr, 0, &tid));
    if (!s_threadDrain) {
        ORB_LOG_WARN("TracePump: 抽数线程创建失败，抽数仍留在调试线程上");
        if (s_threadDecode) {
            InterlockedExchange(&s_stop, 1);
            if (s_qEvent) SetEvent(s_qEvent);
            WaitForSingleObject(s_threadDecode, 2000);
            CloseHandle(s_threadDecode);
            s_threadDecode = nullptr;
        }
        return;
    }

    ORB_LOG_INFO("TracePump: 抽数 + 解码两条线程已启动 (预算 %u ms / 时间片 %u ms / "
                 "周期 %u ms / 队列 %u × %u KB%s)",
                 kBudgetMs, kSliceMs, kPeriodMs, g_qSlots, kStageBytes / 1024,
                 s_inlineDecode ? "，**未启用**：退回就地解码" : "");
}

void TracePumpStop()
{
    if (!s_threadDrain) return;

    InterlockedExchange(&s_stop, 1);
    if (s_wake)   SetEvent(s_wake);
    if (s_qEvent) SetEvent(s_qEvent);   // 叫醒解码线程：看 stop 位，把队列排空再退

    // 一轮最多 kBudgetMs，所以正常情况下几毫秒内就退了；2 s 只是兜底。
    const DWORD w = WaitForSingleObject(s_threadDrain, 2000);
    if (w != WAIT_OBJECT_0) {
        // 不 TerminateThread（它可能正握着探针），也**不**把句柄置空 ——
        // 置空会让下一次 TracePumpStart() 再建一个 worker，而老的还在跑，于是
        // 两个线程一起抽同一个探针。宁可留着这个句柄：Start 见到它就什么都不做。
        ORB_LOG_WARN("TracePump: 抽数线程 2 s 内未退出，保留句柄不重启");
        return;
    }

    // 解码线程会把队列里剩下的块处理完再退（stop 之后不再有新块进来）。
    // 给 5 s：积压 48 槽 × 256 KB，提速后约 100 ms/块，排空最多 5 s 左右。
    if (s_threadDecode) {
        const DWORD dw = WaitForSingleObject(s_threadDecode, 5000);
        if (dw != WAIT_OBJECT_0) {
            ORB_LOG_WARN("TracePump: 解码线程 2 s 内未退出，保留句柄与队列不重启");
            return;
        }
    }

    ORB_LOG_INFO("TracePump: 抽数线程已停止 (轮=%ld 挂起跳过=%ld 探针忙跳过=%ld "
                 "无源跳过=%ld 排空=%llu B)",
                 s_rounds, s_skipSuspend, s_skipBusy, s_skipNoSource, s_bytes);
    // 解码线程必须有自己的计时与繁忙度（Todo §3.1 判据）—— 没有这个数就不许说"够用"。
    {
        const double wallMs = static_cast<double>(GetTickCount64() - s_tStart);
        const double decMs  = QpcMs(s_tDecode);
        const double mb     = static_cast<double>(kStageBytes) / 1048576.0;
        ORB_LOG_INFO("TracePump: 解码线程已停止 (块=%llu 解码=%llu B 平均 %.1f ms/槽 "
                     "繁忙度 %.0f%% / 墙钟 %.1f s)",
                     s_decodeBlocks, s_bytesDecoded,
                     s_decodeBlocks ? decMs / static_cast<double>(s_decodeBlocks) : 0.0,
                     wallMs > 0.0 ? 100.0 * decMs / wallMs : 0.0,
                     wallMs / 1000.0);
        ORB_LOG_INFO("TracePump: 队列积压 峰值=%.1f MB 均值=%.1f MB (%u 槽 × %u KB)；"
                     "队列满丢弃=%ld 片 / %.2f MB",
                     static_cast<double>(s_qMaxDepth) * mb,
                     s_qDepthSamples ? static_cast<double>(s_qDepthSum) /
                                       static_cast<double>(s_qDepthSamples) * mb : 0.0,
                     g_qSlots, kStageBytes / 1024,
                     s_qDrops, static_cast<double>(s_qDropBytes) / 1048576.0);
    }

    // 每轮耗时分解 —— 这行是判据：等待占绝大部分 ⇒ 等的是定时器粒度（改等待方式）；
    // 解码占大头 ⇒ 锁外也追不上（该压 ScanTo 的每片预算）；状态折叠占大头 ⇒ 还有跨线程投递；
    // 几段都小、平均却很大 ⇒ 时间花在没计时的别处（拿锁、读探针）。
    if (s_roundsTimed) {
        const double n = static_cast<double>(s_roundsTimed);
        // 注意"就地解码"一栏只在队列建不起来时才有值 —— 正常路径上解码已经搬到
        // 另一条线程，不计入抽数线程的轮耗时（它的总耗时见上一行）。
        ORB_LOG_INFO("TracePump: 每轮耗时 平均 %.2f ms（等待 %.2f / 抽数 %.2f / "
                     "就地解码 %.2f / 状态折叠 %.2f）最大 %.2f ms（%llu 轮）",
                     QpcMs(s_tRound) / n, QpcMs(s_tWait) / n,
                     QpcMs(s_tDrain) / n, QpcMs(s_tDecode) / n,
                     QpcMs(s_tStatus) / n,
                     QpcMs(s_tRoundMax), s_roundsTimed);
    }

    // ---- 吞吐判据 ----
    // ① 抽数/解码的 ms/64KB：处理一个字节要多少算力，与醒来频率无关（基线 0.7 / 0.98 ms/64KB）。
    // ② 等待占比是节拍问题，该修等待方式而不是解码。③ dropped > 0 是环丢包的硬证据，且只由
    // 抽数决定 —— 解码慢不会直接丢 RDDI 环，别把丢包算到解码头上。
    // "解码"里含同步写 cache（fwrite），开了 cache 先减掉"其中 cache"，否则把磁盘写当成解码算力。
    if (s_bytes) {
        const double mb      = static_cast<double>(s_bytes) / (1024.0 * 1024.0);
        const double sessionS = QpcMs(s_tRound) / 1000.0;     // 会话总时长（含等待）
        const double feedS    = QpcMs(s_tFeed)  / 1000.0;     // 有源时段（**含让锁**）
        const double sleepS   = QpcMs(s_tSleep) / 1000.0;     // 其中让锁
        const double workS    = feedS - sleepS;               // 真正干活
        const double per64k   = static_cast<double>(s_bytes) / 65536.0;
        const double drainMs  = QpcMs(s_tDrain);
        const double decodeMs = QpcMs(s_tDecode);
        const double drainPer = drainMs  / per64k;
        const double decodePer = decodeMs / per64k;
        const OrbTraceStats st = OrbTraceGetStats();

        // 数据率的分母必须是**会话时长**：供给速率是"每秒来多少字节"，与 worker
        // 醒了几次无关。拿"有源时段"当分母会得到处理速率，看起来高得离谱。
        ORB_LOG_INFO("TracePump: 吞吐 数据 %.2f MB / 会话 %.1f s = %.1f KB/s；"
                     "干活 %.3f s（有源 %.2f s，其中让锁 %.2f s）；"
                     "抽数 %.2f ms/64KB，解码 %.2f ms/64KB（解码是抽数的 %.1f 倍，"
                     "其中 cache %.2f）",
                     mb, sessionS, sessionS > 0.0 ? mb * 1024.0 / sessionS : 0.0,
                     workS, feedS, sleepS,
                     drainPer, decodePer,
                     drainPer > 0.0 ? decodePer / drainPer : 0.0,
                     static_cast<double>(st.writeMs) / per64k);

        if (st.dropped) {
            ORB_LOG_WARN("TracePump: RDDI 环累计丢了 %llu 字节 —— 抽数跟不上；"
                         "对照上面两个 MB/s 与解码倍数定位", st.dropped);
        }
    }

    CloseHandle(s_threadDrain);
    s_threadDrain = nullptr;
    if (s_threadDecode) { CloseHandle(s_threadDecode); s_threadDecode = nullptr; }
    if (s_timer) { CloseHandle(s_timer); s_timer = nullptr; }
}

bool TracePumpRunning() { return s_threadDrain != nullptr; }

} // namespace agdi
} // namespace orb
