/**
 * @file ORBAGDI_InstrScan.h
 * @brief 指令扫描（P1-0）—— 把"两个已知地址之间的指令"列出来。
 *
 * 为什么需要它
 * ------------
 * `ETMCR` bit8 = 0 时 ETM 只报**间接**分支的目标地址（见 `ORBAGDI_CodeImage.h`）。
 * 于是"这段代码到底执行了哪些指令"这件事，只能由**代码镜像 + 顺序扫描**补出来：
 *
 *     上一个已知地址 ──顺序扫──> 分支指令 ──跳──> ETM 报的下一个地址
 *         （每条都发一条记录）        （目标：直接分支自己算 / 间接分支听 ETM）
 *
 * 所以这一层要回答两个问题：**这条指令多长**、**它是不是分支、目标在哪**。
 *
 * 解码交给 capstone（2026-10-14 重构）
 * -----------------------------------
 * 原来是自己写一张 Thumb 编码表（`Decode16` / `DecodeBranch32` / …）。它踩过的
 * 坑证明这条路维护不起：B.W / BL 的 `I1 = NOT(J1 EOR S)` 取反位抄错一次目标就
 * 偏；`LDR.W PC,[PC,#imm]` 会被误判成"无条件直接分支"，扫描当场停下、后面整段
 * 地址全错 —— 而且是**静默**的。
 *
 * 现在用 capstone（BSD-3-Clause，静态链进驱动，见 `capstone\build.ps1`）：
 *   * 指令长度 -> `cs_insn::size`
 *   * 分支识别 -> `cs_insn::id`（ARM_INS_B / BL / BX / CBZ / TBB / POP…）
 *   * 直接目标 -> `insn->detail->arm.operands[0].imm`
 *   * 条件执行 -> `insn->detail->arm.cc`
 * 顺带：`cs_open()` 换个 `CS_ARCH_*` 就能接别的架构（capstone 的 RISC-V 已经编进去了，
 * 只是本驱动目前只面向 Cortex-M）。
 *
 * 依据：ARMv7-M ARM（DDI0403E）；编码正确性由 capstone 负责，本层不再自己判编码。
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
    bool     call;       ///< 是**调用**（`BL`）—— 扫描时要跟进去，见 ScanTo
    uint32_t target;     ///< direct 时有效：分支目标
};

/// 读代码字节。`len` 只会是 2 或 4。返回 false = 读不到。
typedef bool (*CodeReadFn)(void* ctx, uint32_t addr, uint8_t* out, unsigned len);

/// 每扫出一条指令回调一次（`info` 在回调期间有效）。
typedef void (*InstrEmitFn)(void* ctx, uint32_t pc, const InstrInfo& info);

/// 解一条指令。`p` 至少 2 字节；`avail` 是**已读到的**字节数（< 4 时 32 位指令解不出来，
/// 返回 false 让调用方补读）。
bool InstrDecode(uint32_t pc, const uint8_t* p, unsigned avail, InstrInfo& out);

struct ScanResult {
    unsigned    instrs;   ///< 解过并回调过的指令条数
    bool        reached;  ///< 这一段**正常收尾**（走到 `to`，或分支跳走）
    uint32_t    stopPc;   ///< 停下时的 pc（走到 `to` 时为 `to`，否则是那条分支指令）
    const char* why;      ///< 收尾方式（进日志用）
};

/// 从 `from` 起**跟随控制流**扫到 `to`，每条指令回调一次。
///
/// `to` = ETM 报的下一个地址（= 下一条**要执行**的指令，也就是那次间接分支的目标）。
///
/// 跟随规则（这是"看得到分支之间的指令"的关键）：
///   * 顺序指令 —— 继续；
///   * **无条件 `B`** —— 跳到目标继续（循环回跳、跳出去都跟）；
///   * **`BL`（`call`）** —— **压返回地址、进函数体继续扫**。分支到分支之间的指令
///     绝大部分在被调用的函数里，不跟进去就看不到；
///   * **条件分支** —— 按"没跳"继续（跳没跳只有 ETM 知道，猜错了靠下面的兜底）；
///   * **间接分支** —— ETM 报的 `to` 就是它的目标：栈里有返回地址就当"函数返回"
///     弹栈继续（返回点等于 `to` 就收尾），否则到此为止。
///
/// `reached` = 正常收尾（走到 `to`、或那次间接分支的目标就是 `to`）。
/// 为 false = 读不到 / 解不出 / 调用嵌套太深 / 超上限 —— `why` 说明原因，
/// `stopPc` 指向停下那条指令（失败现场）。
///
/// ⚠ **调用方必须先缓冲、等 `reached` 为真再决定要不要用**：条件分支猜错会让
///   扫描走上没执行的那条路。本函数不做这件事，因为它不知道调用方要拿这些指令干什么。
///
/// `maxInstr` 是防跑飞的硬上限（镜像不可信 / `B .` 自旋时）。
ScanResult ScanTo(CodeReadFn read, void* ctx, uint32_t from, uint32_t to,
                  unsigned maxInstr, InstrEmitFn emit, void* emitCtx);

} // namespace agdi
} // namespace orb
