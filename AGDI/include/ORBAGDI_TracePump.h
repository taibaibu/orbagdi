/**
 * @file ORBAGDI_TracePump.h
 * @brief 抽数工作线程（官方 DSMonitor_Thread 的等价物）。
 * 官方取数不在调试线程上做：调试线程放开目标后只轮询状态，取数交给独立线程，
 * 这样单次抽数耗时不再决定 Stop 的响应粒度（实测阻塞约 106 ms）。
 */
#pragma once

namespace orb {
namespace agdi {

// 启动/停止工作线程（幂等），会话建立后启动、断开时停止。
void TracePumpStart();
void TracePumpStop();
bool TracePumpRunning();

// 排空所有流式 trace 源（SWO / orbtrace），按 timeMs 限流；
// 返回本轮搬走的字节数（0 = 无可搬）。必须在持探针锁时调，且只搬字节：
// 落盘/解帧/解码交给 TraceDecodeStaged() 在锁外做，否则 worker 抱锁不放。
unsigned TraceDrain(unsigned timeMs);

// 处理 TraceDrain 刚搬出的 n 字节，必须在锁外调。
// n 必须是返回值，顺序不能乱、不能重复喂。
void TraceDecodeStaged(unsigned n);

// 挂起握手：调试线程做长目标操作（Flash 擦/写/校验）前后配对调用，可重入。
// 挂起只保证 worker 不开始新一轮，真正让它退出当前时间片的是探针锁。
void TracePumpSuspend();
void TracePumpResume();
bool TracePumpSuspended();

} // namespace agdi
} // namespace orb
