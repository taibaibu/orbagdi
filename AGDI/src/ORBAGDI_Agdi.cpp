/**
 * @file ORBAGDI_Agdi.cpp
 * @brief AGDI export layer - the surface Keil uVision talks to.
 *
 * The build product is ORB_AGDI.dll; uVision loads it from ARM\BIN as an AGDI
 * target driver and drives a Cortex-M target through the classes below
 * (RDDI -> DAP -> Core/Breakpoints).
 *
 * All entry points follow AGDI.H. The prototypes are pulled in with _IN_TARG_
 * defined (see ORBAGDI.h), so AGDI.H declares them __declspec(dllexport) and
 * the definitions here fill them in.
 */
#include "pch.h"
#include "ORBAGDI_Agdi.h"
#include "ORBAGDI_CodeImage.h"
#include "ORBAGDI_DebugAccess.h"
#include "ORBAGDI_Etb.h"
#include "ORBAGDI_Etm.h"
#include "ORBAGDI_Flash.h"
#include "ORBAGDI_Itm.h"
#include "ORBAGDI_OrbTrace.h"
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_TraceFile.h"
#include "ORBAGDI_TracePump.h"
#include "ORBAGDI_TraceWin.h"

#include <map>
#include <string.h>

// uVision's own header: we need struct dbgblk to read dbgblk::TargArgs in
// EnumUvARM7. It is self-contained and pack-balanced, so pull in just this one.
#include "COMTYP.H"

#include "ORBAGDI_V8M.h"

// ---------------------------------------------------------------------------
// Port / AP state for the extended access path (AG_MemAccX, AG_DapAcc).
//
// Those two entry points, like every other AG_* export, are defined at global
// scope further down the file; the state they read is filled in by
// orb::agdi::Connect(), which is inside the namespace below.  The declarations
// therefore have to sit out here.
// ---------------------------------------------------------------------------
void SetDapPortType(bool jtag);
void SetApAccSizes(int dp, int ap, U32 sizes);

// AP supported access sizes (DbgCM/Debug.h:179-182).
#define AP_ACCSZ_BYTE  1u
#define AP_ACCSZ_HWORD 2u
#define AP_ACCSZ_WORD  4u

namespace orb {
namespace agdi {

// uVision-visible breakpoint attribute bits that matter for Cortex-M.
static const uint16_t kAttrBpMask = static_cast<uint16_t>(
    ATRX_EXEC | ATRX_READ | ATRX_WRITE | ATRX_WATCH | ATRX_BREAK | ATRX_BPDIS);

/// Name of an AG_BpInfo command code, for the breakpoint trace below.
static const char* BpCodeName(U16 nCode)
{
    switch (nCode) {
    case AG_BPQUERY:     return "AG_BPQUERY";
    case AG_BPEXQUERY:   return "AG_BPEXQUERY";
    case AG_CADRVALID:   return "AG_CADRVALID";
    case AG_CANHANDLEBP: return "AG_CANHANDLEBP";
    case AG_BPENABLE:    return "AG_BPENABLE";
    case AG_BPDISABLE:   return "AG_BPDISABLE";
    case AG_BPKILL:      return "AG_BPKILL";
    case AG_BPSET:       return "AG_BPSET";
    case AG_BPDISALL:    return "AG_BPDISALL";
    case AG_BPKILLALL:   return "AG_BPKILLALL";
    default:             return "AG_BP?";
    }
}

SUPP g_supp;   // (re)filled by AG_Init(AG_INITFEATURES)

namespace {

// 4-byte granularity address -> attribute word.
//
// The value is a DWORD, not a WORD, because AG_MemAtt(AG_GETMEMATT) hands
// uVision a *pointer* to it and uVision dereferences that as a DWORD* - Keil's
// own MGetAttr() returns exactly that, a pointer into its per-address attribute
// table (DbgCM/AGDI.CPP:329, returned at :5265).  Returning a bit mask in
// pA->Adr, as this used to, gives uVision a small integer it may follow as a
// pointer.
struct AttrStore {
    CRITICAL_SECTION             cs;
    std::map<uint32_t, uint32_t> m;
    AttrStore() { InitializeCriticalSection(&cs); }
};

AttrStore& Attrs() {
    static AttrStore s;   // C++11 magic static: thread-safe construction
    return s;
}

uint32_t AttrGet(uint32_t adr) {
    AttrStore& a = Attrs();
    EnterCriticalSection(&a.cs);
    const std::map<uint32_t, uint32_t>::const_iterator it = a.m.find(adr & ~3u);
    const uint32_t v = (it == a.m.end()) ? 0 : it->second;
    LeaveCriticalSection(&a.cs);
    return v;
}

void AttrSet(uint32_t adr, uint32_t v) {
    AttrStore& a = Attrs();
    EnterCriticalSection(&a.cs);
    a.m[adr & ~3u] = v;
    LeaveCriticalSection(&a.cs);
}

/// Stable pointer to the attribute word for 'adr'.
///
/// Stable for the life of the process - see AttrClear().  std::map nodes are
/// never moved by later inserts, which is what makes this safe to hand out.
uint32_t* AttrPtr(uint32_t adr) {
    AttrStore& a = Attrs();
    EnterCriticalSection(&a.cs);
    uint32_t* p = &a.m[adr & ~3u];
    LeaveCriticalSection(&a.cs);
    return p;
}

void AttrClear() {
    AttrStore& a = Attrs();
    EnterCriticalSection(&a.cs);
    // Zero the words rather than erase the nodes.  AG_GETMEMATT has handed
    // uVision pointers into this map and Keil's own table is fixed-size for
    // exactly that reason, so erasing a node would leave uVision holding a
    // dangling one.  Growth is therefore bounded by the number of distinct
    // addresses uVision ever attributes - the breakpoint and watchpoint count -
    // and not by time, which is the property that matters.
    for (std::map<uint32_t, uint32_t>::iterator it = a.m.begin(); it != a.m.end(); ++it) {
        it->second = 0;
    }
    LeaveCriticalSection(&a.cs);
}

// Memory windows uVision mapped through AG_MemAtt(AG_MEMMAP).
//
// Keil keeps a full attribute table and maps segments on demand; we only need
// the ranges, because the attribute word itself lives in AttrStore and the range
// answers the other half of the question - "is this address mapped at all?" -
// which is what AG_BpInfo(AG_CADRVALID) actually asks (DbgCM/AGDI.CPP:7537 maps
// an unmapped address and fails if it is still unmapped afterwards).
struct MemMapStore {
    CRITICAL_SECTION cs;
    struct Range { uint32_t start, end, attr; };
    Range            r[64];
    int              n;
    MemMapStore() : n(0) { InitializeCriticalSection(&cs); }
};

MemMapStore& MemMap() {
    static MemMapStore s;
    return s;
}

void MapMem(uint32_t adr, uint32_t len, uint32_t attr) {
    if (len == 0) return;
    MemMapStore& m = MemMap();
    EnterCriticalSection(&m.cs);
    for (int i = 0; i < m.n; ++i) {
        if (adr >= m.r[i].start && adr < m.r[i].end) {   // already covered
            LeaveCriticalSection(&m.cs);
            return;
        }
    }
    if (m.n < 64) {
        m.r[m.n].start = adr;
        m.r[m.n].end   = adr + len;
        m.r[m.n].attr  = attr;
        ++m.n;
    }
    LeaveCriticalSection(&m.cs);
}

void MapClear() {
    MemMapStore& m = MemMap();
    EnterCriticalSection(&m.cs);
    m.n = 0;
    LeaveCriticalSection(&m.cs);
}

/// Is 'adr' inside a window uVision mapped?
///
/// With nothing mapped the answer is "yes".  A project that never calls
/// AG_MemAtt(AG_MEMMAP) has to keep working exactly as before, and answering
/// "no" for every address would refuse every breakpoint in it.
bool Mapped(uint32_t adr) {
    MemMapStore& m = MemMap();
    EnterCriticalSection(&m.cs);
    bool ok = (m.n == 0);
    for (int i = 0; i < m.n && !ok; ++i) {
        if (adr >= m.r[i].start && adr < m.r[i].end) ok = true;
    }
    LeaveCriticalSection(&m.cs);
    return ok;
}

// A "still waiting" line every ten seconds.
//
// AG_GoStep(AG_GOFORBRK / AG_GOTILADR) and AG_WAITFORBRK block for as long as the
// target runs, and they run on uVision's debug thread.  If that thread looks
// hung, the log is the only thing that can say whether the driver is still
// waiting on purpose or wedged - and a silent log is indistinguishable from a
// crash.  The behaviour is unchanged; only its visibility is.
struct WaitHeartbeat {
    DWORD       t0;
    DWORD       last;
    const char* what;

    explicit WaitHeartbeat(const char* w)
        : t0(GetTickCount()), last(t0), what(w) {}

