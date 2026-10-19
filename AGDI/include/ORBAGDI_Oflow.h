/**
 * @file ORBAGDI_Oflow.h
 * @brief OFLOW / COBS 解帧器 - orbtrace 探针的 trace 流格式。
 *
 * orbtrace 的 trace 端点交出来的不是裸 ITM/ETM 字节，而是 orbuculum 定义的
 * **OFLOW** 帧：COBS 编码 + 每帧一个 tag（源标识）+ 一个校验和字节。
 * 本模块是 orbuculum `Src/cobs.c` + `Src/oflow.c` 的逐字节等价移植，只保留
 * 解码方向（编码在探针固件里）。
 *
 * 线格式（orbuculum `Src/oflow.c:52` OFLOWEncode）：
 *
 *     编码前：  [ tag (1B) ][ payload (N B) ][ sum (1B) ]
 *     线 上：   COBS( 上面三段 ) 0x00
 *
 *   - tag    源标识，见 kOflowTag*（orbuculum `Inc/nw.h:26`：1 = ITM，2 = ETM）
 *   - sum    校验字节，使 `tag + Σpayload + sum == 0 (mod 256)`
 *   - 0x00   唯一的定界符（COBS 保证帧内不出现裸 0x00）
 *
 * 移植对应关系：
 *     OflowDeframer::Pump()    <-> COBSPump()   Src/cobs.c:110
 *     OflowDeframer::Deliver() <-> _pumpcb()    Src/oflow.c:81
 *
 * 与 orbuculum 的差异：
 *   - 增量状态机（逐字节喂），不是一次性解码整块；
 *   - 长度上界做了硬保护，畸形流只会丢帧并计数，不会越界写。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace orb {
namespace agdi {

// ---------------------------------------------------------------------------
// 常量（数值取自 orbuculum Inc/cobs.h / Inc/oflow.h / Inc/nw.h）
// ---------------------------------------------------------------------------
enum : uint8_t {
    kOflowTagItm = 1,   // DEFAULT_ITM_STREAM - ITM / SWO / TPIU 数据
    kOflowTagEtm = 2,   // DEFAULT_ETM_STREAM - ETM 指令 trace
};

enum : size_t {
    kOflowCobsMaxPacket = 4096,  // COBS_MAX_PACKET_LEN
    kOflowMaxEncoded    = 4122,  // COBS_MAX_ENC_PACKET_LEN
    kOflowMaxPayload    = 4094,  // OFLOW_MAX_PACKET_LEN = COBS_MAX_PACKET_LEN - 2
    kOflowSyncChar      = 0,     // COBS_SYNC_CHAR
};

// 一个解出来的 OFLOW 帧。`data` 指向解帧器内部缓冲，**只在回调期间有效**。
struct OflowFrame {
    uint8_t        tag;   // 源标识
    const uint8_t* data;  // 净荷
    unsigned       len;   // 净荷字节数
    bool           good;  // 校验和是否通过
};

typedef void (*OflowFrameFn)(const OflowFrame& frame, void* user);

// ---------------------------------------------------------------------------
// 增量解帧器
// ---------------------------------------------------------------------------
class OflowDeframer {
public:
    OflowDeframer();

    // 丢弃半个帧、回到 IDLE（统计保留）。
    void Reset();

    // 喂入任意长度的字节；每凑齐一帧就回调一次 `cb`。
    void Pump(const uint8_t* in, int len, OflowFrameFn cb, void* user);

    // ---- 诊断（进日志 / 设置页）-------------------------------------------
    unsigned long long Frames()      const { return frames_; }
    unsigned long long BadSum()      const { return badSum_; }
    unsigned long long CobsErrors()  const { return cobsErrors_; }
    unsigned long long ShortFrames() const { return shortFrames_; }
    unsigned long long BytesIn()     const { return bytesIn_; }

private:
    enum State { kIdle, kRxing, kDraining };

    void Deliver(OflowFrameFn cb, void* user);

    State    state_;
    uint8_t  buf_[kOflowMaxEncoded];  // 解码中的帧（含 tag 与 sum）
    unsigned len_;
    int      intervalCount_;
    bool     maxCount_;

    unsigned long long frames_;
    unsigned long long badSum_;
    unsigned long long cobsErrors_;
    unsigned long long shortFrames_;
    unsigned long long bytesIn_;
};

} // namespace agdi
} // namespace orb
