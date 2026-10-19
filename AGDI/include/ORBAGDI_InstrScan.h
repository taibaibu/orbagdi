/**
 * @file ORBAGDI_InstrScan.h
 * @brief 指令扫描：把两个已知地址之间的指令列出来。
 * ETMCR bit8=0 时 ETM 只报间接分支目标，其余指令只能靠代码镜像顺序扫描补出。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

/// 一条指令的解码结果。
struct InstrInfo {
    unsigned len;        ///< 2 或 4 字节
    bool     branch;     ///< 改变执行流
    bool     direct;     ///< 目标可由编码算出（B / BL / CBZ…）；false = 间接（听 ETM）
    bool     cond;       ///< 条件执行（可能不跳）
    bool     call;       ///< 是调用（BL），扫描时要跟进去
    uint32_t target;     ///< direct 时有效：分支目标
};

/// 读代码字节。len 只会是 2 或 4。返回 false = 读不到。
typedef bool (*CodeReadFn)(void* ctx, uint32_t addr, uint8_t* out, unsigned len);

/// 每扫出一条指令回调一次（info 在回调期间有效）。
typedef void (*InstrEmitFn)(void* ctx, uint32_t pc, const InstrInfo& info);

/// 解一条指令，解码交给 capstone。avail < 4 时 32 位指令解不出来，返回 false。
bool InstrDecode(uint32_t pc, const uint8_t* p, unsigned avail, InstrInfo& out);

struct ScanResult {
    unsigned    instrs;   ///< 解过并回调过的指令条数
    bool        reached;  ///< 这一段正常收尾（走到 to，或分支跳走）
    uint32_t    stopPc;   ///< 停下时的 pc
    const char* why;      ///< 收尾方式（进日志用）
};

/// 从 from 起跟随控制流扫到 to，每条指令回调一次。
/// to 是 ETM 报的下一个地址；无条件 B 跟跳、BL 跟进去、条件分支按“没跳”走、
/// 间接分支按栈里返回地址弹栈。调用方必须等 reached 为真再用结果。
/// maxInstr 是防跑飞的硬上限（镜像不可信 / B . 自旋）。
ScanResult ScanTo(CodeReadFn read, void* ctx, uint32_t from, uint32_t to,
                  unsigned maxInstr, InstrEmitFn emit, void* emitCtx);

} // namespace agdi
} // namespace orb
