/**
 * @file ORBAGDI_OrbTrace.h
 * @brief orbtrace 探针的 trace 采集源（RDDI OrbTrace_* / TraceStream_*）。
 * 端点交回 OFLOW/COBS 成帧的原始字节，解帧后按 tag 分给 ITM / ETM 解码器。
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// 宿主是否导出 orbtrace 入口（廉价，Load() 前调用也安全）。
bool OrbTraceHostReady();

// trace 流是否已打开并在采集。
bool OrbTraceActive();

// ---- 设备信息 ----
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

// 取设备信息（必要时先探测）；返回 false 表示没有 orbtrace 设备。
bool OrbTraceGetInfo(orb::Dap& dap, OrbTraceInfo& out);

// ---- 启动 / 停止 ----

// 配置探针 trace 源并打开原始流：width 1/2/4（0 = 不配），
// swo 0 = 并口 / 1 = UART / 2 = Manchester（-1 = 不配），
// oflow = 流是否 OFLOW/COBS 成帧。失败只影响 trace。
bool OrbTraceStart(orb::Dap& dap, int width, int swo, int baud, bool oflow);

// 关掉 trace 流（幂等）。
void OrbTraceStop(orb::Dap& dap);

// ---- 采集 ----

// 搬 + 解一体，只留给调试线程的一次性收尾（调用方自持锁）。
// timeMs 是时间预算，不是字节上限。
unsigned OrbTracePump(orb::Dap& dap, unsigned timeMs);

// ---- 抽 / 解 分离 ----
// 全指令重建解码慢（64 KB 约 15 ms），worker 若在锁内解码会撑爆 32 MB 环
// 并让调试线程拿不到锁，所以锁内只搬字节，锁外再解释。
unsigned OrbTracePullRaw(orb::Dap& dap, uint8_t* out, unsigned cap, unsigned timeMs);
void     OrbTraceDecodeRaw(const uint8_t* data, unsigned n);

struct OrbTraceStats {
    unsigned long long bytes;       // 从端点读到的原始字节
    unsigned long long frames;      // 解出的 OFLOW 帧
    unsigned long long badSum;      // 校验和错的帧
    unsigned long long cobsErrors;  // COBS 层错误
    unsigned long long itmBytes;    // 交给 ITM 解码器的字节
    unsigned long long etmBytes;    // 交给 ETM 解码器的字节
    unsigned long long otherFrames; // 非 ITM/ETM tag 的帧

    // RDDI 环溢出累计丢的字节：判断“抽数跟不上”的定量依据。
    unsigned long long dropped;

    // 同步落盘（fwrite）耗时，单独列出来才能区分解码慢和写盘慢。
    unsigned long long writeMs;
};
OrbTraceStats OrbTraceGetStats();

} // namespace agdi
} // namespace orb