    void Tick() {
        const DWORD now = GetTickCount();
        if (now - last < 10000) return;
        last = now;
        ORB_LOG_INFO("AGDI: %s still waiting (%u s) - the target has not stopped "
                     "and no stop request has arrived",
                     what, (unsigned)((now - t0) / 1000));
    }
};

// %TEMP% - the log must not go into this DLL's own folder, <Keil>\ARM\BIN:
// that tree belongs to uVision and may be read-only, and a log that silently
// fails to open is worse than no log at all.  GetTempPathW resolves TMP, then
// TEMP, then USERPROFILE, and yields a trailing backslash.
bool TempDir(wchar_t* out, size_t cch) {
    const DWORD n = GetTempPathW(static_cast<DWORD>(cch), out);
    return n > 0 && n < cch;
}

// uVision owns the breakpoint list; bpHead points at its head pointer.  Walk it
// to learn whether the breakpoint at 'adr' is a data watchpoint and, if so, what
// access type it triggers on.  The list is uVision's, so it must only be read.
AG_BP* FindBpNode(uint32_t adr) {
    Context& c = Get();
    if (!c.bpHead || !*c.bpHead) return nullptr;
    const uint32_t a = adr & ~1u;
    for (AG_BP* p = *c.bpHead; p; p = p->next) {
        if ((p->Adr & ~1u) == a) return p;
    }
    return nullptr;
}

bool IsWatch(const uint32_t adr, uint16_t& acc) {
    const AG_BP* p = FindBpNode(adr);
    if (!p || p->type != AG_WBREAK) return false;
    acc = p->acc;
    return true;
}

// AG_BP::acc (1 = read, 2 = write, 3 = read/write) -> DWT function code.
uint32_t WatchFunc(uint16_t acc) {
    switch (acc) {
    case 1:  return kDwtWpRead;
    case 2:  return kDwtWpWrite;
    default: return kDwtWpReadWrite;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------
Context::Context()
    : cb(nullptr), mainFrame(nullptr), bpHead(nullptr), bpStart(nullptr),
      curPC(nullptr), loaded(false), connected(false), running(0), stopReq(0),
      uninitReq(0), tclk(0) {}

Context& Get() {
    // Deliberately leaked: uVision may still touch the DLL while the CRT tears
    // statics down, and the RDDI host is already gone by then.
    static Context* c = new Context();
    return *c;
}

// ---------------------------------------------------------------------------
// Serialises the AGDI entry points that talk to the probe.
//
// The reference driver wraps every such entry in a global mutex
// (DbgCM/AGDI.CPP:1271 CheckCom(), its comment reads "prevent from
// multi-threading recursions").  uVision calls AGDI entries from its own thread
// while run control happens on another, and DCRSR/DCRDR are a single shared
// pair: a register poll and a step that overlap read each other's residue.  That
// is how PC came back as 0x00030003 and 0x02000000 - both of them DHCSR's own
// value, i.e. whatever the last AP access had left in the data register.
//
// TryEnter, not a blocking wait: a caller that cannot get the lock is told
// AG_NOACCESS instead of freezing the IDE for the length of a run.  The
// reference does the same thing in effect - it returns AG_NOACCESS before taking
// the lock whenever the target is running.
//
// 抽数移到 worker 之后（Todo.md 的 P0-A）这把锁多了一个用户：worker 每个时间片
// 非阻塞地拿一次，拿不到就让给调试线程。所以它不能是"一个入口从头持到尾"的
// 结构 —— `Release()` 就是为这件事加的，见 RunAndWait 里放开目标之后的那一处。
// ---------------------------------------------------------------------------
CRITICAL_SECTION g_comLock;
struct ComLockInit {
    ComLockInit() { InitializeCriticalSection(&g_comLock); }
} g_comLockInit;

ComLock::ComLock() : m_held(false) { TryLock(); }
ComLock::~ComLock() { Release(); }
bool ComLock::Held() const { return m_held; }

bool ComLock::TryLock() {
    if (m_held) return true;
    m_held = (TryEnterCriticalSection(&g_comLock) != FALSE);
    return m_held;
}

void ComLock::Release() {
    if (!m_held) return;
    m_held = false;
    LeaveCriticalSection(&g_comLock);
}

ComLockWait::ComLockWait(unsigned timeoutMs) : m_held(false) {
    const DWORD t0 = GetTickCount();
    for (;;) {
        if (TryEnterCriticalSection(&g_comLock)) { m_held = true; return; }
        if ((GetTickCount() - t0) >= timeoutMs) return;
        Sleep(1);
    }
}

ComLockWait::~ComLockWait() {
    if (m_held) LeaveCriticalSection(&g_comLock);
}

bool ComLockWait::Held() const { return m_held; }

// ---------------------------------------------------------------------------
// P0-A′：uVision 入口的探针锁等待语义
// ---------------------------------------------------------------------------
// 基准的 `CheckCom(1)` 是 `WaitForSingleObject(Com_mtx, INFINITE)`
// （DbgCM/AGDI.CPP:1275-1277，注释原文 "Wait forever to get mutex"），本驱动原先是
// **非阻塞** `TryEnter`：拿不到就报 "probe busy" 并让入口失败。P0-A 之前锁会被持有
// 一整轮运行，非阻塞是必需的；P0-A 之后 worker 只按 **2 ms 时间片**持锁，于是
// "等一小会儿"既更忠实也更稳 —— 否则有 trace 源在跑时，寄存器/内存窗口会**间歇性**
// 拿到 `AG_NOACCESS`（worker 每 ~2 ms 抓一次锁，谁先到谁赢）。
//
// 为什么是**有限**等待：基准的无限等待在"长持有者"上会挂死 IDE。本驱动的长持有者
// 只有一个 —— Flash 下载期间（`ORBAGDI_Flash.cpp`，写算法/擦除/编程），
// 所以取 250 ms（= worker 时间片的 125 倍，足够；远小于一次擦除）。
//
// 与 `ComLockWait` 的分工：这里要的是"拿到锁之后**还能提前放**"（`AG_GoStep` 的运行
// 路径在 Resume 之后就不再需要独占探针），而 `ComLockWait` 只在析构时放锁。
enum : unsigned { kProbeWaitMs = 250 };

bool LockProbe(ComLock& lock, unsigned timeoutMs)
{
    for (unsigned waited = 0; !lock.Held(); ++waited) {
        if (waited >= timeoutMs) return false;
        Sleep(1);
        lock.TryLock();
    }
    return true;
}

void Callback(U32 code, void* vp) {
    Context& c = Get();
    if (c.cb) c.cb(code, vp);
}

void StatusMessage(const char* text) {
    Callback(AG_CB_MSGSTRING, const_cast<char*>(text));
}

// ---------------------------------------------------------------------------
// Trace plumbing
// ---------------------------------------------------------------------------

// Is any trace source currently feeding us?  Drives the Trace Data window's
// "processing" state, so a project with trace switched off never sees those
// notifications.
static bool TraceSourceActive() {
    return OrbTraceActive() || ItmActive() ||
           (EtbAvailable() && EtbGetStatus().configured);
}

// uVision's Trace-window "Update" (TDI UpdateTraceBuffer): pull whatever the
// source still holds.  The ETB is re-read from the target buffer; orbtrace just
// drains the endpoint again.
//
static bool TraceWinRefresh() {
    Context& c = Get();

    // 流式源（SWO / orbtrace）**不再在这里抽干**（Todo.md 的 P0-A）：抽数已经
    // 移到 worker 线程，窗口要的数据由 worker 连续填进环。原先这一行是"停止后
    // 约 1 s 窗口被清空"的成因 —— 停止后 uVision 仍会来要一次数据，这一次抽干
    // 会把 RDDI 环里积压的几十万条记录一次灌进窗口缓冲，正在显示的那一批被
    // 整个覆写（COMPAT_ANALYSIS.md §10.26 六）。
    //
    // ETB 不属 worker：它是"停机后读回"的一次性动作，仍在调试线程上做。
    if (EtbAvailable() && EtbGetStatus().configured) {
        ComLock lock;
        if (lock.Held()) EtbProcess(c.dap);
    }
    return true;
}

// 采集的开始/结束要与三样东西同步，官方在三处一起做
// （DbgCM/AGDI.CPP:2758-2771，`Trace_Flush` + `SetTraceRunSynch` 成对出现）：
//
//   1. trace 选项掩码的总开关 `TraceOpt & TRACE_ENABLE`——状态栏上报按它分支；
//   2. Trace Data 窗口的"处理中"状态（`SetTraceRunSynch`，本驱动是
//      `TraceWinSetRunning`）；
//   3. 状态栏那一行（`OutTraceMsg`）。
//
// 三处各自去判断"有没有 trace 源"会得出不一致的结果（例如窗口说在处理、状态栏
// 却是空的），所以合并成一个入口。选项掩码对应官方的 `TraceConf.Opt`：本驱动没有
// 那张设置页，改按"实际在跑什么"映射——总开关，加上合成周期号（时间戳）与
// DWT PC 采样，这两样是 Trace Data 窗口始终会产出的记录类型。
static void TraceSetRunning(bool bState, bool bWinUpdate) {
    const bool active = bState && TraceSourceActive();

    // ≙ 官方 0x1008B7C0 @ 0x1009622B（trace 启动）/ @ 0x10095102（停止-复位收尾）：
    // start 与 stop 共用这一个入口，所以**一处插入覆盖官方两个方向**。放函数最前是
    // 有意的：VTR 内容在函数体内不变，放这里就不依赖下面"先同步、后上报"的顺序
    // （那几段都带顺序注释，别把新调用塞进去）。
    RefreshTraceClocks();

    // 采集开始时把 TDI 接口表与窗口配置再发一次。注册只在 Connect 里做一遍，
    // 而窗口是 uVision 收到 UV_TRACE_IF_INIT 之后才建的——紧随其后的
    // UV_TRACE_WIN_CONFIG 可能早于窗口存在。见 TraceWinReconfigure() 的说明。
    if (active) orb::agdi::TraceWinReconfigure();

    // 注意：**不要**在下面同步之前把 running 提前清 0。TraceWinSetRunning() 里
    // 有 `if (s.running == bState) return;`，提前清会让随后的停止状态通知被整个
    // 吞掉（实测：日志里 `上报状态 processing=0` 那行直接消失）。
    // "采集期间被来要数据"这条改由 QueryRange 自己放宽，见该函数。

    // UV4.exe 的取数门里有一道 `app[0xafc] & 8`（"trace 数据展示已启用"），
    // 自定义驱动走不到 uVision 自己置它的那条路。这里在发状态**之前**声明一次，
    // 否则 uVision 处理状态时那道门仍是关的。见 TraceWinAssertTraceDisplay()。
    orb::agdi::TraceWinAssertTraceDisplay(true, active ? "采集开始" : "停止");

    // 停止时先做视图同步、再发状态。顺序要紧：同步负责告诉 uVision "当前是
    // 哪个周期"，随后的状态通知（bSkipViewUpdate=0）才是"去把这个周期显示出来"
    // 的指令 —— 反过来的话 uVision 先被要求刷新，此时它还不知道该看哪一段，
    // 实测结果就是只查了 GetFirstCycle/GetLastCycle 便停下，不再取记录。
    if (!bState) orb::agdi::TraceWinSyncViews(TraceWinLastCycle());

    // TraceOpt 的组成照 DbgCM/Trace.cpp 的 `Trace_Setup()`：总开关 + 时间戳 + PC 采样，
    // **再加上 Trace 页那 6 个事件复选框**（`events=` 存的 TraceOpt 事件位）与
    // "ETM Trace Enable" / "Unlimited Trace" 两位。窗口按这些位给记录分类，少一位
    // 窗口就不显示那一类（过滤器已经预留了 Event / Exception Trace）。
    if (active) {
        const orb::cfg::Config& t = orb::cfg::Get();
        uint32_t topt = kTraceEnable | kTraceTimestamp | kTracePcSample |
                        (static_cast<uint32_t>(t.traceEvents) & kTraceEventMask);
        if (t.etmEnable)      topt |= kEtmTrace;
        if (t.unlimitedTrace) topt |= kUnlimitedTrace;
        TraceWinSetOptions(topt);
    } else {
        TraceWinSetOptions(0);
    }

    // 时间列开关（P0-F）。**默认关**：`bHideTimestamps` 同时是 uVision 取数门的
    // 一环，打开可能让窗口一行都不取（见 ORBAGDI_TraceWin.h 的说明）。
    // 真实时间轴不依赖它 —— 那是 ITM 时间戳包 + TRACE_CLK 的事。
    orb::agdi::TraceWinSetTimestampsVisible(orb::cfg::Get().tsWin);

    // 把 trace 配置上报给 uVision —— AG_CB_CFG_TRC（37，"Debug Trace Configuration
    // Update"）。ULP2CM3 在 trace 启动/停止时发它（发送点 0x1009113B，结构体填充
    // 函数 0x1008CD00），本驱动此前完全没有这个通道，现按机器码逐字段补齐。
    //
    // 结构体（逐字段取自 ULP2CM3 机器码）：
    //   +0x00 = 1
    //   +0x04 = 配置值 A
    //   +0x08 = 配置值 B
    //   +0x0C = 位域：bit0 = 时间戳，bit1 = 异常 trace，bit2 = trace 已启用
    //
    // 注意：**它不是 Trace Data 窗口不出数的原因**（那条是 BOM 实例取错，见
    // bug.md）。补齐它只是为了与官方驱动保持一致，避免留下未知差异。
    orb::agdi::Context& c = orb::agdi::Get();
    if (c.cb) {
        struct {
            DWORD f0;
            DWORD f4;
            DWORD f8;
            DWORD fC;
        } cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.f0 = 1;
        // ULP2CM3 这两格取它自己的配置全局量。本驱动没有对应的两份独立时钟，
        // 统一用 uVision 交下来的调试时钟（c.tclk）—— 它们只影响 uVision 侧对
        // 配置的描述，真正的时间基准走 TRACE_CLK 虚拟寄存器。
        cfg.f4 = c.tclk;
        cfg.f8 = c.tclk;
        if (active) cfg.fC |= 1u << 2;           // TRACE_ENABLE
        if (active) cfg.fC |= 1u << 0;           // TRACE_TIMESTAMP
        c.cb(AG_CB_CFG_TRC, &cfg);
        ORB_LOG_INFO("AGDI: 上报 trace 配置 AG_CB_CFG_TRC (启用=%d, 位域=0x%X)",
                     active ? 1 : 0, (unsigned)cfg.fC);
    }

    TraceWinSetRunning(active, bWinUpdate);
    TraceWinStatusMsg(active ? kTMsgRun : 0);
    // 上面那句只是排队（抽数 worker 也会调它，见 TraceWinStatusMsg 的说明）。
    // 这里是调试线程，投递就在这一行 —— 状态变化的那一行文本不能等到下一次
    // 轮询才出现。
    orb::agdi::TraceWinStatusFlush();

    // 最后指名要显示的区间：bExplicitRange + m_pRecordRange。不指名时 uVision
    // 只问范围就不动了（实测），指名之后它必须取这段的记录才能画出来。
    if (!bState) {
        const uint64_t last = orb::agdi::TraceWinLastCycle();
        if (last) orb::agdi::TraceWinShowRange(orb::agdi::TraceWinFirstCycle(), last);
        // 停止流程走完后重新确认 bTrcDisp（只置位，不越界）。
        orb::agdi::TraceWinAssertTraceDisplay(true, "停止");
        // 只读地把取数门链的当前值打出来：是哪道门关着，一眼可见。
        orb::agdi::TraceWinReadHostGates("停止");
    }
}

// ---------------------------------------------------------------------------
// Session control
// ---------------------------------------------------------------------------

// Official SevereTargetError() ends in SetPlayDead(msg).  This driver has no
// global PlayDead state - its equivalent is "the session is no longer
// accessible" - so the teardown is hooked in here: report, then mark the session
// dead so every entry point answers AG_NOTARGET from now on.
static void V8mPlayDead(const char* msg) {
    Context& c = Get();
    ORB_LOG_ERROR("AGDI: fatal target error - %s", msg ? msg : "(unknown)");
    StatusMessage(msg ? msg : "Fatal target error - the session was stopped");
    c.connected = false;
}

/// 给 `CodeImagePreloadFlash()` 用的读函数（P1-0 的会话缓存预读）。
///
/// ⚠ 放在**文件作用域**，不能写成 `Connect()` 里的局部类 —— C++ 的局部类不许有
///   静态成员函数，编译不过。
///
/// 调用时机是 Connect（抽数 worker 还没起、探针锁不忙），所以这里**不取锁**，
/// 与 `ORBAGDI_Etm.cpp` 的 `ReadTargetMem` 同理。
static bool ReadMemForCodeImage(void* ctx, uint32_t addr, uint8_t* out, unsigned len) {
    orb::Dap* d = static_cast<orb::Dap*>(ctx);
    return d && d->IsConnected() && d->MemRead(addr, out, len);
}

/// 按当前配置打开两份落盘文件（`trfile=` / `trrec=`，都默认关）。
///
/// 抽出来是因为它有**两个**调用点（`Connect()` 与 reset 清理），两处必须用
/// **同一套**规则，否则 reset 之后这一段就完全不落盘了。
static void TraceFileOpenFromConfig()
{
    const orb::cfg::Config& tc = orb::cfg::Get();
    // trace 落盘（P2-1）：`trfile=` 没给就是关的（不落盘）。
    // `trrec=1` = 日志所在目录（%TEMP%）下自动命名。
    // ⚠ 2026-10-14（P3-1）之后落盘**不再卡抽数**：写满的块交给后台写线程
    //   （`RetirePush`，4 槽交接队列），抽数只交指针。所以"默认都不开"这条
    //   现在只是**默认策略**（不往机器盘上偷偷写文件），不再是性能顾虑。
    //   `trrecmb=` 给大小上限，默认 2 GB。
    const unsigned capMB = tc.trRecMB > 0 ? static_cast<unsigned>(tc.trRecMB) : 0u;
    const unsigned keep  = tc.trKeep > 0 ? static_cast<unsigned>(tc.trKeep) : 0u;
    orb::agdi::TraceFileOpen(tc.trFile, keep, capMB);
    // 记录日志（P2-2）：内存窗口之外能回看的那一份（紧凑记录，查询不重解）。
    orb::agdi::TraceFileOpenRecords(tc.trRec, keep, capMB);
}

/// "从头开始"：上一轮的 trace 数据**全部作废** —— 内存缓冲、周期轴基准、
/// 导航分片、以及**落盘缓存文件**。
///
/// 调用点有两个，语义相同：
///   * `Connect()` —— 新会话开始；
///   * `AG_RESET`  —— **用户按 Reset，或 Keil 在异常时自己发的那一发**。
///     用户 2026-10-14 的原话："reset 是个特殊操作，由使用者或者 keil 异常才能
///     发出的功能" ⇒ 清空必须挂在**事件**上，不能挂在"用户点了某个菜单"上。
///
/// ⚠ **顺序不能反**：必须先关删文件，再清内存，最后重开。
///   `TraceFileOpen*()` 是幂等的（已开着再调只返回 true），所以若先开再删，
///   删掉的正是刚打开的那份，这一段就静默不落盘了。
///
/// ⚠ 落盘文件是**删**不是截断 —— 用户裁定：它只是缓存，真交付是从 Keil 导出。
static void TraceResetForTarget(const char* why)
{
    const unsigned nFiles = orb::agdi::TraceFileCloseAndDelete(why);
    orb::agdi::TraceWinClear();
    // ≙ 官方 `0x1008B7C0` @ `0x10095102`（"停止-复位收尾"）的**补充**收敛点。官方 3 个
    //   调用点里，事件驱动那条只覆盖到 conf 应用与 trace 启动/停止（见 `TraceSetRunning`），
    //   而"清空 / 复位"不是 run 状态变化（`TraceWinSetRunning` 会早退）⇒ 这里缺一个发点。
    //   幂等 + null-safe：重复调用只是重复写同一个值。
    orb::agdi::RefreshTraceClocks();
    TraceFileOpenFromConfig();
    ORB_LOG_INFO("AGDI: trace 已按 reset 语义清空（%s）—— 落盘缓存删掉 %u 个，"
                 "内存缓冲与周期轴已归零",
                 why ? why : "?", nFiles);
}

/// 复位后的"trace 重新武装"（2026-10-15）。
///
/// 为什么必须有它：目标侧 trace 的**全部**寄存器都归复位管 —— `DEMCR.TRCENA`、
/// TPIU（CSPSR / SPPR / ACPR / FFCR）、`ITM_TCR`、DWT、ETMCR，以及器件描述里那条
/// TraceStart 序列（`DBGMCU_CR` + 引脚复用）。它们的编程只发生在 `Connect()` 里，
/// 而 `Connect()` 第二句就是 `if (c.connected) return true;`。于是"跑一次 →
/// 停止 trace → 按 Reset → 再跑"的第二次：**目标一个 trace 包都不发**，记录缓冲
/// 不再增长，uVision 的 Navigation 条就停在上一次的范围上 —— 正是用户报告的现象
/// （"复位重新跑 stop 后导航条还是之前的数据，没有新的"）。
///
/// 官方在**同一个位置**做同一件事：`ResetTarget()` 每次复位后都写
/// `DEMCR = TRCENA | MONCONF_RST_VECT_CATCH`（`DbgCM/AGDI.CPP:2550`，注释
/// "Enable Trace"）；trace 寄存器子系统则由 `Trace_Recovery()`（= `Trace_Setup()`，
/// `DbgCM/Trace.cpp`）重配。本层把两者合成一件事：照"上次编进目标的那份配置"
/// 原文重放（`ItmRearmTarget`），它内部就写 `DEMCR |= TRCENA`。
///
/// ⚠ 两件"顺手"的事都不能做：
///   * **不重启**探针/宿主的采集会话 —— OrbTrace 流与 SWO 引擎本来就没停，
///     重开会把已经收到的字节丢掉；
///   * **不清**记录缓冲 —— 2026-10-14 裁定"B：撤掉复位清空"，复位后的新记录接在
///     旧的之后，Navigation 条随之变长。
///
/// ETB 那条源不在这里：它的武装本来就是**每次运行前**做的（`GoStep()` 里的
/// `ItmArmForEtb + EtbRecovery + EtbActivate`），复位后的第一次运行自然补上；
/// 在这里再补一遍只会与那里的顺序打架。
static void RearmTargetTraceAfterReset(const char* why)
{
    Context& c = Get();

    // 判据是"**哪条源正在采集**"，不是选项里写了什么：复位只对已经武装上的那条
    // 有意义（Trace 页关着时这里什么都不做，与 Connect 期间那套判断同源）。
    const bool orbSource = orb::agdi::OrbTraceActive();
    const bool swoSource = orb::agdi::ItmActive();
    if (!orbSource && !swoSource) return;

    const bool ok = orb::agdi::ItmRearmTarget(c.dap);

    // 器件描述那条 TraceStart 序列（DBGMCU_CR / 引脚 / TPIU）同样被复位清掉了，
    // 与 Connect 尾部同一口径重跑一次。序列不存在不算失败（RunDebugSeq 的契约）。
    const bool seq = orb::pdsc::TraceStart();

    ORB_LOG_INFO("AGDI: 复位后重新武装 trace（%s）：源=%s，目标侧寄存器=%d，"
                 "TraceStart 序列=%d",
                 why ? why : "?", orbSource ? "orbtrace" : "SWO",
                 ok ? 1 : 0, seq ? 1 : 0);
}

bool Connect() {
    Context& c = Get();
    if (c.connected) return true;

    // 新会话：清掉上一次卸载留下的标志（见 Context::uninitReq）。
    InterlockedExchange(&c.uninitReq, 0);

    if (!c.dap.Connect("CMSIS-DAP")) {
        ORB_LOG_ERROR("AGDI: cannot open the RDDI/CMSIS-DAP session");
        return false;
    }

    // From here the DP answers, so the description's debug sequences may run:
    // they reach the target through AG_MemAccX / AG_DapAcc, and those entries
    // refuse with AG_NOTARGET unless the session is marked connected.  So
    // `connected` means "the DP is up and the target may be accessed", which is
    // already true before the core is attached.
    c.connected = true;

    // The extended access path validates every AG_MemAccX access against the AP
    // context: ACCMX_QUERY_ACC answers out of it, and a zero entry makes every
    // memory access fail with ACCMX_ERR_ACC_SIZE.  The reference fills it while
    // identifying the AP (DbgCM/SWD.CPP); here the AP is known at connect time.
    // An AHB-AP and an AXI-AP both serve byte, half-word and word accesses
    // through CSW.SIZE, which is what the context reports.
    //
    // DP_Type: this connect path drives Serial Wire Debug.  A session on the JTAG
    // port would need this set from the connected interface (CMSIS_DAP_Connect
    // reports 1 = SWD / 2 = JTAG); only the JTAG-only DAP commands depend on it.
    SetDapPortType(false);
    SetApAccSizes(c.dap.DapIndex(), c.dap.ApIndex(),
                  AP_ACCSZ_BYTE | AP_ACCSZ_HWORD | AP_ACCSZ_WORD);

    // v8-M Security Extensions.  The reference detects them right after the first
    // reset (DbgCM/AGDI.CPP:17983, reached from AG_INITFEATURES); here the DP is
    // already answering, so it happens at connect.  For a v7-M part this answers
    // "no Security Extensions" after a single CPUID read.
    {
        uint32_t cpuid = 0;
        if (c.dap.MemRead32(orb::kScsCpuid, cpuid)) {
            const unsigned partNo = (cpuid >> 4) & 0xFFFu;
            const unsigned cpu    = orb::agdi::V8mCpuTypeFromPartNo(partNo);
            if (orb::agdi::IsV8M(cpu)) {
                if (!orb::agdi::V8mDetectSecurityExtensions(c.dap, cpu)) {
                    ORB_LOG_WARN("AGDI: v8-M security extension detection failed");
                } else {
                    const orb::agdi::V8mStatus vs = orb::agdi::V8mGetStatus();
                    ORB_LOG_INFO("AGDI: v8-M CPU (part 0x%03X) SecureExt=%d "
                                 "CPU=%s SDbgEna=%d NSDbgEna=%d",
                                 partNo, vs.secureExt ? 1 : 0,
                                 vs.cpuSecure ? "Secure" : "Non-Secure",
                                 vs.sDbgEna ? 1 : 0, vs.nsDbgEna ? 1 : 0);
                }
            }
        }
    }

    // SevereTargetError()'s fatal path is delegated to this hook.
    orb::agdi::V8mSetPlayDeadHook(&V8mPlayDead);

    // The device's own debug description, when it has one.  This group is what
    // makes a part with a locked debug port attachable at all: DebugPortSetup
    // prepares the port, DebugPortStart powers it, DebugDeviceUnlock writes the
    // magic value that opens it.  They have to run *before* the core is touched -
    // Attach() reads CPUID and halts, and both are refused while the port is
    // locked.  A description with none of them leaves this path exactly as it was.
    if (orb::pdsc::DebugSeqReady()) {
        ORB_LOG_INFO("AGDI: running the description's debug sequences (%u present)",
                     (unsigned)orb::pdsc::DebugSeqCount());
        if (!orb::pdsc::ConnectDebugPort())
            ORB_LOG_WARN("AGDI: DebugPortSetup sequence failed");
        if (!orb::pdsc::StartDebugPort())
            ORB_LOG_WARN("AGDI: DebugPortStart sequence failed");
        // A failure here is reported but not fatal: plenty of descriptions carry
        // an unlock that does nothing, and Attach() below fails loudly enough if
        // the port really is still locked.
        if (!orb::pdsc::UnlockDevice())
            ORB_LOG_WARN("AGDI: DebugDeviceUnlock sequence failed - attaching anyway");
    }

    // "Connect: without Stop" (Debug page): attach and leave the target running.
    // Everything downstream that assumes a stopped core has to know, which is
    // what c.running says.
    const bool withoutStop = (cfg::Get().connect == cfg::kConnectWithoutStop);

    if (!c.core.Attach(&c.dap, !withoutStop)) {
        ORB_LOG_ERROR("AGDI: core attach failed");
        orb::pdsc::StopDebugPort();
        c.connected = false;
        c.dap.Disconnect();
        return false;
    }

    // "Configure device specific logic for debug/trace", now that the core is
    // under our control.
    orb::pdsc::StartCore();

    // The device-side half of surviving a lost debug connection.  A description
    // that carries RecoverySupportStart is saying the device needs it for its
    // debug port to outlive a low-power mode, so it is run whenever it exists;
    // RecoveryStop() undoes it on the way out.
    orb::pdsc::RecoveryStart();

    c.bps.Init(&c.dap, &c.core);
    c.bps.ProbeFpb();
    c.bps.ProbeDwt();
    ORB_LOG_INFO("AGDI: target attached (%d FPB comparators, %d DWT watchpoints)",
                 c.bps.HwCapacity(), c.bps.WatchCapacity());

    c.running   = withoutStop ? 1 : 0;
    c.stopReq   = 0;
    c.connected = true;
    if (withoutStop) {
        ORB_LOG_INFO("AGDI: 'without Stop' - the target is left running");
        orb::ui::Info("Connected without stopping - the target is running.");
    }

    // Trace capture (off unless the project asked for it).  Three mutually
    // exclusive sources, in priority order:
    //   orbtrace=1  - the probe's own trace endpoint (parallel port / SWO),
    //                 OFLOW/COBS framed (ORBAGDI_OrbTrace.cpp)
    //   etb=1       - the on-chip ETB, read back over the MEM-AP
    //   swo=<baud>  - the SWO pin through the RDDI host's SWO engine
    // A refusal is soft and only leaves trace disabled.
    HistReset();
    EtbReset();
    EtmTraceReset();
    // P1-0 的代码镜像：生命周期是"一次会话"，所以清在这里（Connect）。
    // ⚠ **不能**放 `EtmTraceReset()` 里 —— 那个函数每次开流（OrbTraceStart）也会调，
    //   而镜像可能是刚 Download 存下来的，清了就每次开流都退化成没镜像。
    CodeImageClear();

    // 清完立刻**预读一次**器件声明的 Flash（P1-0 的主来源，会话缓存）。
    //
    // 为什么放在这里：这是"连接之后、开 trace 之前"的唯一稳定点 —— 探针在（能读）、
    // 抽数 worker 还没跑起来（探针锁不忙）。读的范围是 `cfg::Effective()` 的
    // `iromStart/iromSize`（uVision 报的器件描述），所以每一次读都落在真实映射的内存上。
    //
    // 这一步让"代码镜像有没有"**不再取决于 uVision 什么时候 Download** ——
    // 原来那个设计把驱动内部缓存的生命周期推给用户去对齐 uVision 的时序，是错的。
    CodeImagePreloadFlash(&ReadMemForCodeImage, &c.dap);
    // 按目标内核选 ETM 解码引擎（M3/M4 → ETMv3.5，M7/M33 → ETMv4，
    // M0+/M23 → MTB）。放在 EtmTraceReset() 之后：reset 会销毁引擎，
    // 这一步决定重建时用哪一套。读不到 CPUID 就保持默认（ETMv3.5）。
    EtmTraceSelectProtocol(c.dap);
    // 记录缓冲的规模（块链）：必须在 TraceWinInit() 之前给 —— 上限决定索引数组
    // 的大小，那是一次性分配的。默认（选项为 0）= 4 MB/块、上限物理内存 1/4 封顶
    // 1 GB，见 TraceWinSetBuffer()。
    // `trmax=`（记录条数上限）默认 0 = 不限：原来那个硬编码的 1<<20 在 ETM 下把
    // "能回看多久"钉死在 0.13 s，静默吃掉一整段采集（见 Todo.md §4.3）。
    {
        const orb::cfg::Config& tc = orb::cfg::Get();
        TraceWinSetBuffer(tc.trChunkMB > 0 ? static_cast<unsigned>(tc.trChunkMB) : 0u,
                          tc.trMemMB   > 0 ? static_cast<unsigned>(tc.trMemMB)   : 0u,
                          tc.trMaxRecs > 0 ? static_cast<unsigned long long>(tc.trMaxRecs) : 0ull);
    }
    // 新会话 = 从头开始：清内存缓冲、删掉上一轮的落盘缓存、按配置重开新文件。
    // 顺序与理由见 TraceResetForTarget()（**先关删，再清内存，最后重开**）。
    TraceResetForTarget("connect");

    // uVision's Trace Data window.  Registered once the BOM is available; when
    // it is not, the ITM / printf viewers still work and only this window stays
    // dark.
    TraceWinInit();
    TraceWinSetRefreshHook(&TraceWinRefresh);
    // 把配置串注册进项目（UV_NEW_TARG_SETTINGS）。官方与 ULP2CM3 都在会话里做
    // 这件事；本驱动此前只靠设置页的返回值，从不主动注册。
    TraceWinPublishSettings();
    // 声明 uVision 侧的"trace 数据展示已启用"（app[0xafc] bit3）。这是 Trace Data
    // 窗口取记录的硬门之一，自定义驱动必须自己声明 —— 详见该函数的说明。
    TraceWinAssertTraceDisplay(true, "Connect");

    const cfg::Config& opt = cfg::Get();

    // The SWO bit clock.  An explicit swo= wins; otherwise the Trace page's
    // "Trace Clock" and "SWO Clock Prescaler" are divided down (cfg::SwoBaud).
    // That pair used to be parsed and then ignored, because nothing ever called
    // SwoBaud() (bug.md §I2).
    // "Is the SWO pin the selected source?"  `swo=` is the only thing that says
    // so: the Trace page writes swoBaud for "SWO pin" and 0 for the other two
    // sources (ORBAGDI_Settings.cpp, ReadTrace).  The Trace Clock / prescaler
    // pair is a *derived bit clock*, not a request - treating it as one made
    // "Trace: off" start the RDDI SWO engine whenever tclk= was set, and the
    // probe then refused the nonsense rate it was handed.
    //
    // "Trace Enable" (`tren=`, the Trace page's master switch) is the gate over
    // all three: with it off nothing starts, whatever the source flags say.
    // These three booleans are the only places the source choice is read, so the
    // gate cannot end up applied in one branch and forgotten in another.
    const bool orbSource = opt.traceEnable && opt.orbtrace;
    const bool etbSource = opt.traceEnable && opt.etb;
    const bool swoSource = opt.traceEnable && (opt.swoBaud > 0);

    // 同时给了多个源时下面那串 `if / else if` 按 **orbtrace > ETB > SWO** 取值，另外
    // 两个静默不生效。设置页不会造出这种串（它三个键都显式赋值），所以这只会来自
    // 手改的选项串或过期的 TOOLS.INI —— 那种情况下"我明明选了 ETB"是最难自查的一类：
    // 日志里只有 orbtrace 在跑，看不出是"没选上"还是"选了没生效"。说一句。
    if (static_cast<int>(orbSource) + static_cast<int>(etbSource) +
        static_cast<int>(swoSource) > 1) {
        ORB_LOG_WARN("AGDI: 同时给了多个 trace 源（orbtrace=%d etb=%d swo=%d）—— "
                     "按 orbtrace > ETB > SWO 只启用 **%s**，其余两个不生效",
                     opt.orbtrace ? 1 : 0, opt.etb ? 1 : 0, opt.swoBaud,
                     orbSource ? "orbtrace" : (etbSource ? "ETB" : "SWO"));
    }

    // The derived value is still the one the orbtrace SWO path uses when trbaud=
    // is absent.  That SWO travels on the probe's own trace channel, not through
    // the RDDI SWO engine, so it is a different thing - do not conflate the two.
    int swoBaud = opt.swoBaud;
    if (swoBaud <= 0) swoBaud = cfg::SwoBaud(opt);

    // Plausibility.  A bit clock faster than the core clock means the two values
    // were entered in different units: this page takes kHz, Keil's own Trace Clock
    // box takes a period in ns, and mixing them is a factor of a million.  Without
    // this check that mistake produces a silent ITM console - the hardest kind of
    // failure to attribute.
    // The core clock is the anchor here **deliberately**: it is the Trace page's
    // core-clock value (the `CORE_CLK` VTR / the `cpu=` option) and is not
    // derived from `tclk`, so it still catches a mistyped `tclk`.
    // (It is **not** the debug block's oscillator: `cpuKhz` is not `dbgblk::Clock`
    // and the two must not be conflated - see ORBAGDI_Config.cpp.)
    // Whether the SWO bit clock really divides the
    // core clock or an independent TRACECLKIN is the undecided H1/H2 question
    // (`cfg::SwoAcprBaseKhz()`) - this bound is valid either way for a
    // factor-of-a-million slip.
    // Only worth complaining about a rate that a source will actually use: with
    // trace off the derived value is unused, and warning about it was pure noise
    // (it fired on every "Trace: off" session that had tclk= set).
    if ((swoSource || orbSource) && swoBaud > 0 && opt.cpuKhz > 0) {
        const int64_t coreHz = static_cast<int64_t>(opt.cpuKhz) * 1000;
        if (swoBaud > coreHz) {
            orb::ui::Error("SWO bit clock %d Hz is faster than the core clock "
                           "%d Hz (Trace page says %d kHz). One of the two is in "
                           "the wrong unit - the trace clock here is a frequency "
                           "in kHz, not a period in ns. ITM/SWO is left off.",
                           swoBaud, static_cast<int>(coreHz), opt.cpuKhz);
            swoBaud = 0;
        } else if (swoBaud > coreHz / 2) {
            #if 0
            orb::ui::Warn("SWO bit clock %d Hz is more than half the core clock "
                          "(%d Hz) - the TPIU cannot sustain that for NRZ, so "
                          "expect a garbled ITM stream.",
                          swoBaud, static_cast<int>(coreHz));
                          
            #endif
        }
    }

    // ---- 告诉器件 pack"目标要怎么配 trace 输出"（`__traceout`）---------------
    //
    // 这是驱动唯一该做的 trace 物理层动作：真正的寄存器编程（GPIO/AF/`DBGMCU_CR`）由
    // pack 的 `EnableTraceSWO` / `EnableTraceTPIU` 完成 —— 器件厂写的、带参数、读-改-写。
    // **以前这里什么都不做、`__traceout` 写死 0**，于是 pack 的 TraceStart 空转，引脚配置
    // 只能靠固件手写顶着，改个端口宽度还得重烧固件。见 COMPAT_ANALYSIS.md §10.28。
    {
        uint32_t tr = 0;
        if (orbSource) {
            if (opt.trSwoMode > 0) {
                tr = 0x1u;                                   // bit0: SWO
            } else {
                const int w = (opt.trWidth == 1) ? 1 : (opt.trWidth == 2) ? 2 : 4;
                tr = 0x2u | (static_cast<uint32_t>(w) << 16); // bit1: TPIU + 位宽
            }
        } else if (swoSource) {
            tr = 0x1u;                                       // bit0: SWO
        }
        // ETB：sink 在片内，不占 trace 引脚 ⇒ bit0/bit1 都为 0
        orb::pdsc::SetTraceOut(tr);
        ORB_LOG_INFO("AGDI: __traceout = 0x%05X (SWO=%d TPIU=%d width=%d)",
                     (unsigned)tr, (tr & 0x1u) ? 1 : 0, (tr & 0x2u) ? 1 : 0,
                     (int)((tr >> 16) & 0x3Fu));
    }

    // ---- 周期轴的机制按**实际选中的源**分两套（2026-10-14 用户裁定）----------
    //
    //   ITM / SWO  —— 数据量小，轴**追墙钟**（可以灵活地按时间追踪）；
    //   ETM        —— 数据量太大（实测 4.9 M 条/s），追不起 ⇒ 轴只按解码推进。
    //
    // 判据是"这个源是不是**只有 ITM/SWO 一条通道**"：
    //   * RDDI 的 SWO 引擎（`swoSource`）—— 只有 ITM；
    //   * orbtrace 的 SWO 档（`orbSource && trSwoMode > 0`）—— 只有 ITM；
    //   * orbtrace 的并口档（`trSwoMode == 0`）与 ETB —— 走 TPIU，含 ETM。
    // ⚠ 与 BOUNDS 无关：两种机制下 uVision 的取数区间都由 `StatusUpdate()` 的
    //   `m_pRecordRange` 给死（官方做法）。这里只决定"轴要不要追墙钟"。
    {
        const bool itmOnly = swoSource || (orbSource && opt.trSwoMode > 0);
        orb::agdi::TraceWinSetAxisMode(itmOnly ? orb::agdi::kAxisModeItm
                                               : orb::agdi::kAxisModeEtm);
    }

    if (orbSource) {
        const int baud = (opt.trBaud > 0) ? opt.trBaud : swoBaud;

        // The probe only captures and formats; the target's own trace hardware
        // has to be programmed over SWD by us.  Do it **before** the probe
        // starts listening, or the packets emitted in between are lost.
        TraceSetup setup;
        if (opt.trSwoMode > 0) {
            setup.mode   = (opt.trSwoMode == 2) ? TraceSetup::SwoManchester
                                                : TraceSetup::SwoNrz;
            setup.width  = 0;
            setup.baudHz = baud;
        } else {
            setup.mode   = TraceSetup::Parallel;
            setup.width  = opt.trWidth;
            setup.baudHz = 0;
        }
        // ACPR 的基数走单点 `SwoAcprBaseKhz()`（当前 = 核心时钟；HCLK vs 独立
        // TRACECLKIN 未判别，见该函数）。这条路与 ItmStart 那条共用同一口径。
        setup.acprBaseKhz = orb::cfg::SwoAcprBaseKhz(opt);
        ItmProgramTarget(c.dap, setup);

        // 并口位宽只在并行模式下有意义。SWO 模式下把它一起传下去，会让探针先按
        // 4-bit 配一遍再被 SWO 覆盖 —— 结果虽对，但中间那一步的日志会误导人
        // （"width=4 swo=1"），排查时很容易被带偏。
        const int width = (opt.trSwoMode > 0) ? 0 : opt.trWidth;
        // ⚠ 1 位并口在"formatter 打开"的目标上是**坏的**（2026-10-13 实测）。
        //
        // TPIU formatter 的输出是"8 数据位 + 1 个 id 位"（那 1 位就是 OFLOW 的 tag =
        // 通道号），探针按 `width` 位取值。width=1 时那 1 个 id 位会把整条流错位 ——
        // 症状是**通道号散成 20 多个**、载荷位序也错，于是大部分帧被判成"不认识的
        // tag"丢掉。同一块板、同一段采集实测：
        //     1 位 -> ETM 5.08 MB，丢弃 505,120 帧（80% 载荷）
        //     2 位 -> ETM 33.37 MB，丢弃 154 帧
        //     4 位 -> ETM 33.30 MB，丢弃 56 帧
        if (width == 1) {
            ORB_LOG_WARN("AGDI: 并口 1 位 —— formatter 开着时 id 位会让整条流错位"
                         "（实测丢弃 80%% 载荷），建议 trwidth=4");
        }
        if (!OrbTraceStart(c.dap, width, opt.trSwoMode, baud, opt.trOflow)) {
            ORB_LOG_WARN("AGDI: orbtrace trace source requested but not started");
        }
    } else if (etbSource) {
        // EtbInit() 是官方的 ETB_Init（DbgCM/ETB.cpp:1317）：确认 sink 存在并保证
        // Trace Data 窗口已注册。官方在此返回 EU32 表示"要了 ETB 但目标没有"。
        if (EtbDetect(c.dap) && EtbSetup(c.dap) && EtbInit()) {
            const EtbStatus es = EtbGetStatus();
            ORB_LOG_INFO("AGDI: ETB trace ready (addr=0x%08X, depth=%u words)",
                         es.addr, es.depth);
        } else {
            ORB_LOG_WARN("AGDI: ETB requested but not available on this target");
        }
    } else if (swoSource) {
        // 第三参是**目标侧** ACPR 的基数，走单点 `SwoAcprBaseKhz()`（当前 = 核心
        // 时钟）。它与探针侧那个 `SwoBaud()` 是不同角色，别为了"同源"合并两者。
        if (!ItmStart(c.dap, swoBaud, orb::cfg::SwoAcprBaseKhz(opt))) {
            ORB_LOG_WARN("AGDI: SWO console requested (%d baud) but not started", swoBaud);
        }
    }

    // The description's own trace setup, when it has one.  Only worth running
    // with a source actually on - otherwise it configures hardware nothing will
    // read.  Absent sequences are not a failure (RunDebugSeq's contract).
    if (orbSource || etbSource || swoSource) {
        if (!orb::pdsc::TraceStart())
            ORB_LOG_WARN("AGDI: TraceStart sequence failed");
    }

    // "Reset after Connect" / "Stop after Reset" (Debug page).  Without the
    // reset request "Stop after Reset" has nothing to stop after - the session
    // halts the core anyway.
    if (opt.resetAfterConnect && !c.core.Reset(opt.stopAfterReset)) {
        ORB_LOG_WARN("AGDI: reset after connect failed");
    } else if (opt.resetAfterConnect) {
        // 这一发复位把**刚编好**的目标侧 trace 寄存器同样打回默认：不在这里补
        // 一次，勾了 "Reset after Connect" 的会话从头到尾都收不到 trace
        // （见 RearmTargetTraceAfterReset）。
        RearmTargetTraceAfterReset("reset after connect");
    }

    // 抽数工作线程随会话起（官方 `DSMonitor_Thread` 也是 `DSM_StartMonitor` 里
    // 建、会话结束才收）。它自己会在没有源在跑时什么都不做。
    TracePumpStart();
    return true;
}

void Disconnect() {
    Context& c = Get();
    if (!c.connected) return;

    // 先置标志：让后台恢复撒手、让出探针锁，否则下面每一步都卡在死链路上。
    InterlockedExchange(&c.uninitReq, 1);

    TraceSetRunning(false, false);
    // **先**停 worker，再拆源：否则它会一边被拆一边读探针。
    TracePumpStop();
    OrbTraceStop(c.dap);
    ItmStop(c.dap);
    // **无条件**把目标侧的 trace 硬件关回去。
    //
    // 原先 `ItmTraceOff` 只在 `EtbAvailable()` 为真时调，于是 orbtrace / SWO 会话结束后
    // ETM 仍在全速灌指令 trace，而没人排空 —— 缓冲溢出后收到的数据是**错乱**的。
    // 见 COMPAT_ANALYSIS.md §10.28 与 Todo 的 P0-A 配套。
    ItmTraceOff(c.dap);
    if (EtbAvailable()) EtbReset();
    // 落盘（P3-1）：抽数已停，但**退休队列里可能还有在飞的块**（写线程正在写）。
    // 必须等它们落完 —— 下一句 `TraceFileClose()` 一关文件，队列里没写的块就只能丢。
    TraceWinDrain();
    // 落盘（P2-1）：三条源都已停，这里收尾并打一行汇总（路径/字节/索引条数）。
    TraceFileClose("disconnect");
    TraceWinShutdown();

    // Let the description clean up after itself while the port still answers.
    // All best effort by design: a device that cannot tear down must not stop the
    // session from ending.
    orb::pdsc::TraceStop();
    orb::pdsc::StopCore();
    orb::pdsc::RecoveryStop();

    // **必须在 StopDebugPort() 之前**：Breakpoints::Clear() 会真的写目标
    // （卸下 FPB/DWT 比较器，最后还写 FP_CTRL 关掉 FPB），而 MemWrite32 要经过
    // AP，也就是先写 DP SELECT。排在 StopDebugPort() 之后时调试口已经拆掉，
    // 于是日志里留下两行
    //     Dap: DP write reg=2 val=0x00000000 failed (rc=4110, err=8192)
    // —— 纯噪声，但正好落在断开路径上，容易被误读成链路故障。
    c.bps.Clear();

    orb::pdsc::StopDebugPort();

    c.connected = false;
    c.dap.Disconnect();
    // The attribute words are zeroed, not freed - uVision holds pointers into
    // them (see AttrClear) - but the memory map belongs to the session that just
    // ended, and the next one has to map its own.
    AttrClear();
    MapClear();
}

void Shutdown() {
    Context& c = Get();
    TracePumpStop();
    c.bps.Clear();
    if (c.connected) {
        c.dap.Disconnect();
        c.connected = false;
    }
    log::Shutdown();
}

uint32_t ReadPC() {
    Context& c = Get();
    uint32_t pc = 0;
    if (c.connected) c.core.ReadRegSel(0x0Fu, pc);   // kSelR0 + 15 (PC)
    return pc;
}

// ---------------------------------------------------------------------------
// Hand the PC back to uVision
//
// uVision does not take the current PC out of AG_AllReg: AG_INITCURPC hands the
// driver a pointer to uVision's *own* PC variable, and the driver is expected to
// keep that variable current.  DbgCM does exactly that on every register access
// and after every run-control command - DbgCM/AGDI.CPP:3623
//     *pCURPC = RegARM.PC; // let uVision2 know about PC...
// and publishes the instruction set two lines later (:3624, `pio->Thumb`).
//
// This driver stored the pointer (AG_INITCURPC -> c.curPC) and then never wrote
// through it, and never touched BOM->Thumb.  Everything uVision derives from the
// PC - the source-line arrow above all - therefore stayed put, while the log
// showed the PC advancing step by step and AG_AllReg reporting it correctly.
// ---------------------------------------------------------------------------
void PublishPC(uint32_t pc) {
    Context& c = Get();
    // Logged once per session: if uVision never sent AG_INITCURPC the pointer is
    // null and every write below is a silent no-op - which is exactly the state
    // the arrow bug lived in, so the first publish says which it is.
    static bool logged = false;
    if (!logged) {
        logged = true;
        ORB_LOG_INFO("AGDI: AG_INITCURPC pointer %s - publishing PC 0x%08X",
                     c.curPC ? "present" : "MISSING", pc);
    }
    if (c.curPC) *c.curPC = pc;
}

// ---------------------------------------------------------------------------
// SetupTmpBrk / UpdatePBStart - the temporary breakpoint, and the walking start
//
// Ported from DbgCM/AGDI.CPP:5455-5472 and :1293-1300.
//
// "Run until address X" (AG_GOTILADR) is implemented as a temporary breakpoint
// that the driver *inserts into uVision's own list*: TBp is linked in front of
// the head, the head's prev is rewired, and pBStart - the pointer every walk
// starts from - is aimed at it.  BrkWalk skips it by address (&TBp), so the stop
// it causes is never reported as a user breakpoint hit; it only makes the core
// stop where the run was asked to stop.
//
// This driver kept its temporary breakpoint in its own table instead.  The
// hardware outcome was the same, but nothing about the temporary breakpoint was
// visible to a walk over uVision's list.
// ---------------------------------------------------------------------------
AG_BP g_tbp;

void SetupTmpBrk(uint32_t nA) {
    Context& c = Get();

    g_tbp.type   = AG_ABREAK;
    g_tbp.rcount = 1;
    g_tbp.ocount = 1;
    if (nA != 0xFFFFFFFFu) {
        g_tbp.enabled = 1;
        g_tbp.Adr     = nA;
    } else {
        g_tbp.enabled = 0;
    }
    g_tbp.next = (c.bpHead ? *c.bpHead : nullptr);
    c.bpStart  = &g_tbp;
    if (c.bpHead && *c.bpHead) {
        (*c.bpHead)->prev = &g_tbp;
    }
}

// The reference's UpdatePBStart (DbgCM/AGDI.CPP:1293-1300): keep the walking
// start in sync when uVision replaces its breakpoint head while the target runs.
void UpdatePBStart() {
    Context& c = Get();
    if (c.bpStart == &g_tbp && c.bpHead) {
        g_tbp.next = *c.bpHead;
    } else if (c.bpHead) {
        c.bpStart = *c.bpHead;
    }
}

bool IsTmpBrk(const AG_BP* pB) { return pB == &g_tbp; }

// ---------------------------------------------------------------------------
// AnalyzeBreaks - can this set of breakpoints be served at all?
//
// Ported from DbgCM/AGDI.CPP:5476-5708.  The reference runs it before every
// Go/Step (GoUntil calls it at :7863) and turns a non-zero result into a message
// box plus a stopped HLL step (:7865-7868).  That is the only channel through
// which uVision learns "this breakpoint set cannot be served" - without it a
// breakpoint that cannot be armed simply never fires, which is indistinguishable
// from a breakpoint that was never reached.
//
// Return values are the reference's own:
//     0 = OK
//     1 = Conditional Breakpoints not supported
//     2 = Too many Watchpoints defined
//     3 = Invalid Watchpoint Range
// ---------------------------------------------------------------------------
int AnalyzeBreaks(uint32_t nA) {
    Context& c = Get();

    U32 abreaks = 0, hbreaks = 0, cbreaks = 0, wbreaks = 0, vbreaks = 0;

    for (AG_BP* pB = (c.bpStart ? c.bpStart
                                : (c.bpHead ? *c.bpHead : nullptr));
         pB; pB = pB->next) {
        if (!pB->enabled) continue;

        switch (pB->type) {
        case AG_ABREAK:
            if ((pB->Adr == nA) && !IsTmpBrk(pB)) {
                g_tbp.enabled = 0;      // a user breakpoint already covers it (:5531-5532)
            } else {
                ++abreaks;
                // FPB rev 0 is restricted to the code region; anything at or
                // above 0x20000000 has to become a software breakpoint (:5535).
                if (c.bps.FpbRev() > 0 || pB->Adr < 0x20000000u) ++hbreaks;
            }
            break;

        case AG_CBREAK:
            ++cbreaks;                  // conditional breakpoints are not served (:5539-5541)
            break;

        case AG_WBREAK:
            ++wbreaks;
            // The reference counts value-match watchpoints here through
            // BP_INFO_WVAL and limits them to one comparator.  This repo's AG_Bps
            // has no BP_INFO field, and the DWT comparators this driver programs
            // are address-only, so vbreaks stays 0 and that limit cannot be
            // reached - the check below is kept so the rule stays visible.
            break;

        default:                        // TBREAK / RBREAK
            break;
        }
    }

    if (cbreaks) return 1;              // Conditional BPs not supported (:5686-5688)
    if (wbreaks > static_cast<U32>(c.bps.WatchCapacity())) return 2;
    if (vbreaks > 1) return 2;          // only one value-match comparator (:5702-5704)

    ORB_LOG_INFO("Bp: AnalyzeBreaks -> OK (addr=%u, hw=%u of %d, cond=%u, wp=%u of %d, vmatch=%u)",
                 abreaks, hbreaks, c.bps.HwCapacity(), cbreaks,
                 wbreaks, c.bps.WatchCapacity(), vbreaks);
    return 0;
}

// ---------------------------------------------------------------------------
// BrkWalk - walk uVision's own breakpoint list
//
// Ported from DbgCM/AGDI.CPP:7367.
//
// The driver does not own the breakpoint list.  uVision hands over a pointer to
// the head as AG_INITBPHEAD and keeps the list current, so the only way to learn
// *which* breakpoint was hit - and its number, which uVision matches against its
// own rows - is to walk that list here.  This driver stored the pointer and
// never used it, so every hit was reported with nBpNum = 0xFFFF (undefined).
//
// Two forms, exactly as the reference has them:
//
//   BrkWalk(TRUE)  - called after every single step.  It assumes a BKPT match
//                    and looks for an enabled address breakpoint on the PC.
//   BrkWalk(FALSE) - called after the target stopped.  It reads DFSR and treats
//                    EXTERNAL / VCATCH / HALTED as "not a breakpoint".
//
// In both forms, when nothing matches, the reference still publishes the outcome
// (:7501) - `SetHitBp(NULL, *pCURPC, StopRun ? HIT_ESC : HIT_EXEC, *pCURPC)` -
// which is how uVision learns that a step finished and where it landed.
// ---------------------------------------------------------------------------
U32 BrkWalk(bool step) {
    Context& c = Get();

    U32 brk = 0;
    if (step) {
        brk = 0x00000002u;                      // BKPT - DbgCM/Debug.h:420
    } else {
        c.dap.MemRead32(kDcbDfsr, brk);         // GetBreakEvent() reads DFSR
        if (brk & 0x00000018u) return 1;        // VCATCH | EXTERNAL  (:7377)
        if (brk & 0x00000001u) return 1;        // HALTED
    }

    const uint32_t pc = ReadPC();
    const U32 nReason = 2;                      // "no reason"  (:7381)

    // Walk from pBStart, and skip the temporary breakpoint - the reference does
    // exactly this (`if (pB == &TBp) continue;` at :7388), which is what keeps a
    // "run until address" stop from being reported as a user breakpoint hit.
    for (AG_BP* pB = (c.bpStart ? c.bpStart
                                : (c.bpHead ? *c.bpHead : nullptr));
         pB; pB = pB->next) {
        if (IsTmpBrk(pB)) continue;
        if (!pB->enabled) continue;

        switch (pB->type) {
        case AG_ABREAK:                         // simple code address break
            if ((brk & 0x00000002u) && ((pB->Adr & ~1u) == (pc & ~1u))) {
                orb::pdsc::SetHitBp(pc, 0x0001u /* HIT_EXEC, BOM.H:3052 */,
                                    static_cast<U16>(pB->number), pc);

                if (pB->rcount == 1) {
                    if (pB->cmd) {
                        // A breakpoint with a command does NOT stop the target:
                        // the command runs and the application continues
                        // (DbgCM/AGDI.CPP:7392-7406).  The reference flips iRun
                        // around the call so the command itself can touch memory
                        // - the target is stopped, only the driver's "running"
                        // flag is cleared.
                        ORB_LOG_INFO("Bp: executing command for breakpoint %u: %s",
                                     (unsigned)pB->number, pB->cmd);
                        const LONG wasRunning = InterlockedExchange(&c.running, 0);
                        Callback(AG_CB_EXECCMD, pB->cmd);
                        InterlockedExchange(&c.running, wasRunning);
                        return 0;               // continue with application
                    }
                    return 1;                   // don't continue with application
                }
                --pB->rcount;                   // hit counter not exhausted yet
                return 0;                       // continue with application
            }
            break;

        case AG_WBREAK:                         // data access break
            if (brk & 0x00000004u /* DWTTRAP */) {
                // A watchpoint can carry a condition.  The reference asks uVision
                // to evaluate it and ignores the breakpoint when it comes back
                // false (DbgCM/AGDI.CPP:7454-7464):
                //
                //     if (pB->ReCalc && ep) {
                //         iRun = 0;  nRes = pCbFunc(AG_CB_TRUEXPR, ep);  iRun = 1;
                //         if (!nRes) { nReason = 0; continue; }   // false watch-bp
                //     }
                //
                // This driver never consulted AG_CB_TRUEXPR, so a conditional
                // watchpoint fired on every match instead of only when its
                // expression held.
                if (pB->ReCalc && pB->ep && c.cb) {
                    const LONG wasRunning = InterlockedExchange(&c.running, 0);
                    const U32  truth = c.cb(AG_CB_TRUEXPR, pB->ep);
                    InterlockedExchange(&c.running, wasRunning);
                    if (!truth) {
                        ORB_LOG_INFO("Bp: watchpoint %u condition is false - ignored",
                                     static_cast<unsigned>(pB->number));
                        continue;               // check the other breakpoints
                    }
                }

                // "hitting ReadWrite access BP reports write access on read
                // access" - the reference's own comment (:7467).
                const U16 reason = (pB->acc == 1) ? 0x0002u          /* HIT_READ  */
                                 : (pB->acc == 3) ? 0x0006u          /* READ|WRITE */
                                                  : 0x0004u;         /* HIT_WRITE */
                orb::pdsc::SetHitBp(pc, reason,
                                    static_cast<U16>(pB->number), pc);
                if (pB->rcount == 1) return 1;
                --pB->rcount;
                return 0;
            }
            break;

        default:                                // CBREAK / RBREAK / TBREAK
            break;
        }
    }

    const U16 reason = (InterlockedCompareExchange(&c.stopReq, 0, 0) != 0)
                           ? 0x0010u   /* HIT_ESC,  BOM.H:3056 */
                           : 0x0001u;  /* HIT_EXEC, BOM.H:3052 */
    orb::pdsc::SetHitBp(pc, reason, 0xFFFFu, pc);
    return nReason;
}

U32 RunAndWait(uint32_t tempBpAddr, uint32_t timeoutMs, uint32_t* stopAddr) {
    Context& c = Get();
    if (!c.connected) return AG_NOTARGET;

    // The probe is held for the whole run.  c.running already makes AG_AllReg
    // refuse, but that check and this call race: without the lock a register poll
    // that slipped in first would read DCRDR while we are programming the FPB.
    // AG_STOPRUN does not come through here, so Stop still works.
    orb::agdi::ComLock comLock;
    if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
        ORB_LOG_WARN("GoStep: run refused, the probe is busy (%u ms 没拿到锁)",
                     orb::agdi::kProbeWaitMs);
        return AG_NOACCESS;
    }

    // The temporary breakpoint goes into uVision's own list, the way the
    // reference does it (DbgCM/AGDI.CPP:5455 SetupTmpBrk, called from
    // AnalyzeBreaks at :5507 before every run).  The hardware comparator is
    // still programmed through this driver's slot table - that layer is the
    // equivalent of the reference's SetClrHwBP/AnalyzeBreaks hardware pass - but
    // the breakpoint itself now lives where a walk over uVision's list can see
    // it and skip it.
    const uint32_t temp = tempBpAddr & ~1u;
    const bool wantTemp = (temp != 0u && temp != 0xFFFFFFFEu);
    SetupTmpBrk(wantTemp ? temp : 0xFFFFFFFFu);

    // Can this breakpoint set be served at all?  The reference asks before every
    // run and reports the answer - and does NOT run when the answer is non-zero
    // (DbgCM/AGDI.CPP:7863-7868).  Without this the caller gets a run that
    // silently ignores every breakpoint that could not be armed.
    {
        const int brk = AnalyzeBreaks(wantTemp ? temp : 0xFFFFFFFFu);
        if (brk) {
            static const char* const kMsg[] = {
                "",                                                                 // 0
                "Too many conditional breakpoints - this target cannot serve them.", // 1
                "Too many watchpoints defined.",                                    // 2
                "Invalid watchpoint range.",                                        // 3
            };
            const char* msg = (brk >= 1 && brk <= 3) ? kMsg[brk]
                                                     : "Breakpoints cannot be handled.";
            ORB_LOG_WARN("Bp: AnalyzeBreaks -> %d (%s) - run refused", brk, msg);
            StatusMessage(msg);
            return AG_RDFAILED;
        }
    }

    const bool tempIsNew = wantTemp && !c.bps.Contains(temp);
    if (tempIsNew) {
        bool usedHw = false;
        c.bps.Add(temp, usedHw);
    }

    // Diagnostic, once per run.  "The core stopped where nobody asked it to" has
    // exactly one cause worth ruling out: a comparator left armed at an address
    // we no longer track - a stale slot, a failed uninstall, or a slot uVision
    // moved under us.  Printing the hardware and our own table side by side is
    // what tells the two apart; the measured case is GOTILADR(0x080001E0)
    // returning "stopped at 0x08022F1A" with no stop request in between.
    {
        char hw[192] = {0};
        int  n = 0;
        for (uint32_t comp = 0; comp < 8u; ++comp) {
            uint32_t v = 0;
            if (!c.dap.MemRead32(0xE0002008u + 4u * comp, v)) break;
            if (v == 0u) continue;
            n += snprintf(hw + n, sizeof(hw) - static_cast<size_t>(n),
                          " FP_COMP%u=0x%08X", comp, v);
        }
        uint32_t addrs[32];
        const int cnt = c.bps.Addresses(addrs, 32);
        char tb[256] = {0};
        int  m = 0;
        for (int i = 0; i < cnt; ++i) {
            m += snprintf(tb + m, sizeof(tb) - static_cast<size_t>(m),
                          " 0x%08X", addrs[i]);
        }
        ORB_LOG_INFO("Bp: before run - hardware:%s | tracked:%s",
                     n ? hw : " (none)", m ? tb : " (none)");
    }

    c.stopReq = 0;
    InterlockedExchange(&c.running, 1);
    TraceSetRunning(true, false);
    // 官方在起跑时调 Trace_Flush(setRun=TRUE)（DbgCM/AGDI.CPP:3013）：清掉上一次
    // 运行留下的错误状态与同步超时，让本次采集从干净状态开始。
    TraceWinFlush(true);

    // Stopped on a breakpoint?  Its FPB comparator is still armed at the PC, so
    // resuming re-triggers it immediately and the core never leaves the
    // instruction - "cannot step over a breakpoint".  The reference steps past it
    // first: DbgCM/AGDI.CPP:7902 `if (IsBreak() ...) { // Bp at current PC, step
    // over it.  Step(); ... }`.  The temporary breakpoint of this very run is
    // excluded, or "run to the current line" would overshoot by one instruction.
    if (c.core.IsHalted()) {
        const uint32_t here = ReadPC();
        if (here != 0u && here != temp && c.bps.Contains(here)) {
            // The comparator has to come out first.  A step taken with a
            // breakpoint armed on the instruction re-triggers it, so the core
            // halts on the same address again and the run can never leave it -
            // measured: "stepping past it" logged, then GOTILADR returned
            // "stopped at 0x08024D54", the very address it started from.
            ORB_LOG_INFO("GoStep: breakpoint at 0x%08X - stepping past it with the "
                         "comparator temporarily disabled", here);
            c.bps.SetEnabled(here, false);
            c.core.Step();
            c.bps.SetEnabled(here, true);
        }
    }

    // A BKPT instruction sitting at the PC is the same trap for a *software*
    // breakpoint - one this driver has no record of, typically a __breakpoint()
    // left in the code or a software breakpoint from another debugger.  The
    // reference handles it the same way, from GoUntil's entry
    // (DbgCM/AGDI.CPP:7875-7891): read the two bytes at the PC, and if they are
    // 0xBE00 move the PC past the instruction instead of executing it.
    //
    // This is the direct cause of "Go stops immediately": with the core halted on
    // such an instruction, resuming executes it, the debug event fires again and
    // the run ends after zero instructions.
    if (c.core.IsHalted()) {
        const uint32_t here = ReadPC();
        uint8_t op[2] = {0, 0};
        if (here != 0u && c.dap.MemRead(here, op, 2) &&
            op[0] == 0x00u && op[1] == 0xBEu) {       // 0xBE00, little-endian
            ORB_LOG_INFO("GoStep: BKPT at 0x%08X - stepping over it before the run",
                         here);
            c.core.WriteRegSel(0x0Fu, here + 2u);     // PC += 2
        }
    }

    // The same trap, for a comparator this driver does not know about.
    //
    // A comparator armed at the address the core is halted on fires the instant
    // the core resumes, so the run ends after zero instructions.  From uVision
    // that is "Step Over behaves like Step Into" and "Go stops immediately", and
    // because the address is not in this driver's table the stop is reported as
    // hitbp.nAcc = HIT_NOREASON - which is exactly what the session log showed:
    //
    //     SetHitBp 0x08024D54 acc=0x0000 - BOM present, stop published
    //
    // Measured case: FP_COMP1 and FP_COMP3 both holding 0x48024D55 (address
    // 0x08024D54) with only one of them in the table - the leftover from a
    // uVision session that was killed - pinned the core at 0x08024D54, and every
    // AG_GoStep from there returned "stopped at 0x08024D54".
    //
    // Only the FPB comparators (0xE0002008+) are scanned, and only one that is
    // armed *at the current PC* is cleared.  A breakpoint armed anywhere else is
    // left alone, and so is every DWT watchpoint, which lives at 0xE0001028+.
    if (c.core.IsHalted()) {
        const uint32_t here = ReadPC() & 0x1FFFFFFCu;   // same mask as the comparator
        if (here != 0u) {
            for (uint32_t comp = 0; comp < 8u; ++comp) {
                uint32_t v = 0;
                if (!c.dap.MemRead32(0xE0002008u + 4u * comp, v)) break;
                if ((v & 1u) == 0u) continue;            // comparator not enabled
                if ((v & 0x1FFFFFFCu) != here) continue; // armed somewhere else
                if (c.bps.Contains(here)) continue;      // ours - handled above
                ORB_LOG_WARN("GoStep: stale comparator FP_COMP%u is armed at the "
                             "current PC 0x%08X but is not in this driver's table - "
                             "clearing it, or the run could never leave that address",
                             comp, here);
                c.dap.MemWrite32(0xE0002008u + 4u * comp, 0u);
            }
        }
    }

    // Arm ETB capture while the target is still halted: the ETB fills up as the
    // target runs and is read back once it stops again.
    const bool etb = EtbAvailable() && EtbGetStatus().configured;
    if (etb) {
        ItmArmForEtb(c.dap);
        // 上一次停止可能在 FFCR 里留下停止位；官方的 ETB_Recovery 在重新武装之前
        // 清掉它们，否则 ETB/TMC 不再开始采集（DbgCM/ETB.cpp:1386）。
        EtbRecovery(c.dap);
        EtbActivate(c.dap);
    }

    // 抽数已经移到 worker（P0-A），所以从"让目标跑起来"这一刻起不再需要独占
    // 探针。持有整段会让 worker 全程饿死 —— 这正是官方把抽数放进
    // `DSMonitor_Thread` 之后，调试线程的等待循环只剩一句 `Sleep(50)` 的原因
    // （`DbgCM/AGDI.CPP:3155-3173`；`:3194-3203` 那段 `Trace_Read(50, TRUE)`
    // 是 `DBGCM_DS_MONITOR` 关掉时的旧写法，不是现行配置）。
    const bool resumed = c.core.Resume();
    comLock.Release();

    U32 rc = AG_OK;
    if (!resumed) {
        rc = AG_RDFAILED;
    } else {
        const DWORD start = GetTickCount();
        WaitHeartbeat beat("AG_GOFORBRK");
        for (;;) {
            if (InterlockedCompareExchange(&c.stopReq, 0, 0) != 0) {
                // The result matters: an unconfirmed halt means the core is
                // still executing, and the PC read below then reports the
                // residue of the previous AP access instead of a PC.
                //
                // 停机要碰探针，而 worker 正在按时间片用它，所以这里等一小会儿
                // 而不是拿不到就放弃 —— 放弃等于留下一个还在跑的内核。
                ComLockWait lock(200);
                if (lock.Held() && !c.core.Halt() && rc == AG_OK) {
                    rc = AG_RDFAILED;
                    // The reference runs SevereTargetError(SEV_ERR_STOP) here
                    // (DbgCM/AGDI.CPP:1684-1690): on a v8-M device where a
                    // non-secure debugger cannot halt a secure CPU it downgrades
                    // "cannot stop" to a warning instead of killing the session.
                    // The return code stays AG_RDFAILED - the core really is
                    // still running, so only the reporting changes.
                    orb::agdi::V8mSevereTargetError(orb::agdi::kSevErrStop);
                }
                break;
            }

            // 目标在跑的时候，这里只做一件事：采样一次 DHCSR。锁按**每次一小
            // 段**拿，worker 也是每次一小段，两边交替，谁都不饿死谁。
            bool halted = false;
            {
                ComLock lock;
                if (lock.Held()) halted = c.core.IsHalted();
            }
            if (halted) break;

            if (timeoutMs != 0 && (GetTickCount() - start) >= timeoutMs) {
                ComLockWait lock(200);
                if (lock.Held() && !c.core.Halt() && rc == AG_OK) {
                    rc = AG_RDFAILED;
                    orb::agdi::V8mSevereTargetError(orb::agdi::kSevErrStop);
                }
                break;
            }

            // 取数在 worker 线程上，所以这一圈不再搬字节：单圈耗时与 trace 速率
            // 无关，Stop 的响应粒度也不再由它决定（实测留在调试线程上时一次抽满
            // 2 MB 并当场解码要 ~106 ms，见 COMPAT_ANALYSIS.md §10.26）。
            //
            // 抽数 worker 只把状态栏消息**排队**（见 TraceWinStatusMsg 的说明），
            // 投递放在这里 —— 调试线程本来就在轮询，这一行没有待发布时只是一次
            // `InterlockedExchange`。
            orb::agdi::TraceWinStatusFlush();
            beat.Tick();
            Sleep(1);
        }
    }

    InterlockedExchange(&c.running, 0);
    if (tempIsNew) c.bps.Remove(temp);

    // 停机收尾要碰目标（DFSR / PC / ETB 读回 / 最后一次排空），重新独占探针。
    // worker 的时间片只有 2 ms，所以这里几乎立刻就能拿到。
    ComLockWait stopLock(500);
    if (!stopLock.Held()) {
        ORB_LOG_WARN("GoStep: 停机收尾没拿到探针锁，worker 未让出 —— 继续，"
                     "但这一轮的 DFSR/PC 可能读到脏值");
    }

    // Why did it stop?
    //
    // This is the one question a Cortex-M answers directly and this driver never
    // asked.  DHCSR says the core is halted; DFSR says *what* halted it:
    // HALTED / BKPT / DWTTRAP / VCATCH / EXTERNAL, plus S_LOCKUP in DHCSR.
    // Without it, "the run stopped immediately" cannot be told apart from "the
    // run was told to stop", "a breakpoint fired", "a watchpoint fired", "the
    // core took a fault" or "the core is locked up and cannot execute anything at
    // all" - and the last of those looks exactly like the first three, because
    // IsHalted() only tests S_HALT.
    //
    // The reference reads the same register for the same reason
    // (DbgCM/AGDI.CPP:7289 GetBreakEvent -> `ReadD32(NVIC_DFSR, &val, ...)`).
    // This driver used to *guess* the reason from `c.bps.Contains(stopAddr)`,
    // which is why the log showed hitbp.nAcc = HIT_NOREASON for every stop.
    uint32_t dfsr = 0;
    {
        uint32_t dhcsr = 0;
        c.dap.MemRead32(kDcbDhcsr, dhcsr);
        c.dap.MemRead32(kDcbDfsr, dfsr);
        char why[192] = {0};
        int  w = 0;
        if (dfsr & 0x01u) w += snprintf(why + w, sizeof(why) - (size_t)w, " HALTED");
        if (dfsr & 0x02u) w += snprintf(why + w, sizeof(why) - (size_t)w, " BKPT");
        if (dfsr & 0x04u) w += snprintf(why + w, sizeof(why) - (size_t)w, " DWTTRAP");
        if (dfsr & 0x08u) w += snprintf(why + w, sizeof(why) - (size_t)w, " VCATCH");
        if (dfsr & 0x10u) w += snprintf(why + w, sizeof(why) - (size_t)w, " EXTERNAL");
        ORB_LOG_INFO("GoStep: stop cause - DHCSR=0x%08X%s%s%s | DFSR=0x%08X (%s)",
                     dhcsr,
                     (dhcsr & (1u << 19)) ? " LOCKUP" : "",
                     (dhcsr & (1u << 18)) ? " SLEEP"  : "",
                     (dhcsr & (1u << 25)) ? " RESET_ST" : "",
                     dfsr, w ? why : " none");

        // A vector catch means the core is sitting in a fault handler, and the
        // fault status registers name the fault and the address that caused it.
        // Same principle as DFSR above: read the status register instead of
        // guessing.  Measured need: "Step Over runs off and lands in
        // HardFault_Handler" - the only way forward is to know which fault.
        if (dfsr & 0x08u) {
            uint32_t cfsr = 0, hfsr = 0, bfar = 0, mmfar = 0;
            c.dap.MemRead32(0xE000ED28u, cfsr);    // CFSR (MMFSR | BFSR | UFSR)
            c.dap.MemRead32(0xE000ED2Cu, hfsr);    // HFSR
            c.dap.MemRead32(0xE000ED38u, bfar);    // BFAR
            c.dap.MemRead32(0xE000ED34u, mmfar);   // MMFAR
            ORB_LOG_WARN("GoStep: fault - CFSR=0x%08X HFSR=0x%08X BFAR=0x%08X "
                         "MMFAR=0x%08X", cfsr, hfsr, bfar, mmfar);
        }

        // DFSR is write-1-to-clear.  Leaving it set makes the next run report the
        // previous stop's reason as its own.
        c.dap.MemWrite32(kDcbDfsr, dfsr);
    }

    // Read out and decode the ETB now that the target has stopped.
    if (etb) EtbProcess(c.dap);

    // One last drain: whatever the probe buffered just before the halt.  The
    // worker cannot do this one - it is the last look taken while the debug
    // thread still owns the probe, before the stop is published.
    OrbTracePump(c.dap, 5);
    ItmPump(c.dap, 5);
    TraceSetRunning(false, true);

    // A PC is only meaningful while the core is halted.  DCRDR holds whatever
    // the last DCRSR transfer selected, so reading it out of a running core
    // returns the residue of the previous AP access - measured twice in one
    // session: PC = 0x00030003 and PC = 0x02000000, both of which are DHCSR's own
    // value.  Reporting that as the stop address is what makes a run stop "some
    // where else", and it is why the caller is told the run failed instead.
    if (stopAddr) {
        if (c.core.IsHalted()) {
            *stopAddr = ReadPC();
            // Tell uVision where and why the target stopped.  This is the signal
            // it refreshes the debug views on: without it the Registers window
            // stays empty even though AG_AllReg answers with good values - the
            // data was delivered, but nothing ever said "it stopped".  The
            // reference publishes it from BrkWalk()/SetHitBp()
            // (DbgCM/AGDI.CPP:7274).  bpNum stays 0xFFFF (undefined) because we
            // do not keep a breakpoint-number to comparator mapping.
            // Classify the stop by walking uVision's own breakpoint list - what
            // the reference does from its run loop (DbgCM/AGDI.CPP:7937
            // `if (BrkWalk(0)) break;`).  BrkWalk reads DFSR itself, treats
            // EXTERNAL / VCATCH / HALTED as "not a breakpoint", matches the PC
            // against the enabled address breakpoints, publishes the hit with
            // that breakpoint's own number, and when nothing matches still
            // publishes HIT_EXEC (:7501) so uVision knows the run ended normally
            // and where.
            //
            // This replaces a guess from `c.bps.Contains(stopAddr)`, which knew
            // neither the reason nor the breakpoint number.
            const U32 walk = BrkWalk(false);
            ORB_LOG_INFO("GoStep: BrkWalk -> %u (DFSR=0x%08X)", walk, dfsr);
        } else {
            *stopAddr = 0u;
            ORB_LOG_WARN("GoStep: core is not halted after the run - no PC reported");
            if (rc == AG_OK) rc = AG_RDFAILED;
        }
    }
    return rc;
}

// ===========================================================================
// Register View  -  AG_CB_INITREGV
//
// uVision builds the Registers window from a descriptor table that the *driver*
// installs through its callback:
//
//     pCbFunc(AG_CB_INITREGV, &dsc);        // DbgCM/AGDI.CPP:16922
//
// The reference calls that from AG_INITFEATURES, after the first reset and not
// during a flash download (DbgCM/AGDI.CPP:4372,
//     `InitRegs(); // define register layout for RegWindow`).
//
// This driver never called it at all, and that is the whole reason the Registers
// window showed its two column headers - `Register` and `Value` - with not one
// row under them, while AG_AllReg kept answering with perfectly good values.  The
// values were being delivered into a window that had no rows to put them in.
// Every other view was fine (memory, disassembly, the source arrow), and that
// asymmetry is what made this hard to see: only the register *values* were ever
// logged, never the register *layout*.
//
// The item ids are the ones uVision itself uses (the AR_n* list, AGDI.H:211+):
//   0x00..0x0F  R0..R15          0x100  xPSR
//   0x11 MSP    0x12 PSP         0x20 BASEPRI  0x21 PRIMASK
//   0x22 FAULTMASK               0x23 CONTROL
// ===========================================================================
namespace {

RGROUP kRegGroups[] = {
    { 0x00, 0x03, "Core"   },   // ShEx bit0 = expanded, bit1 = bold
    { 0x00, 0x00, "Banked" },
    { 0x00, 0x00, "System" },
};

RITEM kRegItems[] = {
    //  desc  nGi nItem  szReg[16]   isPC canChg iHigh
    { 0x01, 0, 0x00, "R0",          0, 1, 0 },
    { 0x01, 0, 0x01, "R1",          0, 1, 0 },
    { 0x01, 0, 0x02, "R2",          0, 1, 0 },
    { 0x01, 0, 0x03, "R3",          0, 1, 0 },
    { 0x01, 0, 0x04, "R4",          0, 1, 0 },
    { 0x01, 0, 0x05, "R5",          0, 1, 0 },
    { 0x01, 0, 0x06, "R6",          0, 1, 0 },
    { 0x01, 0, 0x07, "R7",          0, 1, 0 },
    { 0x01, 0, 0x08, "R8",          0, 1, 0 },
    { 0x01, 0, 0x09, "R9",          0, 1, 0 },
    { 0x01, 0, 0x0A, "R10",         0, 1, 0 },
    { 0x01, 0, 0x0B, "R11",         0, 1, 0 },
    { 0x01, 0, 0x0C, "R12",         0, 1, 0 },
    { 0x01, 0, 0x0D, "SP (R13)",    0, 1, 0 },
    { 0x01, 0, 0x0E, "LR (R14)",    0, 1, 0 },
    { 0x01, 0, 0x0F, "PC (R15)",    1, 1, 0 },   // isPC - the source arrow follows this
    { 0x01, 0, 0x100, "xPSR",       0, 1, 0 },
    // xPSR's sub-items.  uVision nests an item under the previous one when its
    // nItem continues that item's number range, and the delimiter has to sit
    // above the whole range to close it - the reference uses 0x10F for exactly
    // that (DbgCM/AGDI.CPP:8649-8657).
    { 0x01, 0, 0x101, "N",          0, 1, 0 },
    { 0x01, 0, 0x102, "Z",          0, 1, 0 },
    { 0x01, 0, 0x103, "C",          0, 1, 0 },
    { 0x01, 0, 0x104, "V",          0, 1, 0 },
    { 0x01, 0, 0x105, "Q",          0, 1, 0 },
    { 0x01, 0, 0x106, "GE",         0, 0, 0 },   // canChg 0, as the reference has it
    { 0x01, 0, 0x107, "T",          0, 1, 0 },
    { 0x01, 0, 0x108, "IT",         0, 0, 0 },
    { 0x01, 0, 0x109, "ISR",        0, 1, 0 },
    // Delimiter - nItem 0x10F, NOT 0.
    //
    // With 0 it collided with R0's item id, so RegGet answered R0's value for it
    // and uVision drew a nameless row carrying R0's number - measured: a row
    // showing 0x00000600 directly under xPSR, the same value as R0.  And because
    // it sat below xPSR's range instead of above it, it also failed to close
    // xPSR's sub-item list: MSP and PSP stayed nested under xPSR and the Banked
    // and System groups came up empty.
    { 0x01, 0, 0x10F, "",           0, 0, 0 },
    { 0x01, 1, 0x11, "MSP",         0, 1, 0 },
    { 0x01, 1, 0x12, "PSP",         0, 1, 0 },
    { 0x01, 2, 0x20, "BASEPRI",     0, 1, 0 },
    { 0x01, 2, 0x21, "PRIMASK",     0, 1, 0 },
    { 0x01, 2, 0x22, "FAULTMASK",   0, 1, 0 },
    { 0x01, 2, 0x23, "CONTROL",     0, 1, 0 },
};

// The register file as uVision last asked for it.  RegGet is called once with
// UPR_NORMAL to let the driver refresh the whole set, and then once per visible
// item; caching the block here is what the reference does as well (uRg).
RgARMCM g_regs;
bool    g_haveRegs  = false;
bool    g_installed = false;
DWORD   g_regsAt    = 0;        // when g_regs was last read from the target

// (Re)read the whole register file from the target.
bool RefreshRegs() {
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return false;
    g_haveRegs = c.core.ReadRegs(g_regs);
    g_regsAt   = GetTickCount();
    return g_haveRegs;
}

bool ReadRegItem(U16 nItem, U32& out) {
    // Refresh when the cached block is missing, or older than this.
    //
    // uVision is supposed to announce a refresh with RegGet(UPR_NORMAL) before
    // asking for the individual items, and this driver relied on that alone: the
    // first item call read the target once and set g_haveRegs, and every call
    // after that served the same block for the rest of the session.  Measured:
    // the window showed the reset values of R0..R12, PC = 0x00000000 and
    // xPSR = 0x01000000 while the log showed the PC at 0x08001AEE - "the data
    // does not refresh".  The age bound is the safety net for the case where
    // UPR_NORMAL never arrives; it costs one register read per 100 ms.
    if (!g_haveRegs || (GetTickCount() - g_regsAt) > 100u) {
        if (!RefreshRegs()) return false;
    }
    if (nItem <= 0x0Fu) { out = g_regs.Rn[nItem]; return true; }
    switch (nItem) {
    case 0x100: out = g_regs.xPSR;              return true;
    // xPSR's sub-items - the same bit positions the reference masks out of xPSR
    // (DbgCM/AGDI.CPP:15990-15996, via its bPSR[] table).
    case 0x101: out = (g_regs.xPSR >> 31) & 0x1u;   return true;   // N
    case 0x102: out = (g_regs.xPSR >> 30) & 0x1u;   return true;   // Z
    case 0x103: out = (g_regs.xPSR >> 29) & 0x1u;   return true;   // C
    case 0x104: out = (g_regs.xPSR >> 28) & 0x1u;   return true;   // V
    case 0x105: out = (g_regs.xPSR >> 27) & 0x1u;   return true;   // Q
    case 0x106: out = (g_regs.xPSR >> 16) & 0xFu;   return true;   // GE
    case 0x107: out = (g_regs.xPSR >> 24) & 0x1u;   return true;   // T
    case 0x108: out = (g_regs.xPSR >> 10) & 0x3Fu;  return true;   // IT
    case 0x109: out =  g_regs.xPSR        & 0x1FFu; return true;   // ISR
    case 0x11:  out = g_regs.MSP;             return true;
    case 0x12:  out = g_regs.PSP;             return true;
    case 0x20:  out = g_regs.SYS.R.BASEPRI;   return true;
    case 0x21:  out = g_regs.SYS.R.PRIMASK;   return true;
    case 0x22:  out = g_regs.SYS.R.FAULTMASK; return true;
    case 0x23:  out = g_regs.SYS.R.CONTROL;   return true;
    default:    return false;
    }
}

// uVision's get callback (DbgCM/AGDI.CPP:15877 RegGet).
void RegGet(RITEM* vp, int nR) {
    if (!vp) return;
    const U32 mode = static_cast<U32>(nR) & 0xF0000000u;

    // Visibility for the one question the layout fix cannot answer: does uVision
    // call this callback at all, and do the answers land?  Installing the table
    // (AG_CB_INITREGV) only creates the rows - the values arrive here, and
    // nothing in this driver had ever proved that this is called.
    {
        static volatile LONG s_normal = 0;
        static volatile LONG s_items  = 0;
        static volatile LONG s_fail   = 0;
        if (mode == UPR_NORMAL) {
            const LONG n = InterlockedIncrement(&s_normal);
            if (n <= 3) {
                orb::agdi::Context& c = orb::agdi::Get();
                ORB_LOG_INFO("RegView: RegGet(UPR_NORMAL) #%ld - connected=%d",
                             n, c.connected ? 1 : 0);
            }
        } else if (mode != UPR_HIST) {
            const LONG n = InterlockedIncrement(&s_items);
            if (n <= 4) {
                ORB_LOG_INFO("RegView: RegGet(item 0x%04X) #%ld", vp->nItem, n);
            }
            U32 probe = 0;
            if (!ReadRegItem(vp->nItem, probe)) {
                if (InterlockedIncrement(&s_fail) == 1) {
                    ORB_LOG_WARN("RegView: no value for item 0x%04X - the target "
                                 "may not be connected yet", vp->nItem);
                }
            }
        }
    }

    if (mode == UPR_NORMAL) {
        // One refresh for the whole set; the per-item calls below only format.
        RefreshRegs();
        return;
    }
    if (mode == UPR_HIST) return;      // no execution history in this driver

    U32 v = 0;
    vp->iDraw = 0;
    if (!ReadRegItem(vp->nItem, v)) return;

    if (v != vp->v.u32 || vp->szVal[0] == 0) {
        _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "0x%08X", v);
    }

    // The reference's tail, verbatim (DbgCM/AGDI.CPP:16391-16400):
    //
    //     if (v.u32 == vp->v.u32) { // value unchanged
    //         if (vp->iHigh) { vp->iHigh = 0; vp->iDraw = 1; }
    //     } else { vp->iHigh = 1; vp->iDraw = 1; }
    //
    // iHigh is what uVision paints blue for "this item changed"; bit 7 = 0 means
    // colour highlight, and the header spells that out (AGDI.H:424-426).  This
    // driver wrote iHigh = 0 unconditionally, so the auto-detect-updated-items
    // feature could never mark anything.
    if (v == vp->v.u32) {
        if (vp->iHigh) {               // was highlighted and has settled again
            vp->iHigh = 0;
            vp->iDraw = 1;
        }
    } else {
        vp->iHigh = 1;                 // highlight - uVision paints it blue
        vp->iDraw = 1;
    }
    vp->v.u32 = v;
}

// uVision's set callback (DbgCM/AGDI.CPP:16408 RegSet).
I32 RegSet(RITEM* vp, GVAL* pV) {
    if (!vp || !pV) return 0;
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return 0;

    // The guard AG_AllReg already has, which this new code was missing
    // (DbgCM/AGDI.CPP:4918 `if (iRun && !supp.RegAccR) return AG_NOACCESS;`).
    // DCRSR only accepts a transfer from a halted core, so a write issued while
    // the target runs is either dropped or lands at an arbitrary later moment -
    // and a PC or xPSR applied at the wrong moment is exactly how a running
    // target ends up in a fault handler.
    if (InterlockedCompareExchange(&c.running, 0, 0) != 0) {
        ORB_LOG_WARN("RegView: RegSet(item 0x%04X) refused while the target runs",
                     vp->nItem);
        return 0;
    }

    U32  sel = 0;
    bool ok  = false;
    switch (vp->nItem) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        sel = vp->nItem;
        ok  = c.core.WriteRegSel(sel, pV->u32);
        if (ok) {
            g_regs.Rn[vp->nItem] = pV->u32;
            if (vp->nItem == 0x0F) orb::agdi::PublishPC(pV->u32);
        }
        break;
    case 0x100: ok = c.core.WriteRegSel(kSelXpsr, pV->u32);
                if (ok) g_regs.xPSR = pV->u32; break;
    case 0x11:  ok = c.core.WriteRegSel(kSelMsp,  pV->u32);
                if (ok) g_regs.MSP  = pV->u32; break;
    case 0x12:  ok = c.core.WriteRegSel(kSelPsp,  pV->u32);
                if (ok) g_regs.PSP  = pV->u32; break;
    default:    return 0;              // the system bytes are read-only here
    }
    return ok ? 1 : 0;
}

} // namespace

// Hand the table to uVision.  Idempotent: the reference installs it once per
// session, and re-installing on every reset would only make uVision rebuild a
// window that is already correct.
void InstallRegView() {
    orb::agdi::Context& c = orb::agdi::Get();
    if (g_installed) return;
    if (!c.cb) {
        ORB_LOG_WARN("RegView: AG_INITCALLBACK not seen - the Registers-window "
                     "layout cannot be installed");
        return;
    }

    REGDSC dsc;
    dsc.nGitems = static_cast<I32>(sizeof(kRegGroups) / sizeof(kRegGroups[0]));
    dsc.GrpArr  = kRegGroups;
    dsc.nRitems = static_cast<I32>(sizeof(kRegItems) / sizeof(kRegItems[0]));
    dsc.RegArr  = kRegItems;
    dsc.RegGet  = RegGet;
    dsc.RegSet  = RegSet;

    c.cb(AG_CB_INITREGV, &dsc);
    g_installed = true;
    ORB_LOG_INFO("RegView: installed %d group(s) / %d register item(s) via "
                 "AG_CB_INITREGV", (int)dsc.nGitems, (int)dsc.nRitems);
}

// ===========================================================================
// Go/Step state
//
// The reference keeps two flags for this (DbgCM/AGDI.CPP:150-151):
//
//     static BOOL MultipleGoStep;     // Multiple Go/Step calls for one operation
//     static BOOL BkpInstructionSkip; // Skip SW Breakpoint instruction
//
// uVision sets them by announcing every run-control command through AG_EXECITEM
// (DbgCM/AGDI.CPP:4526-4534 and :4564-4569), and the step consumes them:
//
//     AG_RUNSTART        -> MultipleGoStep = FALSE; BkpInstructionSkip = TRUE;
//     AG_OVERSTEP_START  -> MultipleGoStep = TRUE;  BkpInstructionSkip = TRUE;
//     AG_STEPLINE_START  -> (same)
//     AG_STEPINTO_START  -> (same)
//
// This driver accepted those items silently ("notifications only") and set
// neither flag, so the BKPT skip below never ran.
// ===========================================================================
volatile LONG g_multiGoStep = 0;
volatile LONG g_bkpSkip     = 0;

void AnnounceRunStart() {
    InterlockedExchange(&g_multiGoStep, 0);
    InterlockedExchange(&g_bkpSkip, 1);
}

void AnnounceStepStart() {
    InterlockedExchange(&g_multiGoStep, 1);
    InterlockedExchange(&g_bkpSkip, 1);
}

} // namespace agdi
} // namespace orb

