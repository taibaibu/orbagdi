/**
 * @file ORBAGDI_Oflow.cpp
 * @brief OFLOW / COBS 解帧器（orbuculum `Src/cobs.c` + `Src/oflow.c` 的移植）。
 *
 * 逐字节等价于 orbuculum 的增量解码状态机，见头文件里的对应关系表。
 * 只做了两处**安全性**调整（不改变正常流的行为）：
 *   1. 每处写入前都做长度上界判断，畸形流只丢帧并计数，不会越界；
 *   2. 帧内出现裸定界符、或长度超过 COBS 上限时，与 orbuculum 一样进入
 *      DRAINING 丢弃到下一个定界符，同时累加 CobsErrors()。
 */
#include "pch.h"
#include "ORBAGDI_Oflow.h"

#include <string.h>

namespace orb {
namespace agdi {

OflowDeframer::OflowDeframer()
{
    memset(buf_, 0, sizeof(buf_));
    frames_      = 0;
    badSum_      = 0;
    cobsErrors_  = 0;
    shortFrames_ = 0;
    bytesIn_     = 0;
    Reset();
}

void OflowDeframer::Reset()
{
    state_         = kIdle;
    len_           = 0;
    intervalCount_ = 0;
    maxCount_      = false;
}

// COBS 包凑齐后的拆分（orbuculum Src/oflow.c:81 _pumpcb）。
void OflowDeframer::Deliver(OflowFrameFn cb, void* user)
{
    // OFLOW 帧至少要有一个 tag 字节和一个校验字节。
    if (len_ < 2) {
        ++shortFrames_;
        return;
    }

    OflowFrame f;
    f.tag  = buf_[0];       // 首字节 = tag
    f.data = &buf_[1];      // 中间   = 净荷
    f.len  = len_ - 2;      // 末字节 = sum

    // 校验：tag + Σpayload + sum 应为 0（8-bit 环绕）
    uint8_t sum = f.tag;
    for (unsigned i = 0; i < f.len; ++i) {
        sum = (uint8_t)(sum + f.data[i]);
    }
    sum = (uint8_t)(sum + buf_[len_ - 1]);
    f.good = (sum == 0);

    ++frames_;
    if (!f.good) ++badSum_;

    if (cb) cb(f, user);
}

// orbuculum Src/cobs.c:110 COBSPump() 的逐字节等价实现。
void OflowDeframer::Pump(const uint8_t* in, int len, OflowFrameFn cb, void* user)
{
    if (!in || len <= 0) return;
    bytesIn_ += (unsigned long long)len;

    const uint8_t* fp = in;
    for (int i = 0; i < len; ++i, ++fp) {
        switch (state_) {

        case kIdle:
            // 定界符之外的第一个字节就是本帧的第一个 interval count。
            if (kOflowSyncChar != *fp) {
                len_           = 0;
                intervalCount_ = *fp;
                maxCount_      = (*fp == 255);
                state_         = kRxing;
            }
            break;

        case kDraining:
            // 丢掉整帧，直到下一个定界符。
            if (kOflowSyncChar == *fp) state_ = kIdle;
            break;

        case kRxing:
            --intervalCount_;

            if (intervalCount_ == 0) {
                if (kOflowSyncChar == *fp) {
                    // 定界符：一帧结束
                    Deliver(cb, user);
                    len_   = 0;
                    state_ = kIdle;
                } else {
                    // 隐含一个 0x00，当前字节是新的 interval count
                    if (!maxCount_) {
                        if (len_ >= kOflowMaxEncoded) {
                            ++cobsErrors_;
                            state_ = kDraining;
                            break;
                        }
                        buf_[len_++] = (uint8_t)kOflowSyncChar;
                    }
                    intervalCount_ = *fp;
                    maxCount_      = (*fp == 255);
                }
            } else {
                // 帧溢出，或帧内出现了不该有的定界符
                if (len_ > kOflowCobsMaxPacket || kOflowSyncChar == *fp) {
                    ++cobsErrors_;
                    state_ = kDraining;
                } else if (len_ < kOflowMaxEncoded) {
                    buf_[len_++] = *fp;
                }
            }
            break;
        }
    }
}

} // namespace agdi
} // namespace orb
