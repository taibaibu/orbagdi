/**
 * @file ORBAGDI_AgdiProbe.cpp
 * @brief Stands in for uVision: drives the built ORB_AGDI.dll through its AGDI
 *        entry points, so the debug / flash / trace contract can be verified
 *        without opening the IDE.
 *
 * uVision is the only host that normally calls AG_Init / AG_MemAcc / AG_GoStep /
 * AG_Serial.  That made "does the driver work" a question only a human clicking
 * through a debug session could answer - not repeatable, not unattended, and
 * silent about *which* entry point broke when it did.
 *
 * This program is the smallest host that can take uVision's place:
 *
 *   - it loads the **built** DLL (the same file uVision loads), not a copy of
 *     src\, so what is exercised is what ships;
 *   - it fills the debug block the way uVision does and hands it over through the
 *     same entry point (EnumUvARM7(2)), so the project's option string arrives
 *     down the same path - including the Keil-style -F/-O spellings;
 *   - it calls AG_Init(AG_INITFEATURES) to connect, then drives the entry points
 *     and prints what each one returned.
 *
 * The checks are grouped so a failure says which part of the AGDI contract broke:
 * ABI, connect, memory, registers, run control.
 *
 * A callback is registered (AG_INITCALLBACK) before anything else, and it answers
 * AG_CB_MSGBOX.  Without one the driver falls back to a modal MessageBoxA, which
 * would sit there waiting for a click nobody can give - the harness would hang
 * rather than report.
 *
 * Usage:
 *   test\bin\ORBAGDI_AgdiProbe.exe [options]
 *
 *     --dll <path>        driver to load        (default: ..\..\bin\ORB_AGDI.dll)
 *     --options "<str>"   project option string (default: "clock=1000 iface=0")
 *     --read <hex>        address to read       (default: 0x08000000)
 *     --readcmp <hex>,<len>[,<passes>]
 *                         read that region N times (default 3) and compare the
 *                         passes with each other.  A MEM-AP read that does not
 *                         repeat says the transport is broken, not the memory -
 *                         and reading several different regions separates "this
 *                         flash is odd" from "every read is odd"
 *     --memwrite <hex>    scratch address for the write/read-back check
 *                         (omitted: read-only, nothing is written to the target)
 *     --device <name>     device name for the debug block
 *     --flash <hex>       Intel HEX image to download, then read back and compare
 *     --flm <path>        flash algorithm; appended to the option string as flm=
 *     --ram <hex>,<hex>   RAM window for the algorithm; appended as ram=
 *     --keep              leave the session up instead of shutting down
 *     --halt              leave the core stopped (default: resume it)
 *
 * Trace source (the orbtrace probe).  Each flag appends one key to the option
 * string, so a trace run is spelled out instead of hand-written.  The mapping
 * to the driver's keys is deliberately one-to-one:
 *
 *     --orbtrace          orbtrace=1   take trace from the probe's own endpoint
 *     --trwidth <1|2|4>   trwidth=N    parallel port width
 *     --trswo <0|1|2>     trswo=N      0=parallel, 1=SWO UART/NRZ, 2=Manchester
 *     --trbaud <Hz>       trbaud=N     SWO bit rate told to the probe
 *     --trtpui <0|1>      trtpui=N     probe unpacks TPIU framing.  Must match
 *                                      the target's TPIU_FFCR formatter state:
 *                                      a target that does not pack (FFCR.EnFCont
 *                                      is 0 and cannot be set on STM32) needs 0,
 *                                      otherwise the probe waits for a TPIU sync
 *                                      that never arrives and delivers nothing.
 *     --cpu <kHz>         cpu=N        core clock; the driver derives the TPIU
 *                                      divider from it
 *     --tclk <kHz>        tclk=N       trace port clock (parallel bit rate)
 *     --run <ms>          start the target, pump trace, then stop it
 *     --reset             after connecting, send AG_RESET the way uVision does
 *                         when it opens a session.  This is what makes the
 *                         target re-run its own startup - and therefore its own
 *                         trace setup.  Without it an ETM that is configured
 *                         correctly still delivers nothing but the idle pattern
 *                         (2026-10-14: 369 bytes of BC, zero branches, while the
 *                         same target under uVision produced 116 k branches).
 *     --poll <0|1>        while the run pumps trace, hammer AG_DapAcc(ACCDAP_ABORT)
 *                         (no c.running guard) and, after the stop, AG_AllReg -
 *                         this is the P0-A' acceptance: no failure under lock
 *                         contention with a trace source running
 *
 * --run is what makes trace collection possible at all: the pumps only run
 * inside the driver's run loop, exactly as they do on uVision's debug thread.
 *
 * A flash run erases and reprograms the target, so it is never implicit: it
 * happens only when --flash names an image.
 *
 * Exit code: 0 when every check passed.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <map>
#include <vector>

#include "AGDI.H"
#include "COMTYP.H"

// uVision 的接口。`struct bom`（AG_CB_GETBOMPTR 返回的 "ioc"）与 UV_* 请求码都在
// BOM.H 里；TDI 的结构体在 TraceView\ 下。**只有 --bom 时才用得上**，但类型要在
// 编译期可见（假 BOM 宿主，见下）。
#include "BOM.H"

// TraceDataTypes.h 用 DOUBLE（TRACE_CLOCK_SAMPLE::m_dLastTime）。完整 MDK 安装的
// ComTyp.h 里有这个 typedef，本仓库这份精简的 COMTYP.H 没有，所以补一个 —— 与
// 驱动侧 ORBAGDI_TraceWin.cpp 的写法一致。
#ifndef DOUBLE
typedef double DOUBLE;
#endif

#include "TraceView/TraceDataTypes.h"
#include "TraceView/TraceDataIf.h"

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
namespace {

int g_pass = 0;
int g_fail = 0;

void Pass(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("    PASS  %s\n", b);
    ++g_pass;
}

void Fail(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("    FAIL  %s\n", b);
    ++g_fail;
}

void Step(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("==> %s\n", b);
}

void Note(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("    %s\n", b);
}

// ---------------------------------------------------------------------------
// AG_RESET - 让目标重跑自己的启动流程（固件自己的 trace 初始化就在里面）。
//
// uVision **每次开调试会话都会发这一发**，而探针以前从来不发 —— 这就是
// "同一个目标：uVision 下 116k 分支、探针下 0 分支（369 字节全是空闲码 BC）"
// 的原因：驱动把 ETM 配好了，但固件的 trace 初始化没重跑，ETM 只吐空闲码。
//
// 只能走 `AG_Init(AG_EXECITEM | AG_RESET)`：AGDI 只导出 `AG_Init`，reset 是它的
// 一个 exec-item（AGDI.H: `AG_RESET = 0x000D`）。驱动侧对应
// `ORBAGDI_Agdi.cpp` 的 `case AG_RESET`（那里做 `c.core.Reset(true)`）。
//
// 参数用函数指针而不是 `Api&`：这样本函数能和其他输出辅助放在一起，
// 不必依赖 `struct Api` 的声明顺序。
// ---------------------------------------------------------------------------
void ResetTarget(U32 (*AG_Init)(U16, void*)) {
    Step("Reset the target: AG_Init(AG_EXECITEM | AG_RESET)");
    const U32 rc = AG_Init(static_cast<U16>(AG_EXECITEM | AG_RESET), nullptr);
    if (rc == AG_OK) Pass("AG_Init(AG_EXECITEM | AG_RESET) = 0");
    else             Fail("AG_Init(AG_EXECITEM | AG_RESET) = %u", rc);
}

// ---------------------------------------------------------------------------
// The exports, resolved at run time
// ---------------------------------------------------------------------------
struct Api {
    int (*EnumUvARM7)(void*, DWORD);
    int (*DllUv3Cap)(DWORD, void*);
    U32 (*AG_Init)(U16, void*);
    U32 (*AG_GoStep)(U16, U32, GADR*);
    U32 (*AG_MemAcc)(U16, UC8*, GADR*, UL32);
    U32 (*AG_MemAccX)(ACCMEMX*);
    U32 (*AG_RegAcc)(U16, U32, GVAL*);
    U32 (*AG_AllReg)(U16, void*);
    U32 (*AG_Serial)(U16, U32, U32, void*);
    U32 (*AG_HistFunc)(U32, I32, I32, void*);
    U32 (*AG_BpInfo)(U16, void*);
    U32 (*AG_MemAtt)(U16, UL32, GADR*);
    U32 (*AG_DapAcc)(ACCDAP*);
    U32 (*AG_BreakResAlloc)(U32, AG_BP*);
    AG_BP* (*AG_BreakFunc)(U16, U16, GADR*, AG_BP*);
};

Api g_api = {};
HMODULE g_mod = nullptr;

bool Resolve(HMODULE m, const char* name, void** out) {
    *out = reinterpret_cast<void*>(GetProcAddress(m, name));
    return *out != nullptr;
}

// ---------------------------------------------------------------------------
// The image uVision would hand over during a flash download.
//
// The driver pulls it with pCbFunc(AG_CB_GETFLASHPARAM): NULL asks for the first
// block, and every later call passes the *previous* block back to ask for the
// next one (AGDI.H:1290 documents the parameter as "(FLASHPARM *) or NULL", and
// Keil's DbgCM/Flash.cpp:1217 chains them).  Returning NULL means "list
// exhausted".  Passing NULL again instead of the previous block makes a pass
// spin for ever, so the previous pointer is what selects the block here.
// ---------------------------------------------------------------------------
class FlashImage {
public:
    bool Load(const char* path) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f) return false;

        std::map<UL32, UC8> bytes;
        UL32 base = 0;
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] != ':') continue;
            unsigned len = 0, addr = 0, type = 0;
            if (sscanf_s(line + 1, "%02x%04x%02x", &len, &addr, &type) != 3) continue;
            const char* p = line + 9;
            if (type == 0x00) {
                for (unsigned i = 0; i < len; ++i) {
                    unsigned b = 0;
                    if (sscanf_s(p + i * 2, "%02x", &b) != 1) break;
                    bytes[base + addr + i] = static_cast<UC8>(b);
                }
            } else if (type == 0x02) {
                unsigned seg = 0;
                if (sscanf_s(p, "%04x", &seg) == 1) base = seg << 4;
            } else if (type == 0x04) {
                unsigned hi = 0;
                if (sscanf_s(p, "%04x", &hi) == 1) base = hi << 16;
            } else if (type == 0x01) {
                break;                                  // EOF
            }
        }
        fclose(f);
        if (bytes.empty()) return false;

        // Coalesce into contiguous runs: the driver erases and programs per
        // block, so one block per run is what uVision would hand over too.
        Seg seg;
        seg.start = 0;
        for (std::map<UL32, UC8>::const_iterator it = bytes.begin(); it != bytes.end(); ++it) {
            if (seg.data.empty()) {
                seg.start = it->first;
            } else if (it->first != seg.start + seg.data.size()) {
                m_segs.push_back(seg);
                seg.data.clear();
                seg.start = it->first;
            }
            seg.data.push_back(it->second);
        }
        if (!seg.data.empty()) m_segs.push_back(seg);

        for (size_t i = 0; i < m_segs.size(); ++i) {
            FLASHPARM b;
            memset(&b, 0, sizeof(b));
            b.start = m_segs[i].start;
            b.many  = static_cast<UL32>(m_segs[i].data.size());
            b.image = &m_segs[i].data[0];
            m_total += b.many;
            m_blocks.push_back(b);
        }
        for (size_t i = 0; i < m_blocks.size(); ++i) m_blocks[i].ActSize = m_total;
        return !m_blocks.empty();
    }

    size_t Count() const { return m_blocks.size(); }
    UL32   Total() const { return m_total; }
    UL32   Start(size_t i) const { return m_segs[i].start; }
    size_t Len(size_t i)   const { return m_segs[i].data.size(); }
    const UC8* Data(size_t i) const { return &m_segs[i].data[0]; }
    UL32   Lo()    const { return m_segs.empty() ? 0 : m_segs.front().start; }
    UL32   Hi()    const {
        return m_segs.empty() ? 0 : m_segs.back().start + static_cast<UL32>(m_segs.back().data.size());
    }

    /// The documented chain: NULL = first block, previous block = next one.
    FLASHPARM* Next(FLASHPARM* prev) {
        if (m_blocks.empty()) return nullptr;
        if (!prev) return &m_blocks[0];
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            if (&m_blocks[i] == prev) {
                return (i + 1 < m_blocks.size()) ? &m_blocks[i + 1] : nullptr;
            }
        }
        return nullptr;                      // not one of ours: end the pass
    }

private:
    struct Seg { UL32 start; std::vector<UC8> data; };
    std::vector<Seg>       m_segs;
    std::vector<FLASHPARM> m_blocks;
    UL32                   m_total = 0;
};

FlashImage g_image;
bool       g_haveImage = false;

// ---------------------------------------------------------------------------
// 崩溃定位：访问违例时打出**故障模块 + 地址 + 读/写**。
// ---------------------------------------------------------------------------
// 假 BOM 一接上，驱动就多了"真的在写记录"这条路径；一旦崩，进程直接没了，
// 而最后一行输出只是主线程缓冲里恰好刷出来的内容（不是崩点）。向量异常处理器
// 比 SEH 先跑，能把 ExceptionAddress 属于哪个模块直接打出来。
LONG WINAPI VexHandler(PEXCEPTION_POINTERS ep)
{
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const void* adr = ep->ExceptionRecord->ExceptionAddress;
        char  mod[MAX_PATH] = "?";
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(adr), &m) && m) {
            GetModuleFileNameA(m, mod, MAX_PATH);
        }
        const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
        printf("\n    [CRASH] access violation: %s at %p  (target %p)  module %s\n",
               (info[0] == 0) ? "read" : (info[0] == 1) ? "write" : "execute",
               adr, reinterpret_cast<const void*>(info[1]), mod);
        printf("            thread id %lu\n", GetCurrentThreadId());
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// 假 BOM 宿主（"ioc"）—— `--bom`
// ---------------------------------------------------------------------------
// uVision 通过 AG_CB_GETBOMPTR 把 `struct bom*`（ioc）交给驱动，驱动的**所有**上报都
// 走它：PDSC 描述、Trace Data 窗口的 TDI 接口表与窗口配置、状态栏文本、VTR。
//
// 探针原来对这一项返回 0 ⇒ `TraceWinInit()` 直接失败 ⇒ **记录缓冲、区间查询、
// 时间列、逐指令记录这几条路一行都跑不到**（日志里恒 `records=0`），于是所有与
// trace *记录* 有关的验收都只能写"待真 uVision"。而 `AG_CB_GETBOMPTR` 本来就是
// 宿主给的 —— 探针完全可以自己造一个：布局就是驱动认的那份 `BOM.H`
// （`probe_bom_layout.cpp` 已经在核对它的字段偏移）。
//
// 造出来之后，`UV_TRACE_IF_INIT` / `UV_TRACE_WIN_CONFIG` / `UV_TRACE_IF_STATUS` /
// `UV_TRACE_WIN_SYNC` 全部留档，跑完再自己调 TDI 回调按周期区间取记录核对。
// 只在 `--bom` 打开时启用，默认行为不变（PDSC 与窗口照旧不可用）。
// ---------------------------------------------------------------------------
struct FakeBom {
    struct bom               bom;
    TRACE_INTERFACE_INIT_64  tdi;        // UV_TRACE_IF_INIT
    bool                     haveTdi;
    int                      tdiInits;   // 注册次数（Connect 一次 + 采集启动再发一次）
    // ⚠ UV_TRACE_WIN_CONFIG 里全是**指向驱动局部变量**的指针（`Configure()` 里的
    //    names/cols/filt/exc），调用返回后立刻悬空。所以只能在 Notify **当时**把
    //   要用的东西拷出来 —— 事后解引用读到的是已经还给栈/堆的地址（实测就是这么
    //    崩的：AV read @0x04E5F6FC）。
    bool                     haveCfg;
    int                      cfgCount;
    int                      cfgColumns;
    int                      cfgFilters;
    int                      cfgTypes;
    char                     cfgColName[TRACE_COL_MAX][32];
    char                     cfgFiltName[16][32];
    DWORD                    notifyCode[16];   // 收到过哪些 UV_* 请求、各几次
    int                      notifyCount[16];
    TRACE_STATUS             status;     // UV_TRACE_IF_STATUS
    bool                     haveStatus;
    TRACE_CLOCK_SAMPLE       clock;      // 它的 m_pClockSample
    bool                     haveClock;
    TRACE_WIN_SYNC           sync;       // UV_TRACE_WIN_SYNC
    bool                     haveSync;
    char                     statusText[256];
    int                      errMsgs;    // UV_RTAH_ERRMSG 次数（状态栏那一行）
    int                      newSettings;
    VTR                      vtr[4];
    int                      nVtr;
};

FakeBom g_fake;
bool    g_useBom = false;

DWORD FakeNotify(DWORD nCode, void* lp1)
{
    // 按请求码计数（收尾时汇总打印）：BOM 一接上，驱动就会走一堆过去根本走不到
    // 的路 —— "它到底发了哪些请求、各几次"是最有用的一条现场证据。
    {
        int i = 0;
        for (; i < 16; ++i) {
            if (g_fake.notifyCode[i] == nCode) { ++g_fake.notifyCount[i]; break; }
            if (g_fake.notifyCode[i] == 0) {
                g_fake.notifyCode[i] = nCode;
                g_fake.notifyCount[i] = 1;
                break;
            }
        }
    }

    switch (nCode) {
    case UV_TRACE_IF_INIT:
        if (lp1) {
            memcpy(&g_fake.tdi, lp1, sizeof(g_fake.tdi));
            g_fake.haveTdi = true;
            ++g_fake.tdiInits;
        }
        return 0;
    case UV_TRACE_WIN_CONFIG:
        // 只在这一刻解引用那些指针：它们指向 `Configure()` 的局部变量。
        if (lp1) {
            const TRACE_WIN_CONFIG* c = static_cast<const TRACE_WIN_CONFIG*>(lp1);
            g_fake.haveCfg = true;
            ++g_fake.cfgCount;
            if (c->m_pTraceColumnInfo) {
                const TRACE_COLUMN_INFO* ci = c->m_pTraceColumnInfo;
                g_fake.cfgColumns = (int)ci->m_nColumns;
                if (g_fake.cfgColumns > TRACE_COL_MAX) g_fake.cfgColumns = TRACE_COL_MAX;
                for (int i = 0; i < g_fake.cfgColumns && ci->m_pColumnNames; ++i) {
                    strncpy_s(g_fake.cfgColName[i], sizeof(g_fake.cfgColName[i]),
                              ci->m_pColumnNames[i] ? ci->m_pColumnNames[i] : "", _TRUNCATE);
                }
            }
            if (c->m_pDisplayFiltConf) {
                const TRACE_DISPLAY_FILTER_CONFIG* fc = c->m_pDisplayFiltConf;
                g_fake.cfgFilters = (int)fc->m_nFilters;
                if (g_fake.cfgFilters > 16) g_fake.cfgFilters = 16;
                for (int i = 0; i < g_fake.cfgFilters && fc->m_pFilters; ++i) {
                    strncpy_s(g_fake.cfgFiltName[i], sizeof(g_fake.cfgFiltName[i]),
                              fc->m_pFilters[i].m_pName ? fc->m_pFilters[i].m_pName : "",
                              _TRUNCATE);
                }
            }
            if (c->m_pTraceRecTypeNames) g_fake.cfgTypes = (int)c->m_pTraceRecTypeNames->m_nSize;
        }
        return 0;
    case UV_TRACE_IF_STATUS: {
        const TRACE_STATUS* st = static_cast<const TRACE_STATUS*>(lp1);
        if (st) {
            g_fake.status = *st;
            g_fake.haveStatus = true;
            if (st->m_pClockSample) {
                g_fake.clock = *st->m_pClockSample;
                g_fake.haveClock = true;
            }
        }
        return 0;
    }
    case UV_TRACE_WIN_SYNC:
        if (lp1) { memcpy(&g_fake.sync, lp1, sizeof(g_fake.sync)); g_fake.haveSync = true; }
        return 0;
    case UV_RTAH_ERRMSG:
        ++g_fake.errMsgs;
        if (lp1) {
            strncpy_s(g_fake.statusText, sizeof(g_fake.statusText),
                      static_cast<const char*>(lp1), _TRUNCATE);
        }
        return 0;
    case UV_NEW_TARG_SETTINGS:
        ++g_fake.newSettings;
        return 0;
    default:
        // PDSC 属性查询等：这里"没有器件描述"，驱动因此走本层兜底分支 ——
        // 这正是探针环境一直以来的行为，保持。
        return 0;
    }
}

VTR* FakeCreate(char* /*name*/, WORD16 /*nTyp*/)
{
    if (g_fake.nVtr >= (int)(sizeof(g_fake.vtr) / sizeof(g_fake.vtr[0])))
        return &g_fake.vtr[0];
    return &g_fake.vtr[g_fake.nVtr++];
}
void FakePutVal(VTR*, union v*) {}
void FakeWatch(pWATCH) {}

