/**
 * @file ORBAGDI_Oflow.cpp
 * @brief OFLOW / COBS 解帧器（orbuculum cobs.c + oflow.c 的移植）。
 *
 * 与 orbuculum 逐字节等价，只加两处安全性：每次写入前判上界，畸形流只丢帧并计数；
 * 裸定界符或超长帧进入 DRAINING 丢到下一个定界符，同时累加 CobsErrors()。
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
    errOffset_   = 0;
    Reset();
}

void OflowDeframer::Reset()
{
    state_         = kIdle;
    len_           = 0;
    intervalCount_ = 0;
    maxCount_      = false;
}

// 丢弃到下一个定界符再从帧边界开始。
// 为什么需要：kIdle 会把**任意**一个非零字节当成 interval count —— 如果流的起点
// 落在半帧中间（接探针时残帧、或链路出错后的第一个字节），它会一路错解到撞出一次
// COBS 错为止。那一次"撞"正是日志里的 `解帧错 ... COBS=n`，而它连带让 ETM 失步、
// 丢掉缺口后的一段。先对齐再解，这些假错误就不存在了。
void OflowDeframer::DropToDelimiter()
{
    state_         = kDraining;
    len_           = 0;
    intervalCount_ = 0;
    maxCount_      = false;
}

// COBS 包凑齐后的拆分（orbuculum Src/oflow.c:81 _pumpcb）。
bool OflowDeframer::Deliver(OflowFrameFn cb, void* user)
{
    // OFLOW 帧至少要有一个 tag 字节和一个校验字节。
    if (len_ < 2) {
        ++shortFrames_;
        return false;
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
    const bool bad = !f.good;
    if (bad) ++badSum_;

    if (cb) cb(f, user);
    return bad;
}

// orbuculum Src/cobs.c:110 COBSPump() 的逐字节等价实现。
void OflowDeframer::Pump(const uint8_t* in, int len, OflowFrameFn cb, void* user)
{
    if (!in || len <= 0) return;

    // ---- 出错帧的定位 ------------------------------------------------------
    // 调用方按 512 B 切片喂入、每片结束才查计数，所以"检测到错"和"错发生"之间
    // 最多差一整片。这里在解帧器内部把每帧的起点记下来：出错时写 errOffset_
    // = 该帧起点在整条流里的绝对字节号，精度 1 字节。
    //
    // frameStart 是帧起点在**本次调用**内的下标；-1 表示这一帧跨了片边界
    // （调用开始时 state_ 已经是 kRxing），真实起点在上一片里，此时退化成用
    // 本片起点 —— 误差 < 一片，仍比原来"报片末"准得多，而且不会指向还没读到的字节。
    const unsigned long long base = bytesIn_;
    bytesIn_ += (unsigned long long)len;
    int frameStart = (state_ == kRxing) ? -1 : 0;

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
                frameStart     = i;
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
                    if (Deliver(cb, user)) {
                        errOffset_ = base + (frameStart < 0 ? 0u
                                                           : (unsigned long long)frameStart);
                    }
                    len_       = 0;
                    state_     = kIdle;
                    frameStart = 0;
                } else {
                    // 隐含一个 0x00，当前字节是新的 interval count
                    if (!maxCount_) {
                        if (len_ >= kOflowMaxEncoded) {
                            ++cobsErrors_;
                            errOffset_ = base + (frameStart < 0 ? 0u
                                                               : (unsigned long long)frameStart);
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
                    errOffset_ = base + (frameStart < 0 ? 0u
                                                       : (unsigned long long)frameStart);
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