// ===========================================================================
// AGDI entry points
// ===========================================================================
using orb::agdi::AttrClear;
using orb::agdi::AttrGet;
using orb::agdi::AttrPtr;
using orb::agdi::AttrSet;
using orb::agdi::kAttrBpMask;
using orb::agdi::MapClear;
using orb::agdi::Mapped;
using orb::agdi::MapMem;
using orb::agdi::WaitHeartbeat;

namespace {

void InitLogOnce() {
    wchar_t dir[MAX_PATH] = L"";
    if (orb::agdi::TempDir(dir, MAX_PATH)) {
        orb::log::Init(dir);
    } else {
        orb::log::Init(nullptr);
    }
}

/// The banner a session opens with: what uVision told us about the project, and
/// which of those settings is actually in force.
///
/// This is the "key information" split from the log.  The log keeps every
/// decision and every register access; this is the dozen lines that answer "is it
/// looking at the right chip, through the right probe, with the right clock" -
/// the questions that otherwise cost a user a support round trip.  Everything
/// below the banner (packet dumps, per-access traces) stays in the file.
void ReportSession() {
    const orb::cfg::Config& raw = orb::cfg::Get();
    const orb::cfg::Config  eff = orb::cfg::Effective();

    orb::ui::Info("ORBAGDI %s - %s, %u kB flash at 0x%08X",
                  ORBAGDI_VERSION_STRING,
                  raw.device[0] ? raw.device : "(device not named by uVision)",
                  raw.iromSize / 1024u, raw.iromStart);

    orb::ui::Info("  RDDI host : %s",
                  eff.rddiDll[0] ? eff.rddiDll : "(none - see the log)");
    orb::ui::Info("  Link      : %s, interface %d, clock %d kHz, AP %d, %s",
                  (eff.port == orb::cfg::kPortJtag) ? "JTAG" : "SWD",
                  eff.iface, eff.clockKhz, eff.ap,
                  eff.swj ? "SWJ-DP" : "plain DP");

    if (eff.orbtrace)      orb::ui::Info("  Trace     : orbtrace probe");
    else if (eff.etb)      orb::ui::Info("  Trace     : on-chip ETB");
    else if (eff.swoBaud)  orb::ui::Info("  Trace     : SWO pin at %d Hz", eff.swoBaud);
    else                   orb::ui::Info("  Trace     : off");

    // The clock pair behind the SWO divider.  Reported because a unit mistake
    // there is a silent ITM console: the trace clock is a frequency in kHz here,
    // Keil's own Trace Clock box is a period in ns, and mixing them is a factor
    // of a million.  Showing the derived bit clock is what makes that visible.
    if (eff.traceClockKhz > 0) {
        orb::ui::Info("  Trace clk : %d kHz core, prescaler %d -> %d Hz bit clock",
                      eff.traceClockKhz,
                      (eff.swoPrescaler > 0) ? eff.swoPrescaler : 1,
                      orb::cfg::SwoBaud(eff));
    }

    // The reset mode is reported because it is a setting the driver acts on
    // directly and it was silently wrong for a while: the combo and the enum
    // disagreed, so "HW RESET" drove AIRCR.VECTRESET (bug.md §N).  A banner line
    // is what makes that visible.
    static const char* const kResetNames[] = {
        "Autodetect", "HW RESET", "SYSRESETREQ", "VECTRESET", "Core and Peripherals"
    };
    const int rm = (eff.reset >= 0 && eff.reset <= 4) ? eff.reset : 0;
    orb::ui::Info("  Reset     : %s", kResetNames[rm]);

    orb::ui::Info("  Flash     : %s",
                  eff.flm[0] ? eff.flm : "(no algorithm - Download cannot run)");
    if (orb::pdsc::Status()) orb::ui::Info("  Pack      : %s", orb::pdsc::Status());
}

} // namespace

