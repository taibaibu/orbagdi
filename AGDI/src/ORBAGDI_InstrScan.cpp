/**
 * @file ORBAGDI_InstrScan.cpp
 * @brief 指令扫描的实现（P1-0）—— 解码交给 capstone，控制流跟随是自己的。
 *
 * 为什么要换掉手写解码（2026-10-14）
 * ---------------------------------
 * 原来这里是一段手写的 Thumb-only 解码：`Decode16` / `DecodeBranch32` /
 * `DecodeIndirect32`，靠人工对照 ARMv7-M ARM 的编码表。它踩过的坑（B.W / BL 的
 * `I1 = NOT(J1 EOR S)` 取反位、`LDR.W PC,[PC,#imm]` 被误判成无条件直接分支）
 * 说明"自己维护一张指令编码表"这条路维护不起 —— 多一个架构就多一张表，
 * 而且错一位就是**静默走错路**（扫描当场停下，后面整段地址全错）。
 *
 * 现在解码用 capstone（BSD-3-Clause，静态链进驱动，见 capstone\build.ps1）：
 *   * 指令长度   -> `insn->size`
 *   * 分支识别   -> `insn->id`（ARM_INS_B / BL / BX / CBZ / TBB / POP…）
 *   * 直接目标   -> `insn->detail->arm.operands[0].imm`
 *   * 条件执行   -> `insn->detail->arm.cc`
 * 顺带把"以后接 RISC-V"变成一件只改 `cs_open()` 参数的事（capstone 已编进去）。
 *
 * ⚠ 这里**只管解码**。"这段执行流是不是真的"由 `ScanTo` 的控制流跟随决定，
 *   它的规则（BL 压返回地址跟进去、间接分支弹栈）见 ORBAGDI_InstrScan.h。
 *
 * 线程：`FillGap()` 只在抽数 worker 线程上调用（ORBAGDI_Etm.cpp），所以句柄与
 *       `cs_insn` 只被那一个线程碰；`InstrDecode()` 是公开 API，仍然上了锁。
 */
#include "pch.h"
#include "ORBAGDI_InstrScan.h"

#include <capstone/capstone.h>

#include <string.h>

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// capstone 句柄
// ---------------------------------------------------------------------------
// `cs_disasm_iter` 要求目标 `cs_insn` 必须由 `cs_malloc()` 分配（capstone 的
// 契约），不能是栈上的。两者都只建一次，进程退出由系统回收（同 ORBAGDI_Etm.cpp
// 的说明：故意泄漏，避免在 DLL 卸载时序上做文章）。
CRITICAL_SECTION g_cs;
bool             g_csInit = false;
csh              g_handle = 0;
cs_insn*         g_insn   = nullptr;
bool             g_failed = false;

void EnsureInit()
{
    if (g_csInit) return;
    InitializeCriticalSection(&g_cs);
    g_csInit = true;
}

/// 取句柄（首次调用时创建）。返回 0 = capstone 用不了，调用方直接放弃扫描。
csh Handle()
{
    EnsureInit();
    EnterCriticalSection(&g_cs);

    if (!g_handle && !g_failed) {
        // Cortex-M 只有 Thumb 状态；MCLASS 打开 M 核特有的指令（含 M33 的
        // 那几条）。本驱动只面向 M 核，见 ORBAGDI_Etm.cpp 的 PARTNO 表。
        // ⚠ C++ 下 `|` 的结果是 int，`cs_mode` 是枚举 ⇒ 必须显式转（C 里不用）。
        const cs_mode mode = static_cast<cs_mode>(CS_MODE_THUMB | CS_MODE_MCLASS);
        if (cs_open(CS_ARCH_ARM, mode, &g_handle) != CS_ERR_OK) {
            g_handle = 0;
            g_failed = true;
            ORB_LOG_WARN("InstrScan: cs_open(ARM/Thumb+MCLASS) 失败 —— 指令扫描关闭");
        } else {
            cs_option(g_handle, CS_OPT_DETAIL, CS_OPT_ON);
            g_insn = cs_malloc(g_handle);
            if (!g_insn) {
                cs_close(&g_handle);
                g_failed = true;
                ORB_LOG_WARN("InstrScan: cs_malloc 失败 —— 指令扫描关闭");
            } else {
                unsigned v = cs_version(nullptr, nullptr);
                ORB_LOG_INFO("InstrScan: capstone %u.%u 已就绪（ARM/Thumb, MCLASS）",
                             v >> 8, v & 0xFF);
            }
        }
    }

    const csh h = g_handle;
    LeaveCriticalSection(&g_cs);
    return h;
}

