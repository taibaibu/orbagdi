/**
 * @file ORBAGDI_CodeImage.cpp
 * @brief 代码镜像的实现 —— 见头文件里的背景说明。
 *
 * 三条约束定了这里的形状：① 下载镜像必须拷走（GETFLASHPARAM 的缓冲下载完就没人保证）；
 * ② 不能预取全片（每次读都是 MEM-AP 事务），改按 512 B 页取 + 页缓存兜顺序扫描；
 * ③ 热路径在 worker 线程上，这一层自己不加探针锁，只用临界区保护镜像表与页缓存。
 */
#include "pch.h"
#include "ORBAGDI_CodeImage.h"
#include "ORBAGDI_Config.h"     // cfg::Get().iromStart/iromSize —— 器件声明的 Flash 范围

#include <string.h>
#include <stdlib.h>

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// 规模上限
// ---------------------------------------------------------------------------
const unsigned kMaxBlocks      = 32;                  // 下载镜像最多分这么多块
const unsigned long long kMaxMirrorBytes = 8ull << 20; // 8 MB —— 够 STM32F4 全片

const unsigned kPageShift = 9;                        // 512 B/页
const unsigned kPageSize  = 1u << kPageShift;
const unsigned kPageCount = 128;                      // 128 × 512 B = 64 KB

// ---------------------------------------------------------------------------
// 兜底读目标内存：默认关闭。会话开始时按器件声明一次性预读，热路径此后只读内存。
// 按需现读跑在已持探针锁的 worker 上，页缓存未命中要占锁做 MEM-AP 读，容易撞超时。
// tren=0 时预读与兜底读都不执行。这条活路只为"器件没声明 Flash 范围"，启用就把下面翻
// 成 true；两道闸仍在：LooksLikeCode() 限地址、EtmTraceIsSynced() + FillPage() 熔断。
// ---------------------------------------------------------------------------
const bool kMemFallbackEnabled = false;

struct MirrorBlock {
    uint32_t base;
    uint32_t len;
    uint8_t* data;
};

struct State {
    CRITICAL_SECTION cs;

    // ---- 来源 ①：留存的下载镜像 -------------------------------------------
    MirrorBlock        blk[kMaxBlocks];
    unsigned           nBlk;
    unsigned long long bytes;
    bool               warnedFull;      // 超上限只报一次

    // ---- 来源 ②：兜底读 ---------------------------------------------------
    CodeImageMemFn     memFn;
    void*              memCtx;
    bool               memDisabled;     // 熔断：本会话不再现读目标内存
    bool               warnDisabled;    // 需要打一次"已熔断"的 WARN（锁外打）

    // ---- 页缓存（直接映射） -----------------------------------------------
    struct Page {
        uint32_t tag;                   // 页基址
        bool     valid;
        uint8_t  data[kPageSize];
    };
    Page page[kPageCount];

    CodeImageStats st;

    State()
        : nBlk(0), bytes(0), warnedFull(false), memFn(nullptr), memCtx(nullptr),
          memDisabled(false), warnDisabled(false)
    {
        memset(blk, 0, sizeof(blk));
        memset(page, 0, sizeof(page));
        memset(&st, 0, sizeof(st));
        InitializeCriticalSection(&cs);
    }
};

// 故意泄漏（同 ORBAGDI_Etm.cpp 的说明）：进程内一份，退出时由系统回收。
State& S()
{
    static State* s = new State();
    return *s;
}

// 从镜像取一整页。**必须整页都在同一块里**才认 —— 半页走兜底读，因为镜像与
// 目标内存混在同一页里时，拼接起来更糟（那半页可能根本没下载过）。
bool MirrorPage(State& s, uint32_t page, uint8_t* out)
{
    const uint64_t lo = page;
    const uint64_t hi = static_cast<uint64_t>(page) + kPageSize;

    for (unsigned i = 0; i < s.nBlk; ++i) {
        const MirrorBlock& b = s.blk[i];
        const uint64_t blo = b.base;
        const uint64_t bhi = static_cast<uint64_t>(b.base) + b.len;
        if (lo >= blo && hi <= bhi) {
            memcpy(out, b.data + (page - b.base), kPageSize);
            s.st.mirrorHits += kPageSize;
            return true;
        }
    }
    return false;
}

