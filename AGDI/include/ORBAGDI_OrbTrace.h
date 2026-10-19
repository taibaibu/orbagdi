/**
 * @file ORBAGDI_OrbTrace.h
 * @brief orbtrace 探针的 trace 采集源（RDDI `OrbTrace_*` / `TraceStream_*`）。
 *
 * 这是本驱动**第三条** trace 数据来源，和已有的两条并列：
 *
 *   | 来源        | 数据通路                          | 谁读          |
 *   |-------------|-----------------------------------|---------------|
 *   | SWO 引脚    | RDDI `CMSIS_DAP_SWO_*`            | ORBAGDI_Itm   |
 *   | 片上 ETB    | MEM-AP 读 RAM                     | ORBAGDI_Etb   |
 *   | **orbtrace**| RDDI `TraceStream_Read`（USB bulk）| **本模块**    |
 *
 * orbtrace 探针的 trace 端点直接交回目标 trace 口的原始字节（并口 4-bit /
 * SWO），由 RDDI 层统一管理 USB，AGDI 只取字节流。**流是 OFLOW/COBS 成帧的**
 * （orbuculum 格式，见 ORBAGDI_Oflow.h），所以本模块负责：
 *
 *     TraceStream_Read  ->  OflowDeframer  ->  按 tag 分派
 *                                                tag 1 (ITM) -> ItmFeedBytes()
 *                                                tag 2 (ETM) -> EtmPump()
 *
 * 注意与 SWO 路径的区别：ITM 解码器在 SWO 下靠 48-bit sync 找同步点，而
 * orbtrace 的流是**从帧边界开始**的，因此进来前要 ItmResyncDecoder() 把解码器
 * 放到已知边界（和 ETB 路径同一个理由）。
 *
 * ⚠️ 未实机验证（本机无 orbtrace + 目标板）。RDDI 层没有这些导出时
 * OrbTraceHostReady() 返回 false，本模块整体不参与，其余调试功能不受影响。
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// RDDI 宿主是否导出了 orbtrace 的入口点（`OrbTrace_*` + `TraceStream_*`）。
// 廉价，Load() 之前调用也安全。
bool OrbTraceHostReady();

// trace 流是否已经打开并在采集。
bool OrbTraceActive();

// ---------------------------------------------------------------------------
// 设备信息（进日志 / 设置页）
// ---------------------------------------------------------------------------
struct OrbTraceInfo {
    bool present;        // 找到 orbtrace 设备
    bool opened;         // trace 接口已打开
    int  vid;
    int  pid;
    int  traceIf;        // 三个 vendor 接口号（-1 = 无）
    int  powerIf;
    int  versionIf;
    int  traceProtocol;  // 0x00 未知 / 0x01 TPIU raw / 0x10 OFLOW v1.0
    bool supportsOflow;  // trace 接口 protocol == 0x10
    char product[128];
    char serial[64];
    char version[64];
};

// 取设备信息（必要时先探测）。返回 false 表示没有 orbtrace 设备。
bool OrbTraceGetInfo(orb::Dap& dap, OrbTraceInfo& out);

// ---------------------------------------------------------------------------
// 启动 / 停止
// ---------------------------------------------------------------------------

// 配置探针的 trace 源并打开原始 trace 流。
//   width   并口宽度（位）：1 / 2 / 4；0 = 不配置
//   swo     0 = 用并口；1 = SWO UART/NRZ；2 = SWO Manchester；-1 = 不配置
//   baud    SWO 波特率（Hz）；<= 0 = 不配置
//   oflow   true = 流是 OFLOW/COBS 成帧的（探针 protocol 0x10，正常情况）；
//           false = 裸字节（protocol 0x01，TPIU raw）
// 失败是软失败：流没打开、返回 false，其余调试功能不受影响。
bool OrbTraceStart(orb::Dap& dap, int width, int swo, int baud, bool oflow);

// 关掉 trace 流（幂等）。
void OrbTraceStop(orb::Dap& dap);

// ---------------------------------------------------------------------------
// 采集
// ---------------------------------------------------------------------------

// 排空探针的 trace 端点、解帧、按 tag 分派给 ITM / ETM 解码器。
//
// `timeMs` 是**时间预算**（官方 `Trace_Read(time, ts)` 同口径），不是字节上限：
// 原先写死的 `round < 32` 是一个隐含的 2 MB 上限，与链路速率无关，换 USB3/USB4
// 时会先成为硬天花板。返回本轮搬走的原始字节数。
//
// 调用者：抽数 worker（`ORBAGDI_TracePump.cpp`）+ 停机收尾那一次。
//
// ⚠ 这里原先写着"**必须在 uVision 自己的线程上跑**" —— **那条断言是错的**，
// 而且它正是上一轮错误架构（把抽数留在调试线程、再拿字节封顶和定时器精度去救）
// 的依据。官方就是从工作线程调这些回调的：`DSMonitor_Thread`
// （`DbgCM/DSMonitor.cpp:454`）里既调 `pio->Notify(...)` 也调
// `pCbFunc(AG_CB_EXECHOOK / AG_CB_RUNNING)`，`AG_CB_ITMDATA` 同样出自那条线程。
// 证据见 COMPAT_ANALYSIS.md §10.27。
unsigned OrbTracePump(orb::Dap& dap, unsigned timeMs);

// ---------------------------------------------------------------------------
// 抽 / 解 分离（2026-10-14）
// ---------------------------------------------------------------------------
// 上面那个 `OrbTracePump` 是"搬 + 解"一体，**只留给调试线程的一次性收尾用**
// （停机那 5 ms，调用方自己持锁，同步做完即可）。
//
// 抽数 worker **不能**用它。理由是一条实测链：
//
//   P1-0 的全指令重建（`FillGap` → `ScanTo`）每个 ETM 包最多顺序扫 4096 条指令，
//   实测解 64 KB 要 ~15 ms（一次会话里 `补出=36,480,877` 条指令、页缓存命中
//   36,680,457 次）。它和 `TraceStream_Read` 挤在同一个 `ComLock` 里时，worker
//   每轮只完成**一次读**就超出 2 ms 时间片 —— 探针 11 MB/s、消费者 14 KB/s，
//   32 MB 环必然溢出（实测一次会话丢 506,396,672 字节）；更要紧的是 worker
//   抱着锁不放，调试线程停不下目标：
//
//       WRN GoStep: 停机收尾没拿到探针锁，worker 未让出 —— 继续，但这一轮的
//                    DFSR/PC 可能读到脏值
//
//   随后 DP 转 WAIT/NO_ACK（`DP write reg=2 failed` 数千行），链路死掉。
//
// 所以 worker 走下面这两个：**锁内只搬字节，锁外才解释内容**。
//
//   - `OrbTracePullRaw`：**必须在持有探针锁时调**。只从端点搬字节到 `out`，
//     不落盘、不解帧、不解码、不回调 uVision。`cap` 取 64 KB 的整数倍
//     （一次读的单位就是 64 KB），装不下整块时它**不读**，剩下的下一片再取。
//   - `OrbTraceDecodeRaw`：**必须在锁外调**，按 pull 的顺序一次性喂完。
unsigned OrbTracePullRaw(orb::Dap& dap, uint8_t* out, unsigned cap, unsigned timeMs);
void     OrbTraceDecodeRaw(const uint8_t* data, unsigned n);

// 诊断计数（进日志）。
struct OrbTraceStats {
    unsigned long long bytes;       // 从端点读到的原始字节
    unsigned long long frames;      // 解出的 OFLOW 帧
    unsigned long long badSum;      // 校验和错的帧
    unsigned long long cobsErrors;  // COBS 层错误
    unsigned long long itmBytes;    // 交给 ITM 解码器的字节
    unsigned long long etmBytes;    // 交给 ETM 解码器的字节
    unsigned long long otherFrames; // 非 ITM/ETM tag 的帧
};
OrbTraceStats OrbTraceGetStats();

// 目标供电（Vtref / Vtpwr）**不在这个文件里** —— 见 `ORBAGDI_Power.h`。单独一个
// 文件是因为它不依赖 trace 的采集状态，设置页的测试程序（`test\build.ps1` 只编
// 四个源文件）才链得上。

} // namespace agdi
} // namespace orb