// ---------------------------------------------------------------------------
// EnumUvARM7 / DllUv3Cap
//
// Not declared in AGDI.H; uVision's target-DLL loader resolves them by name.
// They identify the DLL as an ARM target driver.
// ---------------------------------------------------------------------------
// struct qdll, as defined in BOM.H. BOM.H drags in the whole uVision interface
// for the sake of one seven-field struct, so the layout is mirrored here -
// order and types must stay identical to BOM.H.
struct QdllArgs {
    HWND    parent;   // parent window handle
    char*   pathUv3;  // path of the target DLL
    HMODULE Linst;    // DLL instance handle
    int     ValSize;  // size of the 'value' buffer
    char*   key;      // register key
    char*   value;    // command line, in and out
    char*   prjname;  // project path + name
};

namespace {

// The debug block uVision hands over once per process.  Keil keeps it in a
// global too (DbgCM.cpp:741) and reads the Utilities page out of it long after
// the entry point returned, i.e. the pointer outlives the call.
const void* g_dbgBlk = nullptr;

// uVision delivers the debug block through TWO entry points, depending on the
// flow: EnumUvARM7(2, p) while the target driver is registered, and
// DllUv3Cap(109, p) when a debug/flash session starts.  Keil's reference driver
// handles both (DbgCM.cpp:730 and DbgCM.cpp:790).  Implementing only the first
// left the option string empty in every real session, so flm=/ram=/clock=
// never took effect and Flash download ended in "no flash algorithm
// configured" - with no hint that the cause was a missing entry point rather
// than a wrong path (bug.md E6).
//
// TargArgs is the option string the project stores for this driver - exactly
// what the Settings dialog edits and what DllUv3Cap(1) writes back.
void TakeDebugBlock(void* p, const char* via) {
    if (!p) {
        ORB_LOG_WARN("AGDI: %s got a NULL dbgblk - driver options were NOT read "
                     "(defaults are in force)", via);
        return;
    }
    const dbgblk* d = static_cast<const dbgblk*>(p);
    g_dbgBlk = p;
    ORB_LOG_INFO("AGDI: dbgblk via %s, TargDll = \"%s\"", via, d->TargDll);
    ORB_LOG_INFO("AGDI: options = \"%s\"", d->TargArgs);
    // uVision's own answer to "where is Keil installed" - dbgext::szTRoot is
    // "<Keil>\ARM" (ComTyp.H:397).  It has to be recorded before the first pack
    // lookup below, because the pack-root list is derived from it and then
    // cached; a late answer would arrive after an empty list had been built and
    // the download would fail with "no flash algorithm" for no visible reason.
    orb::cfg::SetToolRoot((d->pDbgX) ? d->pDbgX->szTRoot : nullptr);

    // The debug block carries the BOM too (dbgext::pBom), and that is the route
    // the reference actually uses: DbgCM/DbgCM.cpp:604 InitBomp() takes `pio`
    // from here, not from AG_CB_GETBOMPTR.  Both are supposed to name the same
    // object, and it matters that they do: every pio->* write this driver makes
    // (Thumb, hitbp) is only visible to uVision if it reads the same instance.
    // Taking it from the debug block first is therefore the reference behaviour,
    // and SetBom() logs which instance won.
    if (d->pDbgX && d->pDbgX->pBom) {
        // 走 SetBomAlt（只登记备用实例）：这条路的对象与 AG_CB_GETBOMPTR 返回的
        // ioc **不是同一个**，而 uVision 的取数门读的是 ioc，ioc 必须始终是主指针。
        // 详见 SetBomAlt() 与 AG_INITCALLBACK 处的说明。
        orb::pdsc::SetBomAlt(reinterpret_cast<struct bom *>(d->pDbgX->pBom));
    }
    orb::cfg::Parse(d->TargArgs);
    // A project that was set up for ULINK/CMSIS-DAP spells the flash algorithm
    // the Keil way (-FF0<name> / -FP0(<path>)), which has no '=' and is
    // therefore invisible to Parse().  And even when our own string is empty,
    // the debug block still names the device, its flash settings and its RAM -
    // so a plain "Download" works without anyone touching the Flash dialog.
    orb::cfg::ParseKeilFlash(d->TargArgs);
    orb::cfg::AdoptUVision(d);
    orb::agdi::FlashOptions(d->TargArgs);
}

} // namespace

