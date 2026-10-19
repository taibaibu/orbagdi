/**
 * @file ORBAGDI_InstrScan.cpp
 * @brief 指令扫描的实现 —— 解码交给 capstone，控制流跟随是自己的（规则见头文件）。
 *
 * 手写 Thumb 解码维护不起：I1 = NOT(J1 EOR S) 取反位、LDR.W PC 误判成直接分支，错一位就是
 * 静默走错路。capstone 给长度 / 分支 id / 目标 / 条件码，接 RISC-V 只改 cs_open() 参数。
 * ⚠ 这里只管解码；FillGap() 只在抽数 worker 线程上调，InstrDecode() 是公开 API 仍加锁。
 *
 * ---- 为什么加"指令形状缓存"（ScanToCached）------------------------------------
 * P1-0 补全每秒要解上百万条指令，而同一段代码在一次会话里会被反复扫（循环、反复调用）。
 * 原来每条指令都要：read 回调（CodeImageRead 加锁 + 页查找 + memcpy）→ InstrDecode
 * （第二次加锁 + capstone + detail）。实测 106 ms/64 KB，而数据源是 3 MB/s ⇒ RDDI 环
 * 丢掉 82% 的数据。指令形状只取决于 pc 处的字节，字节来自代码镜像且会话内不变，所以
 * 按 pc 缓存 {len, 分支形状, target} 就够了；镜像一变（CodeImageGeneration 递增）整表丢。
 * 命中路径不再读字节、不再进 capstone。
 */
#include "pch.h"
#include "ORBAGDI_InstrScan.h"
#include "ORBAGDI_CodeImage.h"   // CodeImageGeneration()：缓存作废的唯一依据

#include <capstone/capstone.h>

#include <string.h>

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// capstone 句柄
// cs_disasm_iter 要求 cs_insn 由 cs_malloc() 分配（capstone 契约），不能放栈上。
// 两者都只建一次、进程退出由系统回收（故意泄漏，避免 DLL 卸载时序上的坑）。
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

// ---------------------------------------------------------------------------
// 指令形状缓存（只给 ScanToCached 用，见文件头）
// ---------------------------------------------------------------------------
// 槽位数 = 512 KB Flash / 2 B：Cortex-M 的最小指令是 2 B，所以"每个半字一个槽"能让
// 512 KB 的镜像直接映射、零冲突。RAM 里跑的函数会与 Flash 地址撞槽，撞了只是回到慢路，
// 不影响正确性（命中判定要看 pc 完全相等）。
const unsigned kICacheSlots = 1u << 18;              // 262144
const unsigned kICacheMask  = kICacheSlots - 1u;

// 16 B/条（对齐后），262144 × 16 = 4 MB。故意不释放（同 capstone 句柄）。
struct ICacheEntry {
    uint32_t pc;        // 命中的 pc；len == 0 表示空槽
    uint32_t target;
    uint8_t  len;
    uint8_t  flags;
    uint16_t pad;
};

enum {
    kFlagBranch = 1u << 0,
    kFlagDirect = 1u << 1,
    kFlagCond   = 1u << 2,
    kFlagCall   = 1u << 3,
};

ICacheEntry*     g_icache    = nullptr;
bool             g_icacheFailed = false;
unsigned         g_icacheGen = 0;
unsigned long long g_cacheHits   = 0;
unsigned long long g_cacheMisses = 0;
unsigned long long g_cacheFlushes = 0;

ICacheEntry* ICache()
{
    if (g_icache) return g_icache;
    if (g_icacheFailed) return nullptr;

    // 首次用时才分配：不开 trace 的会话一个字节都不占。
    ICacheEntry* p = static_cast<ICacheEntry*>(malloc(sizeof(ICacheEntry) * kICacheSlots));
    if (!p) {
        g_icacheFailed = true;
        ORB_LOG_WARN("InstrScan: 指令缓存分配失败（%u 槽）—— 回退到逐条解码",
                     kICacheSlots);
        return nullptr;
    }
    memset(p, 0, sizeof(ICacheEntry) * kICacheSlots);
    g_icache = p;
    return p;
}