// ---------------------------------------------------------------------------
// 分支分类
// ---------------------------------------------------------------------------

/// 操作数是不是 PC（`POP {…,PC}` / `LDM {…,PC}` / `LDR PC,[…]` 这几类）。
bool WritesPc(const cs_arm& a)
{
    for (uint8_t i = 0; i < a.op_count; ++i) {
        if (a.operands[i].type == ARM_OP_REG && a.operands[i].reg == ARM_REG_PC) return true;
    }
    return false;
}

/// 把 capstone 的解码结果翻成扫描要的那几个问题。
/// 返回 false = 这条不是分支（`o.len` 仍然有效）。
bool Classify(const cs_insn& insn, InstrInfo& o)
{
    const cs_arm& a = insn.detail->arm;

    // 条件执行：ARM_CC_AL 是无条件，ARM_CC_INVALID 是"不适用"（当无条件看）。
    const bool conditional = (a.cc != ARM_CC_AL && a.cc != ARM_CC_INVALID);

    switch (insn.id) {
    case ARM_INS_B:                     // B / B<cond> / B.W
        o.branch = true; o.direct = true; o.cond = conditional;
        if (a.op_count >= 1 && a.operands[0].type == ARM_OP_IMM) {
            o.target = static_cast<uint32_t>(a.operands[0].imm) & ~1u;
        }
        return true;

    case ARM_INS_BL:                    // BL —— 调用，扫描要跟进去
        o.branch = true; o.direct = true; o.cond = conditional; o.call = true;
        if (a.op_count >= 1 && a.operands[0].type == ARM_OP_IMM) {
            o.target = static_cast<uint32_t>(a.operands[0].imm) & ~1u;
        }
        return true;

    case ARM_INS_CBZ:                   // CBZ / CBNZ：条件 + 目标可由编码算出
    case ARM_INS_CBNZ:
        o.branch = true; o.direct = true; o.cond = true;
        if (a.op_count >= 2 && a.operands[1].type == ARM_OP_IMM) {
            o.target = static_cast<uint32_t>(a.operands[1].imm) & ~1u;
        }
        return true;

    case ARM_INS_BLX:                   // BLX 立即数 = 直接调用；寄存器 = 间接
        o.branch = true;
        if (a.op_count >= 1 && a.operands[0].type == ARM_OP_IMM) {
            o.direct = true; o.call = true; o.cond = conditional;
            o.target = static_cast<uint32_t>(a.operands[0].imm) & ~1u;
        } else {
            o.direct = false;
        }
        return true;

    case ARM_INS_BX:                    // 目标在寄存器里 —— 听 ETM
    case ARM_INS_BXJ:
    case ARM_INS_TBB:                   // 跳转表（TBB / TBH）
    case ARM_INS_TBH:
        o.branch = true; o.direct = false;
        return true;

    case ARM_INS_POP:                   // POP {…,PC} = 函数返回
        if (WritesPc(a)) { o.branch = true; o.direct = false; return true; }
        return false;

    case ARM_INS_LDM:                   // LDM 系列带 PC 也是返回
    case ARM_INS_LDMDA:
    case ARM_INS_LDMDB:
    case ARM_INS_LDMIB:
        if (WritesPc(a)) { o.branch = true; o.direct = false; return true; }
        return false;

    case ARM_INS_LDR:                   // LDR PC,[…] —— 间接分支
        if (a.op_count >= 1 && a.operands[0].type == ARM_OP_REG &&
            a.operands[0].reg == ARM_REG_PC) {
            o.branch = true; o.direct = false;
            return true;
        }
        return false;

    default:
        return false;
    }
}

} // namespace