extern "C" int _EXPO_ EnumUvARM7(void *p, DWORD nCode)
{
    if (nCode == 2) {
        TakeDebugBlock(p, "EnumUvARM7(2)");
        return 7;                 // 7 == ARM family
    }
    return 0;
}

extern "C" int _EXPO_ DllUv3Cap(DWORD nCode, void *p)
{
    switch (nCode) {
    case 1: {                     // Cpu/Target-DLL Settings - the Debug sheet
        QdllArgs* q = static_cast<QdllArgs*>(p);
        if (!q || !q->value || q->ValSize <= 0) return 0;

        // Keil refreshes from the debug block and, when the driver's own string
        // is empty, opens the dialog on the Utilities page's string instead
        // (DbgCM.cpp:790: AnalyzeParms(p->pathUv3, value ? value : ULINKOptions(0))).
        // Same here: adopt what uVision knows first, then fall back to the
        // effective (project + auto-detected) options so the box is pre-filled
        // with the detected RDDI/clock rather than blank.
        if (g_dbgBlk) orb::cfg::AdoptUVision(g_dbgBlk);

        char in[orb::cfg::kArgsMax] = {0};
        if (q->value[0]) strncpy_s(in, q->value, _TRUNCATE);
        else             orb::cfg::Format(orb::cfg::Effective(), in, sizeof(in));
        char out[orb::cfg::kArgsMax] = {0};
        if (!orb::cfg::ShowDialog(q->parent, in, out, sizeof(out))) {
            ORB_LOG_INFO("AGDI: settings dialog cancelled");
            return 0;             // 0 leaves the project's string untouched
        }
        strncpy_s(q->value, static_cast<size_t>(q->ValSize), out, _TRUNCATE);
        orb::cfg::Parse(out);     // effective from the next session start
        return 1;                 // 1 tells uVision to store the string
    }
    case 2:   return 7;   // match family: ARM target driver

    // 109 == "Get Debug Block" (DbgCM.cpp:790).  Same dbgblk payload as
    // EnumUvARM7(2), just delivered through the capability entry point - this
    // is the one real sessions actually use, and NOT answering it (falling into
    // default: return 0) silently discarded the whole option string.
    case 109:
        TakeDebugBlock(p, "DllUv3Cap(109)");
        return 0;                 // Keil returns 0 here too: its case 109 only
                                  // breaks out, leaving i at 0 (DbgCM.cpp:790)

    // 100 == "has Flash-Download Capabilities" (DbgCM.cpp:770 in Keil's own
    // reference driver).  This bit is a promise, not a switch: answering 1
    // makes the IDE enable Download / Flash Download, and uVision then expects
    // the driver to do the whole job - AG_Init(AG_INITFLASHLOAD) to load the
    // FLM, AG_Init(AG_STARTFLASHLOAD) to erase/program/verify, and
    // AG_CB_GETFLASHPARAM (FLASHPARM) to pull the image (AGDI.CPP:4436).
    // All three are implemented in ORBAGDI_Flash.cpp, so the promise holds.
    case 100: return 1;

    // 110 == Cpu/Target/Flash-DLL Settings - the Utilities sheet's Settings
    // button.  Keil answers it with DoFDlgSetup() and writes the result
    // straight back into the option string (DbgCM.cpp:800); that string is
    // where the flash algorithm and its RAM window live for this driver, so
    // the dialog is the only way a user can point Flash Download at an FLM.
    case 110: {
        QdllArgs* q = static_cast<QdllArgs*>(p);
        if (!q || !q->value || q->ValSize <= 0) return 0;

        // Keil falls back to the ULINK options when the driver's own string is
        // empty (DbgCM.cpp:800).  Ours are the values adopted from the debug
        // block, so the dialog opens showing the algorithm the device already
        // implies instead of an empty box the user has to fill in by hand.
        if (g_dbgBlk) orb::cfg::AdoptUVision(g_dbgBlk);

        char in[orb::cfg::kArgsMax] = {0};
        if (q->value[0]) strncpy_s(in, q->value, _TRUNCATE);
        else             orb::cfg::Format(orb::cfg::Effective(), in, sizeof(in));
        char out[orb::cfg::kArgsMax] = {0};
        if (!orb::cfg::ShowFlashDialog(q->parent, in, out, sizeof(out),
                                       q->prjname)) {
            ORB_LOG_INFO("AGDI: flash settings dialog cancelled");
            return 0;             // 0 leaves the project's string untouched
        }
        strncpy_s(q->value, static_cast<size_t>(q->ValSize), out, _TRUNCATE);
        orb::cfg::Parse(out);
        orb::agdi::FlashOptions(out);
        return 1;                 // 1 tells uVision to store the string
    }

    default:  return 0;
    }
}

// ---------------------------------------------------------------------------
// Names for the AG_F_* capability codes (AGDI.H:747+).  Log only.
// ---------------------------------------------------------------------------
static const char* FeatureName(U32 f) {
    switch (f) {
    case AG_F_MEMACCR:     return "MEMACCR";
    case AG_F_REGACCR:     return "REGACCR";
    case AG_F_TRACE:       return "TRACE";
    case AG_F_COVERAGE:    return "COVERAGE";
    case AG_F_PALYZE:      return "PALYZE";
    case AG_F_MEMMAP:      return "MEMMAP";
    case AG_F_RESETR:      return "RESETR";
    case AG_F_LANALYZER:   return "LANALYZER";
    case AG_F_EXTBRKSUPP:  return "EXTBRKSUPP";
    case AG_F_EXCTRC:      return "EXCTRC";
    case AG_F_DBGRECOVERY: return "DBGRECOVERY";
    case AG_F_SWBREAKCONF: return "SWBREAKCONF";
    case AG_F_ITM_TRACE:   return "ITM_TRACE";
    default:               return "?";
    }
}

// ---------------------------------------------------------------------------
// PlayDead —— 官方 `ExecPlayDead()`（DbgCM/AGDI.CPP:1183-1234）
// ---------------------------------------------------------------------------
// 见 ORBAGDI_DebugAccess.h 里那一整段说明：uVision 在"无法访问目标硬件"的恢复
// 对话框里按"停止调试"时，官方对"目标没在跑"那种情况走强杀，而强杀的动作就是
// 下面这两步。

// AG_UNINIT 与 PlayDead 共用的收摊动作（官方那边叫 `StopTarget()`）。抽出来是
// 为了两处**必须一致**：会话结束时该做的事，不该因为"是谁结束的"而不同。
static void SessionTeardown()
{
    orb::agdi::FlashUnInit();
    orb::agdi::Disconnect();
    // The description belongs to the session that just ended; keeping it would
    // let the next target inherit a stale one.
    orb::pdsc::Shutdown();
    // 访问等级与"复位下恢复"的闩锁属于刚结束的这次会话，下一个目标不该继承
    // （参考只在 DLL 初始化时清，本驱动多清一次；见 ORBAGDI_DebugAccess.h）。
    orb::agdi::DebugAccessUninit();
    // The window the reports went to is going away with the session.
    orb::ui::Detach();
}

// 官方 `ExecPlayDead()` 的收摊那一半 + `MSG_UV2_TERMINATE`。
//
// 位置照官方：`AG_Init` 末尾那一句 `if (PlayDead) ExecPlayDead();`
// （DbgCM/AGDI.CPP:4586-4588）。放在函数末尾而不是 `AG_RECOVERY_CONF` 的分支里，
// 是因为官方**两条路都要**：GUI 线程上由 `AG_RECOVERY_CONF` 的分支顺手跑一遍，
// 别的线程置起的 PlayDead 则由下一次入口跑到这里兜住。
static void PlayDeadExec()
{
    if (!orb::agdi::PlayDeadPending()) return;

    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.mainFrame || !c.uvMsg) return;      // 没有 uVision 的窗口句柄，投不出去

    // 官方同一处的判断（`GetWindowThreadProcessId(hMfrm, NULL) == GetCurrentThreadId()`）：
    // 收摊会一路回调 uVision（状态通知、窗口刷新、PDSC 序列），从别的线程做等于
    // 在它的恢复对话框里重入。不在 GUI 线程就留给下一次入口。
    if (GetWindowThreadProcessId(c.mainFrame, nullptr) != GetCurrentThreadId()) {
        ORB_LOG_INFO("AGDI: PlayDead 待执行，但当前不在 uVision 的 GUI 线程上 —— "
                     "留给下一次入口");
        return;
    }

    orb::agdi::PlayDeadClear();
    ORB_LOG_ERROR("AGDI: PlayDead —— 链路已判死且 uVision 要求退出调试：收摊并投递 "
                  "MSG_UV2_TERMINATE (0x%X)", (unsigned)MSG_UV2_TERMINATE);
    SessionTeardown();
    PostMessage(c.mainFrame, c.uvMsg, MSG_UV2_TERMINATE, 0);
}

// ---------------------------------------------------------------------------
// AG_Init
// ---------------------------------------------------------------------------
U32 AG_Init(U16 nCode, void *vp)
{
    orb::agdi::Context& c = orb::agdi::Get();

    // The whole initialisation handshake, logged.  uVision tells the driver
    // where its state lives through these items (AGDI.H:778-796), and a missing
    // one is invisible from the outside: nothing fails, the feature that needed
    // it simply never works.  That is exactly how AG_INITCURPC (0x030F, the
    // address of uVision's own PC variable) stayed unnoticed while the
    // source-line arrow never moved.  A handful of calls per session.

    //暂时屏蔽，26.10.14/19:53
    //ORB_LOG_INFO("Init: nCode=0x%04X vp=0x%p", nCode, vp);

    switch (nCode & 0xFF00) {
    case AG_INITFEATURES:
        InitLogOnce();
        // Re-read the project description for every session.  uVision is not
        // obliged to call EnumUvARM7(2)/DllUv3Cap(109) before this point, and
        // the flash loader needs the device and its RAM window either way.
        if (g_dbgBlk) orb::cfg::AdoptUVision(g_dbgBlk);
        c.loaded = true;
        c.bpHead = nullptr;
        // c.curPC is deliberately NOT cleared here.  AG_INITCURPC (0x030F) is a
        // one-shot item: uVision hands over the address of its own PC variable
        // once, and the reference driver keeps that pointer for the lifetime of
        // the DLL (DbgCM/AGDI.CPP:4422 stores it and nothing ever clears it).
        // Clearing it per session meant the pointer was already gone by the time
        // the first register read tried to publish the PC - the log said
        // "AG_INITCURPC pointer MISSING", and the source-line arrow never moved.

        memset(&orb::agdi::g_supp, 0, sizeof(SUPP));
        orb::agdi::g_supp.MemAccR = 1;
        orb::agdi::g_supp.RegAccR = 0;
        orb::agdi::g_supp.ResetR  = 1;
        // Data watchpoints are served by DWT comparators; ExtBrkSupp tells
        // uVision to hand us its breakpoint list through AG_BreakFunc.
        orb::agdi::g_supp.ExtBrkSupp = 1;
        // ITM/SWO console + DWT PC-sample history + the Trace Data window.
        // Advertised only when the project enabled a trace source, so the Trace
        // / printf windows do not appear (and no target pin is claimed)
        // otherwise.  The history is statistical PC sampling, not full
        // instruction trace.
        {
            const orb::cfg::Config& opt = orb::cfg::Get();
            if (opt.swoBaud > 0 || opt.traceClockKhz > 0 || opt.etb || opt.orbtrace) {
                orb::agdi::g_supp.ItmTrace = 1;   // ITM + DWT trace support
                orb::agdi::g_supp.hTrace   = 1;   // trace recording / Trace window
                orb::agdi::g_supp.hPaLyze  = 1;   // statistical PC profile
            }
        }
        if (vp) *static_cast<SUPP *>(vp) = orb::agdi::g_supp;

        // The whole capability set handed back to uVision, in one line.  Anything
        // wrong here is invisible from the outside: uVision simply never builds
        // the feature that depends on it - which is the state the Registers window
        // is in while AG_AllReg answers correctly.
        ORB_LOG_INFO("Init: AG_INITFEATURES -> MemAccR=%u RegAccR=%u hTrace=%u "
                     "hCover=%u hPaLyze=%u hMemMap=%u ResetR=%u LaSupp=%u "
                     "ExtBrkSupp=%u ExcTrcSupp=%u DbgRecovery=%u SwBreakConf=%u "
                     "ItmTrace=%u TargetMap=%u",
                     orb::agdi::g_supp.MemAccR, orb::agdi::g_supp.RegAccR,
                     orb::agdi::g_supp.hTrace, orb::agdi::g_supp.hCover,
                     orb::agdi::g_supp.hPaLyze, orb::agdi::g_supp.hMemMap,
                     orb::agdi::g_supp.ResetR, orb::agdi::g_supp.LaSupp,
                     orb::agdi::g_supp.ExtBrkSupp, orb::agdi::g_supp.ExcTrcSupp,
                     orb::agdi::g_supp.DbgRecoverySupp,
                     orb::agdi::g_supp.SwBreakConfSupp,
                     orb::agdi::g_supp.ItmTrace, orb::agdi::g_supp.TargetMap);

        // Ask for the debug description *before* connecting.  The connection
        // group (DebugPortSetup / DebugPortStart / DebugDeviceUnlock) is exactly
        // what a device with a locked debug port needs, and it cannot run before
        // we know it exists - asking only after the connect left those sequences
        // dead code on every target (bug.md §N P0).  The call is idempotent and
        // repeats below.
        //
        // The Pack page's "Enable" switch gates the whole PDSC path; without it
        // the driver keeps using the FLM, which is the classic behaviour
        // (bug.md §I: the switch used to be parsed and then ignored).
        if (orb::cfg::Get().pdscEnable) orb::pdsc::Refresh();

        if (!orb::agdi::Connect()) {
            // "The debugger does nothing" is exactly the report this prevents:
            // uVision is about to give up on the driver, so the reason has to be
            // on screen before it does.  Reported, not boxed - uVision raises its
            // own dialog for a failed connect and two boxes is one too many.
            orb::ui::Error("Cannot connect to the target. Check the probe, the "
                           "interface selection and the SWJ clock under Options "
                           "for Target / Debug / Settings.");
            return 1;   // non-zero: uVision gives up on this driver
        }

        // Refresh once more now that the target is up: the description can carry
        // detail that only becomes available with a live connection.  Targets
        // without a description simply leave the PDSC path unavailable and keep
        // using the FLM, which is the normal case.
        if (orb::cfg::Get().pdscEnable) orb::pdsc::Refresh();
        // "without Stop" means exactly that: the session does not halt the core
        // on the way in either.  uVision notices the target is running through
        // AG_WAITFORBRK, which is what that entry point exists for (AGDI.H).
        if (orb::cfg::Get().connect != orb::cfg::kConnectWithoutStop) c.core.Halt();
        orb::agdi::StatusMessage("ORBAGDI connected");
        ReportSession();
        return AG_OK;

    case AG_GETFEATURE: {
        const U32 f = nCode & 0x00FF;
        U32 r = 0;
        switch (f) {
        case AG_F_MEMACCR:     r = orb::agdi::g_supp.MemAccR;       break;
        case AG_F_REGACCR:     r = orb::agdi::g_supp.RegAccR;       break;
        case AG_F_TRACE:       r = orb::agdi::g_supp.hTrace;        break;
        case AG_F_COVERAGE:    r = orb::agdi::g_supp.hCover;        break;
        case AG_F_PALYZE:      r = orb::agdi::g_supp.hPaLyze;       break;
        case AG_F_MEMMAP:      r = orb::agdi::g_supp.hMemMap;       break;
        case AG_F_RESETR:      r = orb::agdi::g_supp.ResetR;        break;
        case AG_F_LANALYZER:   r = orb::agdi::g_supp.LaSupp;        break;
        case AG_F_EXTBRKSUPP:  r = orb::agdi::g_supp.ExtBrkSupp;    break;
        case AG_F_EXCTRC:      r = orb::agdi::g_supp.ExcTrcSupp;    break;
        // 握手：`AG_INITFEATURES` 里这一位是 0，**只有被问到时才答 1 并记住**。
        // 参考的原话（`DbgCM/AGDI.CPP:4488`）：
        //     "Initialize supp.DbgRecoverySupp only when feature is requested,
        //      we need a handshake to determine support in debugger"
        //
        // 答 1 之后 uVision 才会把"调试访问受限"显示出来（它自己收
        // `UV_DBG_ACCESS_LEVEL`），并在恢复流程里发
        // `AG_EXECITEM | AG_RECOVERY_CONF`（见下面那一项）。答 0 时这一整套在
        // uVision 侧根本不会启动 —— 链路断了就只能等用户手动重连。
        case AG_F_DBGRECOVERY: r = orb::agdi::g_supp.DbgRecoverySupp = 1; break;
        case AG_F_SWBREAKCONF: r = orb::agdi::g_supp.SwBreakConfSupp; break;
        case AG_F_ITM_TRACE:   r = orb::agdi::g_supp.ItmTrace;      break;
        default:               r = 0;                               break;
        }
        // Every capability uVision asks about, and what it is told.  uVision
        // decides which debug views exist from these answers, and a wrong one is
        // silent - the view simply never appears.  The Registers window is empty
        // on screen while AG_AllReg answers correctly, so this list is in scope.
        ORB_LOG_INFO("Init: AG_GETFEATURE(%s) -> %u", FeatureName(f), r);
        return r;
    }

    case AG_INITITEM:
        switch (nCode & 0x00FF) {
        case AG_INITCALLBACK:
            c.cb = reinterpret_cast<pCBF>(vp);
            // Store the callback and use it straight away to pick up the BOM
            // ("ioc"): AG_CB_GETBOMPTR is the only way a driver can reach it,
            // and the BOM is what carries the PDSC debug description - the
            // flash sequences are executed through it.  The reference driver
            // does exactly this right after storing the callback
            // (DbgCM/AGDI.CPP:4431).
            //
            // **这里以前有个 `!HaveBom()` 守卫，是个 bug。** 实测顺序是
            // `EnumUvARM7(2)`（dbgblk 那条路）先跑完并 SetBom，于是轮到本项时
            // HaveBom() 已为真，AG_CB_GETBOMPTR **整个被跳过** —— 本驱动从头到尾
            // 用的都是 `dbgblk->pDbgX->pBom`。
            //
            // 而这两个来源**不是同一个实例**（实测：bom=0x015B3F88 与取数门读的
            // app=0x540AC700 不同）。uVision 自己读的是 ioc（AG_CB_GETBOMPTR 的
            // 返回值），它的 Trace Data 取数门就是 ioc 里的 `bTrcDisp` 位
            // （ULP2CM3 机器码 0x100A3EB4 `or dword [pio+0xafc], 8`）。所以必须
            // 无条件取 ioc 并让它成为主指针，dbgblk 那个降为备用实例。
            if (c.cb) {
                struct bom* ioc = reinterpret_cast<struct bom *>(
                    c.cb(AG_CB_GETBOMPTR, NULL));
                if (ioc) orb::pdsc::SetBom(ioc);
            }
            // The callback is one of the three things the reporting channel
            // needs; the other two arrive as separate AG_INITITEM items.
            orb::ui::Attach(c.mainFrame, c.uvMsg, c.cb);
            break;
        case AG_INITPHANDLEP:
            // uVision passes the main frame handle **by value**, as the void*
            // itself - not as a pointer to an HWND.  Keil's own driver casts it
            // straight across (DbgCM/AGDI.CPP:4414 `hMfrm = (HWND)vp`); the
            // AGDI.H comment "pointer to parent handle (MainFrame)" reads the
            // other way, and following it cost us the very first real session:
            // dereferencing the handle value faulted with 0xC0000005 at the
            // moment uVision handed the main frame over (see auto.mk, "崩溃 /
            // 故障定位").  The probe harness never called this item, which is
            // why every offline run stayed green.
            c.mainFrame = reinterpret_cast<HWND>(vp);
            orb::ui::Attach(c.mainFrame, c.uvMsg, c.cb);
            break;
        // The registered message that carries a line of text into uVision's
        // command / build window.  Keil's own drivers store it and then do
        //     SendMessage(hMfrm, Uv2Msg, 0x0C /*or 0x06*/, text);
        // - see ORBAGDI_Ui.h for the disassembly this was read from.  Not
        // answering it used to cost us nothing, because nothing was reported;
        // now it is the difference between "the driver said why" and a session
        // that fails with an empty window.
        case AG_INITUSRMSG:
            c.uvMsg = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(vp));
            orb::ui::Attach(c.mainFrame, c.uvMsg, c.cb);
            break;
        case AG_INITBPHEAD:   c.bpHead = static_cast<AG_BP **>(vp); break;
        case AG_INITCURPC:    c.curPC = static_cast<DWORD *>(vp); break;
        // Flash download: uVision hands the job to the driver in two stages
        // (see ORBAGDI_Flash.cpp).  Returning anything but AG_OK aborts the
        // session - that is the "Target DLL has been cancelled" message.
        case AG_INITFLASHLOAD:
            ORB_LOG_INFO("AGDI: AG_INITFLASHLOAD");
            if (!orb::agdi::FlashInitLoad()) {
                orb::agdi::StatusMessage(orb::agdi::FlashLastError());
                return AG_RDFAILED;
            }
            break;
        case AG_STARTFLASHLOAD:
            // Entry marker: if this is the last line in the log, the freeze is
            // inside FlashStartLoad (its first act is a uVision callback).
            ORB_LOG_INFO("AGDI: AG_STARTFLASHLOAD");
            if (!orb::agdi::FlashStartLoad()) {
                orb::agdi::StatusMessage(orb::agdi::FlashLastError());
                return AG_RDFAILED;
            }
            break;
        default: break;
        }
        break;

    case AG_EXECITEM:
        switch (nCode & 0x00FF) {
        case AG_UNINIT:
            ORB_LOG_INFO("AGDI: AG_UNINIT");
            // 与 PlayDead 走同一套收摊动作（官方那边也是同一个 `StopTarget()`）。
            SessionTeardown();
            break;
        case AG_RESET: {
            // ⚠ **这里不清 trace**（2026-10-14 用户裁定："B：撤掉清空"）。
            // trace 缓冲只在**会话开始**时清（`Connect()` → `TraceResetForTarget("connect")`）。
            if (orb::agdi::Connect()) {
                c.core.Reset(true);
                // 复位把目标侧的 trace 寄存器整套打回默认，而它们的编程只在
                // `Connect()` 里做一次（第二次进来就早退）⇒ 不在这里补一次，
                // "复位后重新跑"这一轮就一个 trace 包都收不到，uVision 的
                // Navigation 条会一直停在上一次的范围上。
                //
                // ⚠ 必须**限定名**调用：本函数（AG_EXECITEM 的 case）在
                // `extern "C"` 的全局作用域里，而 `namespace orb { namespace agdi {`
                // 已在 2031 行闭合 —— 不限定就会 C3861 找不到标识符。
                orb::agdi::RearmTargetTraceAfterReset("reset");
                // The reference installs the Registers-window layout from
                // AG_INITFEATURES, right after the first reset and skipped for a
                // flash download (DbgCM/AGDI.CPP:4372).  This is the equivalent
                // point here - the first time the target is opened.  Idempotent,
                // so the later resets are no-ops.
                orb::agdi::InstallRegView();
                // Tell uVision that the reset completed and the target stopped.
                // The reference does this through the execution hook
                // (DbgCM/AGDI.CPP:2452 and :3254):
                //
                //     pCbFunc(AG_CB_EXECHOOK, "OnStopAfterReset");
                //
                // uVision runs its own "stop after reset" handling from this
                // hook - without it the reset happens and uVision is never told.
                orb::agdi::Callback(AG_CB_EXECHOOK,
                                    const_cast<char *>("OnStopAfterReset"));
            }
            break;
        }
        case AG_STOPREQ:
            // Stop button: unblock a RunAndWait() spinning on another thread.
            InterlockedExchange(&c.stopReq, 1);
            break;
        case AG_RUNSTART:   // 0x0010 - "Go/Step about to start"
            // DbgCM/AGDI.CPP:4526-4529.
            orb::agdi::AnnounceRunStart();
            ORB_LOG_INFO("AGDI: AG_EXECITEM AG_RUNSTART");
            break;
        case AG_RUNSTOP:    // 0x0011 - "Go/Step completed"
            // The reference invalidates its cached target data here
            // (DbgCM/AGDI.CPP:4531-4533); this driver's DAP layer already keeps
            // the System Control Space out of the cache, so there is nothing to
            // flush.
            ORB_LOG_INFO("AGDI: AG_EXECITEM AG_RUNSTOP");
            break;
        case AG_RECOVERY_CONF:  // 0x0060 - Configure Debug Access Recovery
            // DbgCM/AGDI.CPP:4554-4558.  uVision 只会在
            // `AG_GETFEATURE(DBGRECOVERY)` 的握手答了 1 之后才发这一项（见上面
            // AG_GETFEATURE 处的说明）。
            //
            // **返回值必须是 AG_RECOVCONF_ERR_***：uVision 等的是它
            // （AGDI.H:1114-1116），不是 AG_OK。所以算完就 return，不能落到函数
            // 末尾的 `return AG_OK`（那等于答"不支持"）。
            ORB_LOG_INFO("AGDI: AG_EXECITEM AG_RECOVERY_CONF");
            {
                const U32 rc = orb::agdi::ConfigureDebugAccessRecovery(
                    static_cast<const AG_RECOVERY*>(vp));
                // 官方这一支是 `nE = ConfigureDebugAccessRecovery(vp); break;`，
                // 随后落到 `AG_Init` 末尾的 `if (PlayDead) ExecPlayDead();`
                // （DbgCM/AGDI.CPP:4556-4588）—— 也就是说 **exitDebug 的强杀就在
                // 同一次调用里执行**，不是等下一次入口。这里照做。
                PlayDeadExec();
                return rc;
            }
        case AG_STEPINTO_START:  // 0x0014 - Step Into starts
        case AG_STEPLINE_START:  // 0x0015 - Step Over (source line) starts
        case AG_OVERSTEP_START:  // 0x0063 - assembly-mode overstepping
            // DbgCM/AGDI.CPP:4564-4569.  These are what tell the driver that the
            // operation about to run is a step, and therefore that a BKPT sitting
            // at the PC has to be stepped over instead of executed.
            orb::agdi::AnnounceStepStart();
            ORB_LOG_INFO("AGDI: AG_EXECITEM %s",
                         ((nCode & 0x00FF) == AG_STEPINTO_START)  ? "AG_STEPINTO_START" :
                         ((nCode & 0x00FF) == AG_STEPLINE_START)  ? "AG_STEPLINE_START"
                                                                  : "AG_OVERSTEP_START");
            break;
        case AG_TRACESETUP_REQ: {
            // uVision asks the driver to set up trace, once per session, right
            // after AG_INITFEATURES and before the AG_GETFEATURE sweep.  The
            // reference driver has no case for it - its trace window is not
            // ported (DBGCM_FEATURE_TRCDATA_WIN == 0) - so there is no official
            // answer to copy.  What it passes was read off the wire instead:
            // vp is a **zeroed** TRACE_CONFIG, i.e. a buffer for the driver to
            // fill in ("Trace Configuration sent by target", TraceDataTypes.h).
            //
            // Leaving it zeroed is what kept the Trace Data window inert: the
            // driver registered, uVision called ClearTrace, and then never asked
            // for a single record - because from its point of view trace was
            // never configured.  The official driver does the same thing under
            // another name: RegisterTraceClock() publishes TRACE_CLK to uVision
            // (DbgCM/AGDI.CPP:4251).
            //
            // The struct is mirrored rather than included: this file must not
            // pull the TDI headers in, and the layout is two lines.
            struct UvTraceConfig { uint32_t nClock; uint32_t nReserved[23]; };
            UvTraceConfig* tc = static_cast<UvTraceConfig*>(vp);
            if (tc) {
                const orb::cfg::Config& opt = orb::cfg::Get();
                // ⚠ **`nClock` 必须是内核时钟，不是 trace 端口时钟。**
                //
                // 周期号（ETM 的 cycle count、ITM 的时间戳）都是**内核周期**，uVision
                // 拿到 `nClock` 之后按 `time = cycle / nClock` 换算时间。报错时钟，
                // 它算出的时间就与驱动不一致。
                //
                // 2026-10-13 真机实测（F407，内核 48 MHz）：这里报的是 TRACE_CLK
                // 72 MHz，而周期号按内核产生 ⇒ **同一个真实时间**（0.0797 s）在
                // uVision 眼里是 `5,738,724` 个周期、在驱动手里是 `3,825,817` 个
                // （比值正是 72/48）⇒ 它请求的区间整个落在驱动范围之外 ⇒ 驱动回
                // `BOUNDS` ⇒ **窗口只有列没有行、Navigation 条 `0 s → 0 s`**。
                //
                // `tclk=` / TRACE_CLK 只决定 TPIU 的位时钟，与周期号无关；它仍然
                // 通过 `RegisterTraceClock` 报给 uVision（那边要用它算 SWO 分频）。
                uint32_t coreHz = static_cast<uint32_t>(opt.cpuKhz) * 1000u;
                if (coreHz == 0) coreHz = 1;            // 0 就是"没配置"，不能报
                // "Use Core Clock"（`tclkc`）把 trace 端口时钟钉在核心时钟上；没勾时
                // 才看 `tclk=`（0 = 跟随核心时钟）。与 DbgCM 的 TRACE_USE_CORECLK 同一件事。
                const uint32_t trcHz =
                    (opt.useCoreClock || opt.traceClockKhz <= 0)
                        ? coreHz
                        : static_cast<uint32_t>(opt.traceClockKhz) * 1000u;
                tc->nClock = coreHz;
                // TDI 的 Set/GetTraceConfig 用同一个值，两边保持一致。
                orb::agdi::TraceWinSetCoreClock(coreHz);
                // CORE_CLK / TRACE_CLK 两个虚拟寄存器（官方 RegisterClock /
                // RegisterTraceClock）：Trace Data 窗口的时间轴要的就是它们。
                orb::agdi::TraceWinRegisterClocks(coreHz, trcHz);
                // uVision 在这一刻才正式要 trace，窗口多半已经建好了——
                // 顺手把接口表与窗口配置补发一次。
                orb::agdi::TraceWinReconfigure();
                // ≙ 官方 0x1008B7C0 @ 0x10096161（conf 应用）：这是本驱动唯一的
                // "host 改了 trace 配置"通知口，也正是首次发布两个 VTR 的地方 —— 与
                // 官方"conf 应用之后才重发"的时机同构。
                orb::agdi::RefreshTraceClocks();
                ORB_LOG_INFO("AGDI: AG_TRACESETUP_REQ -> nClock=%u Hz (内核；tclk=%d kHz cpu=%d kHz)",
                             coreHz, opt.traceClockKhz, opt.cpuKhz);
            } else {
                ORB_LOG_WARN("AGDI: AG_TRACESETUP_REQ 没有配置结构（vp == NULL）");
            }
            break;
        }

        default:
            break;   // the remaining items are notifications only
        }
        break;

    default:
        break;
    }

    // 官方在 `AG_Init` 末尾的那一句（DbgCM/AGDI.CPP:4584-4588）：
    //
    //     CheckCom(0);
    //     if (PlayDead) { ExecPlayDead(); }
    //
    // 兜住"PlayDead 由别的线程置起"的情况 —— GUI 线程上的那一条（`exitDebug`）
    // 在 `AG_RECOVERY_CONF` 的分支里已经执行过了（见那里的说明）。
    PlayDeadExec();

    return AG_OK;
}

