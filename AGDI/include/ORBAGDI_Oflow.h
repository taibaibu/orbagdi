/**
 * @file ORBAGDI_Oflow.h
 * @brief OFLOW / COBS 解帧：orbtrace 探针的 trace 流格式。
 * 线上是 COBS([tag][payload][sum]) 0x00，本模块只做解码，逐字节增量处理。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace orb {
namespace agdi {

// 常量数值取自 orbuculum Inc/cobs.h / Inc/oflow.h / Inc/nw.h。
enum : uint8_t {
    kOflowTagItm = 1,   // ITM / SWO / TPIU 数据
    kOflowTagEtm = 2,   // ETM 指令 trace
};

enum : size_t {
    kOflowCobsMaxPacket = 4096,  // COBS_MAX_PACKET_LEN
    kOflowMaxEncoded    = 4122,  // COBS_MAX_ENC_PACKET_LEN
    kOflowMaxPayload    = 4094,  // OFLOW_MAX_PACKET_LEN = COBS_MAX_PACKET_LEN - 2
    kOflowSyncChar      = 0,     // COBS_SYNC_CHAR
};

// 一个解出的帧；data 指向内部缓冲，只在回调期间有效。
struct OflowFrame {
    uint8_t        tag;   // 源标识
    const uint8_t* data;  // 净荷
    unsigned       len;   // 净荷字节数
    bool           good;  // 校验和是否通过
};

typedef void (*OflowFrameFn)(const OflowFrame& frame, void* user);

class OflowDeframer {
public:
    OflowDeframer();

    // 丢弃半个帧、回到 IDLE（统计保留）。
    void Reset();

    // 丢弃到下一个定界符、从帧边界重新开始（统计保留）。
    // 接流时、以及每次判定"字节流不可信"之后调 —— 见 cpp 里的说明。
    void DropToDelimiter();

    // 喂入任意长度字节，每凑齐一帧回调一次。
    void Pump(const uint8_t* in, int len, OflowFrameFn cb, void* user);

    // ---- 诊断计数 ----
    unsigned long long Frames()      const { return frames_; }
    unsigned long long BadSum()      const { return badSum_; }
    unsigned long long CobsErrors()  const { return cobsErrors_; }
    unsigned long long ShortFrames() const { return shortFrames_; }
    unsigned long long BytesIn()     const { return bytesIn_; }

    // 最近一次解帧错（COBS 错 / 校验和错）那一帧的**起点**在输入流中的绝对字节
    // 偏移（与 BytesIn() 同一坐标系，即"第几个字节"）。
    //
    // 为什么要在解帧器内部记：调用方（OrbTrace.cpp）把字节按 512 B 切片喂进来，
    // 每片结束才检查计数，原来报出来的偏移永远是 512 的整数倍（实测 512、512、
    // 131584），看着像"错在第 512 字节"，其实那只是检测时机。而 ETM 的分支地址
    // 是相对编码，排查时必须知道坏字节在哪儿 —— 差几百字节就会把一整段假地址
    // 算到错误的帧头上。逐帧记的精度是 1 字节。
    unsigned long long LastErrorOffset() const { return errOffset_; }

private:
    enum State { kIdle, kRxing, kDraining };

    // 返回 true 表示这一帧校验和不对（Pump 用它记录出错帧的起点）。
    bool Deliver(OflowFrameFn cb, void* user);

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
    unsigned long long errOffset_;  // 见 LastErrorOffset()
};

} // namespace agdi
} // namespace orb