// 安全闸：明显不是代码的地址不发事务 —— 读到没映射的地方会让 MEM-AP 报错、DAP 进错误态，
// 之后所有 DAP 操作都失败。Flash 按 iromStart/iromSize 判，声明缺失才退回宽范围，只排除
// 外设区（0x4000_0000-0x5FFF_FFFF）与内核私有区（0xE000_0000+）。
bool LooksLikeCode(uint32_t a)
{
    // Get()：原始配置，只有用户/选项串设过的值。Effective()：副本上补 AutoDetected() 的器件
    // 信息（FLM、RAM、时钟、iromStart/iromSize），只读快照。判 Flash 范围必须用后者，否则
    // iromSize 恒 0，闸门退回宽范围。
    const orb::cfg::Config c = orb::cfg::Effective();

    if (c.iromSize) {
        // Flash 主区：声明尺寸之内才算。
        const uint32_t fl = c.iromStart;
        const uint64_t fh = static_cast<uint64_t>(fl) + c.iromSize;
        if (a >= fl && a < fh) return true;
        // 启动别名区（0x00000000 起）同样受声明尺寸约束。
        if (a < c.iromSize) return true;
    } else {
        // 器件描述没给 Flash 尺寸：退回宽范围（老器件/裸片），并留证据。
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            ORB_LOG_WARN("CodeImage: 器件描述没给 Flash 范围（iromSize=0）—— 粗筛退回"
                         "宽范围 0x08000000..0x0FFFFFFF；越界读的风险由"
                         "\"一次失败即熔断\"兜住");
        }
        if (a >= 0x08000000u && a <= 0x0FFFFFFFu) return true;
        if (a <= 0x003FFFFFu)                     return true;
    }

    if (a >= 0x10000000u && a <= 0x1FFFFFFFu) return true;   // CCM / 系统区
    if (a >= 0x20000000u && a <= 0x3FFFFFFFu) return true;   // SRAM（RAM 函数）
    if (a >= 0x60000000u && a <= 0x9FFFFFFFu) return true;   // 外部 RAM / XIP
    return false;
}