// ---------------------------------------------------------------------------
// AG_MemAtt - memory map / attribute plumbing
// ---------------------------------------------------------------------------
U32 AG_MemAtt(U16 nCode, UL32 nAttr, GADR *pA)
{
    // uVision's attribute traffic is by far the largest part of a working
    // session - measured through the forwarding shim in test/: ~142000
    // AG_GETMEMATT calls, plus ~2900 AG_BPQUERY and ~2900 AG_BPEXQUERY - and it
    // is the clearest sign that uVision is running its per-address refresh cycle
    // at all.  This driver never logged the entry, so there was nothing to
    // compare against when the Registers window stayed empty.  Only the first
    // call of each code is logged, plus a running total every 10000 calls: the
    // volume is far too high for anything else.
    {
        static volatile LONG s_seen  = 0;
        static volatile LONG s_total = 0;
        const LONG total = InterlockedIncrement(&s_total);
        const LONG bit   = 1L << (nCode & 0x1F);
        if ((InterlockedOr(&s_seen, bit) & bit) == 0) {
            ORB_LOG_INFO("MemAtt: first nCode=0x%02X (nAttr=0x%04X, adr=0x%08X)",
                         nCode, nAttr, pA ? pA->Adr : 0u);
        } else if ((total % 10000L) == 0) {
            ORB_LOG_INFO("MemAtt: %ld calls so far", total);
        }
    }

    switch (nCode) {
    case AG_MEMMAP:
        // uVision tells us which address ranges it considers memory.  Keil
        // builds a whole attribute table from this (MMapMem, DbgCM/AGDI.CPP:319);
        // we only record the ranges, which is what AG_BpInfo(AG_CADRVALID) needs.
        if (pA) {
            MapMem(pA->Adr, pA->nLen, nAttr);
            pA->ErrAdr = 0;                // 0 == mapping accepted
        }
        return AG_OK;

    case AG_GETMEMATT:
        // uVision stores this as an opaque descriptor and dereferences it as a
        // DWORD* - Keil's MGetAttr() returns a pointer to the per-address
        // attribute word (DbgCM/AGDI.CPP:329, returned at :5265).  Returning a
        // bit mask here, as this used to, hands uVision a small integer it may
        // follow as a pointer.
        if (pA) {
            pA->Adr = static_cast<UL32>(
                reinterpret_cast<uintptr_t>(AttrPtr(pA->Adr)));
        }
        return AG_OK;

    case AG_GETATTRSEG:
        // Same descriptor, for the 64 kB segment the address lives in - that is
        // what coverage load/save asks for (DbgCM/AGDI.CPP:5274).
        if (pA) {
            pA->Adr = static_cast<UL32>(
                reinterpret_cast<uintptr_t>(AttrPtr(pA->Adr & 0xFFFF0000u)));
        }
        return AG_OK;
    case AG_SETMEMATT:
    case AG_CLEARCOVERAGE:
        return AG_OK;
    default:
        return AG_INVALOP;
    }
}

// ---------------------------------------------------------------------------
// AG_BpInfo - breakpoint queries and notifications.
//
// vp is a GADR* and the return value is the ATRX_* attribute word stored for
// that address (same convention as CMSIS_AGDI.dll).
// ---------------------------------------------------------------------------
U32 AG_BpInfo(U16 nCode, void *vp)
{
    orb::agdi::Context& c = orb::agdi::Get();

    // Breakpoint negotiation trace.  A request uVision never makes and a request
    // the driver answers with 0 look exactly alike from the outside, and both
    // surface as "the breakpoint cannot be set" - so the decision points are
    // logged, in and out.  The high-frequency AG_BPQUERY is left out on purpose.
    const bool trace = (nCode == AG_CADRVALID   || nCode == AG_CANHANDLEBP ||
                        nCode == AG_BPSET       || nCode == AG_BPENABLE   ||
                        nCode == AG_BPDISABLE   || nCode == AG_BPKILL     ||
                        nCode == AG_BPDISALL    || nCode == AG_BPKILLALL);
    const uint32_t bpAdr = (vp && trace) ? (static_cast<GADR *>(vp)->Adr & ~1u) : 0u;
    if (trace) ORB_LOG_INFO("Bp: AG_BpInfo(%s, 0x%08X) ...", orb::agdi::BpCodeName(nCode), bpAdr);

    if (!vp) return 0;

    const uint32_t adr = static_cast<GADR *>(vp)->Adr & ~1u;   // ignore thumb bit
    U32 nA = 0;

    switch (nCode) {
    case AG_BPQUERY:
    case AG_BPEXQUERY:
        nA = static_cast<U32>(AttrGet(adr) & kAttrBpMask);
        break;

    case AG_CADRVALID:
        // The reference has no case for this code, so it falls into its default
        // branch: fetch the attribute pointer, map the address on demand, and
        // return the failure sentinel 0x80000000 when it is still unmapped
        // (DbgCM/AGDI.CPP:7546-7552).  When the address IS mapped the switch
        // matches nothing and nA keeps its initial 0 - so 0 means "valid", and
        // 0x80000000 means "unmapped".  1 is neither.
        //
        // This driver answered 1 for a mapped address.  uVision reads a non-zero
        // answer as the failure sentinel, so every valid code address came back
        // as invalid - and the consequence lands on whatever uVision decides from
        // this answer (it is the "is this code address valid?" question).
        if (!Mapped(adr)) return 0x80000000u;   // failed: unmapped address
        nA = 0;                                 // valid
        break;

    case AG_CANHANDLEBP:
        if (AttrGet(adr) & (ATRX_BREAK | ATRX_WATCH)) nA = 1;  // can remove
        else if (c.bps.Count() < c.bps.Capacity())    nA = 1;  // can add
        break;

    case AG_BPENABLE:
    case AG_BPSET: {
        // Unmapped address: the reference answers with the failure sentinel
        // (DbgCM/AGDI.CPP:7541-7542, `return (0x80000000); // failed: unmapped
        // address`), not with a driver error code.
        if (!Mapped(adr)) return 0x80000000u;

        // Data watchpoint?  uVision marked it as AG_WBREAK in its own list and
        // AG_BreakFunc(AG_BF_LINK) already flagged the attribute cache.  Those
        // are served by a DWT address comparator, not by the FPB.
        uint16_t wacc = 0;
        if (orb::agdi::IsWatch(adr, wacc)) {
            int dwt = -1;
            if (!c.bps.AddWatch(adr, orb::agdi::WatchFunc(wacc), dwt)) return AG_RDFAILED;
            const uint16_t a = static_cast<uint16_t>(
                (AttrGet(adr) & ~ATRX_BPDIS) | ATRX_WATCH);
            AttrSet(adr, a);
            nA = static_cast<U32>(a & kAttrBpMask);
            break;
        }

        if (c.bps.Contains(adr)) {
            c.bps.SetEnabled(adr, true);
        } else {
            bool usedHw = false;
            if (!c.bps.Add(adr, usedHw)) return AG_RDFAILED;
        }
        const uint16_t a = static_cast<uint16_t>(
            (AttrGet(adr) & ~ATRX_BPDIS) | ATRX_BREAK);
        AttrSet(adr, a);
        nA = static_cast<U32>(a & kAttrBpMask);
        break;
    }

    case AG_BPDISABLE: {
        c.bps.SetEnabled(adr, false);   // handles Hw, Sw and Watch slots
        uint16_t a = AttrGet(adr);
        if (a & ATRX_WATCH) a = static_cast<uint16_t>((a & ~ATRX_WATCH) | ATRX_BPDIS);
        else                a = static_cast<uint16_t>((a & ~ATRX_BREAK) | ATRX_BPDIS);
        AttrSet(adr, a);
        nA = static_cast<U32>(a & kAttrBpMask);
        break;
    }

    case AG_BPKILL: {
        c.bps.Remove(adr);
        c.bps.RemoveWatch(adr);
        const uint16_t a = static_cast<uint16_t>(
            AttrGet(adr) & ~(ATRX_BREAK | ATRX_BPDIS | ATRX_WATCH));
        AttrSet(adr, a);
        nA = static_cast<U32>(a & kAttrBpMask);
        break;
    }

    case AG_BPDISALL: {
        // "Disable all" is not "kill all": uVision's breakpoint list keeps its
        // entries and only the target-side installation goes away, so our entries
        // stay too.  Both cases used to call Clear(), which made a disable look
        // like a delete and left uVision's markers flickering against an
        // attribute table that had just been emptied.
        const int n = c.bps.DisableAll();
        uint32_t addrs[64];
        const int cnt = c.bps.Addresses(addrs, 64);
        for (int i = 0; i < cnt; ++i) {
            // ATRX_BPDIS is the "known, but not installed" marker uVision reads
            // back; the BREAK/WATCH bits have to go with it or uVision would
            // still believe the comparator is armed.
            const uint32_t a =
                (static_cast<uint32_t>(AttrGet(addrs[i])) &
                 ~static_cast<uint32_t>(ATRX_BREAK | ATRX_WATCH)) | ATRX_BPDIS;
            AttrSet(addrs[i], a);
        }
        nA = static_cast<U32>(n);
        break;
    }

    case AG_BPKILLALL:
        nA = static_cast<U32>(c.bps.Count());
        c.bps.Clear();
        AttrClear();
        break;

    default:
        break;
    }
    if (trace) ORB_LOG_INFO("Bp: AG_BpInfo(%s, 0x%08X) -> 0x%X", orb::agdi::BpCodeName(nCode), bpAdr, nA);
    return nA;
}

// ---------------------------------------------------------------------------
// AG_BreakFunc - uVision's breakpoint-list hook.
//
// uVision keeps the list itself; this hook is how it tells us what a node is.
// Returning the block unchanged (non-NULL) means "accepted".  The attribute
// cache is kept in sync here so that a data watchpoint (AG_WBREAK) is already
// recognisable when AG_BpInfo(AG_BPQUERY/AG_BPSET) arrives.
// ---------------------------------------------------------------------------
AG_BP * AG_BreakFunc(U16 nCode, U16 n1, GADR *pA, AG_BP *pBp)
{
    orb::agdi::Context& c = orb::agdi::Get();
    (void)n1;
    (void)pA;

    // Same reason as the AG_BpInfo trace: this is where uVision hands a node
    // over (AG_BF_LINK) and where the reference driver is allowed to refuse it.
    ORB_LOG_INFO("Bp: AG_BreakFunc(nCode=0x%X, type=%d, adr=0x%08X, enabled=%d)",
                 nCode, pBp ? static_cast<int>(pBp->type) : -1,
                 pBp ? (pBp->Adr & ~1u) : 0u, pBp ? static_cast<int>(pBp->enabled) : -1);

    if (!pBp) return pBp;

    const uint32_t adr = pBp->Adr & ~1u;
    const bool running = (InterlockedCompareExchange(&c.running, 0, 0) != 0);

    switch (nCode) {
    case AG_BF_ACCEPT:
        // DbgCM/AGDI.CPP:7810-7817 - while the target runs only address
        // breakpoints can still be accepted; a tracepoint, conditional or range
        // breakpoint cannot be armed without stopping it, and the reference
        // refuses by handing back (AG_Bps *)0xFFFFFFFF.
        if (running && (pBp->type == AG_TBREAK || pBp->type == AG_CBREAK ||
                        pBp->type == AG_RBREAK)) {
            ORB_LOG_WARN("Bp: AG_BF_ACCEPT refused (type %u while running)",
                         static_cast<unsigned>(pBp->type));
            return reinterpret_cast<AG_BP *>(static_cast<uintptr_t>(0xFFFFFFFFu));
        }
        return pBp;

    case AG_BF_LINK:        // Notification: 'pBp' will be linked
    case AG_BF_CHANGE:      // 'pBp->enabled' may have changed
    case AG_BF_TARGET:      // Install / uninstall on the target
        // AG_BF_TARGET sends an address breakpoint down this same path
        // (:7819-7821), and refuses to touch anything but a watchpoint for the
        // other types (:7822-7823).
        if (nCode == AG_BF_TARGET && pBp->type != AG_ABREAK &&
            pBp->type != AG_WBREAK) {
            return pBp;
        }
        if (nCode == AG_BF_TARGET || pBp->type == AG_ABREAK ||
            pBp->type == AG_WBREAK) {
            uint16_t a = AttrGet(adr);
            if (pBp->enabled) {
                if (pBp->type == AG_WBREAK) {
                    a = static_cast<uint16_t>(a | ATRX_READ | ATRX_WRITE | ATRX_WATCH);
                } else {
                    a = static_cast<uint16_t>(a | ATRX_EXEC);
                }
            } else {
                if (pBp->type == AG_WBREAK) {
                    a = static_cast<uint16_t>((a & ~ATRX_WATCH) | ATRX_BPDIS);
                } else {
                    a = static_cast<uint16_t>((a & ~ATRX_BREAK) | ATRX_BPDIS);
                }
            }
            AttrSet(adr, a);
        }
        // :7748 / :7780 / :7793 / :7827 - whenever uVision changes a breakpoint
        // while the target runs, the walking start has to be re-synced, or the
        // next BrkWalk walks a list that no longer matches the real one.  This
        // is what the reference's UpdatePBStart() calls are for, and this driver
        // defined the function but never called it.
        if (running) orb::agdi::UpdatePBStart();
        return pBp;

    case AG_BF_UNLINK:      // Notification: 'pBp' will be unlinked
        if (running) orb::agdi::UpdatePBStart();      // :7748
        AttrSet(adr, static_cast<uint16_t>(
            AttrGet(adr) & ~(ATRX_WATCH | ATRX_BREAK | ATRX_BPDIS)));
        return pBp;

    case AG_BF_GETHEAD:
        return nullptr;                 // we do not own uVision's list

    default:                // QUERY / SETATTR / CLEARATTR
        return pBp;
    }
}

// ---------------------------------------------------------------------------
// AG_GoStep - run control
// ---------------------------------------------------------------------------
U32 AG_GoStep(U16 nCode, U32 nSteps, GADR *pA)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return AG_NOTARGET;

    // Every run-control entry starts by clearing the hit reason - the reference
    // does it as the first statement of AG_GoStep (DbgCM/AGDI.CPP:8025
    // `pio->hitbp.nAcc = HIT_NOREASON;`), so uVision never reads a stale "hit"
    // from the previous stop.
    orb::pdsc::ClearHitBp();

    // Run-control trace (codes from AGDI.H:1496-1512: 1 STOPRUN, 2 NSTEP,
    // 3 GOTILADR, 4 GOFORBRK, 5 WAITFORBRK).  "The source-line arrow does not
    // follow the step" and "the register window is empty" are symptoms of this
    // call and of the register read that follows it, and neither used to leave
    // any trace - so a session could not say whether uVision asked at all, or
    // what it was told back.  Both ends are logged.
    ORB_LOG_INFO("GoStep: code=0x%02X nSteps=%u adr=0x%08X ...", nCode, nSteps,
                 pA ? pA->Adr : 0u);

    switch (nCode) {
    case AG_STOPRUN:
        InterlockedExchange(&c.stopReq, 1);
        ORB_LOG_INFO("GoStep: STOPRUN -> stop request posted");
        return AG_OK;

    case AG_NSTEP: {
        // A step is a new operation, so a stop request left over from the
        // previous one must not cancel it.  RunAndWait() already clears the flag
        // for the go-commands; this path did not, and the flag set by AG_STOPRUN
        // therefore stayed set for the rest of the session - the loop below broke
        // on its first iteration, Step() was never reached, and AG_NSTEP returned
        // AG_OK having executed nothing.  The user-visible symptom is that
        // single-stepping silently stops working the first time Stop is pressed.
        // Measured with test\ORBAGDI_AgdiProbe.exe: PC stayed put across
        // AG_NSTEP while a hand-written DHCSR write moved it one instruction
        // (COMPAT_ANALYSIS.md §10.9).
        InterlockedExchange(&c.stopReq, 0);
        InterlockedExchange(&c.running, 1);
        // Hold the probe for the whole step.  AG_STOPRUN above is deliberately
        // outside this, so the Stop button still gets through while a step is in
        // flight.
        orb::agdi::ComLock comLock;
        if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
            InterlockedExchange(&c.running, 0);
            ORB_LOG_WARN("GoStep: NSTEP refused, the probe is busy (%u ms 没拿到锁)",
                         orb::agdi::kProbeWaitMs);
            return AG_NOACCESS;
        }
        // Same trap as in RunAndWait: a breakpoint armed on the instruction being
        // stepped re-triggers on the step itself, so the core halts on the same
        // address and "step" looks like it does nothing.  Disable that one
        // comparator for the duration of the step, re-arm it afterwards.
        const uint32_t atBp = orb::agdi::ReadPC();
        const bool onBp = (atBp != 0u) && c.bps.Contains(atBp);
        if (onBp) {
            ORB_LOG_INFO("GoStep: NSTEP from a breakpoint at 0x%08X - comparator "
                         "temporarily disabled", atBp);
            c.bps.SetEnabled(atBp, false);
        }
        // ---- skip a SW breakpoint instruction sitting at the PC -------------
        //
        // The reference does this as the first thing AG_NSTEP does
        // (DbgCM/AGDI.CPP:8032-8058): read the two bytes at the PC, and if they
        // are the BKPT opcode (0xBE00, SWBKPT16) move the PC past it instead of
        // executing it, decrementing the step count.  uVision arms this through
        // BkpInstructionSkip, which is set by the step-start announcements
        // handled in AG_EXECITEM.
        //
        // Measured case: a target carrying a BKPT at 0x08024D54 stopped there on
        // every run - DFSR reported BKPT and hitbp.nAcc stayed HIT_NOREASON
        // because that address is not in this driver's breakpoint table.  The
        // reference walks past it; this driver executed it, so the core halted
        // where nobody asked it to.
        U32 steps = nSteps;
        if (InterlockedCompareExchange(&orb::agdi::g_bkpSkip, 0, 0) != 0) {
            uint8_t op[2] = {0, 0};
            const uint32_t here = orb::agdi::ReadPC();
            if (here != 0u && c.dap.MemRead(here, op, 2) &&
                op[0] == 0x00u && op[1] == 0xBEu) {        // 0xBE00, little-endian
                ORB_LOG_INFO("GoStep: NSTEP - BKPT at 0x%08X, stepping over it "
                             "instead of executing it", here);
                c.core.WriteRegSel(0x0Fu, here + 2u);      // PC += 2, as the reference
                if (steps > 0) steps--;
                if (InterlockedCompareExchange(&orb::agdi::g_multiGoStep, 0, 0) != 0) {
                    InterlockedExchange(&orb::agdi::g_bkpSkip, 0);   // :8056-8057
                }
            }
        }

        // The step loop, as the reference has it (DbgCM/AGDI.CPP:8065-8077):
        //
        //     for (; nSteps != 0; --nSteps) {
        //         if (StopRun) break;
        //         if (Step() != 0x01) break;
        //         if (pio->hitbp.nAcc != HIT_NOREASON) break;
        //         if (BrkWalk(1) == 1) break;        // landed on a breakpoint
        //         pio->hitbp.nAcc = HIT_NOREASON;
        //     }
        //
        // BrkWalk(1) is what turns "the PC happens to be at an address uVision
        // has a breakpoint on" into a reported hit, with that breakpoint's own
        // number.  Without it a multi-instruction step could walk straight
        // through a breakpoint and uVision would never hear about it.
        U32 rc = AG_OK;
        for (U32 i = 0; i < steps; ++i) {
            if (InterlockedCompareExchange(&c.stopReq, 0, 0) != 0) break;
            if (!c.core.Step()) { rc = AG_RDFAILED; break; }
            if (orb::agdi::BrkWalk(true) == 1) {
                ORB_LOG_INFO("GoStep: NSTEP landed on a breakpoint after %u step(s)",
                             i + 1u);
                break;
            }
            orb::pdsc::ClearHitBp();        // pio->hitbp.nAcc = HIT_NOREASON
        }
        if (onBp) c.bps.SetEnabled(atBp, true);
        InterlockedExchange(&c.running, 0);
        // The PC is published even when the halt could not be confirmed.  Refusing
        // to report one turned every step into "nothing happened", which is worse
        // than a possibly stale value; the anomaly is logged instead.
        const uint32_t pc = orb::agdi::ReadPC();
        if (pA) { pA->Adr = pc; pA->ErrAdr = 0; }

        // Tell uVision how the step ended - DbgCM/AGDI.CPP:8079-8086:
        //
        //     if ((pio->hitbp.nAcc == HIT_NOREASON) || StopRun) {
        //         DWORD bpReason = HIT_EXEC;
        //         if (StopRun) bpReason = HIT_ESC;
        //         SetHitBp(NULL, *pCURPC, bpReason, *pCURPC);
        //     }
        //
        // Without it uVision is left with HIT_NOREASON, "reason undefined", and
        // cannot tell a completed step from anything else.
        {
            const bool stopped = (InterlockedCompareExchange(&c.stopReq, 0, 0) != 0);
            const bool atBp    = (pc != 0u) && c.bps.Contains(pc);
            const uint16_t reason = stopped ? 0x0010u    /* HIT_ESC,  BOM.H:3056 */
                                            : 0x0001u;   /* HIT_EXEC, BOM.H:3052 */
            orb::pdsc::SetHitBp(pc, reason, atBp ? 0x0001u : 0xFFFFu, pc);
            ORB_LOG_INFO("GoStep: NSTEP done - hitbp.nAcc=0x%04X (%s)%s",
                         reason, stopped ? "HIT_ESC" : "HIT_EXEC",
                         atBp ? " at a breakpoint" : "");
        }
        orb::agdi::PublishPC(pc);
        if (rc == AG_OK) {
            ORB_LOG_INFO("GoStep: NSTEP -> 0x00, PC now 0x%08X", pc);
        } else {
            ORB_LOG_WARN("GoStep: NSTEP -> 0x%02X, PC now 0x%08X (halt not confirmed)",
                         rc, pc);
            rc = AG_OK;                 // the step may well have executed
        }
        return rc;
    }

    case AG_GOTILADR: {
        const uint32_t tmp = pA ? pA->Adr : 0;
        uint32_t stop = 0;
        const U32 rc = orb::agdi::RunAndWait(tmp, 0, &stop);
        if (pA) { pA->Adr = stop; pA->ErrAdr = 0; }
        orb::agdi::PublishPC(stop);
        ORB_LOG_INFO("GoStep: GOTILADR(0x%08X) -> 0x%02X, stopped at 0x%08X",
                     tmp, rc, stop);
        return rc;
    }

    case AG_GOFORBRK: {
        uint32_t stop = 0;
        const U32 rc = orb::agdi::RunAndWait(0, 0, &stop);
        if (pA) { pA->Adr = stop; pA->ErrAdr = 0; }
        orb::agdi::PublishPC(stop);
        ORB_LOG_INFO("GoStep: GOFORBRK -> 0x%02X, stopped at 0x%08X", rc, stop);
        return rc;
    }

    case AG_WAITFORBRK: {
        // Target was started externally; block until it stops.
        InterlockedExchange(&c.running, 1);
        orb::agdi::TraceSetRunning(true, false);
        WaitHeartbeat beat("AG_WAITFORBRK");
        for (;;) {
            if (InterlockedCompareExchange(&c.stopReq, 0, 0) != 0) {
                orb::agdi::ComLockWait lock(200);
                if (lock.Held()) c.core.Halt();
                break;
            }
            // 与 RunAndWait 同构：目标在跑的时候只采样一次 DHCSR，锁每次一小
            // 段地拿，worker 才能同时抽数（P0-A）。
            bool halted = false;
            {
                orb::agdi::ComLock lock;
                if (lock.Held()) halted = c.core.IsHalted();
            }
            if (halted) break;
            // 与 RunAndWait 同构：抽数 worker 只排队状态栏消息，投递在这里
            // （调试线程的轮询，没有待发布时只是一次 InterlockedExchange）。
            orb::agdi::TraceWinStatusFlush();
            beat.Tick();
            Sleep(1);
        }
        InterlockedExchange(&c.running, 0);

        // 停机收尾：读回 ETB、最后一次排空、发布停止状态，全都要碰目标或探针。
        orb::agdi::ComLockWait stopLock(500);
        // The ETB may have been armed by an earlier run; read it out now.
        if (orb::agdi::EtbAvailable() && orb::agdi::EtbGetStatus().configured) {
            orb::agdi::EtbProcess(c.dap);
        }
        // Whatever the probe buffered just before the halt.
        orb::agdi::OrbTracePump(c.dap, 5);
        orb::agdi::ItmPump(c.dap, 5);
        orb::agdi::TraceSetRunning(false, true);
        if (pA) { pA->Adr = orb::agdi::ReadPC(); pA->ErrAdr = 0; }
        return AG_OK;
    }

    default:
        return AG_INVALOP;
    }
}

