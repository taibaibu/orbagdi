// ORBAGDI_Cover.cpp - 覆盖率 / 性能分析的数据面，见 ORBAGDI_Cover.h。
//
// 存储按 64 kB 段懒分配：每段 `attr[16384]`（uVision 直接读的属性字）+
// `count[16384]`（性能分析计数）。AG_GETATTRSEG 的契约是"段首指针，后面
// 跟着 16384 个连续 DWORD"，所以必须整段连续 —— 这正是原来 AttrStore
// （std::map 单节点）不能直接拿来当覆盖率存储的原因：它返回的指针只覆盖
// 4 字节，uVision 按段索引读会读到堆里，hCover=1 之后必崩。
#include "ORBAGDI_Cover.h"

#include <windows.h>
#include <map>
#include <new>
#include <cstring>

namespace orb {
namespace cover {
namespace {

enum {
    kSegSize     = 0x10000,   // 一段 = 64 kB 地址空间
    kWordsPerSeg = 0x4000,    // 16384 个 DWORD 属性字
    kMaxSegs     = 256        // 256 × 128 kB ≈ 32 MB，段表满就不再分配
};

// COLLECT.H: ATRX_EXECD=0x00000100 / ATRX_EXECDO=0x01000000。
// 一个 DWORD 覆盖 4 字节 = 两条 Thumb 指令：低半字用 EXECD，高半字用 EXECDO。
// ⚠ 这不是 ATRX_EXEC(0x01)/READ/WRITE 那组"可执行/可读写"位 —— 那组由 uVision
// 自己从 .axf 的段属性知道，覆盖率的"已执行"是另一组位。
const uint32_t kExecd  = 0x00000100u;
const uint32_t kExecdo = 0x01000000u;

struct Segment {
    uint32_t attr[kWordsPerSeg];    // 覆盖率属性字
    uint32_t count[kWordsPerSeg];   // 执行次数（性能分析）
};

struct Store {
    CRITICAL_SECTION             cs;
    std::map<uint32_t, Segment*> segs;
    Store() { InitializeCriticalSection(&cs); }
};

Store& S()
{
    static Store s;      // C++11 magic static：线程安全
    return s;
}

Segment* FindOrAdd(uint32_t base)
{
    Store& st = S();
    EnterCriticalSection(&st.cs);
    Segment* seg = nullptr;
    std::map<uint32_t, Segment*>::const_iterator it = st.segs.find(base);
    if (it != st.segs.end()) {
        seg = it->second;
    } else if (st.segs.size() < static_cast<size_t>(kMaxSegs)) {
        seg = new (std::nothrow) Segment();
        if (seg) st.segs[base] = seg;
    }
    LeaveCriticalSection(&st.cs);
    return seg;
}

// 解码线程的段缓存。只有解码线程写这两个变量；uVision 线程只读段内数组
// （DWORD 对齐读写是原子的，x86 下无需额外屏障）。
uint32_t g_curBase = 0xFFFFFFFFu;
Segment* g_curSeg  = nullptr;

} // namespace

void OnInstruction(uint32_t addr, bool executed)
{
    const uint32_t a = addr & ~1u;
    if (a >= 0xFFFF0000u) return;      // trace 里不该出现；也别为它分配段
    const uint32_t base = a & 0xFFFF0000u;

    if (base != g_curBase) {
        g_curSeg  = FindOrAdd(base);   // 只在换段时进锁：稳态热路径无锁
        g_curBase = base;
    }
    Segment* s = g_curSeg;
    if (!s) return;

    const uint32_t idx = (a & 0xFFFFu) >> 2;
    s->attr[idx] |= ((a & 2u) == 0u) ? kExecd : kExecdo;
    if (executed) {
        const uint32_t c = s->count[idx];
        if (c != 0xFFFFFFFFu) s->count[idx] = c + 1;   // 饱和，不溢出回绕
    }
}

uint32_t* AttrSeg(uint32_t segBase)
{
    const uint32_t base = segBase & 0xFFFF0000u;
    Segment* s = FindOrAdd(base);
    if (s) return s->attr;

        // 段表满（256 段 = 16 MB 代码空间）：给一块共享的空段，绝不给 NULL ——
        // AG_GETATTRSEG 返回 0 就是让 uVision 拿空指针去索引 16384 项。
    static uint32_t s_full[kWordsPerSeg];
    return s_full;
}

uint32_t* AttrPtr(uint32_t adr)
{
    const uint32_t a = adr & ~3u;
    Segment* s = FindOrAdd(a & 0xFFFF0000u);
    return s ? &s->attr[(a & 0xFFFFu) >> 2] : nullptr;
}

uint32_t AttrGet(uint32_t adr)
{
    const uint32_t a    = adr & ~3u;
    const uint32_t base = a & 0xFFFF0000u;
    Store& st = S();

    EnterCriticalSection(&st.cs);
    const std::map<uint32_t, Segment*>::const_iterator it = st.segs.find(base);
    const uint32_t v = (it == st.segs.end()) ? 0u
                                             : it->second->attr[(a & 0xFFFFu) >> 2];
    LeaveCriticalSection(&st.cs);
    return v;
}

void AttrSet(uint32_t adr, uint32_t v)
{
    const uint32_t a = adr & ~3u;
    Segment* s = FindOrAdd(a & 0xFFFF0000u);
    if (!s) return;

    const uint32_t idx = (a & 0xFFFFu) >> 2;
        // 调用方传的都是 16 位断点属性（BREAK/BPDIS/WATCH/READ/WRITE）。高 16 位是
        // 同一 DWORD 里另一条 Thumb 指令的 EXECDO/ITAKENO/BREAKO，低 16 位的
        // EXECD/ITAKEN 是解码线程标的"执行过" —— 这两部分都不能被一次断点写抹掉，
        // 否则覆盖率会随着"设/删断点"随机掉数据。
    const uint32_t keep = 0xFFFF0000u | 0x00000300u;   // 高半字 + EXECD|ITAKEN
    s->attr[idx] = (v & 0x0000FFFFu) | (s->attr[idx] & keep);
}

void AttrClearBp()
{
        // BREAK | BPDIS | WATCH | WRBRK | RDBRK | BREAKO | BPDISO
    const uint32_t kMask = 0x0C030C08u;
    Store& st = S();

    EnterCriticalSection(&st.cs);
    for (std::map<uint32_t, Segment*>::iterator it = st.segs.begin();
         it != st.segs.end(); ++it) {
        uint32_t* a = it->second->attr;
        for (uint32_t i = 0; i < kWordsPerSeg; ++i) a[i] &= ~kMask;
    }
    LeaveCriticalSection(&st.cs);
}

void MapSeg(uint32_t addr, uint32_t len, uint32_t attr)
{
    if (len == 0 || addr >= 0xFFFF0000u) return;
    if (len > 0xFFFF0000u - addr) len = 0xFFFF0000u - addr;   // 溢出截断

    uint32_t a = addr;
    const uint32_t end = addr + len;
    while (a < end) {
        const uint32_t base   = a & 0xFFFF0000u;
        const uint32_t segEnd = base + kSegSize;
        if (segEnd <= a) break;
        const uint32_t hi = (end < segEnd) ? end : segEnd;

        Segment* s = FindOrAdd(base);
        if (s) {
            uint32_t i0 = (a - base) >> 2;
            uint32_t i1 = (hi - base + 3u) >> 2;
            if (i1 > kWordsPerSeg) i1 = kWordsPerSeg;
            for (uint32_t i = i0; i < i1; ++i) {
                if (attr == 0) s->attr[i] = 0;
                else           s->attr[i] |= attr;
            }
        }
        a = hi;
    }
}

void Clear()
{
    Store& st = S();
    EnterCriticalSection(&st.cs);
    for (std::map<uint32_t, Segment*>::iterator it = st.segs.begin();
         it != st.segs.end(); ++it) {
        memset(it->second->attr,  0, sizeof it->second->attr);
        memset(it->second->count, 0, sizeof it->second->count);
    }
    LeaveCriticalSection(&st.cs);
}

uint64_t ExecCount(uint32_t addr)
{
    Store& st = S();
    const uint32_t base = addr & 0xFFFF0000u;
    const uint32_t idx  = (addr & 0xFFFFu) >> 2;

    EnterCriticalSection(&st.cs);
    const std::map<uint32_t, Segment*>::const_iterator it = st.segs.find(base);
    const uint64_t n = (it == st.segs.end()) ? 0 : it->second->count[idx];
    LeaveCriticalSection(&st.cs);
    return n;
}

uint64_t ExecCountRange(uint32_t start, uint32_t end)
{
    if (end <= start) return 0;

    Store& st = S();
    uint64_t n = 0;
    uint32_t a = start;

    EnterCriticalSection(&st.cs);
    while (a < end) {
        const uint32_t base   = a & 0xFFFF0000u;
        const uint32_t segEnd = base + kSegSize;
        if (segEnd <= a) break;                   // 溢出保护
        const uint32_t hi = (end < segEnd) ? end : segEnd;

        const std::map<uint32_t, Segment*>::const_iterator it = st.segs.find(base);
        if (it != st.segs.end()) {
            // 按 DWORD 粒度累加：尾部不足 4 字节的那一条也算进去。
            const uint32_t i0 = (a - base) >> 2;
            uint32_t       i1 = (hi - base + 3u) >> 2;
            if (i1 > kWordsPerSeg) i1 = kWordsPerSeg;
            const uint32_t* c = it->second->count;
            for (uint32_t i = i0; i < i1; ++i) n += c[i];
        }
        a = hi;
    }
    LeaveCriticalSection(&st.cs);
    return n;
}

uint64_t MarkedWords()
{
    const uint32_t kMask = kExecd | kExecdo;
    Store& st = S();
    uint64_t n = 0;

    EnterCriticalSection(&st.cs);
    for (std::map<uint32_t, Segment*>::const_iterator it = st.segs.begin();
         it != st.segs.end(); ++it) {
        const uint32_t* a = it->second->attr;
        for (uint32_t i = 0; i < kWordsPerSeg; ++i) {
            if (a[i] & kMask) ++n;
        }
    }
    LeaveCriticalSection(&st.cs);
    return n;
}

unsigned SegmentCount()
{
    Store& st = S();
    EnterCriticalSection(&st.cs);
    const unsigned n = static_cast<unsigned>(st.segs.size());
    LeaveCriticalSection(&st.cs);
    return n;
}

} // namespace cover
} // namespace orb