bool InstrDecode(uint32_t pc, const uint8_t* p, unsigned avail, InstrInfo& o)
{
    o.len = 0; o.branch = false; o.direct = false;
    o.cond = false; o.call = false; o.target = 0;

    if (!p || avail < 2) return false;

    const csh h = Handle();
    if (!h) return false;

    EnsureInit();
    EnterCriticalSection(&g_cs);

    // `cs_disasm_iter` 直接吃"当前位置 + 剩余长度"，把指令解进预先分配的 insn。
    // ⚠ 它要求 insn 是 `cs_malloc()` 的；`g_insn` 就是。
    bool ok = false;
    if (g_insn) {
        const uint8_t* code = p;
        size_t         size = avail;
        uint64_t       addr = pc;
        if (cs_disasm_iter(h, &code, &size, &addr, g_insn)) {
            o.len = static_cast<unsigned>(g_insn->size);
            if (g_insn->detail) Classify(*g_insn, o);
            ok = true;
        }
    }

    LeaveCriticalSection(&g_cs);
    return ok;
}

ScanResult ScanTo(CodeReadFn read, void* ctx, uint32_t from, uint32_t to,
                  unsigned maxInstr, InstrEmitFn emit, void* emitCtx)
{
    // 跟随控制流时的调用栈深度上限。真实调用链到不了这么深，超了就当"这一段
    // 补不出来"—— 宁缺勿假。
    const unsigned kMaxCallDepth = 16;

    ScanResult r;
    r.instrs  = 0;
    r.reached = false;
    r.stopPc  = from;
    r.why     = "?";

    if (!read) { r.why = "没有读函数"; return r; }

    // ⚠ **不能**因为 `from == to` 就直接收尾：真机上绝大多数 ETM 地址对都是
    //   "同一个返回地址反复出现"（循环里的调用/返回），那种区间长度为 0，但中间
    //   **执行过一大段**（函数体 + 调用点）。所以照常往下扫，靠"遇到分支"收尾。
    uint32_t pc    = from;
    uint32_t stack[kMaxCallDepth];
    unsigned depth = 0;

    for (unsigned i = 0; i < maxInstr; ++i) {
        uint8_t  buf[4];
        InstrInfo info;

        if (!read(ctx, pc, buf, 2)) { r.stopPc = pc; r.why = "读不到指令"; return r; }
        if (!InstrDecode(pc, buf, 2, info)) {
            // 两种可能：32 位指令的后半还没读到，或者 capstone 解不出来。
            if (!read(ctx, pc + 2, buf + 2, 2)) {
                r.stopPc = pc;
                r.why    = "32 位指令后半读不到";
                return r;
            }
            if (!InstrDecode(pc, buf, 4, info)) {
                r.stopPc = pc;
                r.why    = "指令解不出来";
                return r;
            }
        }

        ++r.instrs;
        if (emit) emit(emitCtx, pc, info);

        if (info.branch) {
            if (info.direct) {
                if (info.target == to) {
                    r.reached = true; r.stopPc = to; r.why = "ok";
                    return r;
                }
                if (info.cond) {
                    // 条件分支：按"没跳"继续往下。跳没跳只有 ETM 知道 —— 猜错了会
                    // 走上没执行的那条路，所以调用方**必须**等 `reached` 再用这些指令。
                } else if (info.call) {
                    // BL：**压返回地址、进函数体**。分支到分支之间的指令绝大部分在
                    // 被调用的函数里，不跟进去就看不到（别的调试器能看到的就是这块）。
                    if (depth >= kMaxCallDepth) {
                        r.stopPc = pc; r.why = "调用嵌套太深"; return r;
                    }
                    stack[depth++] = pc + info.len;
                    pc = info.target;
                    continue;
                } else {
                    // 无条件 B：跟随（循环回跳、跳出去都跟）。
                    pc = info.target;
                    continue;
                }
            } else {
                // 间接分支：ETM 报的 `to` 就是它的目标。
                if (depth > 0) {
                    // 栈里有返回地址 ⇒ 当成"函数返回"，弹栈继续扫。
                    const uint32_t ret = stack[--depth];
                    if (ret == to) {
                        r.reached = true; r.stopPc = to; r.why = "ok（返回点）";
                        return r;
                    }
                    pc = ret;
                    continue;
                }
                r.reached = true;
                r.stopPc  = to;
                r.why     = "ok（间接分支，目标由 ETM 给）";
                return r;
            }
        }

        pc += info.len;
        if (pc == to) { r.reached = true; r.stopPc = to; r.why = "ok"; return r; }
    }

    r.stopPc = pc;
    r.why    = "超过指令条数上限";
    return r;
}

} // namespace agdi
} // namespace orb