// ---------------------------------------------------------------------------
// AG_MemAcc - memory access from the memory / watch / disassembly windows
// ---------------------------------------------------------------------------
U32 AG_MemAcc(U16 nCode, UC8 *pB, GADR *pA, UL32 nMany)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return AG_NOTARGET;

    // uVision's Flash menu arrives with pA == NULL - it is asking for an
    // operation, not for memory - so the two commands are answered before the
    // accessors below reject a null address.  The reference driver does the same
    // (DbgCM/AGDI.CPP:4889 handles AG_F_ERASE with pA == NULL).
    switch (nCode) {
    case AG_F_ERASE:
        if (!orb::agdi::FlashEraseChip()) {
            orb::agdi::StatusMessage(orb::agdi::FlashLastError());
            return AG_WRFAILED;
        }
        return AG_OK;

    case AG_F_RUN:
        if (!orb::agdi::FlashRunApp()) {
            orb::agdi::StatusMessage(orb::agdi::FlashLastError());
            return AG_WRFAILED;
        }
        return AG_OK;

    default:
        break;
    }

    if (!pA) return AG_INVALOP;

    // Memory traffic from uVision's memory / watch / disassembly windows.
    //
    // This entry point had no logging at all, which is why a failing read was
    // invisible.  The symptom that made it worth adding: an empty Memory window,
    // an empty Disassembly window and an empty Registers window - while every
    // register read through the SCS (which bypasses the cache) still answered
    // correctly and breakpoints still fired.  Failures are always logged; the
    // first calls and a running total give the shape of the traffic.
    {
        static volatile LONG s_calls = 0;
        const LONG call = InterlockedIncrement(&s_calls);
        if (call <= 8) {
            ORB_LOG_INFO("MemAcc: call %ld nCode=0x%02X adr=0x%08X len=%u",
                         call, nCode, pA->Adr, nMany);
        }
    }

    switch (nCode) {
    case AG_READ: {
        if (nMany == 0) return AG_OK;
        if (!c.dap.MemRead(pA->Adr, pB, nMany)) {
            pA->ErrAdr = pA->Adr;
            ORB_LOG_WARN("MemAcc: READ failed at 0x%08X (%u bytes) - the memory "
                         "window stays empty while this fails", pA->Adr, nMany);
            return AG_RDFAILED;
        }
        return AG_OK;
    }

    case AG_F_VERIFY: {
        // Compare target memory against the caller's buffer, chunk by chunk.
        if (nMany == 0) return AG_OK;
        UC8 chunk[256];
        UL32 done = 0;
        while (done < nMany) {
            UL32 n = nMany - done;
            if (n > sizeof(chunk)) n = static_cast<UL32>(sizeof(chunk));
            if (!c.dap.MemRead(pA->Adr + done, chunk, n) ||
                memcmp(chunk, pB + done, n) != 0) {
                pA->ErrAdr = pA->Adr + done;
                ORB_LOG_WARN("MemAcc: VERIFY failed at 0x%08X (%u bytes)",
                             pA->Adr + done, n);
                return AG_RDFAILED;
            }
            done += n;
        }
        return AG_OK;
    }

    case AG_WRITE:
    case AG_WROPC:
    case AG_F_WRITE:
        if (nMany == 0) return AG_OK;
        if (!c.dap.MemWrite(pA->Adr, pB, nMany)) {
            pA->ErrAdr = pA->Adr;
            ORB_LOG_WARN("MemAcc: WRITE failed at 0x%08X (%u bytes)",
                         pA->Adr, nMany);
            return AG_WRFAILED;
        }
        return AG_OK;

    case AG_RDOPC: {
        // Read opcodes - this is the disassembly window's path, so a failure here
        // is what leaves the Disassembly pane showing source lines and no
        // instructions at all.
        if (nMany == 0) return AG_OK;
        if (!c.dap.MemRead(pA->Adr, pB, nMany)) {
            pA->ErrAdr = pA->Adr;
            ORB_LOG_WARN("MemAcc: RDOPC failed at 0x%08X (%u bytes) - the "
                         "disassembly window stays empty while this fails",
                         pA->Adr, nMany);
            return AG_RDFAILED;
        }
        return AG_OK;
    }

    case AG_READFPU:
    case AG_WRITEFPU: {
        // Same target memory, but the access unit is a 32-bit float word.
        if (nMany == 0) return AG_OK;
        const uint32_t bytes = nMany * 4u;
        if (nCode == AG_READFPU) {
            if (!c.dap.MemRead(pA->Adr, pB, bytes)) {
                pA->ErrAdr = pA->Adr;
                return AG_RDFAILED;
            }
        } else {
            if (!c.dap.MemWrite(pA->Adr, pB, bytes)) {
                pA->ErrAdr = pA->Adr;
                return AG_WRFAILED;
            }
        }
        return AG_OK;
    }

    default:
        return AG_INVALOP;
    }
}

// ---------------------------------------------------------------------------
// AG_RegAcc / AG_AllReg - register access
// ---------------------------------------------------------------------------
U32 AG_RegAcc(U16 nCode, U32 nReg, GVAL *pV)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!pV) return AG_INVALOP;
    if (!c.connected) return AG_NOTARGET;

    uint32_t sel = 0;
    if (!orb::Core::RegIndexToSel(nReg, sel)) {
        // A register uVision knows and this driver does not.  Logged, because
        // AG_INVALOP on its own says only "AGDI: invalid operation".
        ORB_LOG_WARN("RegAcc: register id 0x%02X is not mapped -> AG_INVALOP", nReg);
        return AG_INVALOP;
    }

    if (nCode == AG_READ) {
        uint32_t v = 0;
        if (!c.core.ReadRegSel(sel, v)) {
            // Per-register reads are polled constantly, so only failures are
            // logged - but they are logged, because a single failing selector
            // (xPSR / MSP / PSP / SYS at the end of the sequence) is enough to
            // make the whole register window go empty.
            ORB_LOG_ERROR("RegAcc: read of reg 0x%02X (DCRSR sel 0x%02X) failed",
                          nReg, sel);
            return AG_RDFAILED;
        }
        pV->ul = v;
        return AG_OK;
    }
    if (nCode == AG_WRITE) {
        if (!c.core.WriteRegSel(sel, pV->ul)) {
            ORB_LOG_ERROR("RegAcc: write of reg 0x%02X (DCRSR sel 0x%02X) failed",
                          nReg, sel);
            return AG_WRFAILED;
        }
        return AG_OK;
    }
    ORB_LOG_WARN("RegAcc: unhandled nCode 0x%02X (reg 0x%02X) -> AG_INVALOP",
                 nCode, nReg);
    return AG_INVALOP;
}

U32 AG_AllReg(U16 nCode, void *pR)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!pR) return AG_INVALOP;
    if (!c.connected) return AG_NOTARGET;

    // The reference refuses register access while the target runs
    // (DbgCM/AGDI.CPP:4918 `if (iRun && !supp.RegAccR) return AG_NOACCESS;`, and
    // RegAccR is 0 for this driver as well).  A running core cannot answer
    // DCRSR, so serving the read returns whatever the AP happens to hold - which
    // is how a register window ends up showing DHCSR's value.
    if (InterlockedCompareExchange(&c.running, 0, 0) != 0) {
        ORB_LOG_WARN("AllReg: refused while the target runs (AG_NOACCESS)");
        return AG_NOACCESS;
    }
    // ...and hold the probe for the transfer itself, or a step starting on the
    // other thread lands in the middle of it and both read the same DCRDR.
    orb::agdi::ComLock comLock;
    if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
        ORB_LOG_WARN("AllReg: refused, the probe is busy (%u ms 没拿到锁, AG_NOACCESS)",
                     orb::agdi::kProbeWaitMs);
        return AG_NOACCESS;
    }

    RgARMCM* r = static_cast<RgARMCM *>(pR);
    if (nCode == AG_READ) {
        // uVision paints the Registers window and moves the source-line arrow
        // from this call, so a failure here is the difference between "the
        // registers are empty and the arrow never moves" and a working session.
        // The PC is logged because that is the value the arrow follows.
        if (!c.core.ReadRegs(*r)) {
            ORB_LOG_ERROR("AllReg: AG_READ failed - a register read did not complete");
            return AG_RDFAILED;
        }
        ORB_LOG_INFO("AllReg: AG_READ -> PC=0x%08X SP=0x%08X LR=0x%08X xPSR=0x%08X",
                     r->Rn[15], r->Rn[13], r->Rn[14], r->xPSR);
        orb::agdi::PublishPC(r->Rn[15]);
        orb::pdsc::SetThumbState((r->xPSR & 0x01000000u) != 0u);  // T_Bit
        return AG_OK;
    }
    if (nCode == AG_WRITE) {
        if (!c.core.WriteRegs(*r)) return AG_WRFAILED;
        orb::agdi::PublishPC(r->Rn[15]);
        // T bit is bit 24 of xPSR (T_Bit, COLLECT.H:274) - bit 5 is where the
        // ARM7 CPSR keeps it.  The read path above was corrected and this one was
        // missed, so every register write told uVision the core had switched to
        // ARM state, and it decoded what followed accordingly.
        orb::pdsc::SetThumbState((r->xPSR & 0x01000000u) != 0u);
        return AG_OK;
    }

    // The FPU block.  The official driver implements both of these
    // (DbgCM/AGDI.CPP:4949 AG_READFPU, :4954 AG_WRITEFPU, both on RgARMFPU);
    // this one used to fall through to AG_INVALOP.  On an M4F target - the
    // STM32F407 here - uVision asks for the FPU registers on every register
    // refresh, so the answer it got was "*** error 123: AGDI: invalid operation"
    // and the failed refresh is why the Registers window stayed empty and the
    // source-line arrow never followed a step.
    if (nCode == AG_READFPU) {
        RgARMFPU* f = static_cast<RgARMFPU *>(pR);
        if (!c.core.ReadFpuRegs(*f)) return AG_RDFAILED;
        ORB_LOG_INFO("AllReg: AG_READFPU -> S0=0x%08X FPSCR=0x%08X",
                     f->Sn[0], f->FPSCR);
        return AG_OK;
    }
    if (nCode == AG_WRITEFPU) {
        const RgARMFPU* f = static_cast<const RgARMFPU *>(pR);
        if (!c.core.WriteFpuRegs(*f)) return AG_WRFAILED;
        return AG_OK;
    }

    // Anything else is a code this driver does not implement.  It is logged
    // rather than returned silently: uVision only ever says "AGDI: invalid
    // operation", which names neither the entry point nor the code, and a
    // missing handler stayed invisible for exactly that reason.
    ORB_LOG_WARN("AllReg: unhandled nCode 0x%02X -> AG_INVALOP", nCode);
    return AG_INVALOP;
}

// ---------------------------------------------------------------------------
// AG_BreakResAlloc - how many breakpoints the debugger may allocate
// ---------------------------------------------------------------------------
U32 AG_BreakResAlloc(U32 nAlloc, AG_BP *pBp)
{
    // "Returns 0 on success, otherwise error code" is the whole contract
    // (DbgCM/AGDI.CPP:8322-8326; the codes are in DbgCM/BreakResources.h:149-178,
    // RES_OK = 0, RES_ERR_USED_BREAK = 19).  This used to return the number of
    // *available* breakpoints instead, so a perfectly healthy "26" was read as an
    // error code and uVision answered every attempt to set a breakpoint with
    // "Cannot set breakpoint(s)." - while the driver's own temporary breakpoints
    // (RunAndWait -> bps.Add) kept working, which is exactly what made it look
    // like a target problem rather than an ABI one.
    (void)pBp;
    orb::agdi::Context& c = orb::agdi::Get();
    const int avail = c.bps.Capacity() - c.bps.Count();
    const U32 rc = (avail >= static_cast<int>(nAlloc)) ? 0u   /* RES_OK */
                                                        : 19u  /* RES_ERR_USED_BREAK */;
    ORB_LOG_INFO("Bp: AG_BreakResAlloc(%u) -> %u (used %d of %d)",
                 nAlloc, rc, c.bps.Count(), c.bps.Capacity());
    return rc;
}

// ---------------------------------------------------------------------------
// AG_Serial - debugger <-> DLL serial channel.
//
// The reference forwards the serial-window traffic through uVision's own
// registered message (DbgCM/AGDI.CPP:8173-8216):
//
//     struct SerAS va;
//     va.n1 = nSerNo; // 0:=Serial #1, 1:=Serial #2
//     va.n2 = nMany;  // number of items to write
//     case AG_SERBOUT: va.v.pS = (SC8 *)vp;
//                      SendMessage(hMfrm, Uv2Msg, MSG_UV2_SERBOUT, (LPARAM)&va);
//     case AG_SERWOUT: va.v.pW = (U16 *)vp;
//                      SendMessage(hMfrm, Uv2Msg, MSG_UV2_SERWOUT, (LPARAM)&va);
//     case AG_SERXIN:  if (nSerNo == 3) { ... } else MessageBeep(MB_OK);
//
// This driver answered all three with AG_OK and dropped the data, so a target
// that writes to Serial #1/#2 (a monitor, a printf redirect, RTA) produced
// nothing at all - and there was no trace of it, because the entry point
// returned success.
//
// AG_ITMBOUT is the ITM console: nSerNo is the stimulus port, nMany the byte
// count and vp the bytes.  It goes to the ITM/printf viewers through
// AG_CB_ITMDATA with the same struct.
// ---------------------------------------------------------------------------
U32 AG_Serial(U16 nCode, U32 nSerNo, U32 nMany, void *vp)
{
    orb::agdi::Context& c = orb::agdi::Get();

    SerAS va;
    memset(&va, 0, sizeof(va));
    va.n1 = nSerNo;                     // 0 = Serial #1, 1 = Serial #2
    va.n2 = nMany;                      // number of items

    switch (nCode) {
    case AG_SERBOUT:                    // nMany bytes to Serial #1
        if (!vp || nMany == 0) return AG_OK;
        va.v.pS = static_cast<SC8 *>(vp);
        if (c.mainFrame && c.uvMsg) {
            SendMessage(c.mainFrame, c.uvMsg, MSG_UV2_SERBOUT,
                        reinterpret_cast<LPARAM>(&va));
        }
        return AG_OK;

    case AG_SERWOUT:                    // nMany words to Serial #1
        if (!vp || nMany == 0) return AG_OK;
        va.v.pW = static_cast<U16 *>(vp);
        if (c.mainFrame && c.uvMsg) {
            SendMessage(c.mainFrame, c.uvMsg, MSG_UV2_SERWOUT,
                        reinterpret_cast<LPARAM>(&va));
        }
        return AG_OK;

    case AG_SERXIN:                     // key pressed in Serial Window #1 / #2
        if (nSerNo != 3) MessageBeep(MB_OK);    // :8199-8203
        return AG_OK;

    case AG_ITMBOUT: {
        if (nMany == 0 || !vp) return AG_OK;
        SerAS itm;
        memset(&itm, 0, sizeof(itm));
        itm.n1 = nSerNo;                // ITM stimulus port
        itm.n2 = nMany;                 // bytes
        itm.n3 = 1;                     // 1 = byte data
        itm.v.pU = static_cast<UC8 *>(vp);
        orb::agdi::Callback(AG_CB_ITMDATA, &itm);
        return AG_OK;
    }

    default:
        return AG_INVALOP;
    }
}

// ---------------------------------------------------------------------------
// AG_HistFunc - execution history (back-trace window).
//
// Contract taken from Keil's own driver (DbgCM/AGDI.CPP CalcNumRecords /
// AG_HistFunc): record 1 is the newest sample, larger indices are older, and
// while the target runs the record count is reported as 0 so uVision does not
// read the buffer mid-flight.  The samples come from the DWT PC-sample packets
// decoded in ORBAGDI_Itm.cpp.
// ---------------------------------------------------------------------------
U32 AG_HistFunc(U32 nCode, I32 indx, I32 dir, void *vp)
{
    orb::agdi::Context& c = orb::agdi::Get();

    switch (nCode) {
    case 0:   // index navigation: dir != 0 -> older, dir == 0 -> newer
        if (c.running) return 0;
        {
            const I32 n = static_cast<I32>(orb::agdi::HistCount());
            if (n == 0) return 0;
            if (dir) return (indx < n) ? (indx + 1) : indx;
            return (indx > 1) ? (indx - 1) : 1;
        }

    case 1: { // fetch the PC of one record; uVision reads the return value
        (void)vp;
        uint32_t pc = 0;
        if (indx >= 1 && orb::agdi::HistPc(static_cast<unsigned>(indx), pc)) {
            return pc;
        }
        return 0;
    }

    case 2:   // register snapshot for a record: not provided
        (void)vp;
        return 0;

    case 3:   // number of records
        if (c.running) return 0;
        return orb::agdi::HistCount();

    default:
        return 0;
    }
}

// ===========================================================================
// Extended target access: AG_MemAccX / AG_DapAcc
//
// Ported in full from DbgCM/AGDI.CPP:17548-18793 - the *active* branch.  The
// copies between :16952 and :17545 sit inside an `#if 0`; only the reworked
// implementation at the end of the file is compiled.
//
// The reference's structure is kept as it is: a dispatch entry point, one
// handler per command, and a validate/execute split for the three access types
// (MEM / AP / DP).  What is mapped onto this driver is the transport and the
// port/AP state:
//
//   reference                        this driver
//   -------------------------------------------------------------------------
//   PlayDead                         !c.connected
//   iRun                             c.running
//   supp.MemAccR                     orb::agdi::g_supp.MemAccR
//   LinkCom(1) / CheckCom(1)         orb::agdi::ComLock
//   SwitchDP(id, false)              c.dap.SetDapIndex(id)
//   ReadDP / WriteDP                 c.dap.ReadDP / c.dap.WriteDP
//   ReadAP / WriteAP                 c.dap.ReadApReg / c.dap.WriteApReg
//   ReadARMMemD8/16/32, Write...     c.dap.MemRead / c.dap.MemWrite
//   ReadCache / WriteCache           the Dap read cache, applied internally
//   AP_Context[dp][ap].AccSizes      AP_Context below
//   JTAG_devs.com_no / nCPU          this driver's DAP index
//   MonConf.AP / AP_Sel              AP_Sel below
//   PDSCDebug_*                      orb::pdsc, or the "not enabled" answer
//   SWJ_Sequence / JTAG_Sequence     rddi CMSIS_DAP_SWJ_Sequence
//   DAPAbortVal / SWJ_Clock          rddi DAP_WriteReg / CMSIS_DAP_SWJ_Clock
// ===========================================================================

// ---- DP SELECT AP field (Cortex-M: APSEL is bits 31:24) -------------------
#define APSEL   0xFF000000u
#define APSEL_P 24
#define AP_SEL2IDX(ap) (((ap) & APSEL) >> APSEL_P)

// ---- memory access attributes (DbgCM/Debug.h:950-956) ---------------------
#define BLOCK_NADDRINC    0x1u
#define BLOCK_SECTYPE     0x6u
#define BLOCK_SECTYPE_P   1
#define BLOCK_SECTYPE_ANY 0x0u

// ---- AP supported access sizes (DbgCM/Debug.h:179-182) --------------------
// Defined above, next to SetApAccSizes(), because Connect() needs them.

// ---- debug port type (DbgCM/Debug.h:37-41) --------------------------------
#define JTAG_DP 0
#define SW_DP   1
#define DP_NUM  64
#define AP_NUM  32

// ---- protocol errors the transport reports (DbgCM/COLLECT.H:107-109) ------
#define EU05 5
#define EU08 8
#define EU14 14
#define EU41 41
#define EU42 42
#define EU43 43

static DWORD DP_Type = SW_DP;    // which protocol the probe drives
static U32   AP_Sel  = 0;        // DP SELECT[31:24] - the selected AP
static BOOL  MemAccX_AP = FALSE; // an explicit AP was selected for this access

struct AP_CONTEXT {
    U32 AccSizes;                // AP_ACCSZ_BYTE | AP_ACCSZ_HWORD | AP_ACCSZ_WORD
};
static AP_CONTEXT AP_Context[DP_NUM][AP_NUM];

// Called from the connect path once the protocol is known.
void SetDapPortType(bool jtag) { DP_Type = jtag ? JTAG_DP : SW_DP; }

// An AP context is what AG_MemAccX(ACCMX_QUERY_ACC) answers out of, so it has to
// be filled before uVision asks.  AHB-AP and AXI-AP both serve byte, half-word
// and word accesses through CSW.SIZE.
void SetApAccSizes(int dp, int ap, U32 sizes) {
    if (dp < 0 || dp >= DP_NUM || ap < 0 || ap >= AP_NUM) return;
    AP_Context[dp][ap].AccSizes = sizes;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// DbgCM/AGDI.CPP:16956-16969 - "Clear error codes that can happen during memory
// accesses and indicate potentially non-persistant issues".
static DWORD SkipTargetAccErr(DWORD status) {
    switch (status) {
    case EU05:  // JTAG communication error
    case EU08:  // SWD communication error
    case EU14:  // Cannot access memory
        return 0;
    default:
        return status;
    }
}

// DbgCM/AGDI.CPP:16972-16992.
static U32 MemAccX_SetErr(ACCMEMX* pIn, U64 addr, U32 err) {
    pIn->bErr     = 1;
    pIn->nErrAddr = addr;
    pIn->nErr     = err;
    if (pIn->bDebugPort) {
        pIn->nErrDP = pIn->debugPort;
    } else {
        pIn->nErrDP = static_cast<U32>(-1);
    }
    pIn->nErrAP = pIn->bAccessPort ? pIn->accessPort : ((AP_Sel & APSEL) >> APSEL_P);
    return ACCMX_FAILED;
}

// The reference reaches the PDSC debug engine here.  This driver's PDSC support
// is the sequence runner in orb::pdsc; when the project has no debug description
// there is nothing to ask, which is exactly the case the reference guards with
// PDSCDebug_IsEnabled().
static bool PdscDebugEnabled() { return orb::pdsc::HaveBom(); }

// ---------------------------------------------------------------------------
// DAP access command handlers (DbgCM/AGDI.CPP:17548-17863)
// ---------------------------------------------------------------------------

// DbgCM/AGDI.CPP:17549-17554.
static __inline U32 DapAcc_SetErr(ACCDAP* pIn, U32 err) {
    pIn->bErr = 1;
    pIn->nErr = err;
    return ACCDAP_FAILED;
}

// DbgCM/AGDI.CPP:17563-17620 - read/write the debug pins.
//
//   pIn->pOut8 : value to write
//   pIn->pIn8  : value read back
//   pIn->nMask : pin select
//   pIn->nDelay: time to wait for the pins to settle (microseconds)
//
// Bit layout: 0 TCK/SWCLK, 1 TMS/SWDIO, 2 TDI, 3 TDO/SWO, 4 nTRST, 5 nRESET.
static U32 DapAcc_Pins(ACCDAP* pIn) {
    int status = 0;
    U32 nE     = ACCDAP_DONE;
    int res    = 0;

    if (pIn == NULL) return ACCDAP_BADPARM;
    if (pIn->pOut8 == NULL || pIn->pIn8 == NULL) {
        nE = ACCDAP_BADPARM;
        goto end;
    }

    // The debug unit may not be able to drive every pin.
    switch (DP_Type) {
    case JTAG_DP:
        if (pIn->nMask & ~(ACCDAP_PIN_TCK_SWCLK | ACCDAP_PIN_TMS_SWDIO |
                           ACCDAP_PIN_TDI | ACCDAP_PIN_TDO_SWO |
                           ACCDAP_PIN_nTRST | ACCDAP_PIN_nRESET)) {
            nE = DapAcc_SetErr(pIn, ACCDAP_ERR_PIN_INVALID);
            goto end;
        }
        break;
    case SW_DP:
        if (pIn->nMask & ~(ACCDAP_PIN_TCK_SWCLK | ACCDAP_PIN_TMS_SWDIO |
                           ACCDAP_PIN_nRESET)) {
            nE = DapAcc_SetErr(pIn, ACCDAP_ERR_PIN_INVALID);
            goto end;
        }
        break;
    }

    {
        const orb::rddi::Api& api = orb::rddi::Get();
        if (!api.CMSIS_DAP_SWJ_Pins) {
            nE = DapAcc_SetErr(pIn, ACCDAP_ERR_PIN_NOTSUPP);
            goto end;
        }
        status = api.CMSIS_DAP_SWJ_Pins(orb::agdi::Get().dap.Handle(),
                                        static_cast<unsigned char>(pIn->nMask),
                                        *pIn->pOut8, &res,
                                        static_cast<int>(pIn->nDelay));
        *pIn->pIn8 = static_cast<uint8_t>(res);
    }

end:
    if (status != 0 && nE == ACCDAP_DONE) {
        nE = DapAcc_SetErr(pIn, ACCDAP_ERR_GENERAL);
    }
    return nE;
}

// DbgCM/AGDI.CPP:17627-17654 - delay execution.  The reference notes that this
// is done on the driver level, so the USB round trip already makes it
// inaccurate; the delay is in microseconds.
static U32 DapAcc_Delay(ACCDAP* pIn) {
    if (pIn == NULL) return ACCDAP_BADPARM;
    if (pIn->nDelay == 0) return ACCDAP_DONE;

    if (pIn->nDelay % 1000) Sleep((pIn->nDelay / 1000) + 1);
    else                    Sleep(pIn->nDelay / 1000);
    return ACCDAP_DONE;
}

// DbgCM/AGDI.CPP:17662-17694 - write the SWDIO/TMS pin for nCnt cycles.
static U32 DapAcc_SeqSWJ(ACCDAP* pIn) {
    int status = 0;
    U32 nE     = ACCDAP_DONE;

    if (pIn == NULL) return ACCDAP_BADPARM;
    if (pIn->nManyOut * 8 < pIn->nCnt) return ACCDAP_BADPARM;

    {
        const orb::rddi::Api& api = orb::rddi::Get();
        if (!api.CMSIS_DAP_SWJ_Sequence) {
            return DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_NOTSUPP);
        }
        // The RDDI entry point takes the sequence as bytes; the reference passes
        // a 64-bit word, so it is marshalled in little-endian order.
        uint8_t bits[8];
        const uint64_t v = pIn->pOut64 ? *pIn->pOut64 : 0ull;
        for (int i = 0; i < 8; ++i) bits[i] = static_cast<uint8_t>(v >> (8 * i));
        status = api.CMSIS_DAP_SWJ_Sequence(orb::agdi::Get().dap.Handle(),
                                            static_cast<int>(pIn->nCnt), bits);
    }

    switch (status) {
    case 0:   break;
    case EU41: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_NOTSUPP); break;
    case EU42: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_ASM);     break;
    case EU43: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_EXEC);    break;
    default:   nE = DapAcc_SetErr(pIn, ACCDAP_ERR_GENERAL);      break;
    }
    return nE;
}