// 镜像版本变了就整表作废。memset 4 MB ≈ 0.3 ms，只在"下载/清空镜像之后第一次扫"发生。
bool CacheReady()
{
    if (!ICache()) return false;
    const unsigned g = CodeImageGeneration();
    if (g != g_icacheGen) {
        memset(g_icache, 0, sizeof(ICacheEntry) * kICacheSlots);
        g_icacheGen = g;
        ++g_cacheFlushes;
    }
    return true;
}

uint8_t FlagsOf(const InstrInfo& o)
{
    uint8_t f = 0;
    if (o.branch) f |= kFlagBranch;
    if (o.direct) f |= kFlagDirect;
    if (o.cond)   f |= kFlagCond;
    if (o.call)   f |= kFlagCall;
    return f;
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

// ScanTo / ScanToCached 的公共实现。useCache 只影响"怎么拿到这条指令的形状"，
// 控制流跟随完全一样。
ScanResult ScanCore(CodeReadFn read, void* ctx, uint32_t from, uint32_t to,
                    unsigned maxInstr, InstrEmitFn emit, void* emitCtx, bool useCache)
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

    // 版本校验放在最外层：一次 ScanTo 调用期间镜像不会变（变化只发生在下载/清空，
    // 那时 trace 不会在解码），所以表项在本次调用里是一致的。
    if (useCache && !CacheReady()) useCache = false;

    // ⚠ **不能**因为 `from == to` 就直接收尾：真机上绝大多数 ETM 地址对都是
    //   "同一个返回地址反复出现"（循环里的调用/返回），那种区间长度为 0，但中间
    //   **执行过一大段**（函数体 + 调用点）。所以照常往下扫，靠"遇到分支"收尾。
    uint32_t pc    = from;
    uint32_t stack[kMaxCallDepth];
    unsigned depth = 0;

    ICacheEntry* ic = useCache ? g_icache : nullptr;

    for (unsigned i = 0; i < maxInstr; ++i) {
        InstrInfo info;

        // ---- 快路：这条指令的形状已经解过 ----------------------------------
        // 命中后既不读镜像（CodeImageRead 的锁 + memcpy 全省）、也不进 capstone。
        ICacheEntry* e = ic ? &ic[(pc >> 1) & kICacheMask] : nullptr;
        if (e && e->len && e->pc == pc) {
            ++g_cacheHits;
            info.len    = e->len;
            info.branch = (e->flags & kFlagBranch) != 0;
            info.direct = (e->flags & kFlagDirect) != 0;
            info.cond   = (e->flags & kFlagCond)   != 0;
            info.call   = (e->flags & kFlagCall)   != 0;
            info.target = e->target;
        } else {
            if (e) ++g_cacheMisses;

            uint8_t buf[4];
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
            if (e) {
                e->pc     = pc;
                e->target = info.target;
                e->len    = static_cast<uint8_t>(info.len);
                e->flags  = FlagsOf(info);
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

ScanResult ScanTo(CodeReadFn read, void* ctx, uint32_t from, uint32_t to,
                  unsigned maxInstr, InstrEmitFn emit, void* emitCtx)
{
    return ScanCore(read, ctx, from, to, maxInstr, emit, emitCtx, false);
}

ScanResult ScanToCached(CodeReadFn read, void* ctx, uint32_t from, uint32_t to,
                        unsigned maxInstr, InstrEmitFn emit, void* emitCtx)
{
    return ScanCore(read, ctx, from, to, maxInstr, emit, emitCtx, true);
}

void InstrScanGetCacheStats(InstrScanCacheStats& out)
{
    out.hits    = g_cacheHits;
    out.misses  = g_cacheMisses;
    out.flushes = g_cacheFlushes;
    out.slots   = g_icache ? kICacheSlots : 0u;
}

} // namespace agdi
} // namespace orb
