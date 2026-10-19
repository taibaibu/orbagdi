/**
 * @file ORBAGDI_TracePump.cpp
 * @brief 抽数工作线程的实现（见 ORBAGDI_TracePump.h 的结构说明）。
 *
 * 线程体只有一个循环：
 *
 *     挂起？        -> 跳过本轮
 *     拿到探针锁？  -> 否：跳过本轮（调试线程正用探针）
 *     排空一个时间片
 *
 * "排空一个时间片"是 `TraceDrain(2)`，**锁只持有这一个时间片**。这一点是有意
 * 的：worker 要是把整段预算（20 ms）连人带锁占住，调试线程的运行循环就抽不到
 * `IsHalted()`，Stop 的响应会退回到几十毫秒量级 —— 那正是这一轮要消灭的东西。
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

HANDLE          s_thread = nullptr;
HANDLE          s_wake   = nullptr;
volatile LONG   s_stop   = 0;
volatile LONG   s_suspend = 0;

// 诊断计数（只在 stop 时打一次日志）。
volatile LONG   s_rounds = 0;
volatile LONG   s_skipSuspend = 0;
volatile LONG   s_skipBusy = 0;
volatile LONG   s_skipNoSource = 0;
unsigned long long s_bytes = 0;

// 每轮耗时的分解。用 QPC 而不是 `GetTickCount`：后者的 15.6 ms 粒度量不出
// "等待 / 排空 / 解码 / 状态折叠"这几段。停线程时打一行 —— 21 ms 的周期到底
// 花在哪，靠这一行定案，不靠推测。
unsigned long long s_tWait = 0;      // WaitForSingleObject() 的时间
unsigned long long s_tDrain = 0;     // TraceDrain()（**锁内**：只搬字节）的时间
unsigned long long s_tDecode = 0;    // TraceDecodeStaged()（**锁外**：落盘/解帧/解码）
unsigned long long s_tStatus = 0;    // TraceWinRead/TraceWinRecoverTick 的时间
unsigned long long s_tRound = 0;     // 整轮（含等待）
unsigned long long s_tRoundMax = 0;
unsigned long long s_roundsTimed = 0;

// 抽（锁内）与解（锁外）之间的交接缓冲。**只有 worker 用它** —— 调试线程走
// `OrbTracePump()`，那里有自己的 buf（见 ORBAGDI_OrbTrace.cpp）。
//
// 256 KB = 4 个 RDDI 传输块：一次拿满锁（纯 memcpy，约 0.4 ms）就放，既让调试
// 线程随时能插进来，又小到不占内存。为什么必须分开，见 ORBAGDI_OrbTrace.h 的
// "抽 / 解 分离"一节 —— 那里记着一次会话丢 506 MB 的完整证据链。
const unsigned kStageBytes = 256u * 1024u;
uint8_t        s_stage[kStageBytes];

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

// 整轮计时：`continue` 那条路也要计到，所以用析构而不是在循环末尾累加。
struct RoundTimer {
    unsigned long long t0;
    RoundTimer() : t0(QpcNow()) {}
    ~RoundTimer() {
        const unsigned long long d = QpcNow() - t0;
        s_tRound += d;
        if (d > s_tRoundMax) s_tRoundMax = d;
        ++s_roundsTimed;
    }
};

// 现在有流式 trace 源在跑吗？没有的话 worker 连探针锁都不该碰（见线程体里的说明）。
bool TraceDrainWanted()
{
    return ItmActive() || OrbTraceActive();
}

// ---------------------------------------------------------------------------
// 后台访问监视 —— 参考 DSMonitor 的等价物
// ---------------------------------------------------------------------------
// 参考的 DSMonitor 线程在它的循环里调 `DebugAccessRecovery()`（`DSMonitor.cpp:325-329`）。
// 这是"一次坏访问之后链路能自己回来"的关键：**恢复不发生在访问路径上** ——
// 等 uVision 再来读内存/寄存器时，后台已经修好了。
//
// 本驱动没有独立的监控线程，这个 worker 就是等价物：没有 trace 源时它本来就在空转，
// 正好用它。周期取 100 ms（参考正常态是 50 ms，`AGDI.CPP:3161`）。
//
// ⚠ **只在 `AccessSuspect()` 为真、或等级已经变坏时才动。** 健康会话一次探针都不碰，
//   所以 P0-A 的"无流式源就不拿探针锁"（`skipBusy == 0`）不受影响；只有真出过错才去
//   探测，而那时用户的窗口本来就是坏的，占一下锁是应该的。
const DWORD kAccCheckMs = 100u;
DWORD       s_lastAccCheck = 0;

void MaybeRecoverAccess()
{
    Context& c = Get();
    if (!c.connected) return;
    // 会话正在卸载：连锁都不碰，让开给卸载路径（见 Context::uninitReq）。
    if (InterlockedCompareExchange(&c.uninitReq, 0, 0) != 0) return;
    if (!c.dap.AccessSuspect() && dbgAccLevel == kAccNormal) return;

    const DWORD now = GetTickCount();
    if ((now - s_lastAccCheck) < kAccCheckMs) return;

    // 拿不到探针锁就让下一轮 —— 调试线程在用探针时本来也不该插进去。
    ComLock lock;
    if (!lock.Held()) return;
    s_lastAccCheck = now;

    const int before = dbgAccLevel;
    DebugAccessEnsure(c.dap, c.core, &c.bps);

    // 等级变了就告诉 uVision —— 参考 `DSMonitor.cpp:374-384`。
    //
    // 这一步不是装饰。链路坏掉时 uVision 会把 Memory / Watch / 逻辑分析仪窗口挂起
    // 不再刷新，而"已经修好了"只有 driver 知道；不发这个通知，恢复成功了窗口仍然是
    // 空的，用户看到的还是"卡死"。参考的同一个位置正是这么做的：
    //
    //     pio->Notify (UV_DBG_ACCESS_LEVEL, (void *)dbgAccLevel);
    //     if (dbgAccLevel == DA_NORMAL && ... && (dhcsr & S_HALT))
    //         pio->Notify (UV_UPDATE_SELECTIVE, VW_MEMORY | VW_WATCHWINDOW | ...);
    //
    // 参考在中间还调了 Invalidate()/UpdateAllDlg()，那是它自己窗口的内部函数，
    // driver 侧没有对应物，UV_UPDATE_SELECTIVE 才是等价动作。
    if (dbgAccLevel == before) return;

    struct bom* pio = orb::pdsc::Bom();
    if (!pio || !pio->Notify) {
        ORB_LOG_INFO("DbgAcc: 访问等级 %d -> %d（无 BOM，无法通知 uVision）",
                     before, dbgAccLevel);
        return;
    }

    // 这里只发"等级变了"（参考的 DSMonitor 在 `DSMonitor.cpp:377` 做的正是这一件）。
    // 窗口刷新（`UV_UPDATE_SELECTIVE`）不在这里做 —— 它在 `DebugAccessEnsure()`
    // 里，那才是参考放它的位置（`DbgCM/DebugAccess.cpp:711-721`），而上面那次调用
    // 已经走过一遍。在这里再来一次等于每个周期刷两遍同样的窗口。
    ORB_LOG_INFO("DbgAcc: 访问等级 %d -> %d，通知 uVision", before, dbgAccLevel);
    pio->Notify(UV_DBG_ACCESS_LEVEL, (void*)(INT_PTR)dbgAccLevel);
}

unsigned __stdcall Body(void*)
{
    for (;;) {
        RoundTimer round;                        // 整轮计时（见 RoundTimer 的说明）
        if (InterlockedCompareExchange(&s_stop, 0, 0) != 0) break;

        // 可中断的等待：Stop 时置事件，不必等满一个周期。
        {
            const unsigned long long w0 = QpcNow();
            WaitForSingleObject(s_wake, kPeriodMs);
            s_tWait += QpcNow() - w0;
        }

        if (InterlockedCompareExchange(&s_stop, 0, 0) != 0) break;
        ++s_rounds;

        if (TracePumpSuspended()) {
            ++s_skipSuspend;
            continue;
        }

        // 后台访问监视**每轮都跑**，与有没有 trace 源无关。
        //
        // 参考的 DSMonitor 线程就是这样：`DSMonitor.cpp:324-363` 的
        // DebugAccessRecovery() / DebugAccessDetection() 在循环里无条件执行，读
        // trace 只是同一轮里的另一段（:318-323 与 :325+ 并列）。
        //
        // 这里原来把它挂在 `!TraceDrainWanted()` 分支下，是个 bug：**只要开着流式
        // trace，访问监视就整个停摆**。实测（2026-10-13 14:45 会话，orbtrace=1）：
        // 目标在 0x80000000 的 96 字节块读上卡死 AHB（FMC 区没接器件），DP 从此只回
        // WAIT/NO_ACK，日志 1933 行全是 `DP write reg=2 failed`、24 秒无人施救。
        // 线程停止时的计数器 `无源跳过=0` 就是证据 —— 那条分支一次都没进。
        //
        // 代价为零：MaybeRecoverAccess() 在健康会话里立刻返回（`AccessSuspect()` 为假
        // 且等级仍是 kAccNormal），连锁都不碰，所以 P0-A 的 skipBusy==0 不受影响。
        MaybeRecoverAccess();

        // 没有流式源时连锁都不碰：调试线程的 AG_AllReg / AG_DapAcc / AG_GoStep
        // 用非阻塞 ComLock（拿不到就报 probe busy），worker 白抢会让它们随机失败
        // （实测：无源的 probe 跑一遍，AG_DapAcc(ACCDAP_ABORT) 间歇返回 100）。
        if (!TraceDrainWanted()) {
            ++s_skipNoSource;
            continue;
        }

        const DWORD t0 = GetTickCount();
        for (;;) {
            if (InterlockedCompareExchange(&s_stop, 0, 0) != 0) break;
            if (TracePumpSuspended()) break;
            if ((GetTickCount() - t0) >= kBudgetMs) break;        // 预算用完

            // 非阻塞地拿探针锁；拿不到 = 调试线程在用探针。
            // **拿不到不能把整轮扔掉**：调试线程的运行循环每 1 ms 采一次
            // IsHalted()，worker 一轮只试一次的话实测 41% 的轮次白扔 —— 扔掉的
            // 是这一轮剩下的 20 ms 预算。所以在预算内让一下再试。
            unsigned n = 0;
            {
                ComLock lock;
                if (!lock.Held()) {
                    ++s_skipBusy;
                    Sleep(1);
                    continue;
                }

                // 拿到锁后**再确认一次挂起位**：第一次检查可能在本函数拿到锁之前
                // 就通过了，而这段时间调试线程可能刚好挂起（Flash 的 PumpQuiesce
                // 是"挂起 → 拿一次锁确认 worker 已退出 → 立刻放开"）。少这一次
                // 确认，worker 会在它放开锁之后接着排空，和 Flash 撞在一起。
                if (TracePumpSuspended()) {
                    ++s_skipSuspend;
                    break;
                }

                const unsigned long long d0 = QpcNow();
                n = TraceDrain(kSliceMs);            // ← 锁内：**只搬字节**
                s_tDrain += QpcNow() - d0;
            }
            s_bytes += n;

            // 解码在**锁外**（2026-10-14）。orbtrace 的落盘 / 解帧 / P1-0 全指令
            // 重建（实测 ~15 ms/64 KB）原先和 `TraceStream_Read` 挤在同一个
            // `ComLock` 里，于是 worker 每轮只完成一次读就超出 2 ms 时间片，而且
            // 抱着锁不放 —— 调试线程停不下目标（实测 `GoStep: 停机收尾没拿到
            // 探针锁，worker 未让出`），随后 DP 转 WAIT/NO_ACK、链路死掉，32 MB
            // 环里丢掉 506 MB。见 ORBAGDI_OrbTrace.h 的"抽 / 解 分离"。
            if (n) {
                const unsigned long long dc0 = QpcNow();
                TraceDecodeStaged(n);
                s_tDecode += QpcNow() - dc0;
            }

            // 状态折叠与 T_Recover 倒计时（官方在 Trace_Read 里做，而 Trace_Read
            // 从 DSMonitor 线程调）都在**锁之外**。
            //
            // 这里**不再有跨线程投递**：`TraceWinStatusMsg()` 现在只排队，真正
            // `bom->Notify(UV_RTAH_ERRMSG, ...)` 的那一下在 `TraceWinStatusFlush()`，
            // 由调试线程的轮询调用（见 ORBAGDI_TraceWin.cpp 的说明）。原先这一轮
            // 每一圈都要在 uVision 的 GUI 线程门口等一次。
            // timeMs 传 0：TraceWinRead 这边只剩折叠、没有阻塞读。
            const unsigned long long st0 = QpcNow();
            TraceWinRead(0, false);
            TraceWinRecoverTick(kSliceMs);
            s_tStatus += QpcNow() - st0;

            if (n == 0) break;                                   // 源排空了
        }
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

    // 三条来源互斥（见 ORBAGDI_Config.cpp 的取值顺序），两个 Pump 各自会在
    // 源没开时立即返回，所以这里直接都调。
    //
    // ⚠ 本函数在**探针锁里**跑，所以它**只搬字节**、不解释内容：orbtrace 走
    // `OrbTracePullRaw`，落盘 / 解帧 / 解码交给锁外的 `TraceDecodeStaged()`。
    // SWO 那条暂时仍在锁内解 —— 它是逐字节的，代价低一个量级（见 Todo）。
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

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void TracePumpStart()
{
    if (s_thread) return;

    if (!s_wake) {
        s_wake = CreateEvent(nullptr, FALSE, FALSE, nullptr);   // 自动复位
        if (!s_wake) {
            ORB_LOG_WARN("TracePump: 事件创建失败，抽数仍留在调试线程上");
            return;
        }
    }

    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_suspend, 0);
    s_rounds = s_skipSuspend = s_skipBusy = s_skipNoSource = 0;
    s_bytes  = 0;
    s_tWait = s_tDrain = s_tDecode = s_tStatus = s_tRound = s_tRoundMax = s_roundsTimed = 0;

    unsigned tid = 0;
    s_thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, Body, nullptr, 0, &tid));
    if (!s_thread) {
        ORB_LOG_WARN("TracePump: 工作线程创建失败，抽数仍留在调试线程上");
        return;
    }
    ORB_LOG_INFO("TracePump: 抽数工作线程已启动 (预算 %u ms / 时间片 %u ms / 周期 %u ms)",
                 kBudgetMs, kSliceMs, kPeriodMs);
}

void TracePumpStop()
{
    if (!s_thread) return;

    InterlockedExchange(&s_stop, 1);
    if (s_wake) SetEvent(s_wake);

    // 一轮最多 kBudgetMs，所以正常情况下几毫秒内就退了；2 s 只是兜底。
    const DWORD w = WaitForSingleObject(s_thread, 2000);
    if (w != WAIT_OBJECT_0) {
        // 不 TerminateThread（它可能正握着探针），也**不**把 s_thread 置空 ——
        // 置空会让下一次 TracePumpStart() 再建一个 worker，而老的还在跑，于是
        // 两个线程一起抽同一个探针。宁可留着这个句柄：Start 见到它就什么都不做。
        ORB_LOG_WARN("TracePump: 工作线程 2 s 内未退出，保留句柄不重启");
        return;
    }
    ORB_LOG_INFO("TracePump: 抽数工作线程已停止 (轮=%ld 挂起跳过=%ld 探针忙跳过=%ld "
                 "无源跳过=%ld 排空=%llu B)",
                 s_rounds, s_skipSuspend, s_skipBusy, s_skipNoSource, s_bytes);

    // 每轮耗时的分解 —— **这行是判据**。四种可能的落点：
    //   · "等待"占绝大部分 ⇒ 周期是 `WaitForSingleObject(2 ms)` 被系统定时器
    //     粒度（未抬精度时 15.6 ms）向上取整，与 GUI 无关，要改的是等待方式；
    //   · "解码"占大头 ⇒ 锁外也追不上（P1-0 全指令重建的代价），那时丢帧是
    //     算力不够，不是锁的问题 —— 要压的是 `ScanTo` 的每片预算；
    //   · "状态折叠"占大头 ⇒ 还有东西在 worker 上跨线程投递；
    //   · 几段都很小、平均却很大 ⇒ 时间花在本函数没计时的别处（拿锁、读探针）。
    if (s_roundsTimed) {
        const double n = static_cast<double>(s_roundsTimed);
        ORB_LOG_INFO("TracePump: 每轮耗时 平均 %.2f ms（等待 %.2f / 抽数 %.2f / "
                     "解码 %.2f / 状态折叠 %.2f）最大 %.2f ms（%llu 轮）",
                     QpcMs(s_tRound) / n, QpcMs(s_tWait) / n,
                     QpcMs(s_tDrain) / n, QpcMs(s_tDecode) / n,
                     QpcMs(s_tStatus) / n,
                     QpcMs(s_tRoundMax), s_roundsTimed);
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
}

bool TracePumpRunning() { return s_thread != nullptr; }

} // namespace agdi
} // namespace orb