// DbgCM/AGDI.CPP:17702-17743 - write a JTAG sequence (fixed TMS, TDI values).
// The reference refuses this outright for SW-DP (:17715-17722).
static U32 DapAcc_SeqJTAG(ACCDAP* pIn) {
    U32 nE = ACCDAP_DONE;

    if (pIn == NULL || pIn->pOut64 == NULL || pIn->pIn64 == NULL) {
        return ACCDAP_BADPARM;
    }
    if (pIn->nManyOut * 8 < pIn->nCnt) return ACCDAP_BADPARM;

    switch (DP_Type) {
    case JTAG_DP:
        break;
    case SW_DP:
    default:
        // Only supported for JTAG debugging!
        return DapAcc_SetErr(pIn, ACCDAP_ERR_PROT_CMDNOTSUPP);
    }

    // A raw JTAG bit sequence is not something this driver's transport exposes.
    return DapAcc_SetErr(pIn, ACCDAP_ERR_CMDNOTSUPP);
}

// DbgCM/AGDI.CPP:17749-17817 - write the 32-bit value to the DAP ABORT register.
static U32 DapAcc_ABORT(ACCDAP* pIn) {
    int   status = 0;
    U32   nE     = ACCDAP_DONE;
    U32   _dpID  = 0;
    DWORD oDP    = 0;                       // JTAG_devs.com_no

    if (pIn == NULL || pIn->pOut32 == NULL) return ACCDAP_BADPARM;
    if (pIn->nManyOut != 4) return ACCDAP_BADPARM;

    orb::agdi::Context& c = orb::agdi::Get();
    oDP = static_cast<DWORD>(c.dap.DapIndex());

    if (pIn->bDebugPort) {
        if (!PdscDebugEnabled()) {
            return DapAcc_SetErr(pIn, ACCDAP_ERR_NOSUPP_DP);
        }
        // SWD has exactly one DP, so the internal id is 0; the PDSC device id is
        // only a selector in a multi-DP JTAG chain.
        _dpID = 0;
        if (pIn->debugPort != 0) {
            return DapAcc_SetErr(pIn, ACCDAP_ERR_INVALID_DP);
        }
        c.dap.SetDapIndex(static_cast<int>(_dpID));
    }

    if (!c.dap.WriteDP(DAP_DP_REG_ABORT, *pIn->pOut32)) status = EU08;

    if (pIn->bDebugPort) {
        if (status) c.dap.SetDapIndex(static_cast<int>(oDP));
        else        c.dap.SetDapIndex(static_cast<int>(oDP));
    }

    if (pIn->bSkipAccErr) status = static_cast<int>(SkipTargetAccErr(static_cast<DWORD>(status)));

    if (nE == ACCDAP_DONE) {
        switch (status) {
        case 0:   break;
        case EU41: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_NOTSUPP); break;
        case EU42: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_ASM);     break;
        case EU43: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_EXEC);    break;
        default:   nE = DapAcc_SetErr(pIn, ACCDAP_ERR_GENERAL);      break;
        }
    }
    return nE;
}

// DbgCM/AGDI.CPP:17823-17863 - change the debug clock.
static U32 DapAcc_Clock(ACCDAP* pIn) {
    int status = 0;
    U32 nE     = ACCDAP_DONE;
    U32 cid    = 0;

    if (pIn == NULL || pIn->pOut32 == NULL) return ACCDAP_BADPARM;
    if (pIn->nManyOut != 4) return ACCDAP_BADPARM;

    // The reference runs the value through PDSCDebug_DebugClockId(); this driver
    // has no such mapping table, so the value is the clock in Hz.
    cid = *pIn->pOut32;

    {
        const orb::rddi::Api& api = orb::rddi::Get();
        if (!api.CMSIS_DAP_SWJ_Clock) return DapAcc_SetErr(pIn, ACCDAP_ERR_GENERAL);
        status = api.CMSIS_DAP_SWJ_Clock(orb::agdi::Get().dap.Handle(), cid);
    }

    switch (status) {
    case 0:   break;
    case EU41: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_NOTSUPP); break;
    case EU42: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_ASM);     break;
    case EU43: nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_EXEC);    break;
    default:   nE = DapAcc_SetErr(pIn, ACCDAP_ERR_GENERAL);      break;
    }
    return nE;
}

// ---------------------------------------------------------------------------
// AG_DapAcc (DbgCM/AGDI.CPP:17869-17950)
// ---------------------------------------------------------------------------
U32 AG_DapAcc(ACCDAP *pIn)
{
    DWORD nE         = 0;
    int   lockStatus = 0;

    orb::agdi::Context& c = orb::agdi::Get();

    // Driver disconnected.  The reference also lets target accesses through
    // while a PDSC sequence runs; this driver is connected for the whole of one.
    if (!c.connected) {
        return ACCDAP_CONNECTION;                   // :17876-17879
    }

    if (pIn == NULL) {
        return ACCDAP_BADPARM;                      // :17884-17886
    }

    nE = ACCDAP_DONE;

    // "Lock target communication.  The following must not be interrupted."
    orb::agdi::ComLock comLock;                     // LinkCom(1) / CheckCom(1)
    if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
        return ACCDAP_FAILED;                       // :17897-17899
    }
    lockStatus = 0;

    switch (pIn->accCmd) {
    case ACCDAP_PINS:       nE = DapAcc_Pins(pIn);     break;   // :17903
    case ACCDAP_DELAY:      nE = DapAcc_Delay(pIn);    break;   // :17906
    case ACCDAP_SEQ_SWJ:    nE = DapAcc_SeqSWJ(pIn);   break;   // :17909
    case ACCDAP_SEQ_JTAG:   nE = DapAcc_SeqJTAG(pIn);  break;   // :17912
    case ACCDAP_ABORT:      nE = DapAcc_ABORT(pIn);    break;   // :17915
    case ACCDAP_CLOCK:      nE = DapAcc_Clock(pIn);    break;   // :17918
    case ACCDAP_ATOMIC_START:                                   // :17921-17925
    case ACCDAP_ATOMIC_EXEC:
    case ACCDAP_ATOMIC_RESULT:
    case ACCDAP_ATOMIC_ABORT:
        nE = DapAcc_SetErr(pIn, ACCDAP_ERR_ATOM_NOTSUPP);
        break;
    default:
        nE = ACCDAP_BADCMD;                                     // :17926-17928
        break;
    }

    // "Unlock target communication."
    (void)lockStatus;

    if (nE == ACCDAP_DONE && pIn->bErr == 0) {
        if (lockStatus) nE = ACCDAP_FAILED;         // :17939-17943
    }

    return nE;
}

// ---------------------------------------------------------------------------
// MemAccX helpers (DbgCM/AGDI.CPP:18166-18683)
// ---------------------------------------------------------------------------

// DbgCM/AGDI.CPP:18166-18191.
static U32 MemAccx_SelectDP(ACCMEMX *pIn, U32 *internalDP)
{
    const DWORD nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);

    if (!PdscDebugEnabled()) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_NOSUPP_DP);
    }
    // SWD exposes exactly one DP.  The PDSC device id selects an index in a JTAG
    // chain; with one DP the only valid value is 0.
    *internalDP = 0;
    if (pIn->debugPort != 0) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_INVALID_DP);
    }
    orb::agdi::Get().dap.SetDapIndex(static_cast<int>(*internalDP));
    return ACCMX_DONE;
}

// DbgCM/AGDI.CPP:18194-18281.
static U32 MemAccx_MemAcc_Validate(ACCMEMX *pIn)
{
    DWORD       nAddr;
    AP_CONTEXT *apCtx;
    DWORD       ap, dp;

    if (pIn->nMany == 0) {
        return ACCMX_BADPARM;                       // invalid input parameters
    }

    if (pIn->nAddr & 0xFFFFFFFF00000000ULL) {       // supported address range?
        return MemAccX_SetErr(pIn, pIn->nAddr, ACCMX_ERR_ACC_BOUNDS);
    }
    nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);

    if (pIn->bAccessPort && (pIn->accessPort & 0xFFFFFF00)) {   // valid AP index?
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_INVALID_AP);
    }

    dp = pIn->bDebugPort ? pIn->debugPort : 0;      // JTAG_devs.com_no
    ap = pIn->bAccessPort ? pIn->accessPort : AP_SEL2IDX(AP_Sel);
    if (dp >= DP_NUM) return MemAccX_SetErr(pIn, dp, ACCMX_ERR_INVALID_DP);
    if (ap >= AP_NUM) return MemAccX_SetErr(pIn, ap, ACCMX_ERR_INVALID_AP);

    apCtx = &AP_Context[dp][ap];

    switch (pIn->accSize) {
    case ACCMX_U8:
        if (!(apCtx->AccSizes & AP_ACCSZ_BYTE)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_SIZE);
        }
        if (!pIn->bNoAddrIncr &&
            ((pIn->nAddr + pIn->nMany - 1) & 0xFFFFFFFF00000000ULL)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_BOUNDS);
        }
        break;

    case ACCMX_U16:
        if (!(apCtx->AccSizes & AP_ACCSZ_HWORD)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_SIZE);
        }
        if (!pIn->bNoAddrIncr &&
            ((pIn->nAddr + pIn->nMany * 2 - 1) & 0xFFFFFFFF00000000ULL)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_BOUNDS);
        }
        if (pIn->bNoAddrIncr && (pIn->nAddr & 0x1)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ALIGN);
        }
        break;

    case ACCMX_U32:
        if (!pIn->bNoAddrIncr &&
            ((pIn->nAddr + pIn->nMany * 4 - 1) & 0xFFFFFFFF00000000ULL)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_BOUNDS);
        }
        if (pIn->bNoAddrIncr && (pIn->nAddr & 0x3)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ALIGN);
        }
        break;

    case ACCMX_ANYSZ:
        if (!pIn->bNoAddrIncr &&
            ((pIn->nAddr + pIn->nMany - 1) & 0xFFFFFFFF00000000ULL)) {
            return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_BOUNDS);
        }
        if (pIn->bNoAddrIncr) {
            // Only 32-bit aligned accesses without address increment.
            if ((pIn->nAddr & 0x3) || (pIn->nMany & 0x03)) {
                return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ALIGN);
            }
        }
        break;

    default:
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_SIZE);
    }

    if (orb::agdi::Get().running && !orb::agdi::g_supp.MemAccR) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_RUN);
    }

    return ACCMX_DONE;
}

// DbgCM/AGDI.CPP:18284-18291.
static BYTE MemAccx_MemAcc_AssembleAccAttr(ACCMEMX *pIn)
{
    BYTE accAttr = 0;
    if (pIn->bNoAddrIncr) accAttr |= BLOCK_NADDRINC;
    accAttr |= static_cast<BYTE>((pIn->secureType & ACCMX_SECTYPE_CPU) << BLOCK_SECTYPE_P);
    return accAttr;
}

// DbgCM/AGDI.CPP:18294-18351.  The reference consults ReadCache/WriteCache
// around the access; this driver's Dap layer applies its own read cache inside
// MemRead/MemWrite, so those calls have no counterpart here.
static int MemAccx_MemAcc_DoRead(ACCMEMX *pIn, U32 /*internalDP*/)
{
    orb::agdi::Context& c = orb::agdi::Get();
    const DWORD nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);
    const DWORD nBytes = static_cast<DWORD>(pIn->nMany * pIn->accSize);

    // ACCMX_ANYSZ lets the driver pick the widths; MemRead/MemWrite already
    // choose them per address and length, so it and the sized cases converge.
    if (!c.dap.MemRead(nAddr, pIn->pB8, nBytes)) return EU14;
    return 0;
}

// DbgCM/AGDI.CPP:18354-18393.
static int MemAccx_MemAcc_DoWrite(ACCMEMX *pIn, U32 /*internalDP*/)
{
    orb::agdi::Context& c = orb::agdi::Get();
    const DWORD nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);
    const DWORD nBytes = static_cast<DWORD>(pIn->nMany * pIn->accSize);

    if (!c.dap.MemWrite(nAddr, pIn->pB8, nBytes)) return EU14;
    return 0;
}

// DbgCM/AGDI.CPP:18396-18469.
static U32 MemAccX_MemAcc(ACCMEMX *pIn)
{
    DWORD nAddr;
    int   status        = 0;
    U32   nE            = ACCMX_DONE;
    U32   _dpID         = 0;
    BOOL  MemAccX_APOrg = MemAccX_AP;
    DWORD oDP           = static_cast<DWORD>(orb::agdi::Get().dap.DapIndex());
    DWORD oAP_Sel       = AP_Sel;

    nE = MemAccx_MemAcc_Validate(pIn);
    if (nE != ACCMX_DONE) return nE;

    nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);

    if (pIn->bDebugPort) {
        nE = MemAccx_SelectDP(pIn, &_dpID);
        if (nE != ACCMX_DONE) return nE;
    }

    if (pIn->bAccessPort) {
        AP_Sel     = (pIn->accessPort << 24);
        MemAccX_AP = TRUE;
    }

    // Don't return until the port selections and MemAccX_AP are restored.
    switch (pIn->accCmd) {
    case ACCMX_READ:
    case ACCMX_WRITE_FLASH:                 // the flash path writes through the
                                            // same buffer; READ/WRITE below decide
    case ACCMX_WRITE:
        status = (pIn->accCmd == ACCMX_READ) ? MemAccx_MemAcc_DoRead(pIn, _dpID)
                                             : MemAccx_MemAcc_DoWrite(pIn, _dpID);
        break;
    default:
        nE = ACCMX_BADCMD;
        break;
    }

    if (pIn->bDebugPort) {
        orb::agdi::Get().dap.SetDapIndex(static_cast<int>(oDP));
    }

    AP_Sel = oAP_Sel;

    if (pIn->bSkipAccErr) status = static_cast<int>(SkipTargetAccErr(static_cast<DWORD>(status)));

    if (status != 0 && nE == ACCMX_DONE) {
        nE = MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_GENERAL);
    }

    MemAccX_AP = MemAccX_APOrg;
    return nE;
}

// DbgCM/AGDI.CPP:18472-18508.
static U32 MemAccx_APAcc_Validate(ACCMEMX *pIn)
{
    DWORD nAddr;

    if (pIn->nMany == 0) return ACCMX_BADPARM;

    if (pIn->nAddr & 0xFFFFFFFFFFFFFF00ULL) {       // 0x00 - 0xFC, 4-byte aligned
        return MemAccX_SetErr(pIn, pIn->nAddr, ACCMX_ERR_ACC_BOUNDS);
    }
    nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000000000FFULL);

    if (pIn->bAccessPort && (pIn->accessPort & 0xFFFFFF00)) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_INVALID_AP);
    }
    if (nAddr & 0x00000003) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ALIGN);
    }
    if (pIn->nMany > 1) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_NOBLOCK);
    }
    if (pIn->accSize != ACCMX_U32 && pIn->accSize != ACCMX_ANYSZ) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_SIZE);
    }
    return ACCMX_DONE;
}

// DbgCM/AGDI.CPP:18511-18581.
static U32 MemAccX_APAcc(ACCMEMX *pIn)
{
    int   status        = 0;
    U32   nE            = ACCMX_DONE;
    U32   _dpID         = 0;
    BOOL  MemAccX_APOrg = MemAccX_AP;
    DWORD oDP           = static_cast<DWORD>(orb::agdi::Get().dap.DapIndex());
    DWORD oAP_Sel       = AP_Sel;

    nE = MemAccx_APAcc_Validate(pIn);
    if (nE != ACCMX_DONE) return nE;

    if (pIn->bDebugPort) {
        nE = MemAccx_SelectDP(pIn, &_dpID);
        if (nE != ACCMX_DONE) return nE;
    }

    if (pIn->bAccessPort) {
        AP_Sel     = (pIn->accessPort << 24);
        MemAccX_AP = TRUE;
    }

    {
        orb::agdi::Context& c = orb::agdi::Get();
        // AP register access: the byte address picks the register and the bank
        // (0x00/0x04/0x08/0x0C in bank 0 or bank 0xF).
        const uint32_t adr  = static_cast<uint32_t>(pIn->nAddr & 0xFFu);
        const uint32_t bank = (adr >> 4) & 0xFu;
        const uint32_t reg  = 4u + ((adr >> 2) & 0x3u);   // DAP_AP_REG_CSW .. DRW

        switch (pIn->accCmd) {
        case ACCMX_READ: {
            uint32_t v = 0;
            if (!c.dap.ReadApReg(bank, reg, v)) status = EU08;
            else if (pIn->pB32) *pIn->pB32 = v;
            break;
        }
        case ACCMX_WRITE:
            if (!c.dap.WriteApReg(bank, reg, pIn->pB32 ? *pIn->pB32 : 0u)) status = EU08;
            break;
        default:
            nE = ACCMX_BADCMD;
            break;
        }
    }

    if (pIn->bDebugPort) {
        orb::agdi::Get().dap.SetDapIndex(static_cast<int>(oDP));
    }
    AP_Sel = oAP_Sel;

    if (pIn->bSkipAccErr) status = static_cast<int>(SkipTargetAccErr(static_cast<DWORD>(status)));

    if (status != 0 && nE == ACCMX_DONE) {
        nE = MemAccX_SetErr(pIn, static_cast<DWORD>(pIn->nAddr & 0xFFu), ACCMX_ERR_GENERAL);
    }

    MemAccX_AP = MemAccX_APOrg;
    return nE;
}

// DbgCM/AGDI.CPP:18584-18617.
static U32 MemAccx_DPAcc_Validate(ACCMEMX *pIn)
{
    DWORD nAddr;

    if (pIn->bAccessPort || pIn->nMany == 0) {
        // a debug port register access does not know about an access port
        return ACCMX_BADPARM;
    }

    if (pIn->nAddr & 0xFFFFFFFFFFFFFFF0ULL) {       // 0x0 - 0xC, 4-byte aligned
        return MemAccX_SetErr(pIn, pIn->nAddr, ACCMX_ERR_ACC_BOUNDS);
    }
    nAddr = static_cast<DWORD>(pIn->nAddr & 0x000000000000000FULL);

    if (nAddr & 0x00000003) return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ALIGN);
    if (pIn->nMany > 1)     return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_NOBLOCK);
    if (pIn->accSize != ACCMX_U32 && pIn->accSize != ACCMX_ANYSZ) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_ACC_SIZE);
    }
    return ACCMX_DONE;
}

// DbgCM/AGDI.CPP:18620-18683.
static U32 MemAccX_DPAcc(ACCMEMX *pIn)
{
    int   status = 0;
    U32   nE     = ACCMX_DONE;
    U32   _dpID  = 0;
    DWORD oDP    = static_cast<DWORD>(orb::agdi::Get().dap.DapIndex());

    nE = MemAccx_DPAcc_Validate(pIn);
    if (nE != ACCMX_DONE) return nE;

    if (pIn->bDebugPort) {
        nE = MemAccx_SelectDP(pIn, &_dpID);
        if (nE != ACCMX_DONE) return nE;
    }

    {
        orb::agdi::Context& c = orb::agdi::Get();
        const uint32_t reg = (static_cast<uint32_t>(pIn->nAddr) >> 2) & 0x3u;  // DP 0..3

        switch (pIn->accCmd) {
        case ACCMX_READ: {
            uint32_t v = 0;
            if (!c.dap.ReadDP(reg, v)) status = EU08;
            else if (pIn->pB32) *pIn->pB32 = v;
            break;
        }
        case ACCMX_WRITE:
            if (!c.dap.WriteDP(reg, pIn->pB32 ? *pIn->pB32 : 0u)) status = EU08;
            break;
        default:
            nE = ACCMX_BADCMD;
            break;
        }
    }

    if (pIn->bDebugPort) {
        orb::agdi::Get().dap.SetDapIndex(static_cast<int>(oDP));
    }

    if (pIn->bSkipAccErr) status = static_cast<int>(SkipTargetAccErr(static_cast<DWORD>(status)));

    if (status != 0 && nE == ACCMX_DONE) {
        nE = MemAccX_SetErr(pIn, static_cast<DWORD>(pIn->nAddr & 0xFu), ACCMX_ERR_GENERAL);
    }
    return nE;
}

// ---------------------------------------------------------------------------
// AG_MemAccX (DbgCM/AGDI.CPP:18689-18792)
// ---------------------------------------------------------------------------
U32 AG_MemAccX(ACCMEMX *pIn)
{
    DWORD       nE         = 0;
    int         lockStatus = 0;
    AP_CONTEXT *apCtx      = NULL;

    orb::agdi::Context& c = orb::agdi::Get();

    // Driver disconnected.
    if (!c.connected) {
        return ACCMX_CONNECTION;                    // :18705-18708
    }

    if (pIn == NULL) {
        return ACCMX_BADPARM;                       // :18715-18717
    }

    nE = ACCMX_DONE;

    // Lock target communication - "particularly AP_Sel in AP_CurrentCtx()".
    orb::agdi::ComLock comLock;                     // LinkCom(1) / CheckCom(1)
    if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
        return ACCMX_FAILED;                        // :18742-18744
    }
    lockStatus = 0;

    if (pIn->accCmd == ACCMX_QUERY_ACC) {
        if (pIn->pB8 == NULL) {
            nE = ACCMX_BADPARM;                     // :18735-18737
            goto end;
        }
    }

    if (pIn->accCmd == ACCMX_QUERY_ACC) {
        // "Query access attributes for memory range, use access buffer to fetch
        // the access attributes" (AGDI.H:1628).  The reference answers out of the
        // AP context (:18746-18755):
        //
        //     *(pIn->pB8) = apCtx->AccSizes;
        {
            const DWORD dp = 0;
            const DWORD ap = AP_SEL2IDX(AP_Sel);
            if (dp >= DP_NUM || ap >= AP_NUM) {
                nE = MemAccX_SetErr(pIn, pIn->nAddr, ACCMX_ERR_INVALID_AP);
            } else {
                apCtx = &AP_Context[dp][ap];
                *(pIn->pB8) = static_cast<UC8>(apCtx->AccSizes);
            }
        }
    } else {
        switch (pIn->accType) {
        case ACCMX_TYPE_MEM: nE = MemAccX_MemAcc(pIn); break;   // :18758
        case ACCMX_TYPE_AP:  nE = MemAccX_APAcc(pIn);  break;   // :18761
        case ACCMX_TYPE_DP:  nE = MemAccX_DPAcc(pIn);  break;   // :18764
        default:             nE = ACCMX_BADPARM;       break;   // :18767
        }
    }

    // Unlock target communication.
    (void)lockStatus;

end:
    if (nE == ACCMX_DONE && pIn->bErr == 0) {
        if (lockStatus) nE = ACCMX_FAILED;          // :18781-18785
    }

    return nE;
}

// ---------------------------------------------------------------------------
// AGDIMsgBox - declared (not exported) by AGDI.H; used for driver errors.
// ---------------------------------------------------------------------------
UINT AGDIMsgBox(HWND hWnd, const char* pText, const char* pCaption,
                UINT uType, UINT uiDefAnswer)
{
    // AGDI.H documents this helper twice over - the AG_MSGBOX structure at :1254
    // and a worked example at :2062 - and the callback takes the *structure*,
    // not a string.  The bare MessageBoxA is only the documented fallback for
    // "no callback registered".
    //
    // Calling MessageBoxA directly, as this used to, blocks whichever thread
    // called us; from uVision's worker threads (flash, wait-for-break) that box
    // sits behind a window that is not pumping, so it never appears at all -
    // the failure mode is "the driver said nothing", which is worse than a box.
    orb::agdi::Context& c = orb::agdi::Get();
    if (c.cb) {
        AG_MSGBOX mb;
        mb.hWnd        = hWnd ? hWnd : c.mainFrame;
        mb.pMsg        = pText;
        mb.pCaption    = pCaption;
        mb.uiType      = uType;
        mb.uiDefAnswer = uiDefAnswer;
        return static_cast<UINT>(c.cb(AG_CB_MSGBOX, &mb));
    }

    HWND owner = hWnd ? hWnd : c.mainFrame;
    if (owner && GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId()) {
        // Same reason again: report instead of raising a box nobody will see.
        ORB_LOG_INFO("AGDI: AGDIMsgBox with no callback and off the UI thread - "
                     "the box is skipped; text was \"%s\" / \"%s\"",
                     pText ? pText : "", pCaption ? pCaption : "");
        return uiDefAnswer;
    }
    return static_cast<UINT>(MessageBoxA(owner, pText, pCaption, uType));
}

