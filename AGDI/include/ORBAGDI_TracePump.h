/**
 * @file ORBAGDI_TracePump.h
 * @brief 抽数工作线程 —— 官方 `DSMonitor_Thread` 的等价物（Todo.md 的 P0-A）。
 *
 * 官方的 trace 数据**不在 uVision 的调试线程上抽**：调试线程把目标放开之后只
 * 每 50 ms 轮询一次状态
 *
 *     DbgCM/AGDI.CPP:3155-3173     do { ... Sleep(50); } while (!(DSMonitorState
 *                                   & DSM_STATE_CPU_HALTED));
 *
 * 真正的取数由独立线程做，并在 trace 打开时按"时间预算"循环
 *
 *     DbgCM/DSMonitor.cpp:454      DSMonitorThread.h = _beginthread(DSMonitor_Thread, ...);
 *     DbgCM/DSMonitor.cpp:138-147  if (DSMonitorThread.traceActive) { ... Trace_Read(50, TRUE); }
 *
 * 本模块把这条结构搬过来：一个 worker 线程排空 trace 源（`ItmPump` /
 * `OrbTracePump`），调试线程不再抽数。这样单次取数的耗时**不再决定 Stop 的响应
 * 粒度** —— 实测留在调试线程上时，一次抽满 2 MB 并当场解码要阻塞约 106 ms
 * （证据 `COMPAT_ANALYSIS.md` §10.26）。
 *
 * ⚠ 与官方的三点配套（`COMPAT_ANALYSIS.md` §10.27）：
 *
 *   1. **挂起握手**（官方 `DSMonitorThread.suspendCount`，`DbgCM/DSMonitor.h:76`）：
 *      调试线程做"必须不被打断"的目标操作前 `TracePumpSuspend()`，做完
 *      `TracePumpResume()`。worker 每个阶段检查它，挂起期间不碰探针。
 *   2. **探针占用锁**：worker 每次只锁一小段（一个时间片），让调试线程随时能插
 *      进来；反过来，要独占探针的调用方（Flash 擦 / 写 / 校验）挂起 worker 之后
 *      再 `ComLockWait` 拿一次锁 —— 挂起只保证"worker 不再开始新一轮"，真正让它
 *      **退出**正在跑的那一片的是锁。两者缺一就有竞态。
 *   3. **回调纪律**：回调在锁之外发。worker 不持有任何自己的锁去调 uVision 回调。
 *
 * 归属：**AGDI 层**。ETB 是"停机后读回"，不是流式源，所以**不**走 worker，
 * 仍由调试线程在停机后 `EtbProcess()`（见 ORBAGDI_Etb.h）。
 */
#pragma once

namespace orb {
namespace agdi {

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

// 启动 / 停止工作线程（幂等）。会话建立后启动、断开时停止。
void TracePumpStart();
void TracePumpStop();
bool TracePumpRunning();

// ---------------------------------------------------------------------------
// 单次排空
// ---------------------------------------------------------------------------

// 排空所有**流式** trace 源（SWO / orbtrace），按 `timeMs` 限流。
// 返回本轮搬走的原始字节数（0 = 没东西可搬，或没有源在跑）。
//
// ⚠ **必须在持有探针锁时调**，而且它**只搬字节**：orbtrace 的落盘 / 解帧 / 解码
// 由 `TraceDecodeStaged()` 在**锁外**做。理由见 `ORBAGDI_OrbTrace.h` 的"抽 / 解
// 分离"一节 —— P1-0 全指令重建实测 ~15 ms/64 KB，留在锁里 worker 就会抱着锁不放
// （调试线程停不下目标、RDDI 环溢出丢 506 MB）。
//
// （SWO 那条暂时仍在锁内解 —— 它是逐字节的，代价低一个量级；要一起拆见 Todo。）
unsigned TraceDrain(unsigned timeMs);

// 处理 `TraceDrain` 刚搬出来的那 `n` 字节（**必须在锁外调**）。
// `n` 必须是 `TraceDrain` 的返回值 —— 顺序不能乱，也不能重复喂。
void TraceDecodeStaged(unsigned n);

// ---------------------------------------------------------------------------
// 挂起握手（官方 `suspendCount`）
// ---------------------------------------------------------------------------

// 调试线程在长目标操作（Flash 擦 / 写 / 校验）前后配对调用。可重入（计数）。
void TracePumpSuspend();
void TracePumpResume();
bool TracePumpSuspended();

} // namespace agdi
} // namespace orb