// 把一页读进缓存槽。失败时**不写** `p`（保持原状，避免把半页当有效页用）。
bool FillPage(State& s, uint32_t page, State::Page& p)
{
    if (MirrorPage(s, page, p.data)) {
        p.tag = page;
        p.valid = true;
        return true;
    }

    // ---- 来源 ②：兜底读目标内存（默认关；两道闸见文件头与头文件） ---------
    if (!kMemFallbackEnabled || !s.memFn || s.memDisabled) return false;
    if (!orb::cfg::Get().traceEnable) return false;      // trace 关着就别碰目标内存

    if (!LooksLikeCode(page)) {
        ++s.st.memRejected;
        return false;
    }

    if (s.memFn(s.memCtx, page, p.data, kPageSize)) {
        s.st.memReads += kPageSize;
        p.tag = page;
        p.valid = true;
        return true;
    }

    // 一次失败即熔断：越界读会让 DAP 进错误态，宁可这一段补不出（有日志）。
    ++s.st.memFails;
    s.st.memDisabled = true;
    s.memDisabled    = true;
    s.warnDisabled   = true;        // 日志在锁外打
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// 来源 ①
// ---------------------------------------------------------------------------
void CodeImageAdd(uint32_t base, const void* data, uint32_t len)
{
    if (!data || len == 0) return;

    State& s = S();
    EnterCriticalSection(&s.cs);

    // ---- 先摘掉重叠的旧块 ----
    // Flash 预读与 AG_CB_GETFLASHPARAM 会覆盖同一片地址，而 MirrorPage() 取第一个装得下整页的
    // 块 ⇒ 不摘旧的，后写的永远轮不到（"下载了新固件、反汇编还是上一版"）。后写者赢；
    // 页缓存一并作废。
    const uint64_t lo = base;
    const uint64_t hi = static_cast<uint64_t>(base) + len;
    bool replaced = false;
    for (unsigned i = 0; i < s.nBlk; ) {
        const uint64_t blo = s.blk[i].base;
        const uint64_t bhi = static_cast<uint64_t>(s.blk[i].base) + s.blk[i].len;
        if (lo < bhi && blo < hi) {                 // 相交
            free(s.blk[i].data);
            s.bytes -= s.blk[i].len;
            s.blk[i] = s.blk[s.nBlk - 1];           // 尾部前移填空（块的顺序不重要）
            --s.nBlk;
            replaced = true;
            continue;
        }
        ++i;
    }
    if (replaced) memset(s.page, 0, sizeof(s.page));

    bool taken = false;
    if (s.nBlk < kMaxBlocks && (s.bytes + len) <= kMaxMirrorBytes) {
        uint8_t* copy = static_cast<uint8_t*>(malloc(len));
        if (copy) {
            memcpy(copy, data, len);
            s.blk[s.nBlk].base = base;
            s.blk[s.nBlk].len  = len;
            s.blk[s.nBlk].data = copy;
            ++s.nBlk;
            s.bytes += len;
            taken = true;
        }
    }
    const bool warn = (!taken && !s.warnedFull);
    if (warn) s.warnedFull = true;

    LeaveCriticalSection(&s.cs);

    // 日志放在锁外：ORB_LOG_* 会写文件，不该在临界区里做。
    if (warn) {
        ORB_LOG_WARN("CodeImage: 镜像留存已满（%u 块 / %llu B 上限）—— "
                     "地址 0x%08X + %u B 这一块没留，直接分支将补不出",
                     kMaxBlocks, kMaxMirrorBytes, base, len);
    }
}

// ---------------------------------------------------------------------------
// 会话缓存：一次性预读器件声明的 Flash
// ---------------------------------------------------------------------------
void CodeImagePreloadFlash(CodeImageMemFn read, void* ctx)
{
    if (!read) return;

    const orb::cfg::Config c = orb::cfg::Effective();

    // Trace 没开就什么都不做：代码镜像只服务反汇编窗口，而预读是若干次大块 MEM-AP
    // 读，在 Connect 时占着探针锁。放在 iromSize 之前判，免得白刷一条 WARN。
    if (!c.traceEnable) {
        ORB_LOG_INFO("CodeImage: Trace 未启用（tren=0）—— 跳过 Flash 预读");
        return;
    }

    if (!c.iromSize) {
        ORB_LOG_WARN("CodeImage: 器件描述没给 Flash 范围（iromSize=0）—— 跳过预读，"
                     "直接分支将补不出（下载时会另存镜像）");
        return;
    }

    // 上限与镜像表一致：超了就只读前 kMaxMirrorBytes，并说清楚。
    unsigned long long want = c.iromSize;
    if (want > kMaxMirrorBytes) {
        ORB_LOG_WARN("CodeImage: Flash 声明 %llu B，超过镜像上限 %llu B —— "
                     "只预读前 %llu B", want, (unsigned long long)kMaxMirrorBytes,
                     (unsigned long long)kMaxMirrorBytes);
        want = kMaxMirrorBytes;
    }

    // 分块读。块大小与偏移都按 kPageSize 对齐 —— `MirrorPage()` 要求**整页落在同一块
    // 里**，不对齐就会在块边界上退化成"这一页取不到"。
    const unsigned kChunk = 64u << 10;                  // 64 KB/次：512 KB → 8 次事务
    uint8_t* buf = static_cast<uint8_t*>(malloc(kChunk));
    if (!buf) {
        ORB_LOG_ERROR("CodeImage: 预读缓冲分配失败（%u B）", kChunk);
        return;
    }

    const uint64_t t0 = GetTickCount64();
    unsigned long long done = 0;
    unsigned reads = 0;
    bool failed = false;

    while (done < want) {
        unsigned n = static_cast<unsigned>(want - done);
        if (n > kChunk) n = kChunk;

        const uint32_t at = c.iromStart + static_cast<uint32_t>(done);
        if (!read(ctx, at, buf, n)) { failed = true; break; }

        CodeImageAdd(at, buf, n);
        done += n;
        ++reads;
    }

    free(buf);

    ORB_LOG_INFO("CodeImage: Flash 预读%s —— 0x%08X + %llu B / %u 次读 / %u ms"
                 "（会话缓存，热路径此后零探针访问）%s",
                 failed ? "**中断**" : "完成", c.iromStart, done, reads,
                 static_cast<unsigned>(GetTickCount64() - t0),
                 failed ? "；读到一半失败，已读到的部分照用" : "");
}

void CodeImageClear()
{
    State& s = S();
    EnterCriticalSection(&s.cs);

    for (unsigned i = 0; i < s.nBlk; ++i) {
        free(s.blk[i].data);
        s.blk[i].data = nullptr;
    }
    s.nBlk         = 0;
    s.bytes        = 0;
    s.warnedFull   = false;
    s.memDisabled  = false;     // 新会话重新允许兜底读（但闸门还在）
    s.warnDisabled = false;
    memset(s.page, 0, sizeof(s.page));
    memset(&s.st, 0, sizeof(s.st));

    LeaveCriticalSection(&s.cs);
}

unsigned long long CodeImageBytes()
{
    State& s = S();
    EnterCriticalSection(&s.cs);
    const unsigned long long n = s.bytes;
    LeaveCriticalSection(&s.cs);
    return n;
}

// ---------------------------------------------------------------------------
// 来源 ②
// ---------------------------------------------------------------------------
void CodeImageSetReader(CodeImageMemFn fn, void* ctx)
{
    State& s = S();
    EnterCriticalSection(&s.cs);
    s.memFn  = fn;
    s.memCtx = ctx;
    LeaveCriticalSection(&s.cs);
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------
bool CodeImageRead(uint32_t addr, uint8_t* out, unsigned len)
{
    if (!out || len == 0) return true;

    State& s = S();
    EnterCriticalSection(&s.cs);

    bool ok = true;
    while (len) {
        const uint32_t page = addr & ~(kPageSize - 1u);
        const unsigned off  = addr - page;
        unsigned       n    = kPageSize - off;
        if (n > len) n = len;

        State::Page& p = s.page[(page >> kPageShift) % kPageCount];
        if (p.valid && p.tag == page) {
            ++s.st.pageHits;
        } else if (FillPage(s, page, p)) {
            ++s.st.pageMiss;
        } else {
            ok = false;                 // 这一段读不到 —— 调用方应当停
            break;
        }

        memcpy(out, p.data + off, n);
        addr += n;
        out  += n;
        len  -= n;
    }

    const bool warn = s.warnDisabled;
    if (warn) s.warnDisabled = false;

    LeaveCriticalSection(&s.cs);

    // 熔断只报一次（锁外打：ORB_LOG_* 会写文件，不该在临界区里做）。
    if (warn) {
        ORB_LOG_WARN("CodeImage: 兜底读失败一次 —— **本会话不再现读目标内存**"
                     "（只认下载镜像）。越界读会让 DAP 进错误态、整条调试链路要靠"
                     "\"链路恢复\"重建，所以这里一次就熔断");
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
void CodeImageStatsGet(CodeImageStats& out)
{
    State& s = S();
    EnterCriticalSection(&s.cs);
    out = s.st;
    LeaveCriticalSection(&s.cs);
}

void CodeImageSummary(const char* where)
{
    State& s = S();
    CodeImageStats st;
    unsigned       nBlk;
    unsigned long long bytes;

    EnterCriticalSection(&s.cs);
    st    = s.st;
    nBlk  = s.nBlk;
    bytes = s.bytes;
    LeaveCriticalSection(&s.cs);

    ORB_LOG_INFO("CodeImage(%s): 镜像 %u 块 / %llu B；页缓存 命中 %llu 未命中 %llu；"
                 "兜底读 %llu B（失败 %llu，粗筛挡掉 %llu）%s",
                 where ? where : "?", nBlk, bytes,
                 st.pageHits, st.pageMiss, st.memReads, st.memFails, st.memRejected,
                 !kMemFallbackEnabled
                     ? " —— 兜底读**未启用**（默认只认下载镜像；要试见 kMemFallbackEnabled）"
                     : (st.memDisabled ? " —— **已熔断**（本会话不再现读目标内存）" : ""));
}

} // namespace agdi
} // namespace orb