void FakeBomInit()
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.bom.Notify   = &FakeNotify;
    g_fake.bom.p_create = &FakeCreate;
    g_fake.bom.p_putval = &FakePutVal;
    g_fake.bom.w_create = &FakeWatch;
    // 状态栏那两条路（TraceWinStatusText / TraceWinStatusMsg）要求这两个非空，
    // 驱动只判空、不真用，所以给个非 0 的假句柄。
    g_fake.bom.hwnd     = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
    g_fake.bom.hmsg     = 1;
    g_fake.bom.pVTA     = nullptr;   // 没有 VTR 表：RegisterClocks 的清理循环会跳过
    g_fake.bom.nVT      = 0;
    g_fake.bom.bSecureExt = 0;       // v7-M：安全扩展分支不进入
    g_fake.bom.bSTrcEna   = 0;
}

// ---------------------------------------------------------------------------
// The callback uVision would provide.  Registered before AG_Init so the driver
// never reaches its MessageBoxA fallback - a modal box in a console harness is a
// hang, not a diagnostic.
// ---------------------------------------------------------------------------
U32 g_msgLines = 0;
U32 g_itmBytes = 0;          // ITM console bytes uVision would have printed

U32 Callback(U32 nCode, void* vp) {
    switch (nCode) {
    case AG_CB_MSGBOX: {
        const AG_MSGBOX* mb = static_cast<const AG_MSGBOX*>(vp);
        printf("    [box] %s: %s\n",
               (mb && mb->pCaption) ? mb->pCaption : "?",
               (mb && mb->pMsg) ? mb->pMsg : "?");
        return (mb && mb->uiDefAnswer) ? mb->uiDefAnswer : 1;   // IDOK
    }
    case AG_CB_MSGSTRING:
        ++g_msgLines;
        if (vp) printf("    [msg] %s", static_cast<const char*>(vp));
        return 0;
    case AG_CB_GETFLASHPARAM: {
        if (!g_haveImage) return 0;                  // nothing to download
        FLASHPARM* p = g_image.Next(static_cast<FLASHPARM*>(vp));
        if (!p) return 0;
        printf("    [flash] block 0x%08lX + %lu bytes (image total %lu)\n",
               p->start, p->many, p->ActSize);
        return reinterpret_cast<U32>(p);
    }
    case AG_CB_ITMDATA: {
        // uVision's ITM / printf viewer.  SerAS: n1 = stimulus port, n2 = byte
        // count, n3 = 1 for byte data, v.pU = the bytes (AGDI.H:1528).
        const SerAS* sa = static_cast<const SerAS*>(vp);
        if (!sa || !sa->v.pU || sa->n2 == 0) return 0;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(sa->v.pU);
        g_itmBytes += sa->n2;
        printf("    [itm] port %lu: ", sa->n1);
        for (DWORD i = 0; i < sa->n2; ++i) {
            putchar((p[i] >= 32 && p[i] < 127) ? static_cast<int>(p[i]) : '.');
        }
        printf("\n");
        return 0;
    }
    case AG_CB_PROGRESS:
        return 0;
    case AG_CB_FORCEUPDATE:
        return 0;
    case AG_CB_GETBOMPTR:
        // 默认返回 0：PDSC 与 Trace Data 窗口保持不可用（与过去一致）。
        // `--bom` 时交出自己造的那个 ioc，驱动就会把 TDI 表、窗口配置、状态全报上来。
        return g_useBom ? reinterpret_cast<U32>(&g_fake.bom) : 0;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// --bom 的验收：跑完之后**自己当 uVision**，按周期区间向驱动要记录
// ---------------------------------------------------------------------------
// 这条正是 uVision 的 Trace Data 窗口取数走的那条路：先问可查的周期区间
// （GetFirstCycle / GetLastCycle），再按区间取记录（GetTraceRecords64 /
// GetFiltTraceRecords64）。探针能调到它，就说明"驱动手里的记录"是可核对的，
// 不必等真 uVision。
void BomTraceChecks()
{
    Step("Fake BOM: what the driver reported through Notify");

    // 驱动发过哪些 UV_* 请求、各几次（UV_* 的名字取自 BOM.H）。
    {
        printf("          notify:");
        for (int i = 0; i < 16 && g_fake.notifyCode[i]; ++i) {
            const DWORD c = g_fake.notifyCode[i];
            const char* nm = (c == UV_TRACE_IF_INIT)    ? "UV_TRACE_IF_INIT"
                           : (c == UV_TRACE_IF_STATUS)  ? "UV_TRACE_IF_STATUS"
                           : (c == UV_TRACE_WIN_CONFIG) ? "UV_TRACE_WIN_CONFIG"
                           : (c == UV_TRACE_WIN_SYNC)   ? "UV_TRACE_WIN_SYNC"
                           : (c == UV_RTAH_ERRMSG)      ? "UV_RTAH_ERRMSG"
                           : (c == UV_NEW_TARG_SETTINGS)? "UV_NEW_TARG_SETTINGS"
                           : (c == UV_PDSCDBG_GET_PROPERTIES) ? "UV_PDSCDBG_GET_PROPERTIES"
                           : "?";
            printf(" %s(0x%X)x%d", nm, (unsigned)c, g_fake.notifyCount[i]);
        }
        printf("\n");
    }

    if (!g_fake.haveTdi) {
        Fail("no UV_TRACE_IF_INIT - the TDI table was never registered");
        return;
    }
    Note("TDI v%d registered %d time(s), unlimited=%u etb=%u nVersionUV=%d",
         (int)g_fake.tdi.nVersion, g_fake.tdiInits,
         (unsigned)g_fake.tdi.bUnlimitedTrace, (unsigned)g_fake.tdi.bEmbeddedTraceBuffer,
         (int)g_fake.tdi.nVersionUV);
    if (g_fake.tdi.pGetFirstCycle && g_fake.tdi.pGetLastCycle &&
        g_fake.tdi.pGetTraceRecords64 && g_fake.tdi.pGetFiltTraceRecords64) {
        Pass("the TDI callback table is complete (first/last/records/filt/clear)");
    } else {
        Fail("the TDI callback table is missing entries");
    }

    if (g_fake.haveCfg && g_fake.cfgColumns > 0 && g_fake.cfgFilters > 0) {
        Pass("UV_TRACE_WIN_CONFIG x%d: %d column(s), %d filter(s), %d type name(s)",
             g_fake.cfgCount, g_fake.cfgColumns, g_fake.cfgFilters, g_fake.cfgTypes);
        printf("          columns:");
        for (int i = 0; i < g_fake.cfgColumns; ++i) printf(" \"%s\"", g_fake.cfgColName[i]);
        printf("\n          filters:");
        for (int i = 0; i < g_fake.cfgFilters; ++i) printf(" \"%s\"", g_fake.cfgFiltName[i]);
        printf("\n");
    } else {
        Fail("no usable UV_TRACE_WIN_CONFIG - the window would have no columns");
    }

    if (g_fake.haveStatus) {
        Note("last UV_TRACE_IF_STATUS: processing=%u hideTS=%u explicitRange=%u "
             "clockSample=%s",
             (unsigned)g_fake.status.bProcessing, (unsigned)g_fake.status.bHideTimestamps,
             (unsigned)g_fake.status.bExplicitRange, g_fake.haveClock ? "yes" : "no");
        if (g_fake.haveClock) {
            Note("  clock sample: clock=%u Hz lastCycle=%lld lastTime=%.9f",
                 g_fake.clock.m_nClock, (long long)g_fake.clock.m_nLastCycle,
                 (double)g_fake.clock.m_dLastTime);
        }
    } else {
        Fail("no UV_TRACE_IF_STATUS - the window state never updated");
    }
    if (g_fake.haveSync) {
        Note("last UV_TRACE_WIN_SYNC: types=0x%X views=0x%X cycle=%lld",
             (unsigned)g_fake.sync.m_nTypes, (unsigned)g_fake.sync.m_nViews,
             (long long)g_fake.sync.m_nCycle);
    }
    Note("UV_RTAH_ERRMSG %d time(s), last text: \"%s\"", g_fake.errMsgs, g_fake.statusText);

    // ---- 区间查询（uVision 取数走的就是这条） -----------------------------
    static TRACE_RECORD_64 recs[512];
    UINT64 first = 0, last = 0;
    if (g_fake.tdi.pGetFirstCycle(&first) != TDI_ERR_SUCCESS) {
        Fail("GetFirstCycle did not return a first cycle");
        return;
    }
    if (g_fake.tdi.pGetLastCycle(&last) != TDI_ERR_SUCCESS) {
        Fail("GetLastCycle did not return a last cycle");
        return;
    }
    Note("cycle range [%llu, %llu] = %llu cycles",
         (unsigned long long)first, (unsigned long long)last,
         (unsigned long long)(last - first));
    if (last > first) Pass("the driver holds records over a real cycle range");
    else              Fail("degenerate cycle range (first == last)");

    UINT64 lo = first, hi = last, n = sizeof(recs) / sizeof(recs[0]);
    memset(recs, 0, sizeof(recs));
    const INT32 rc = g_fake.tdi.pGetTraceRecords64(&lo, &hi, &n, recs);
    if (rc == TDI_ERR_SUCCESS && n > 0) {
        Pass("GetTraceRecords64 -> %llu record(s)", (unsigned long long)n);
        for (unsigned i = 0; i < n && i < 5; ++i) {
            Note("  #%u type=0x%02X bTS=%u bPC=%u exec=%u cycle=%llu PC=0x%08X",
                 i, recs[i].nType, recs[i].bTS, recs[i].bPC, recs[i].bExec,
                 (unsigned long long)recs[i].nCycle, recs[i].nPC);
        }
    } else {
        Fail("GetTraceRecords64 = %d, count=%llu", (int)rc, (unsigned long long)n);
    }

    // ---- 过滤器（uVision 的 "ETM - Code Exec" 一档） ---------------------
    {
        TRACE_FILTER_CONF f;
        memset(&f, 0, sizeof(f));
        f.nTypeMask    = TR_TYPE_ETM | TR_TYPE_ETM_HLL;
        f.nExtTypeMask = 0x1;
        UINT64 ofs = 0, n2 = sizeof(recs) / sizeof(recs[0]);
        lo = first; hi = last;
        memset(recs, 0, sizeof(recs));
        const INT32 rc2 = g_fake.tdi.pGetFiltTraceRecords64(&lo, &hi, &n2, &ofs, &f, recs);
        Note("GetFiltTraceRecords64(ETM) -> rc=%d count=%llu", (int)rc2,
             (unsigned long long)n2);
        if (rc2 == TDI_ERR_SUCCESS && n2 > 0) {
            Pass("the filtered query returns ETM records (the window's filter works)");
        } else {
            Note("no ETM record came back - check whether the ETM stream produced any");
        }
    }

    // ---- P1-1：块链的容量 + 按周期定位 -----------------------------------
    //
    // 单块环（131,072 条）实测只装得下 1,549,863 个周期 ≈ 48 MHz 下 10 ms。
    // 块链之后要能看出两件事：
    //   ① 常驻记录条数**超过**单块环的上限 —— 说明第二块起真的挂上了，而且区间
    //      遍历确实走过了所有块（这个数就是遍历的命中总数，不是记账值）；
    //   ② 从区间**中部**起查，返回的第一条记录就在中部附近 —— 这条测的是块索引
    //      二分有没有真的跳到那一块。坏了的话它会从头返回，cycle 差一大截。
    //
    // ① 的对照值在驱动日志的 `TraceWin: buf(...) 常驻记录=` 里（两条独立路径：
    // 驱动自己的记账 vs 这里的遍历结果，应当相等）。
    {
        UINT64 cnt = 0, lo2 = first, hi2 = last;
        const INT32 rc = g_fake.tdi.pGetTraceRecords64(&lo2, &hi2, &cnt, nullptr);
        Note("P1-1 容量：常驻 %llu 条记录 / 区间 %llu 周期（单块环上限 131072 条）",
             (unsigned long long)cnt, (unsigned long long)(last - first));
        if (rc == TDI_ERR_SUCCESS && cnt > 131072) {
            Pass("常驻记录已超过单块环上限 —— 块链生效，区间遍历跨过了多块");
        } else if (rc == TDI_ERR_SUCCESS) {
            Note("数据量还不足以填满一块（4 MB / 190650 条）—— 块链的换块路径没被走到");
        } else {
            Fail("count-only 查询 rc=%d（pTrace64 = NULL 应当只回条数）", (int)rc);
        }

        // 子区间计数：从中间切一半，命中数应当在总数的一半量级（差 4 倍就算异常）。
        if (rc == TDI_ERR_SUCCESS && cnt > 0) {
            UINT64 lo3 = first + (last - first) / 2, hi3 = last, half = 0;
            const INT32 rc3 = g_fake.tdi.pGetTraceRecords64(&lo3, &hi3, &half, nullptr);
            Note("P1-1 子区间 [%llu, %llu] -> %llu 条（总数的一半 = %llu）",
                 (unsigned long long)lo3, (unsigned long long)hi3,
                 (unsigned long long)half, (unsigned long long)(cnt / 2));
            if (rc3 == TDI_ERR_SUCCESS && half > cnt / 8 && half <= cnt)
                Pass("后半段计数与总数自洽（起点确实定位到了中段那块）");
            else
                Fail("后半段计数 %llu 与总数 %llu 不自洽（块索引定位有问题？）",
                     (unsigned long long)half, (unsigned long long)cnt);
        }

        // 结构自洽：把 [first, last] 切成四段分别计数，四段之和必须**正好等于**
        // 全区间计数。这条不需要知道块有多大：遍历只要在任何一段里提前停下
        // （比如停在块边界），和就会小于总数。边界用 b+1 切，保证每条记录只落
        // 在一段里（周期相同的多条记录也各自归属唯一）。
        if (rc == TDI_ERR_SUCCESS && cnt > 0 && (last - first) >= 4) {
            const UINT64 q  = (last - first) / 4;
            const UINT64 b1 = first + q, b2 = b1 + q, b3 = b2 + q;
            const UINT64 seg[4][2] = {
                { first, b1 }, { b1 + 1, b2 }, { b2 + 1, b3 }, { b3 + 1, last }
            };
            UINT64 sum = 0;
            int    bad = -1;
            for (int i = 0; i < 4; ++i) {
                UINT64 lo5 = seg[i][0], hi5 = seg[i][1], n5 = 0;
                const INT32 r5 = g_fake.tdi.pGetTraceRecords64(&lo5, &hi5, &n5, nullptr);
                // ⚠ 某一段**本来就可能一条记录都没有**（周期轴是稀疏的，尤其 ETM 的
                //   真实周期会跳变），那时驱动按既有语义回 `TDI_ERR_BOUNDS` —— 那是
                //   "这一段没有记录"，不是失败。只有别的返回码才算异常。
                if (r5 == TDI_ERR_SUCCESS)     { sum += n5; }
                else if (r5 == TDI_ERR_BOUNDS || r5 == TDI_ERR_EMPTY) { /* 0 条 */ }
                else { bad = i; break; }
            }
            Note("P1-1 分段：四段计数之和 = %llu（全区间 = %llu）",
                 (unsigned long long)sum, (unsigned long long)cnt);
            if (bad >= 0) {
                Fail("第 %d 段计数查询返回了意外的码 —— 段边界落在块链的边界上？", bad + 1);
            } else if (sum == cnt) {
                Pass("四段计数之和与全区间相等 —— 遍历没有在块边界提前停下");
            } else {
                Fail("四段之和 %llu != 全区间 %llu（少了 %lld 条）—— 遍历在某段提前停",
                     (unsigned long long)sum, (unsigned long long)cnt,
                     (long long)((int64_t)cnt - (int64_t)sum));
            }
        }

        // 定位：从中部起取一小批，首条必须是"缓冲里第一条周期 >= 中部的记录"。
        //
        // ⚠ 判据不能写成"首条落在中部附近"：周期轴是**稀疏**的（尤其 ETM 的真实周期
        //   会跳变），中部与"下一条记录"之间可能隔着很远 —— 那时"离得远"是**正确**
        //   结果。所以：缓冲能一次取完（≤512 条）时用**精确**判据（把它取回来，验证
        //   首条就是第一条 >= mid 的记录）；取不完时说明记录够密，才用容差。
        {
            const UINT64 mid = first + (last - first) / 2;
            UINT64 lo4 = mid, hi4 = last, n4 = sizeof(recs) / sizeof(recs[0]);
            memset(recs, 0, sizeof(recs));
            const INT32 rc4 = g_fake.tdi.pGetTraceRecords64(&lo4, &hi4, &n4, recs);
            if (rc4 == TDI_ERR_SUCCESS && n4 > 0) {
                const UINT64 c0 = recs[0].nCycle;
                bool ok = (c0 >= mid);
                if (ok && cnt <= 512) {
                    // 精确：整个缓冲取回来，找第一条 >= mid 的
                    static TRACE_RECORD_64 recsE[512];
                    UINT64 loE = first, hiE = last, nE = 512;
                    memset(recsE, 0, sizeof(recsE));
                    if (g_fake.tdi.pGetTraceRecords64(&loE, &hiE, &nE, recsE) == TDI_ERR_SUCCESS) {
                        UINT64 expect = 0;
                        bool   have   = false;
                        for (unsigned i = 0; i < nE; ++i) {
                            if (recsE[i].nCycle >= mid) { expect = recsE[i].nCycle; have = true; break; }
                        }
                        ok = have && (expect == c0);
                    }
                } else if (ok) {
                    ok = (c0 <= mid + (last - first) / 4 + 1024);
                }
                Note("P1-1 定位：请求起点 %llu -> 首条 cycle=%llu（偏移 %lld，缓冲 %llu 条）",
                     (unsigned long long)mid, (unsigned long long)c0,
                     (long long)((int64_t)c0 - (int64_t)mid), (unsigned long long)cnt);
                if (ok) {
                    Pass("按周期定位命中了正确的块（返回的是第一条 >= 起点的记录）");
                } else {
                    Fail("首条记录 cycle=%llu 不是第一条 >= %llu 的记录 —— 块索引定位有问题",
                         (unsigned long long)c0, (unsigned long long)mid);
                }
            } else {
                Fail("mid-range 查询 rc=%d count=%llu", (int)rc4, (unsigned long long)n4);
            }
        }
    }

    // ---- P1-2：紧凑记录（字节流 + varint 周期差）解回来对不对 --------------
    //
    // P1-1 的判据管"能装多少"，这一组管"还原得对不对"。周期号**不存绝对值**，
    // 存的是相对上一条的差（varint），查询时从块的 firstCycle 累加 —— 所以
    // "末尾对得上"是最强的判据：中间任何一处差解错，末尾就会偏。
    {
        UINT64 total = 0, loA = first, hiA = last;
        const INT32 rcA = g_fake.tdi.pGetTraceRecords64(&loA, &hiA, &total, nullptr);

        // ① 同一区间查两次必须**逐字节一致**（判据原文来自 Todo.md §四 P1-2）。
        {
            static TRACE_RECORD_64 recsB[512];
            UINT64 lo1 = first, hi1 = last, n1 = sizeof(recs) / sizeof(recs[0]);
            UINT64 lo2 = first, hi2 = last, n2 = sizeof(recsB) / sizeof(recsB[0]);
            memset(recs,  0, sizeof(recs));
            memset(recsB, 0, sizeof(recsB));
            const INT32 r1 = g_fake.tdi.pGetTraceRecords64(&lo1, &hi1, &n1, recs);
            const INT32 r2 = g_fake.tdi.pGetTraceRecords64(&lo2, &hi2, &n2, recsB);
            if (r1 == TDI_ERR_SUCCESS && r2 == TDI_ERR_SUCCESS && n1 == n2 && n1 > 0 &&
                memcmp(recs, recsB, (size_t)n1 * sizeof(TRACE_RECORD_64)) == 0) {
                Pass("同一区间查两次逐字节一致（%llu 条 × %u B）",
                     (unsigned long long)n1, (unsigned)sizeof(TRACE_RECORD_64));
            } else {
                Fail("两次查询不一致：rc=%d/%d count=%llu/%llu",
                     (int)r1, (int)r2, (unsigned long long)n1, (unsigned long long)n2);
            }
        }

        // ② 头尾两条必须**正好**落在 first / last 上。
        //    尾部那条走 `nRecordOfs` 跳到末尾取 —— 顺带把 TDI 的"区间内偏移"这条
        //    参数也测了（此前没有任何用例碰过它）。
        if (rcA == TDI_ERR_SUCCESS && total > 0) {
            UINT64 lo3 = first, hi3 = last, n3 = sizeof(recs) / sizeof(recs[0]), ofs3 = 0;
            memset(recs, 0, sizeof(recs));
            const INT32 rc3 = g_fake.tdi.pGetFiltTraceRecords64(&lo3, &hi3, &n3, &ofs3,
                                                               nullptr, recs);
            if (rc3 == TDI_ERR_SUCCESS && n3 > 0 && recs[0].nCycle == first) {
                Pass("区间起点首条 cycle=%llu == first（块头累加起点正确）",
                     (unsigned long long)recs[0].nCycle);
            } else {
                Fail("起点首条 cycle=%llu != first=%llu（rc=%d n=%llu）",
                     (unsigned long long)(n3 ? recs[0].nCycle : 0),
                     (unsigned long long)first, (int)rc3, (unsigned long long)n3);
            }

            const UINT64 ofsT = (total > 512) ? (total - 512) : 0;
            UINT64 lo4 = first, hi4 = last, n4 = sizeof(recs) / sizeof(recs[0]), ofs4 = ofsT;
            memset(recs, 0, sizeof(recs));
            const INT32 rc4 = g_fake.tdi.pGetFiltTraceRecords64(&lo4, &hi4, &n4, &ofs4,
                                                               nullptr, recs);
            if (rc4 == TDI_ERR_SUCCESS && n4 > 0) {
                const UINT64 cl = recs[n4 - 1].nCycle;
                Note("P1-2 还原：nRecordOfs=%llu 取到 %llu 条，末条 cycle=%llu（last=%llu）",
                     (unsigned long long)ofsT, (unsigned long long)n4,
                     (unsigned long long)cl, (unsigned long long)last);
                if (cl == last) {
                    Pass("末条 cycle 正好等于 last —— varint 差累加没有跑偏");
                } else {
                    Fail("末条 cycle=%llu != last=%llu（差 %lld）—— 周期差累加有偏差",
                         (unsigned long long)cl, (unsigned long long)last,
                         (long long)((int64_t)cl - (int64_t)last));
                }
                // 窗口内必须单调不减（差是累加的，解错会出现回退）。
                bool mono = true;
                for (unsigned i = 1; i < n4; ++i) {
                    if (recs[i].nCycle < recs[i - 1].nCycle) { mono = false; break; }
                }
                if (mono) Pass("取回的 %llu 条周期号单调不减", (unsigned long long)n4);
                else      Fail("取回的记录里周期号出现回退 —— 解码越界或差读错");
            } else {
                Fail("末尾窗口查询 rc=%d count=%llu（nRecordOfs=%llu）",
                     (int)rc4, (unsigned long long)n4, (unsigned long long)ofsT);
            }
        }
    }

    // ---- P2-2：内存窗口**之前**的区间要从记录日志取回来 --------------------
    //
    // 内存块链只装得下秒级窗口（Todo.md §二）。用 `trrec=` 打开记录日志、再把内存
    // 上限压到 2 MB（每块 1 MB）之后，块会被反复退休 ⇒ 记录日志里是"内存之前
    // 的整段历史"。判据：
    //   ① `GetFirstCycle` 报的**最早可查周期号**（= 记录日志的第一条索引）必须
    //      真的能取到记录 —— 那一条在内存里根本不存在；
    //   ② 全区间计数必须**超过内存上限能装下的条数**（2 MB / 最小记录 5 B ≈ 42 万），
    //      否则说明只读了内存、文件那一段没接上。
    //   数据量不足时 ② 退化成 Note（和 P1-1 的容量判据同一种写法）。
    {
        UINT64 f0 = 0;
        const INT32 rc0 = g_fake.tdi.pGetFirstCycle(&f0);
        if (rc0 == TDI_ERR_SUCCESS) {
            UINT64 lo0 = f0, hi0 = f0, n0 = sizeof(recs) / sizeof(recs[0]);
            memset(recs, 0, sizeof(recs));
            const INT32 rc1 = g_fake.tdi.pGetTraceRecords64(&lo0, &hi0, &n0, recs);
            if (rc1 == TDI_ERR_SUCCESS && n0 > 0 && recs[0].nCycle == f0) {
                Pass("最早可查周期号 %llu 取到了记录（内存窗口之外的区间也能取）",
                     (unsigned long long)f0);
            } else {
                Fail("最早可查周期号 %llu 取不到记录：rc=%d n=%llu cycle=%llu",
                     (unsigned long long)f0, (int)rc1, (unsigned long long)n0,
                     (unsigned long long)(n0 ? recs[0].nCycle : 0));
            }
        } else {
            Note("GetFirstCycle -> %d（没有记录日志，或这一段没有记录）", (int)rc0);
        }

        UINT64 lo1 = first, hi1 = last, all = 0;
        const INT32 rc2 = g_fake.tdi.pGetTraceRecords64(&lo1, &hi1, &all, nullptr);
        // 内存上限 2 MB（trchunk=1 trmem=2 的两块）能装下的**上界**：2 MB / 5 B。
        const UINT64 memBound = 419430ull;
        if (rc2 == TDI_ERR_SUCCESS && all > memBound) {
            Pass("全区间计数 %llu > 内存上限能装的 %llu —— 文件那一段确实接上了",
                 (unsigned long long)all, (unsigned long long)memBound);
        } else if (rc2 == TDI_ERR_SUCCESS) {
            Note("全区间计数 %llu（内存上限约能装 %llu）—— 数据量不足以判这条",
                 (unsigned long long)all, (unsigned long long)memBound);
        } else {
            Fail("全区间计数查询 rc=%d", (int)rc2);
        }
    }

    // ---- P1-0：全指令 trace（靠代码镜像补直接分支） ------------------------
    //
    // 固件 ETMCR bit8 = 0 之后，ETM **只报间接分支**（BX / LDR PC / POP{PC}…），
    // 直接分支（B / BL / CBZ）要靠**代码镜像**补出来（见 ORBAGDI_CodeImage.h）。
    // 所以"到底有没有全指令"在窗口侧等价于一件事：**记录里出现连续地址**。
    //
    // 没补之前，循环体里同一个分支被反复执行 ⇒ 相邻记录是同一个地址（差 0）；
    // 补上之后应看到大量 addr 差 2/4 的相邻对（Thumb 指令长度）。
    //
    // ⚠ 这一条在**没下载**的会话里也应当成立：来源 ② 现读目标内存（抽数 worker
    //   线程上已经持有探针锁，所以 `ReadTargetMem` 直接读，见 ORBAGDI_Etm.cpp）。
    {
        static TRACE_RECORD_64 rr[512];
        UINT64 lo = first, hi = last, n = sizeof(rr) / sizeof(rr[0]);
        memset(rr, 0, sizeof(rr));
        const INT32 rc = g_fake.tdi.pGetTraceRecords64(&lo, &hi, &n, rr);

        if (rc == TDI_ERR_SUCCESS && n >= 2) {
            unsigned consec = 0, same = 0, other = 0, exec = 0;
            for (unsigned i = 0; i + 1 < n; ++i) {
                const long long d = (long long)rr[i + 1].nPC - (long long)rr[i].nPC;
                if (d == 2 || d == 4) ++consec;
                else if (d == 0)      ++same;
                else                  ++other;
            }
            for (unsigned i = 0; i < n; ++i) {
                if (rr[i].bExec) ++exec;
            }

            Note("P1-0 全指令：%u 条记录里 连续地址=%u 同地址=%u 其它=%u（bExec=%u）",
                 (unsigned)n, consec, same, other, exec);

            if (consec > 0) {
                Pass("记录里有 %u 对连续地址 —— 代码镜像把直接分支之间的指令补上了",
                     consec);
            } else {
                // ⚠ **这一条是 Note，不是 Fail**：P1-0 还没做完（缺返回栈 / 控制流跟随，
                //   见 Todo.md 的 P1-0 行），而且兜底现读目标内存**默认关闭**（2026-10-13
                //   真机实测：worker 上一次越界读就打挂整条 DAP 链路）⇒ 没下载过的会话
                //   本来就补不出。已知未完成项不该让整套回归变红。
                Note("P1-0 未达标：连续地址 0 / 同地址 %u —— 代码镜像没生效（缺返回栈，"
                     "且默认只认下载镜像；见 Todo.md 的 P1-0 行）", same);
            }
        } else {
            Note("P1-0 全指令：取不到记录（rc=%d n=%llu）",
                 (int)rc, (unsigned long long)n);
        }
    }

    // ---- ClearTrace：清空后不能还留着旧记录 -------------------------------
    //
    // ⚠ 不能要求"一定 EMPTY"：停止之后抽数 worker 还会把 RDDI 环里的积压排空
    // （实测停止后又进了几千条），所以清空之后可能立刻又有新记录进来 —— 那是
    // **正确**行为。而且清空会把周期轴一起归零（新一段采集），所以新记录的周期号
    // 从 0 附近重新开始，**不是**"比旧 last 更大"。
    // 判据因此写成：要么 EMPTY，要么新首周期落在**旧区间之外**（旧记录确实没了）。
    if (g_fake.tdi.pClearTrace) {
        const INT32 cc = g_fake.tdi.pClearTrace();
        UINT64 f2 = 0;
        const INT32 g2 = g_fake.tdi.pGetFirstCycle(&f2);
        if (cc == TDI_ERR_SUCCESS && g2 == TDI_ERR_EMPTY) {
            Pass("ClearTrace emptied the buffer (GetFirstCycle -> TDI_ERR_EMPTY)");
        } else if (cc == TDI_ERR_SUCCESS && g2 == TDI_ERR_SUCCESS &&
                   (f2 < first || f2 > last)) {
            Pass("ClearTrace 清掉了旧记录：新首周期 %llu 在旧区间 [%llu, %llu] 之外"
                 "（worker 还在排空，进来的是清空之后的新记录；周期轴已归零）",
                 (unsigned long long)f2, (unsigned long long)first,
                 (unsigned long long)last);
        } else {
            Fail("ClearTrace rc=%d, 之后 GetFirstCycle rc=%d cycle=%llu（旧区间 [%llu, %llu]）",
                 (int)cc, (int)g2, (unsigned long long)f2,
                 (unsigned long long)first, (unsigned long long)last);
        }
    }
}

// ---------------------------------------------------------------------------
// Running the target for a bounded time.
//
// AG_GOFORBRK blocks until the target stops, and the trace pumps (ItmPump /
// OrbTracePump) only run inside that loop - so this is the only way trace data
// is ever collected.  uVision does exactly this: AG_GoStep(AG_GOFORBRK) on its
// own thread and AG_GoStep(AG_STOPRUN) when the user presses Stop.
//
// The stop has to arrive *after* RunAndWait has cleared stopReq at its entry,
// which is why the caller sleeps before posting it.
// ---------------------------------------------------------------------------
struct RunResult { U32 pc; U32 rc; };

DWORD WINAPI RunThread(LPVOID param) {
    RunResult* r = static_cast<RunResult*>(param);
    GADR a; a.Adr = 0; a.ErrAdr = 0;
    r->rc = g_api.AG_GoStep(AG_GOFORBRK, 0, &a);
    r->pc = a.Adr;
    return 0;
}

// ---------------------------------------------------------------------------
// P0-A′ 的验收：锁争用下 uVision 的那些入口还成不成功
// ---------------------------------------------------------------------------
// 判据（Todo.md 四、方案 P0-A′）：**有 trace 源在跑时，寄存器/内存窗口不出现间歇性
// `AG_NOACCESS`**。实测方法是把争用造出来再数失败 —— 探针原来只在运行**之前/之后**
// 调这些入口，恰好避开了 worker 抓锁的时刻，所以"间歇失败"一直只是纸面推测。
//
// 选谁当探针：
//   * `AG_DapAcc(ACCDAP_ABORT)` —— **没有 `c.running` 守卫**，所以运行期间也能调；
//     拿不到锁时它返回 `ACCDAP_FAILED`（这正是 P0-A′ 要消掉的那个码）。
//   * `AG_AllReg(AG_READ)` —— 寄存器窗口的入口；运行中它按设计返回 `AG_NOACCESS`
//     （`c.running` 守卫），所以只能在**目标已停、trace 源仍开着**时打 ——
//     而那种状态下 worker 仍在排空缓冲，正是"间歇 AG_NOACCESS"的现场。
struct PollStats {
    volatile LONG stop;
    volatile LONG dapCalls, dapFail;      // 运行期间
    volatile LONG allRegCalls, allRegFail; // 停机之后
};

DWORD WINAPI PollThread(LPVOID param) {
    PollStats* s = static_cast<PollStats*>(param);
    while (!s->stop) {
        ACCDAP in;
        memset(&in, 0, sizeof(in));
        in.accCmd   = ACCDAP_ABORT;
        U32 v       = 0x0000001Eu;
        in.pOut32   = &v;
        in.nManyOut = 4;
        InterlockedIncrement(&s->dapCalls);
        if (g_api.AG_DapAcc(&in) != ACCDAP_DONE) InterlockedIncrement(&s->dapFail);
        Sleep(1);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const char* DllPathForExe() {
    static char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';                     // ...\test\bin
    _snprintf_s(path + strlen(path), MAX_PATH - strlen(path), _TRUNCATE,
                "\\..\\..\\bin\\ORB_AGDI.dll");
    return path;
}

bool AllSame(const unsigned char* p, size_t n, unsigned char v) {
    for (size_t i = 0; i < n; ++i) if (p[i] != v) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Repeatability of the MEM-AP read path.
//
// A read that does not repeat says the transport is broken; a read that repeats
// but differs from the file says the memory really holds something else.  The
// whole-image verify in --flash can only ever look at ONE region - the flash the
// CPU itself runs from - so this takes the region as a parameter and can be run
// on several (COMPAT_ANALYSIS.md §10.33).
//
// It is called twice on purpose: once with the target in its reset state and
// once after the application has been allowed to run.  If the two calls
// disagree, the target's state is what breaks the reads.
// ---------------------------------------------------------------------------
void ReadCompare(unsigned adr, unsigned len, int passes) {
    Step("Read repeatability: 0x%08X + %u bytes, %d pass(es)", adr, len, passes);
    std::vector<UC8> first(len, 0);
    for (int pass = 1; pass <= passes; ++pass) {
        std::vector<UC8> rd(len, 0);
        GADR a; a.Adr = adr; a.ErrAdr = 0;
        const U32 rc = g_api.AG_MemAcc(AG_READ, &rd[0], &a, len);
        if (rc != AG_OK) { Fail("pass %d: AG_MemAcc = %u", pass, rc); break; }
        if (pass == 1) {
            first = rd;
            Note("pass 1: read %u bytes", len);
            continue;
        }
        size_t bad = 0;
        UL32 firstBad = 0;
        unsigned shown = 0;
        for (size_t k = 0; k < len; ++k) {
            if (rd[k] != first[k]) {
                ++bad;
                if (!firstBad) firstBad = adr + static_cast<UL32>(k);
                if (shown < 40) {
                    Note("    0x%08X  +%-5u  pass1=%02X  pass%d=%02X",
                         adr + static_cast<UL32>(k), static_cast<unsigned>(k),
                         first[k], pass, rd[k]);
                    ++shown;
                }
            }
        }
        if (bad == 0) Pass("pass %d: identical to pass 1", pass);
        else          Fail("pass %d: %u byte(s) differ from pass 1 (first at 0x%08X)",
                          pass, static_cast<unsigned>(bad), firstBad);
    }

    // If an image was given, also say whether the read agrees with the FILE.
    // That is the difference between "the transport returned something else" and
    // "the flash really holds something else" - and after a download that
    // reported success it is the only way to tell the two apart.
    if (g_image.Count() > 0) {
        for (size_t i = 0; i < g_image.Count(); ++i) {
            const UL32 bs = g_image.Start(i);
            const size_t bl = g_image.Len(i);
            if (adr < bs || adr >= bs + bl) continue;
            size_t n = bl - (adr - bs);
            if (n > len) n = len;
            size_t bad = 0;
            UL32 firstBad = 0;
            unsigned shown = 0;
            for (size_t k = 0; k < n; ++k) {
                const UC8 want = g_image.Data(i)[(adr - bs) + k];
                if (first[k] != want) {
                    ++bad;
                    if (!firstBad) firstBad = adr + static_cast<UL32>(k);
                    if (shown < 40) {
                        Note("    0x%08X  +%-5u  file=%02X  read=%02X",
                             adr + static_cast<UL32>(k), static_cast<unsigned>(k),
                             want, first[k]);
                        ++shown;
                    }
                }
            }
            if (bad == 0) {
                Pass("pass 1 matches the file over %u byte(s)", static_cast<unsigned>(n));
            } else {
                Fail("pass 1 differs from the file in %u of %u byte(s) (first at 0x%08X)",
                     static_cast<unsigned>(bad), static_cast<unsigned>(n), firstBad);
            }
            break;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, &VexHandler);

    const char* dll     = nullptr;
    const char* options = "clock=1000 iface=0";
    const char* device  = nullptr;
    const char* flashHex = nullptr;
    const char* flmPath  = nullptr;
    const char* ramSpec  = nullptr;
    unsigned    readAdr = 0x08000000u;
    unsigned    readCmpAdr = 0;     // --readcmp: region for the repeatability test
    unsigned    readCmpLen = 0;
    int         readCmpPasses = 3;
    unsigned    wrAdr   = 0;
    unsigned    pokeAdr = 0;
    unsigned    pokeVal = 0;
    unsigned    runMs   = 0;
    unsigned    pollMs  = 0;        // --poll: 运行期间狂打"拿不到锁就失败"的入口（P0-A′）
    bool        useBom  = false;    // --bom: 自己当宿主交出 ioc，验收记录/区间查询
    bool        keep    = false;
    bool        haltAtEnd = false;
    bool        resetTarget = false; // --reset: 连上后发 AG_RESET（实现见 ResetTarget()）
    bool        etmTest = false;    // --etm-test: ETMCR 编程序列 + bit8 对照
    int         etmBit8 = -1;       // --etm-bit8 0|1（-1 = 不动）
    int         etmBit12 = -1;      // --etm-bit12 0|1（周期精确 trace；-1 = 不动）
    int         etmTs = -1;         // --etm-ts 0|1（ETMCR[28] 时间戳使能；-1 = 不动）
    bool        noDownload = false; // --image: load the image, do NOT program it

    // Trace-source parameters, one per driver key (see the usage block).
    // -1 / false means "not given": the option string is only touched when the
    // caller actually asked for something, so a plain run stays byte-identical
    // to the default "clock=1000 iface=0".
    bool        orbtrace = false;
    int         trWidth  = -1;      // --trwidth 1|2|4
    int         trSwo    = -1;      // --trswo   0=parallel 1=NRZ 2=Manchester
    int         trBaud   = -1;      // --trbaud  Hz
    int         trTpui   = -1;      // --trtpui  0|1
    int         cpuKhz   = -1;      // --cpu     kHz
    int         tclkHz   = -1;      // --tclk    kHz

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc)          dll = argv[++i];
        else if (!strcmp(argv[i], "--options") && i + 1 < argc) options = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)  device = argv[++i];
        else if (!strcmp(argv[i], "--read") && i + 1 < argc)    readAdr = strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--readcmp") && i + 1 < argc) {
            const char* s = argv[++i];
            readCmpAdr = strtoul(s, nullptr, 0);
            const char* c = strchr(s, ',');
            if (c) {
                readCmpLen = strtoul(c + 1, nullptr, 0);
                const char* c2 = strchr(c + 1, ',');
                if (c2) readCmpPasses = static_cast<int>(strtol(c2 + 1, nullptr, 0));
            }
        }
        else if (!strcmp(argv[i], "--memwrite") && i + 1 < argc) wrAdr = strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc)   flashHex = argv[++i];
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) {
            flashHex = argv[++i];      // parse it so reads can be compared with it,
            noDownload = true;         // but never program the target
        }
        else if (!strcmp(argv[i], "--flm") && i + 1 < argc)     flmPath  = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc)     ramSpec  = argv[++i];
        else if (!strcmp(argv[i], "--poke") && i + 2 < argc) {
            pokeAdr = strtoul(argv[++i], nullptr, 0);
            pokeVal = strtoul(argv[++i], nullptr, 0);
        }
        else if (!strcmp(argv[i], "--run") && i + 1 < argc)     runMs = strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--poll") && i + 1 < argc)    pollMs = strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--bom"))                     useBom = true;
        else if (!strcmp(argv[i], "--keep"))                    keep = true;
        else if (!strcmp(argv[i], "--halt"))                    haltAtEnd = true;
        else if (!strcmp(argv[i], "--reset"))                   resetTarget = true;
        else if (!strcmp(argv[i], "--etm-test"))                etmTest = true;
        else if (!strcmp(argv[i], "--etm-bit8") && i + 1 < argc) etmBit8 = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--etm-bit12") && i + 1 < argc) etmBit12 = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--etm-ts") && i + 1 < argc) etmTs = (int)strtol(argv[++i], nullptr, 0);
        // ---- trace source ----
        else if (!strcmp(argv[i], "--orbtrace"))                orbtrace = true;
        else if (!strcmp(argv[i], "--trwidth") && i + 1 < argc) trWidth = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--trswo")   && i + 1 < argc) trSwo   = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--trbaud")  && i + 1 < argc) trBaud  = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--trtpui")  && i + 1 < argc) trTpui  = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--cpu")     && i + 1 < argc) cpuKhz  = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--tclk")    && i + 1 < argc) tclkHz  = (int)strtol(argv[++i], nullptr, 0);
        else { printf("unknown argument: %s\n", argv[i]); return 2; }
    }
    if (!dll) dll = DllPathForExe();

    // flm= and ram= belong in the option string - that is where the driver reads
    // them, and where the Settings sheet writes them.
    static char optionsBuf[1024];
    strncpy_s(optionsBuf, options, _TRUNCATE);

    // --bom: 从这一刻起由本进程扮演宿主的 ioc 提供者（要在 AG_INITCALLBACK 之前
    // 就绪，驱动拿到回调后立刻就会来要 BOM）。
    g_useBom = useBom;
    if (useBom) FakeBomInit();
    if (flmPath) {
        strncat_s(optionsBuf, " flm=\"", _TRUNCATE);
        strncat_s(optionsBuf, flmPath, _TRUNCATE);
        strncat_s(optionsBuf, "\"", _TRUNCATE);
    }
    if (ramSpec) {
        strncat_s(optionsBuf, " ram=", _TRUNCATE);
        strncat_s(optionsBuf, ramSpec, _TRUNCATE);
    }

    // ---- trace source -------------------------------------------------------
    // One flag -> one driver key, in the same shape the Settings sheet writes.
    // Only what was actually given is emitted, so a run with no trace flags
    // still produces exactly the option string the caller passed.
    {
        char piece[48];
        auto addInt = [&](const char* key, int v) {
            _snprintf_s(piece, sizeof(piece), _TRUNCATE, " %s=%d", key, v);
            strncat_s(optionsBuf, piece, _TRUNCATE);
        };
        if (orbtrace)      strncat_s(optionsBuf, " orbtrace=1", _TRUNCATE);
        if (trWidth >= 0)  addInt("trwidth", trWidth);
        if (trSwo   >= 0)  addInt("trswo",   trSwo);
        if (trBaud  >= 0)  addInt("trbaud",  trBaud);
        if (trTpui  >= 0)  addInt("trtpui",  trTpui);
        if (cpuKhz  >= 0)  addInt("cpu",     cpuKhz);
        if (tclkHz  >= 0)  addInt("tclk",    tclkHz);
    }

    // A flash run needs two switches that are off by default, and both are
    // uVision's to set - the host resets the target before a download, and the
    // "Download to Flash" box decides flash or RAM:
    //
    //   dlflash=1  without it the driver writes the image to RAM instead and
    //              reports success, leaving the flash exactly as it was
    //   rstcon=1   the algorithm's first act is a flash-register write, and on a
    //              target left in its application's power state that store bus
    //              faults before Init() can return
    //
    // They are appended only when absent, so a caller that wants the RAM path
    // (dlflash=0) or no reset still gets it.
    if (flashHex) {
        if (!strstr(optionsBuf, "dlflash")) strncat_s(optionsBuf, " dlflash=1", _TRUNCATE);
        if (!strstr(optionsBuf, "rstcon"))  strncat_s(optionsBuf, " rstcon=1",  _TRUNCATE);
    }
    options = optionsBuf;

    if (flashHex) {
        g_haveImage = g_image.Load(flashHex);
        if (!g_haveImage) { printf("could not read the image: %s\n", flashHex); return 2; }
    }

    printf("ORBAGDI AGDI probe\n");
    printf("    driver  : %s\n", dll);
    printf("    options : %s\n", options);
    if (wrAdr) printf("    memwrite: 0x%08X\n", wrAdr);
    if (g_haveImage) {
        printf("    image   : %s - %u block(s), 0x%08X..0x%08X, %lu bytes\n",
               flashHex, static_cast<unsigned>(g_image.Count()),
               g_image.Lo(), g_image.Hi(), g_image.Total());
    }
    printf("\n");

    // -----------------------------------------------------------------------
    Step("Load the built DLL and resolve the AGDI entry points");
    g_mod = LoadLibraryA(dll);
    if (!g_mod) {
        Fail("LoadLibrary failed, GetLastError=%lu", GetLastError());
        printf("\nFAILED  (1 problem)\n");
        return 1;
    }
    Pass("loaded %s", dll);

    struct { const char* name; void** slot; bool required; } syms[] = {
        { "EnumUvARM7",        reinterpret_cast<void**>(&g_api.EnumUvARM7),       true  },
        { "DllUv3Cap",         reinterpret_cast<void**>(&g_api.DllUv3Cap),        true  },
        { "AG_Init",           reinterpret_cast<void**>(&g_api.AG_Init),          true  },
        { "AG_GoStep",         reinterpret_cast<void**>(&g_api.AG_GoStep),        true  },
        { "AG_MemAcc",         reinterpret_cast<void**>(&g_api.AG_MemAcc),        true  },
        { "AG_MemAccX",        reinterpret_cast<void**>(&g_api.AG_MemAccX),       true  },
        { "AG_RegAcc",         reinterpret_cast<void**>(&g_api.AG_RegAcc),        true  },
        { "AG_AllReg",         reinterpret_cast<void**>(&g_api.AG_AllReg),        true  },
        { "AG_Serial",         reinterpret_cast<void**>(&g_api.AG_Serial),        true  },
        { "AG_HistFunc",       reinterpret_cast<void**>(&g_api.AG_HistFunc),      true  },
        { "AG_BpInfo",         reinterpret_cast<void**>(&g_api.AG_BpInfo),        true  },
        { "AG_MemAtt",         reinterpret_cast<void**>(&g_api.AG_MemAtt),        true  },
        { "AG_DapAcc",         reinterpret_cast<void**>(&g_api.AG_DapAcc),        true  },
        { "AG_BreakResAlloc",  reinterpret_cast<void**>(&g_api.AG_BreakResAlloc), true  },
        { "AG_BreakFunc",      reinterpret_cast<void**>(&g_api.AG_BreakFunc),     true  },
    };
    int missing = 0;
    for (const auto& s : syms) {
        if (!Resolve(g_mod, s.name, s.slot)) { Fail("export %s is missing", s.name); ++missing; }
    }
    if (!missing) Pass("all 15 exports resolved");

    // -----------------------------------------------------------------------
    Step("ABI: the two capability entry points");
    {
        const int fam = g_api.EnumUvARM7(nullptr, 2);
        if (fam == 7) Pass("EnumUvARM7(NULL,2) = 7 (ARM family)");
        else          Fail("EnumUvARM7(NULL,2) = %d, expected 7", fam);

        const int cap2 = g_api.DllUv3Cap(2, nullptr);
        if (cap2 == 7) Pass("DllUv3Cap(2) = 7");
        else           Fail("DllUv3Cap(2) = %d, expected 7", cap2);

        const int cap100 = g_api.DllUv3Cap(100, nullptr);
        if (cap100 == 1) Pass("DllUv3Cap(100) = 1 (flash download promised)");
        else             Fail("DllUv3Cap(100) = %d, expected 1", cap100);

        const int cap109 = g_api.DllUv3Cap(109, nullptr);
        if (cap109 == 0) Pass("DllUv3Cap(109) = 0");
        else             Fail("DllUv3Cap(109) = %d, expected 0", cap109);
    }

    // -----------------------------------------------------------------------
    Step("Debug block: hand over the option string the way uVision does");
    static dbgblk blk;
    memset(&blk, 0, sizeof(blk));
    strncpy_s(blk.TargDll, "ORB_AGDI.dll", _TRUNCATE);
    strncpy_s(blk.TargArgs, options, _TRUNCATE);
    if (device) strncpy_s(blk.Device, device, _TRUNCATE);
    // STM32F1 defaults, so the flash-side lookups have something real to chew on
    // even when the harness is pointed at a bare board.
    //
    // dbgblk carries TWO sets of ranges: the iMCS166 one (Irom / Iram, capital)
    // and the iMCS51 one (irom / iram).  AdoptUVision reads the capital pair -
    // filling only the lowercase ones left the banner reporting "0 kB flash at
    // 0x00000000", which is a harness mistake and not a driver one.  Both are
    // filled here so the intent is unmistakable.
    blk.Irom.nStart = 0x08000000u; blk.Irom.nSize = 0x00010000u;
    blk.Iram.nStart = 0x20000000u; blk.Iram.nSize = 0x00005000u;
    blk.irom.nStart = 0x08000000u; blk.irom.nSize = 0x00010000u;
    blk.iram.nStart = 0x20000000u; blk.iram.nSize = 0x00005000u;

    const int fam = g_api.EnumUvARM7(&blk, 2);
    if (fam == 7) Pass("EnumUvARM7(dbgblk,2) = 7 - option string adopted");
    else          Fail("EnumUvARM7(dbgblk,2) = %d, expected 7", fam);

    // -----------------------------------------------------------------------
    Step("Register the callback before anything can want a message box");
    const U32 cbRc = g_api.AG_Init(static_cast<U16>(AG_INITITEM | AG_INITCALLBACK),
                                   reinterpret_cast<void*>(&Callback));
    if (cbRc == 0) Pass("AG_Init(AG_INITITEM|AG_INITCALLBACK) = 0");
    else           Fail("AG_Init(AG_INITITEM|AG_INITCALLBACK) = %u", cbRc);

    // -----------------------------------------------------------------------
    Step("Connect: AG_Init(AG_INITFEATURES)");
    SUPP supp;
    memset(&supp, 0, sizeof(supp));
    const DWORD t0 = GetTickCount();
    const U32 rc = g_api.AG_Init(AG_INITFEATURES, &supp);
    const DWORD dt = GetTickCount() - t0;
    if (rc == AG_OK) Pass("connected in %lu ms", dt);
    else {
        Fail("AG_Init(AG_INITFEATURES) = %u - no target, or the link could not "
             "be established (check the wiring and the SWJ clock)", rc);
        printf("\nFAILED  (%d problem(s))\n", g_fail);
        return 1;
    }
    // --reset：uVision 每次开会话都发这一发，让目标重跑自己的启动流程（固件的
    // trace 初始化就在里面）。没有它，ETM 配了也只吐空闲码 —— 说明见 ResetTarget()。
    if (resetTarget) ResetTarget(g_api.AG_Init);

    Note("supp: MemAccR=%u RegAccR=%u ResetR=%u ExtBrkSupp=%u ItmTrace=%u hTrace=%u hPaLyze=%u",
         supp.MemAccR, supp.RegAccR, supp.ResetR, supp.ExtBrkSupp,
         supp.ItmTrace, supp.hTrace, supp.hPaLyze);

    // -----------------------------------------------------------------------
    if (pokeAdr) {
        Step("Poke: write 0x%08X to 0x%08X", pokeVal, pokeAdr);
        U32 v = pokeVal;
        UC8 w[4];
        memcpy(w, &v, 4);
        GADR a; a.Adr = pokeAdr; a.ErrAdr = 0;
        const U32 rc = g_api.AG_MemAcc(AG_WRITE, w, &a, 4);
        if (rc == AG_OK) {
            Pass("the write was accepted - the bus reaches that address");
        } else {
            Fail("the write failed (rc=%u) - the bus refused it", rc);
        }
    }

    // -----------------------------------------------------------------------
    // ETM 实验：编程序列（握手）+ ETMCR bit8 对照。
    //
    // **这一段故意留在 probe 里，不进驱动。** 两条理由：
    //   * Keil 侧以 **PDSC / 初始化脚本**为准，器件相关的 trace 配置不该由驱动硬编码
    //     （见 COMPAT_ANALYSIS.md §10.28 的审计）；
    //   * 手册的编程序列与 ETMCR 位语义要先用**可观测的实验**确认，再决定驱动怎么写。
    //
    // 依据：ARM IHI0014Q —— Table 3-5（ETMCR 位表）、Figure 3-3（编程流程 p.3-97）、
    // §3.4.3（三条约束：只改 Progbit 那一位 / 改其他位前必须等 ETMSR[1]=1 / 推荐 RMW）。
    if (etmTest) {
        const U32 kEtmCr   = 0xE0041000u;   // 0x000 Main Control
        const U32 kEtmCcr  = 0xE0041004u;   // 0x001 Configuration Code
        const U32 kEtmMsr  = 0xE0041010u;   // 0x004 Status
        const U32 kEtmScr  = 0xE0041014u;   // 0x005 System Configuration
        const U32 kEtmIdr  = 0xE00411E4u;   // 0x079 ID Register
        const U32 kEtmCcer = 0xE00411E8u;   // 0x07A Configuration Code Extension
        const U32 kEtmTsevr= 0xE00411F8u;   // 0x07E Timestamp Event
        const U32 kEtmLar  = 0xE0041FB0u;   // Lock Access
        const U32 kProgBit = 1u << 10;      // ETMCR[10] ETM programming
        const U32 kMsrProg = 1u << 1;       // ETMSR[1] effective Progbit
        const U32 kBranch  = 1u << 8;       // ETMCR[8]  Branch output

        auto Rd = [&](U32 adr, U32& v) -> bool {
            UC8 b[4] = {0, 0, 0, 0};
            GADR a; a.Adr = adr; a.ErrAdr = 0;
            if (g_api.AG_MemAcc(AG_READ, b, &a, 4) != AG_OK) return false;
            v = (U32)b[0] | ((U32)b[1] << 8) | ((U32)b[2] << 16) | ((U32)b[3] << 24);
            return true;
        };
        auto Wr = [&](U32 adr, U32 v) -> bool {
            UC8 b[4]; memcpy(b, &v, 4);
            GADR a; a.Adr = adr; a.ErrAdr = 0;
            return g_api.AG_MemAcc(AG_WRITE, b, &a, 4) == AG_OK;
        };
        // 轮询 ETMSR[1] 到 want 值；返回耗时 ms，<0 表示失败/超时。
        auto Poll = [&](U32 want, int ms) -> int {
            const DWORD t = GetTickCount();
            for (;;) {
                U32 msr = 0;
                if (!Rd(kEtmMsr, msr)) return -1;
                if ((msr & kMsrProg) == (want & kMsrProg))
                    return static_cast<int>(GetTickCount() - t);
                if (static_cast<int>(GetTickCount() - t) >= ms) return -2;
                Sleep(1);
            }
        };
        // 按 Figure 3-3 改 ETMCR。**四步一步都不能少** —— 手册 §3.4.3 的两条约束：
        //   "When setting the Programming bit, you must not change any other bits of the ETMCR"
        //   "You must only change the value of bits other than the Programming bit of the
        //    ETMCR when bit [1] of the ETMSR is set to 1"
        //
        // 实测（2026-10-13）：把"改其他位"和"清 Progbit"合并成一次写，**硬件会静默丢弃**
        // 那次写里的非 Progbit 位 —— 表现是写的时候读回正确，运行期间又变回原值，同时
        // trace 质量下降（字节减半、COBS 错帧从 1 涨到 31）。
        auto SetCrWithHandshake = [&](U32 crNow, U32 want) -> bool {
            Wr(kEtmLar, 0xC5ACCE55u);
            Wr(kEtmCr, crNow | kProgBit);   // 1) 只置 Progbit
            if (Poll(kMsrProg, 200) < 0) return false;
            Wr(kEtmCr, want | kProgBit);    // 2) 现在才改其他位（Progbit 保持 1）
            Wr(kEtmCr, want & ~kProgBit);   // 3) 只清 Progbit
            if (Poll(0, 200) < 0) return false;
            return true;
        };

        Step("ETM: identity + state");
        {
            U32 cr = 0, msr = 0, ccr = 0, scr = 0, idr = 0, ccer = 0;
            const bool okCr = Rd(kEtmCr, cr), okMsr = Rd(kEtmMsr, msr);
            Rd(kEtmCcr, ccr); Rd(kEtmScr, scr); Rd(kEtmIdr, idr); Rd(kEtmCcer, ccer);
            if (okCr && okMsr) Pass("ETMCR=0x%08X ETMSR=0x%08X", cr, msr);
            else               Fail("ETMCR / ETMSR read failed");
            Note("ETMCCR=0x%08X ETMSCR=0x%08X ETMIDR=0x%08X ETMCCER=0x%08X",
                 ccr, scr, idr, ccer);
            Note("ETMCR bits: pwdn[0]=%u portsiz[6:4]=%u bit21=%u BO[8]=%u DRC[9]=%u "
                 "prog[10]=%u ETMEN[11]=%u **TSenable[28]=%u**",
                 cr & 1u, (cr >> 4) & 7u, (cr >> 21) & 1u,
                 (cr >> 8) & 1u, (cr >> 9) & 1u, (cr >> 10) & 1u, (cr >> 11) & 1u,
                 (cr >> 28) & 1u);
            Note("ETMSR[1] effective Progbit = %u   (0 = 已退出编程模式)", (msr >> 1) & 1u);

            // ETMCCER（reg 0x07A）——**ETM 模式下时间轴的开关就在这里**：
            //   [22] Timestamping implemented   （=1 才有 ETM 时间戳）
            //   [28] Timestamp encoding         （1 = 自然二进制，0 = 格雷码）
            //   [29] Timestamp packet size      （0 = 48 bit，1 = 64 bit）
            // 出处：IHI0014Q Table 3-52 + §7.7 Timestamping（ETMv3.5）。
            // 手册也给了官方判据："You enable timestamping by setting the Timestamp
            // enable bit in the Main Control Register to 1"；若 [22]=0 则 ETMCR[28] 是 RAZ/WI。
            Note("ETMCCER bits: Timestamping[22]=%u Timestamp-encoding[28]=%s "
                 "Timestamp-size[29]=%s",
                 (ccer >> 22) & 1u,
                 ((ccer >> 28) & 1u) ? "binary" : "gray",
                 ((ccer >> 29) & 1u) ? "64-bit" : "48-bit");
            if ((ccer >> 22) & 1u) {
                Pass("ETM 实现时间戳 ⇒ ETMCR[28] 可用来打开 ETM 时间轴（用 --etm-ts 1 验）");
            } else {
                Fail("ETM 未实现时间戳（ETMCCER[22]=0）⇒ ETMCR[28] 是 RAZ/WI，"
                     "这颗 ETM 没有任何时间来源");
            }
            {
                U32 tsevr = 0; Rd(kEtmTsevr, tsevr);
                Note("ETMTSEVR(reg 0x07E)=0x%08X   (只在 ETMCCER[22]=1 时实现)", tsevr);
            }
        }

        // 驱动 `ProgramEtm` 写的三个常量，逐个对照手册。按"不硬编码"的要求，
        // **驱动里只改有出处的那些**，没有出处的一律先在这里量。
        Step("ETM: 驱动写的常量 vs IHI0014Q §3.5.7 \"Tracing all memory\" 配方");
        {
            const U32 kTrigger = 0xE0041008u;   // 0x002
            const U32 kTeevr   = 0xE0041020u;   // 0x008
            const U32 kTecr1   = 0xE0041024u;   // 0x009
            const U32 kTecr2   = 0xE004101Cu;   // 0x007
            U32 tg = 0, te = 0, t1 = 0, t2 = 0;
            Rd(kTrigger, tg); Rd(kTeevr, te); Rd(kTecr1, t1); Rd(kTecr2, t2);
            Note("ETMTRIGGER=0x%08X (驱动写 0x406F，**无出处**)  ETMTEEVR=0x%08X "
                 "(驱动写 0x6F，手册常量 TRUE)  ETMTECR1=0x%08X (驱动写 0)  ETMTECR2=0x%08X",
                 tg, te, t1, t2);
            if (te == 0x6Fu) Pass("ETMTEEVR 读回 0x6F，与手册的 TRUE 一致");
            else             Fail("ETMTEEVR 读回 0x%08X，不是 0x6F", te);
            if (t1 == 0u) Note("ETMTECR1 = 0；手册配方是 1<<24（只置 bit24）—— 两者是否等价待实测");
            else          Note("ETMTECR1 = 0x%08X（手册配方为 1<<24）", t1);
            Note("手册 §3.5.7 配方：ETMTECR1 = (1<<24)、其余位 0；ETMTECR2 = 0；ETMTEEVR = 0x6F");
        }

        Step("ETM: Figure 3-3 握手（置 Progbit -> 等 ETMSR[1]=1 -> 清 -> 等 =0）");
        {
            U32 cr0 = 0;
            if (!Rd(kEtmCr, cr0)) {
                Fail("ETMCR read failed");
            } else {
                const int t1 = [&] {
                    Wr(kEtmLar, 0xC5ACCE55u);
                    Wr(kEtmCr, cr0 | kProgBit);
                    return Poll(kMsrProg, 200);
                }();
                if (t1 >= 0) Pass("ETMSR[1] went to 1 in %d ms", t1);
                else         Fail("ETMSR[1] did not become 1 (rc=%d)", t1);

                const int t2 = [&] {
                    Wr(kEtmCr, cr0 & ~kProgBit);
                    return Poll(0, 200);
                }();
                if (t2 >= 0) Pass("ETMSR[1] went to 0 in %d ms", t2);
                else         Fail("ETMSR[1] did not become 0 (rc=%d)", t2);
                Note("ETMCR restored to 0x%08X", cr0);
            }
        }

        if (etmBit8 >= 0) {
            Step("ETM: ETMCR bit8 (Branch output) -> %d，走握手", etmBit8);
            U32 cr0 = 0;
            if (!Rd(kEtmCr, cr0)) {
                Fail("ETMCR read failed");
            } else {
                const U32 want = etmBit8 ? (cr0 | kBranch) : (cr0 & ~kBranch);
                if (!SetCrWithHandshake(cr0, want)) {
                    Fail("handshake failed while setting bit8");
                } else {
                    U32 rb = 0; Rd(kEtmCr, rb);
                    Note("wrote 0x%08X -> read back 0x%08X (BO[8]=%u)",
                         want, rb, (rb >> 8) & 1u);
                    if ((rb & kBranch) == (want & kBranch)) Pass("bit8 write took effect");
                    else Fail("bit8 did NOT take effect (read 0x%08X)", rb);

                    // 延时重读。实测（2026-10-13）：**写完立刻读是对的，过一会儿再读就变回去了**
                    // —— 而那时目标没在跑、驱动没写、读缓存又排除 SCS ⇒ **是硬件自己拉回去的**。
                    // 这一条决定 P0-E 能不能用"翻 bit8"来做对照。
                    Sleep(200);
                    U32 rb2 = 0; Rd(kEtmCr, rb2);
                    if ((rb2 & kBranch) == (want & kBranch)) {
                        Note("200 ms 后重读 0x%08X - 写入是持久的", rb2);
                    } else {
                        Fail("200 ms 后 bit8 变回 %u（0x%08X）- 硬件把它拉回去了，"
                             "写入不持久", (rb2 >> 8) & 1u, rb2);
                    }
                }
            }
            Note("接下来的 run 就用这个配置采 trace —— 对比两次的 `字节=`/`分支=` 即 P0-E");
        }

        // -------------------------------------------------------------------
        // ETMCR bit12 "Cycle-accurate tracing"（IHI0014Q Table 3-5 [12]）。
        //
        // 这一位是 **ETM 模式下时间轴的开关**：只有它置 1，ETM 才会发周期计数包
        // （`traceDec/traceDecoder_etm35.cpp:623` 的 EV_CH_CYCLECOUNT），驱动的
        // 周期轴才有 ETM 来源（Todo.md 的 P0-F′）。
        //
        // 实测线索（2026-10-13，§10.29/§10.31）：固件 `ETM_Pin_Init()` 写的 0x191E
        // **含** bit12，但 ETMCR 回读恒为 0x810/0x910 —— bit12 与 bit1/2/3 一起
        // 读回为 0。所以要么硬件不实现它（RAZ/WI），要么有东西在写回。
        // 这个实验回答"驱动要不要自己置它"：置得上且 200 ms 后还在 ⇒ 是配置项，
        // 该由谁配是个决策；置不上 ⇒ 这颗 ETM 没有周期计数，ETM 时间轴不可能有。
        if (etmBit12 >= 0) {
            Step("ETM: ETMCR bit12 (Cycle-accurate tracing) -> %d，走握手", etmBit12);
            U32 cr0 = 0;
            if (!Rd(kEtmCr, cr0)) {
                Fail("ETMCR read failed");
            } else {
                const U32 kCycleAcc = 1u << 12;
                const U32 want = etmBit12 ? (cr0 | kCycleAcc) : (cr0 & ~kCycleAcc);
                if (!SetCrWithHandshake(cr0, want)) {
                    Fail("handshake failed while setting bit12");
                } else {
                    U32 rb = 0; Rd(kEtmCr, rb);
                    Note("wrote 0x%08X -> read back 0x%08X (bit12=%u)",
                         want, rb, (rb >> 12) & 1u);
                    if ((rb & kCycleAcc) == (want & kCycleAcc)) Pass("bit12 write took effect");
                    else Fail("bit12 did NOT take effect (read 0x%08X) —— 该位在这颗 ETM 上"
                              "读回恒 0（RAZ/WI）", rb);

                    Sleep(200);
                    U32 rb2 = 0; Rd(kEtmCr, rb2);
                    if ((rb2 & kCycleAcc) == (want & kCycleAcc)) {
                        Note("200 ms 后重读 0x%08X - 写入是持久的", rb2);
                    } else {
                        Fail("200 ms 后 bit12 变回 %u（0x%08X）", (rb2 >> 12) & 1u, rb2);
                    }
                }
            }
            Note("接下来的 run 若 bit12 生效，`ETM: 停止(...)` 一行的 `周期=` 应 > 0，"
                 "且日志出现 `TraceWin: cycle axis source = ETM cycle count`");
        }

        // -------------------------------------------------------------------
        // ETMCR bit28 "Timestamp enable"（IHI0014Q Table 3-5 [28] + §7.7）。
        //
        // **这才是 ETM 模式下时间轴的开关**（不是 bit12）：
        //   §7.7 原文 "You enable timestamping by setting the Timestamp enable bit in
        //   the Main Control Register to 1."；前提是 ETMCCER[22]=1，否则该位 RAZ/WI。
        // 打开之后 ETM 会周期性（以及 ISB / 异常进出 / 时间戳事件时）往 trace 流里
        // 插**时间戳包**（Figure 7-40/7-41，绝对时间值，压缩 + 可能格雷码），
        // 解码器把它报成 `EV_CH_TSTAMP`（`cpu->ts`）。
        if (etmTs >= 0) {
            Step("ETM: ETMCR bit28 (Timestamp enable) -> %d，走握手", etmTs);
            U32 cr0 = 0, ccer = 0;
            if (!Rd(kEtmCr, cr0) || !Rd(kEtmCcer, ccer)) {
                Fail("ETMCR / ETMCCER read failed");
            } else if (((ccer >> 22) & 1u) == 0u) {
                Fail("ETMCCER[22]=0（时间戳未实现）—— ETMCR[28] 是 RAZ/WI，写它没有意义");
            } else {
                const U32 kTsEna = 1u << 28;
                const U32 want = etmTs ? (cr0 | kTsEna) : (cr0 & ~kTsEna);
                if (!SetCrWithHandshake(cr0, want)) {
                    Fail("handshake failed while setting bit28");
                } else {
                    U32 rb = 0; Rd(kEtmCr, rb);
                    Note("wrote 0x%08X -> read back 0x%08X (TSena[28]=%u)",
                         want, rb, (rb >> 28) & 1u);
                    if ((rb & kTsEna) == (want & kTsEna)) Pass("bit28 write took effect");
                    else Fail("bit28 did NOT take effect (read 0x%08X)", rb);

                    Sleep(200);
                    U32 rb2 = 0; Rd(kEtmCr, rb2);
                    if ((rb2 & kTsEna) == (want & kTsEna)) {
                        Note("200 ms 后重读 0x%08X - 写入是持久的", rb2);
                    } else {
                        Fail("200 ms 后 bit28 变回 %u（0x%08X）", (rb2 >> 28) & 1u, rb2);
                    }
                }
            }
            Note("接下来的 run 若时间戳生效，`ETM: 停止(...)` 一行的 `时间戳=` 应 > 0，"
                 "且日志出现 `TraceWin: cycle axis source = ETM timestamp`");
        }

        // -------------------------------------------------------------------
        // 标记判定：**只有这一段能分出"是谁把 ETMCR 写回去了"**。
        //
        // 背景（2026-10-13）：把 bit8 清掉之后，写完立刻读是对的，过一会儿再读 bit8 又变回 1。
        // 但回读值 0x910 本身**有歧义** —— 它既是 `ProgramEtm` 写的值，也是器件固件
        // `ETM_Pin_Init()` 写 0x191E、被硬件抹掉 bit1/2/3/12 之后的值（两者都是 bit4|bit8|bit11）：
        //
        //     驱动 ProgramEtm : 0x910 = bit4 | bit8 | bit11
        //     固件 0x191E      : 0x910 = 同上（0x191E 的 bit1/2/3/12 读回恒为 0）
        //
        // ⇒ 光看 0x910 分不出"驱动的写入还在"和"目标中途复位、固件又写了一遍"。
        // 判据：写一个**两边都不会产生**的值 —— 把 bit8 和 bit11 **一起**清掉（得 0x010），
        // 再延时重读。三种结果各指一个结论：
        //
        //     0x010   两位都没回来 ⇒ 没有东西在写 ETMCR，之前那次是偶发
        //     0x110   只有 bit8 回来 ⇒ **硬件单独把 bit8 拉回**（不是整字重写）
        //     0x910   两位都回来     ⇒ **整字被重写** = 固件复位重跑（或驱动重配）
        Step("ETM: 标记判定 —— 清 bit8+bit11，延时重读，分辨是谁写回的");
        {
            U32 cr0 = 0;
            if (!Rd(kEtmCr, cr0)) {
                Fail("ETMCR read failed");
            } else {
                const U32 kEtmEnable = 1u << 11;                 // ETMCR[11] ETMEN
                const U32 marker    = cr0 & ~kBranch & ~kEtmEnable;
                Note("基准 0x%08X -> 标记 0x%08X（bit8=0, bit11=0；驱动和固件都不会产生这个值）",
                     cr0, marker);
                if (!SetCrWithHandshake(cr0, marker)) {
                    Fail("handshake failed while writing the marker");
                } else {
                    U32 rb = 0; Rd(kEtmCr, rb);
                    Note("立即回读 0x%08X (BO[8]=%u ETMEN[11]=%u)",
                         rb, (rb >> 8) & 1u, (rb >> 11) & 1u);
                    Sleep(200);
                    U32 rb2 = 0; Rd(kEtmCr, rb2);
                    const unsigned bo = (rb2 >> 8) & 1u, en = (rb2 >> 11) & 1u;
                    if (rb2 == marker) {
                        Pass("200 ms 后 0x%08X - 标记完好：**没有东西在写 ETMCR**", rb2);
                    } else if (bo && !en) {
                        Fail("200 ms 后 0x%08X - 只有 bit8 自己回来了 => **硬件单独把 bit8 拉回**",
                             rb2);
                    } else if (bo && en) {
                        Fail("200 ms 后 0x%08X - bit8/bit11 都回来了 => **整字被重写**"
                             "（器件固件 0x191E 或驱动重配）", rb2);
                    } else {
                        Fail("200 ms 后 0x%08X - 只有 bit11 回来（BO[8]=%u ETMEN[11]=%u），"
                             "不符合任何已知来源", rb2, bo, en);
                    }
                    // 还原到本步开始时的值 —— 后面的 run 要用它采 trace（P0-E）。
                    if (SetCrWithHandshake(rb2, cr0)) {
                        U32 rb3 = 0; Rd(kEtmCr, rb3);
                        Note("已还原 ETMCR = 0x%08X", rb3);
                    } else {
                        Fail("restore failed - 后面的 run 可能用错配置");
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    Step("Features: AG_Init(AG_GETFEATURE)");
    {
        struct { U16 code; const char* name; } feats[] = {
            { AG_F_MEMACCR,     "AG_F_MEMACCR"     },
            { AG_F_REGACCR,     "AG_F_REGACCR"     },
            { AG_F_RESETR,      "AG_F_RESETR"      },
            { AG_F_EXTBRKSUPP,  "AG_F_EXTBRKSUPP"  },
            { AG_F_TRACE,       "AG_F_TRACE"       },
            { AG_F_ITM_TRACE,   "AG_F_ITM_TRACE"   },
            { AG_F_PALYZE,      "AG_F_PALYZE"      },
        };
        for (const auto& f : feats) {
            const U32 v = g_api.AG_Init(static_cast<U16>(AG_GETFEATURE | f.code), nullptr);
            Note("%-16s = %u", f.name, v);
        }
        Pass("feature query answered for all 7 codes");
    }

    // -----------------------------------------------------------------------
    Step("Registers: AG_AllReg(AG_READ)");
    RgARMCM regs;
    memset(&regs, 0, sizeof(regs));
    const U32 allRc = g_api.AG_AllReg(AG_READ, &regs);
    if (allRc == AG_OK) {
        Pass("all registers read");
        Note("PC=0x%08X SP=0x%08X LR=0x%08X xPSR=0x%08X",
             regs.Rn[15], regs.Rn[13], regs.Rn[14], regs.xPSR);
        Note("MSP=0x%08X PSP=0x%08X", regs.MSP, regs.PSP);
        if (regs.Rn[15] & 1u) Note("PC has bit0 set (Thumb) - expected on Cortex-M");
        if (regs.Rn[13] == 0 || regs.Rn[13] == 0xFFFFFFFFu) {
            Fail("SP reads 0x%08X - the register block did not come back", regs.Rn[13]);
        } else {
            Pass("SP looks like a real stack pointer (0x%08X)", regs.Rn[13]);
        }
    } else {
        Fail("AG_AllReg(AG_READ) = %u", allRc);
    }

    // -----------------------------------------------------------------------
    Step("Memory: AG_MemAcc(AG_READ) at 0x%08X", readAdr);
    {
        UC8 buf[64];
        memset(buf, 0, sizeof(buf));
        GADR a; a.Adr = readAdr; a.ErrAdr = 0;
        const U32 mrc = g_api.AG_MemAcc(AG_READ, buf, &a, sizeof(buf));
        if (mrc == AG_OK) {
            Pass("read %u bytes", (unsigned)sizeof(buf));
            printf("          ");
            for (int i = 0; i < 16; ++i) printf("%02X ", buf[i]);
            printf("\n");
            if (AllSame(buf, sizeof(buf), 0x00) || AllSame(buf, sizeof(buf), 0xFF)) {
                Note("the block is all %02X - readable, but check the address is "
                     "really populated on this target", buf[0]);
            } else {
                Pass("the block is not a constant fill - real memory");
            }
        } else {
            Fail("AG_MemAcc(AG_READ) = %u at 0x%08X", mrc, readAdr);
        }
    }

    // -----------------------------------------------------------------------
    // Read repeatability, first with the target still in its reset state.
    if (readCmpAdr && readCmpLen) ReadCompare(readCmpAdr, readCmpLen, readCmpPasses);

    // -----------------------------------------------------------------------
    if (wrAdr) {
        Step("Memory: write / read back / restore at 0x%08X", wrAdr);
        UC8 orig[16], pat[16], got[16];
        GADR a; a.Adr = wrAdr; a.ErrAdr = 0;
        if (g_api.AG_MemAcc(AG_READ, orig, &a, sizeof(orig)) != AG_OK) {
            Fail("could not read the scratch block first - nothing was written");
        } else {
            for (int i = 0; i < 16; ++i) pat[i] = static_cast<UC8>(0xA5 ^ i);
            U32 rc1 = g_api.AG_MemAcc(AG_WRITE, pat, &a, sizeof(pat));
            U32 rc2 = g_api.AG_MemAcc(AG_READ, got, &a, sizeof(got));
            U32 rc3 = g_api.AG_MemAcc(AG_WRITE, orig, &a, sizeof(orig));   // restore
            if (rc1 == AG_OK && rc2 == AG_OK && memcmp(pat, got, sizeof(pat)) == 0) {
                Pass("wrote, read back and matched");
            } else {
                Fail("write/read-back failed (write=%u read=%u)", rc1, rc2);
            }
            if (rc3 == AG_OK) Pass("the original contents were put back");
            else              Fail("restore failed (%u) - the scratch block is left modified", rc3);
        }
    } else {
        Step("Memory: write check skipped (no --memwrite address given)");
        Note("nothing was written to the target");
    }

    // -----------------------------------------------------------------------
    Step("Run control: halt, single step, resume, stop");
    {
        // DHCSR is the only honest witness of what the core did.  "PC did not
        // move" has three completely different causes - the core was never
        // halted, the step is still pending, or the code really does spin - and
        // the status bits tell them apart.  C_DEBUGEN / C_STEP / S_HALT:
        const U32 kCDebugEn = 0x00000001u;
        const U32 kCStep    = 0x00000004u;
        const U32 kSHalt    = 0x00020000u;

        auto Dhcsr = [&](const char* when) -> U32 {
            UC8 b[4] = {0, 0, 0, 0};
            GADR a; a.Adr = 0xE000EDF0u; a.ErrAdr = 0;
            if (g_api.AG_MemAcc(AG_READ, b, &a, 4) != AG_OK) {
                Note("DHCSR %-9s = <read failed>", when);
                return 0xDEADBEEFu;
            }
            U32 v = 0; memcpy(&v, b, 4);
            Note("DHCSR %-9s = 0x%08X  %s%s%s", when, v,
                 (v & kCDebugEn) ? "C_DEBUGEN " : "",
                 (v & kCStep)    ? "C_STEP "    : "",
                 (v & kSHalt)    ? "S_HALT"     : "(running)");
            return v;
        };

        auto ReadPc = [&]() -> U32 {
            RgARMCM r; memset(&r, 0, sizeof(r));
            if (g_api.AG_AllReg(AG_READ, &r) != AG_OK) return 0xFFFFFFFFu;
            return r.Rn[15];
        };

        GADR pc; pc.Adr = 0; pc.ErrAdr = 0;
        const U32 before = Dhcsr("before");
        if (before & kSHalt) Pass("the core is halted before the step test");
        else                 Fail("the core is NOT halted (DHCSR=0x%08X) - a step "
                                  "cannot work from here", before);

        const U32 stopRc = g_api.AG_GoStep(AG_STOPRUN, 0, nullptr);
        if (stopRc == AG_OK) Pass("AG_GoStep(AG_STOPRUN) = 0");
        else                 Fail("AG_GoStep(AG_STOPRUN) = %u", stopRc);

        // One step, then look at where it landed AND at what DHCSR says.
        const U32 pc0 = ReadPc();
        const U32 stepRc = g_api.AG_GoStep(AG_NSTEP, 1, &pc);
        Dhcsr("after 1");
        if (stepRc == AG_OK) Pass("AG_GoStep(AG_NSTEP,1) = 0, PC = 0x%08X", pc.Adr);
        else                 Fail("AG_GoStep(AG_NSTEP,1) = %u", stepRc);

        const U32 step2 = g_api.AG_GoStep(AG_NSTEP, 3, &pc);
        if (step2 == AG_OK) Pass("AG_GoStep(AG_NSTEP,3) = 0, PC = 0x%08X", pc.Adr);
        else                Fail("AG_GoStep(AG_NSTEP,3) = %u", step2);

        // "PC never moves" is only a defect if the code would have moved it.
        // Reading the two halfwords at PC is what tells the two apart: 0xE7FE
        // is `b .`, and a short backward branch around PC is the same story.
        {
            UC8 code[8] = {0};
            GADR a; a.Adr = pc.Adr & ~1u; a.ErrAdr = 0;
            if (g_api.AG_MemAcc(AG_READ, code, &a, sizeof(code)) == AG_OK) {
                const unsigned short h0 = static_cast<unsigned short>(code[0] | (code[1] << 8));
                const unsigned short h1 = static_cast<unsigned short>(code[2] | (code[3] << 8));
                Note("at PC: %04X %04X", h0, h1);
                if (h0 == 0xE7FE) Note("`b .` - the target really is spinning, so a "
                                       "PC that does not move is correct");
            }
        }

        if (pc0 == pc.Adr) {
            Note("PC did not change across the steps (0x%08X)", pc0);
            if (before & kSHalt) {
                Note("the core was halted and stayed halted - see the DHCSR lines "
                     "above for whether C_STEP was ever accepted");
            }
        } else {
            Pass("PC moved 0x%08X -> 0x%08X, so stepping works", pc0, pc.Adr);
        }
    }

    // -----------------------------------------------------------------------
    Step("Single step, done by hand (bypasses Core::Step)");
    {
        // This is the experiment that separates the two possible causes of "PC
        // did not move": the driver's step sequence, or everything below it.  A
        // hand-written DHCSR write goes through the same AG_MemAcc the driver
        // uses, but not through Core::Step - so if this one steps and that one
        // does not, the difference is in the driver.
        const U32 kKey = 0xA05F0000u;
        GADR a; a.Adr = 0xE000EDF0u; a.ErrAdr = 0;

        auto ReadDhcsr = [&]() -> U32 {
            UC8 b[4] = {0, 0, 0, 0};
            if (g_api.AG_MemAcc(AG_READ, b, &a, 4) != AG_OK) return 0xDEADBEEFu;
            U32 v = 0; memcpy(&v, b, 4); return v;
        };
        auto ReadPc = [&]() -> U32 {
            RgARMCM r; memset(&r, 0, sizeof(r));
            if (g_api.AG_AllReg(AG_READ, &r) != AG_OK) return 0xFFFFFFFFu;
            return r.Rn[15];
        };

        const U32 pcBefore = ReadPc();
        const U32 want = kKey | 0x1u | 0x4u;            // DBGKEY|C_DEBUGEN|C_STEP
        UC8 w[4];
        memcpy(w, &want, 4);
        const U32 wrc = g_api.AG_MemAcc(AG_WRITE, w, &a, 4);
        Sleep(50);
        const U32 after = ReadDhcsr();
        const U32 pcAfter = ReadPc();
        Note("wrote DHCSR = 0x%08X (rc=%u), read back 0x%08X", want, wrc, after);
        Note("PC 0x%08X -> 0x%08X", pcBefore, pcAfter);
        if (pcAfter != pcBefore) {
            Pass("a hand-written C_STEP moves PC - the layer below the driver "
                 "accepts a step, so a step that does not work is the driver's");
        } else if (after == 0xDEADBEEFu) {
            Fail("DHCSR could not be read at all");
        } else {
            Note("a hand-written C_STEP does not move PC either, so the step is "
                 "refused below the driver (probe, RDDI layer or target)");
        }
    }

    // -----------------------------------------------------------------------
    Step("DAP: AG_DapAcc(ACCDAP_ABORT)");
    {
        // ⚠ 值走的是 **pOut32 / nManyOut**，不是 pIn32 / nManyIn。
        //
        // 字段的 in/out 是从 **DAP 的角度**命名的（AGDI.H:1814-1829）：
        //   nManyOut / pOut*  : "in : Number of bytes in output buffer"
        //                       —— 驱动要**写下去**的值
        //   nManyIn  / pIn*   : "out: Number of bytes in input buffer"
        //                       —— 驱动**读回来**的缓冲
        // 所以"往 DAP 写一个 32 位值"这条命令，值在 pOut32 里。
        //
        // 官方 DapAcc_ABORT（DbgCM/AGDI.CPP:17756-17762）和本驱动
        // （ORBAGDI_Agdi.cpp 的 DapAcc_ABORT）都是这么校验的，两边逐字一致：
        //   pOut32 == NULL 或 nManyOut != 4  ->  ACCDAP_BADPARM(100)
        // 本用例原来填的是 pIn8/nManyIn，于是拿到 100 —— 那是**用例填错字段**，
        // 不是驱动的问题（实测对照见 COMPAT_ANALYSIS.md §10.34）。
        ACCDAP in;
        memset(&in, 0, sizeof(in));
        in.accCmd   = ACCDAP_ABORT;
        U32 abortVal = 0x0000001Eu;      // 与原来那 4 个小端字节同值
        in.pOut32   = &abortVal;
        in.nManyOut = 4;
        const U32 d = g_api.AG_DapAcc(&in);
        if (d == ACCDAP_DONE) Pass("raw DAP access works (abort written)");
        else                  Fail("AG_DapAcc(ACCDAP_ABORT) = %u, expected %u", d, ACCDAP_DONE);
    }

    // -----------------------------------------------------------------------
    if (runMs) {
        Step("Run the target for %u ms and stop (this is what pumps trace)", runMs);
        RunResult rr; rr.pc = 0; rr.rc = 0xFFFFFFFFu;
        PollStats ps; memset(&ps, 0, sizeof(ps));
        HANDLE poller = nullptr;
        HANDLE th = CreateThread(nullptr, 0, RunThread, &rr, 0, nullptr);
        if (!th) {
            Fail("CreateThread failed (%lu)", GetLastError());
        } else {
            // 争用制造者：运行期间一直打 AG_DapAcc（它没有 c.running 守卫）。
            if (pollMs) poller = CreateThread(nullptr, 0, PollThread, &ps, 0, nullptr);
            const DWORD t0 = GetTickCount();
            Sleep(runMs);
            const U32 stopRc = g_api.AG_GoStep(AG_STOPRUN, 0, nullptr);
            const DWORD waited = WaitForSingleObject(th, 15000);
            const DWORD dt = GetTickCount() - t0;
            CloseHandle(th);
            if (waited != WAIT_OBJECT_0) {
                Fail("the run did not stop within 15 s of AG_STOPRUN");
            } else if (rr.rc == AG_OK) {
                Pass("ran %lu ms, stopped at PC=0x%08X (AG_STOPRUN=%u)", dt, rr.pc, stopRc);
            } else {
                Fail("AG_GoStep(AG_GOFORBRK) = %u", rr.rc);
            }

            // ---- P0-A′：运行期间的锁争用 ----------------------------------
            if (poller) {
                InterlockedExchange(&ps.stop, 1);
                WaitForSingleObject(poller, 5000);
                CloseHandle(poller);
                Step("P0-A': AG_DapAcc(ACCDAP_ABORT) while the run pumps trace");
                Note("calls=%ld failed=%ld", ps.dapCalls, ps.dapFail);
                if (ps.dapCalls < 10) {
                    Note("too few calls to say anything (%ld)", ps.dapCalls);
                } else if (ps.dapFail == 0) {
                    Pass("no ACCDAP_FAILED under contention (P0-A': the entry waits "
                         "for the probe instead of giving up)");
                } else {
                    Fail("%ld of %ld calls returned ACCDAP_FAILED - the entry gives up "
                         "instead of waiting (P0-A')", ps.dapFail, ps.dapCalls);
                }
            }

            // ---- P0-A′：停机之后、trace 源仍开着时的寄存器窗口 -------------
            // 这是"间歇 AG_NOACCESS"真正的现场：目标已停，worker 还在排空缓冲。
            if (pollMs) {
                Step("P0-A': AG_AllReg while the target is halted but the trace source is open");
                RgARMCM r; memset(&r, 0, sizeof(r));
                for (int i = 0; i < 200; ++i) {
                    InterlockedIncrement(&ps.allRegCalls);
                    if (g_api.AG_AllReg(AG_READ, &r) != AG_OK) {
                        InterlockedIncrement(&ps.allRegFail);
                    }
                    Sleep(1);
                }
                Note("calls=%ld failed=%ld", ps.allRegCalls, ps.allRegFail);
                if (ps.allRegFail == 0) {
                    Pass("no AG_NOACCESS from AG_AllReg (P0-A': the register window "
                         "does not fail intermittently)");
                } else {
                    Fail("%ld of %ld AG_AllReg calls returned AG_NOACCESS (P0-A')",
                         ps.allRegFail, ps.allRegCalls);
                }
            }
        }
        // The same read test again, now that the application has been running.
        // This is the comparison that matters: identical before and broken
        // after means the target's state is what breaks the MEM-AP read.
        if (readCmpAdr && readCmpLen) ReadCompare(readCmpAdr, readCmpLen, readCmpPasses);

        // What the target's trace hardware actually holds now.  The driver
        // programs it, but the application may program it too, and whoever
        // wrote last wins - this is the only way to see who that was.
        Step("Target trace registers after the run");
        {
            struct Reg { unsigned adr; const char* name; };
            static const Reg regs[] = {
                // ---- 先回答"CPU 现在到底在哪儿" ----------------------------
                // RUN 灯不闪 = 应用没在跑。最可能是停在某个异常里，而
                // ICSR.VECTACTIVE 直接给出当前异常号（3 = HardFault）。
                // 这一组放在最前面：目标不跑的话，后面所有 trace 计数都没有意义。
                { 0xE000ED04u, "ICSR        (VECTACTIVE = bits 8:0, 3 = HardFault)" },
                { 0xE000ED28u, "CFSR        (configurable fault status)" },
                { 0xE000ED2Cu, "HFSR        (hardfault status)" },
                { 0x40023800u, "RCC_CR      (HSION[0] HSIRDY[1] HSEON[16] HSERDY[17] PLLON[24] PLLRDY[25])" },
                { 0x40023808u, "RCC_CFGR    (SWS: 2=PLL as sysclk)" },
                { 0x40023804u, "RCC_PLLCFGR (PLLM[5:0] PLLN[14:6] PLLP[17:16] PLLQ[27:24])" },
                { 0xE000EDFCu, "DEMCR      (TRCENA = bit 24)" },
                { 0xE0042004u, "DBGMCU_CR  (trace pin mode)" },
                // 调试域里"内核停机时冻结外设"的位。它们在**调试域**，系统复位清不掉，
                // 只有上电复位才清。一旦某个定时器的冻结位被置起且没被清，
                // 该定时器在调试会话结束后可能不再走动 —— 若它正是 RTOS 的 tick 源，
                // 表现就是"所有任务永久阻塞、只剩空闲任务在跑、RUN 灯不闪"。
                { 0xE0042008u, "DBGMCU_APB1_FZ (bit4=TIM6, bit5=TIM7, ... 冻结位)" },
                { 0xE004200Cu, "DBGMCU_APB2_FZ (冻结位)" },
                // FreeRTOS 的内部状态（地址取自 F407Ful.map）。
                // xTickCount 是判据：1 ms tick 下每秒应 +1000。两次不同时长的运行
                // 一比就知道 tick 到底有没有在推进、速率对不对。
                { 0x20000080u, "FreeRTOS xTickCount" },
                { 0x20000088u, "FreeRTOS xSchedulerRunning" },
                { 0x2000007Cu, "FreeRTOS uxCurrentNumberOfTasks" },
                { 0x2000006Cu, "FreeRTOS pxCurrentTCB" },
                { 0xE000E010u, "SysTick_CTRL (bit0 ENABLE, bit1 TICKINT, bit2 CLKSOURCE)" },
                { 0xE000E018u, "SysTick_LOAD" },
                { 0xE000E01Cu, "SysTick_VAL" },
                { 0xE0040004u, "TPIU_CSPSR (port size)" },
                { 0xE0040010u, "TPIU_ACPR  (async prescaler)" },
                { 0xE00400F0u, "TPIU_SPPR  (0=par 1=man 2=nrz)" },
                { 0xE0040304u, "TPIU_FFCR" },
                { 0xE0000E80u, "ITM_TCR" },
                { 0xE0000E00u, "ITM_TER" },
                { 0xE0001000u, "DWT_CTRL" },
                // ---- trace 引脚的复用到底配成什么了 -------------------------
                // DBGMCU_CR 只管"用哪组 trace 引脚"，引脚本身的复用/时钟是
                // GPIOx_MODER + GPIOx_AFRL + RCC_AHB1ENR 的事，三者缺一不可：
                //   PB3 = TRACESWO   -> MODER[7:6]=10(AF), AFRL[15:12]=0(AF0)
                //   PE2..PE6 = TRACECLK/TRACED0..3 -> MODER 对应位=10, AFRL 对应 nibble=0
                // 只查 DBGMCU_CR 会漏掉"引脚根本没配成复用"这种情形。
                { 0x40023830u, "RCC_AHB1ENR (GPIOBEN=bit1, GPIOEEN=bit4)" },
                { 0x40020400u, "GPIOB_MODER (PB3 = bits 7:6, 2 = AF)" },
                { 0x40020420u, "GPIOB_AFRL  (PB3 = bits 15:12, 0 = AF0/TRACE)" },
                { 0x40021000u, "GPIOE_MODER (PE2..PE6 = bits 15:4, 2 = AF)" },
                { 0x40021020u, "GPIOE_AFRL  (PE2..PE6 nibbles, 0 = AF0/TRACE)" },
                { 0xE0041000u, "ETMCR       (ETM control)" },
                { 0xE0041004u, "ETMCCR      (capabilities)" },
                { 0xE0041008u, "ETMTRIGGER" },
                { 0xE0041010u, "ETMSR       (status)" },
                { 0xE0041014u, "ETMSCR      (sync ctrl)" },
                { 0xE0041018u, "ETMTSSCR    (TraceEnable start/stop)" },
                // 偏移按 IHI0014Q Table 3-3（寄存器号 × 4）：0x007 = ETMTECR2 = 0x01C，
                // 0x00A = ETMFFRR = 0x028，0x00B = ETMFFLR = 0x02C。
                // 原来 0x028 被标成 "ETMTECR2" 是**错的**（那正是 orbuculum
                // `_startETMv35` 写 8 的那个 FIFO 阈值寄存器，注释也对得上）。
                { 0xE004101Cu, "ETMTECR2    (reg 0x007)" },
                { 0xE0041020u, "ETMTEEVR    (trace enable event)" },
                { 0xE0041024u, "ETMTECR1    (trace enable ctrl 1)" },
                { 0xE0041028u, "ETMFFRR     (FIFOFULL region)" },
                { 0xE004102Cu, "ETMFFLR     (FIFOFULL level)" },
                { 0xE00411E0u, "ETMSYNCFR   (reg 0x078, sync period)" },
                // 实测（2026-10-13）：**0xE00411E4 才是 ETMIDR**（register 0x079，读回
                // 0x4114F250）；0xE00411C8 读回 0。原来这两个标签是**反的**。
                { 0xE00411C8u, "0x072       (reads 0 on this part)" },
                { 0xE00411E4u, "ETMIDR      (implementation id, reg 0x079)" },
                { 0xE00411E8u, "ETMCCER     (reg 0x07A: TS[22] enc[28] size[29])" },
                { 0xE00411F8u, "ETMTSEVR    (reg 0x07E, timestamp event)" },
                { 0xE0041200u, "ETMTRACEIDR (ATB id)" },
                { 0xE0041FB0u, "ETMLAR      (lock)" },
            };
            for (const Reg& r : regs) {
                UC8 b[4] = {0, 0, 0, 0};
                GADR a; a.Adr = r.adr; a.ErrAdr = 0;
                if (g_api.AG_MemAcc(AG_READ, b, &a, 4) == AG_OK) {
                    const unsigned v = (unsigned)b[0] | ((unsigned)b[1] << 8) |
                                       ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
                    printf("    0x%08X %-32s = 0x%08X\n", r.adr, r.name, v);
                } else {
                    printf("    0x%08X %-32s = <read failed>\n", r.adr, r.name);
                }
            }
        }

        // 解码结果：字节数只说明"收到了字节"，这里才说明"解出了东西"。
        // AG_HistFunc(3) = 记录条数；AG_HistFunc(1, i) 返回第 i 条的 PC
        // （Keil 约定：记录 1 最新，下标越大越旧）。
        Step("Decoded execution history (AG_HistFunc, fed by the DWT PC samples)");
        {
            const U32 n = g_api.AG_HistFunc(3, 0, 0, nullptr);
            if (n == 0) {
                Note("no history records - the ITM/DWT decode produced no PC samples");
            } else {
                Pass("%u record(s) decoded", n);
                printf("          newest first: ");
                const U32 show = (n < 8) ? n : 8;
                for (U32 i = 1; i <= show; ++i) {
                    printf("%08X ", g_api.AG_HistFunc(1, static_cast<I32>(i), 0, nullptr));
                }
                printf("\n");
            }
        }

        Note("ITM console bytes delivered: %lu", g_itmBytes);
        if (!g_itmBytes) {
            Note("the ITM console stayed empty - either the target's application "
                 "writes nothing to ITM, or its trace hardware was never programmed "
                 "(the orbtrace path configures the probe, not the target)");
        }
    }

    // --bom：目标已停、记录已经攒下，这时候按 uVision 的取数方式核对一遍。
    if (g_useBom && runMs) BomTraceChecks();

    // -----------------------------------------------------------------------
    if (g_haveImage && !noDownload) {
        Step("Flash download: erase / program / verify the image");

        const U32 initRc = g_api.AG_Init(static_cast<U16>(AG_INITITEM | AG_INITFLASHLOAD),
                                         nullptr);
        if (initRc == AG_OK) {
            Pass("AG_INITFLASHLOAD accepted - FLM parsed, copied to RAM, Init() ran");
        } else {
            Fail("AG_INITFLASHLOAD = %u (see the log and the [msg] lines above)", initRc);
        }

        if (initRc == AG_OK) {
            const DWORD t0 = GetTickCount();
            const U32 loadRc = g_api.AG_Init(static_cast<U16>(AG_INITITEM | AG_STARTFLASHLOAD),
                                             nullptr);
            const DWORD dt = GetTickCount() - t0;
            if (loadRc == AG_OK) {
                Pass("AG_STARTFLASHLOAD = 0 - erase/program/verify finished in %lu ms", dt);
            } else {
                Fail("AG_STARTFLASHLOAD = %u after %lu ms", loadRc, dt);
            }

            // The honest check.  "Download succeeded" is a claim; reading the
            // whole image back over the MEM-AP and comparing it with the file is
            // the evidence.  A download that reports success and leaves different
            // bytes is exactly what this exists to catch.
            if (loadRc == AG_OK) {
                size_t bad = 0;
                UL32 firstBad = 0;
                std::vector<UC8> pass1;
                for (size_t i = 0; i < g_image.Count(); ++i) {
                    const UL32 start = g_image.Start(i);
                    const size_t len = g_image.Len(i);
                    std::vector<UC8> rd(len, 0);
                    GADR ra; ra.Adr = start; ra.ErrAdr = 0;
                    if (g_api.AG_MemAcc(AG_READ, &rd[0], &ra, static_cast<UL32>(len)) != AG_OK) {
                        Fail("read-back failed at 0x%08X", start);
                        bad = 1; firstBad = start;
                        break;
                    }
                    if (i == 0) pass1 = rd;
                    for (size_t k = 0; k < len; ++k) {
                        if (rd[k] != g_image.Data(i)[k]) {
                            ++bad;
                            if (!firstBad) firstBad = start + static_cast<UL32>(k);
                        }
                    }
                }
                if (!bad) {
                    Pass("read the whole image back over the MEM-AP: it matches the file");
                } else {
                    Fail("%u byte(s) differ from the file, first at 0x%08X",
                         static_cast<unsigned>(bad), firstBad);
                }

                // A second pass over the SAME addresses.  This is what separates
                // the two possible causes of a mismatch (COMPAT_ANALYSIS.md §10.33):
                //
                //   pass2 != pass1 -> the read itself is not repeatable, so the
                //                     difference says nothing about the flash
                //   pass2 == pass1 -> the reads are consistent and the flash
                //                     content really is different
                //
                // Only run when the first pass found something, so the happy path
                // keeps costing one pass.
                if (bad && g_image.Count() > 0 && pass1.size() == g_image.Len(0)) {
                    const UL32 start = g_image.Start(0);
                    const size_t len = g_image.Len(0);
                    std::vector<UC8> rd2(len, 0);
                    GADR ra; ra.Adr = start; ra.ErrAdr = 0;
                    if (g_api.AG_MemAcc(AG_READ, &rd2[0], &ra, static_cast<UL32>(len)) != AG_OK) {
                        Fail("second read-back failed at 0x%08X", start);
                    } else {
                        size_t bad2 = 0, badRR = 0;
                        UL32 firstBad2 = 0, firstBadRR = 0;
                        for (size_t k = 0; k < len; ++k) {
                            if (rd2[k] != pass1[k]) {
                                ++badRR;
                                if (!firstBadRR) firstBadRR = start + static_cast<UL32>(k);
                            }
                            if (rd2[k] != g_image.Data(0)[k]) {
                                ++bad2;
                                if (!firstBad2) firstBad2 = start + static_cast<UL32>(k);
                            }
                        }
                        Note("pass1 vs file: %u bad | pass2 vs file: %u bad | pass2 vs pass1: %u bad",
                             static_cast<unsigned>(bad), static_cast<unsigned>(bad2),
                             static_cast<unsigned>(badRR));
                        if (badRR == 0) {
                            Pass("two read-backs agree byte for byte - the MEM-AP read IS "
                                 "repeatable, so the flash content really differs");
                        } else {
                            Fail("%u byte(s) differ BETWEEN two reads of the same addresses "
                                 "(first at 0x%08X) - the MEM-AP read itself is not repeatable",
                                 static_cast<unsigned>(badRR), firstBadRR);
                        }
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // And once more after the flash download, if there was one.  Reading the
    // region the flash algorithm just wrote is the one case where a difference
    // between passes has an innocent explanation (the controller may still be
    // busy), so it is worth looking at the bytes rather than the count.
    if (readCmpAdr && readCmpLen) ReadCompare(readCmpAdr, readCmpLen, readCmpPasses);

    // -----------------------------------------------------------------------
    // Hand the core back running.
    //
    // Everything above ends with the target halted: AG_STOPRUN halts it (see
    // RunAndWait in ORBAGDI_Agdi.cpp) and nothing ever resumes it.  A debugger
    // normally leaves it stopped on purpose - the user is looking at the source
    // - but this tool is not interactive, and a board left stopped keeps its
    // application frozen after the tool exits (RUN LED stuck, LCD not updating).
    // That looks exactly like a crashed target and sends the next debugging
    // session after the wrong problem, so resuming is the default here.
    // --halt keeps the old behaviour, for when the next step is to inspect the
    // stopped core.
    //
    // Cleared C_HALT is what actually releases it, and DHCSR is where that bit
    // lives - reading it back is the only way to know the write took.
    if (!haltAtEnd) {
        Step("Leave the target running");
        GADR a; a.Adr = 0xE000EDF0u; a.ErrAdr = 0;
        const U32 resume = 0xA05F0001u;   // DBGKEY | C_DEBUGEN, C_HALT cleared
        UC8 w[4]; memcpy(w, &resume, 4);
        const U32 wrc = g_api.AG_MemAcc(AG_WRITE, w, &a, 4);
        Sleep(100);
        UC8 b[4] = {0, 0, 0, 0};
        U32 after = 0;
        if (g_api.AG_MemAcc(AG_READ, b, &a, 4) == AG_OK) memcpy(&after, b, 4);
        if (wrc == AG_OK && (after & 0x00020000u) == 0) {
            Pass("DHCSR = 0x%08X, S_HALT clear - the core is running again", after);
        } else {
            Fail("the core is still stopped (write rc=%u, DHCSR=0x%08X) - the board "
                 "will need a reset or a power cycle", wrc, after);
        }
    } else {
        Note("--halt given, the core is deliberately left stopped");
    }

    // -----------------------------------------------------------------------
    // AG_UNINIT is an AG_EXECITEM item, not a top-level AG_Init code: AGDI.H
    // defines it as 0x000C and the dispatcher switches on nCode & 0xFF00 with
    // AG_EXECITEM = 0x0400.  Calling AG_Init(AG_UNINIT) alone lands in the
    // "unknown code" default and does nothing at all - the session then leaks,
    // the trace source is never stopped, and the next run inherits it.
    Step("Shut down: AG_Init(AG_EXECITEM | AG_UNINIT)");
    if (keep) {
        Note("--keep given, the session is left up");
    } else {
        const U32 u = g_api.AG_Init(AG_EXECITEM | AG_UNINIT, nullptr);
        if (u == AG_OK) Pass("AG_Init(AG_EXECITEM | AG_UNINIT) = 0");
        else            Fail("AG_Init(AG_EXECITEM | AG_UNINIT) = %u", u);
    }

    // -----------------------------------------------------------------------
    printf("\n");
    if (g_fail) {
        printf("FAILED  (%d problem(s), %d passed)\n", g_fail, g_pass);
        return 1;
    }
    printf("ALL PASSED  (%d checks)\n", g_pass);
    return 0;
}
