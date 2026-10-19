/**
 * @file ORBAGDI_TracePump.cpp
 * @brief 抽数工作线程的实现（结构见 ORBAGDI_TracePump.h）。
 *
 * 循环只有三步：挂起就跳过 → 拿不到探针锁就跳过（调试线程在用）→ 排空一个时间片。
 * 锁只持有那一个时间片（TraceDrain(2)）：worker 若连人带锁占住 20 ms 预算，调试线程的
 * 运行循环就抽不到 IsHalted()，Stop 的响应退回到几十毫秒 —— 正是这一轮要消灭的东西。
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
HANDLE          s_wake   = nullptr;   // Stop 的打断事件
HANDLE          s_timer  = nullptr;   // 高精度可等待计时器，见 PumpSleep()
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

// 有源时段的总耗时：只有内层循环（真在抽/解）才累加。⚠ 它含 s_tSleep —— 拿不到锁时的让锁
// 也发生在内层，算数据率要减掉，否则把 Sleep(1) 当成劳动力（实测 601 次让锁睡 9.4 s）。
unsigned long long s_tFeed = 0;

// 内层循环里"拿不到锁就让一下"的累计耗时。单列的唯一原因是它**不是干活**：
// `Sleep(1)` 在没抬定时器精度时实际睡 15.6 ms，这一项曾经被读成"活跃 9.63 s"。
unsigned long long s_tSleep = 0;

unsigned long long s_tRound = 0;     // 整轮（含等待）
unsigned long long s_tRoundMax = 0;
unsigned long long s_roundsTimed = 0;

// 抽（锁内）与解（锁外）之间的交接缓冲，只有 worker 用（调试线程走 OrbTracePump()，自带 buf）。
// 256 KB = 4 个 RDDI 传输块：拿满锁纯 memcpy 约 0.4 ms 就放，既不挡调试线程又不占内存。
// 为什么必须分开，见 ORBAGDI_OrbTrace.h 的"抽/解 分离"（那里记着一次丢 506 MB 的证据链）。
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
    }
};

// 现在有流式 trace 源在跑吗？没有的话 worker 连探针锁都不该碰（见线程体里的说明）。
bool TraceDrainWanted()
{
    return ItmActive() || OrbTraceActive();
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

unsigned __stdcall Body(void*)
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

            // 非阻塞地拿探针锁；拿不到 = 调试线程在用。但不能把整轮扔掉：调试线程每 1 ms 采一次
            // IsHalted()，而 worker 一轮只试一次实测 41% 的轮次白扔（扔的是这一轮剩下的 20 ms 预算）。
            unsigned n = 0;
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

                const unsigned long long d0 = QpcNow();
                n = TraceDrain(kSliceMs);            // ← 锁内：**只搬字节**
                s_tDrain += QpcNow() - d0;
            }
            s_bytes += n;

            // 解码在锁外：orbtrace 的落盘 / 解帧 / P1-0 全指令重建（实测 ~15 ms/64 KB）原先和
            // TraceStream_Read 挤在同一个 ComLock 里，worker 抱着锁不放 ⇒ 调试线程停不下目标、
            // DP 转 WAIT/NO_ACK、链路死掉，32 MB 环里丢 506 MB。
            if (n) {
                const unsigned long long dc0 = QpcNow();
                TraceDecodeStaged(n);
                s_tDecode += QpcNow() - dc0;
            }

            // 状态折叠与 T_Recover 倒计时（官方在 Trace_Read 里做，而它从 DSMonitor 线程调）都在锁外。
            // 这里不再有跨线程投递：TraceWinStatusMsg() 只排队，真正 Notify 的是 TraceWinStatusFlush()，
            // 由调试线程轮询调用。timeMs 传 0：这边只剩折叠、没有阻塞读。
            const unsigned long long st0 = QpcNow();
            TraceWinRead(0, false);
            TraceWinRecoverTick(kSliceMs);
            s_tStatus += QpcNow() - st0;

            if (n == 0) break;                                   // 源排空了
        }
        s_tFeed += QpcNow() - f0;
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
    // ⚠ 本函数在探针锁里跑，只搬字节不解释内容：orbtrace 走 OrbTracePullRaw，落盘/解帧/解码交给
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

    // 高精度计时器建不起来不算错：PumpSleep 会退回 `WaitForSingleObject`，
    // 也就是回到 15.6 ms 粒度的老行为（慢，但能跑）。
    if (!s_timer) s_timer = CreatePumpTimer();

    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_suspend, 0);
    s_rounds = s_skipSuspend = s_skipBusy = s_skipNoSource = 0;
    s_bytes  = 0;
    s_tWait = s_tDrain = s_tDecode = s_tStatus = s_tRound = s_tRoundMax = s_roundsTimed = 0;
    s_tFeed = s_tSleep = 0;

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

    // 每轮耗时分解 —— 这行是判据：等待占绝大部分 ⇒ 等的是定时器粒度（改等待方式）；
    // 解码占大头 ⇒ 锁外也追不上（该压 ScanTo 的每片预算）；状态折叠占大头 ⇒ 还有跨线程投递；
    // 几段都小、平均却很大 ⇒ 时间花在没计时的别处（拿锁、读探针）。
    if (s_roundsTimed) {
        const double n = static_cast<double>(s_roundsTimed);
        ORB_LOG_INFO("TracePump: 每轮耗时 平均 %.2f ms（等待 %.2f / 抽数 %.2f / "
                     "解码 %.2f / 状态折叠 %.2f）最大 %.2f ms（%llu 轮）",
                     QpcMs(s_tRound) / n, QpcMs(s_tWait) / n,
                     QpcMs(s_tDrain) / n, QpcMs(s_tDecode) / n,
                     QpcMs(s_tStatus) / n,
                     QpcMs(s_tRoundMax), s_roundsTimed);
    }

    // ---- 吞吐判据 ----
    // ① 抽数/解码的 ms/64KB：处理一个字节要多少算力，与醒来频率无关（基线 0.7 / 0.98 ms/64KB）。
    // ② 等待占比是节拍问题，该修等待方式而不是解码。③ dropped > 0 是环丢包的硬证据，且只由
    // 抽数决定 —— 解码慢不会直接丢 RDDI 环，别把丢包算到解码头上。
    // "解码"里含同步落盘（fwrite），开了落盘先减掉"其中落盘"，否则把磁盘写当成解码算力。
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
                     "其中落盘 %.2f）",
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

    CloseHandle(s_thread);
    s_thread = nullptr;
    if (s_timer) { CloseHandle(s_timer); s_timer = nullptr; }
}

bool TracePumpRunning() { return s_thread != nullptr; }

} // namespace agdi
} // namespace orb
