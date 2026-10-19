/**
 * @file ORBAGDI_Agdi.cpp
 * @brief AGDI 导出层：uVision 调用的全部入口。
 * 产物 ORB_AGDI.dll 由 uVision 从 ARM\BIN 加载，内部经 RDDI → DAP → Core 驱动目标。
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

// ---- 扩展访问路径（AG_MemAccX / AG_DapAcc）的端口/AP 状态 ----
// 导出在全局作用域定义，状态由命名空间内的 Connect() 填，故声明放在这里。
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

// 4 字节粒度地址 → 属性字。
// 必须是 DWORD：AG_GETMEMATT 把指针交给 uVision，它按 DWORD* 解引用。
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

/// 'adr' 属性字的稳定指针（进程生命期内有效，见 AttrClear()）。
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
        // 清零而不擦除节点：AG_GETMEMATT 已把指针交给 uVision，删节点会留下悬空指针。
        // 因此增长只受 uVision 实际设置过属性的地址数限制。
    for (std::map<uint32_t, uint32_t>::iterator it = a.m.begin(); it != a.m.end(); ++it) {
        it->second = 0;
    }
    LeaveCriticalSection(&a.cs);
}

// uVision 通过 AG_MemAtt(AG_MEMMAP) 映射的内存窗口。
// 只存区间：属性字在 AttrStore，区间回答 AG_CADRVALID 的“是否已映射”。
struct MemMapStore {
    enum { kMax = 64 };
    CRITICAL_SECTION cs;
    struct Range { uint32_t start, end, attr; };
    Range            r[kMax];
    int              n;
        // 因段表满而丢弃的次数（只在第一次丢时打）。
        // 超限后 CADRVALID 会误答“未映射”，断点随机失效且日志无痕。
    unsigned         dropped;
    MemMapStore() : n(0), dropped(0) { InitializeCriticalSection(&cs); }
};

MemMapStore& MemMap() {
    static MemMapStore s;
    return s;
}

void MapMem(uint32_t adr, uint32_t len, uint32_t attr) {
    if (len == 0) return;
    MemMapStore& m = MemMap();
    bool warn = false;
    EnterCriticalSection(&m.cs);
    for (int i = 0; i < m.n; ++i) {
        if (adr >= m.r[i].start && adr < m.r[i].end) {   // already covered
            LeaveCriticalSection(&m.cs);
            return;
        }
    }
    if (m.n < MemMapStore::kMax) {
        m.r[m.n].start = adr;
        m.r[m.n].end   = adr + len;
        m.r[m.n].attr  = attr;
        ++m.n;
    } else if (m.dropped++ == 0) {
        warn = true;                                     // 只在第一次丢时报
    }
    LeaveCriticalSection(&m.cs);
    // 日志放在临界区外：它要开文件，不该占着这把锁。
    if (warn) {
        ORB_LOG_WARN("memmap: 段表已满（%d 段），此后映射的段不再记录 —— "
                     "0x%08X+0x%X 起 AG_BpInfo(AG_CADRVALID) 会将其视为未映射，"
                     "该地址上的断点会静默失效",
                     static_cast<int>(MemMapStore::kMax), adr, len);
    }
}

void MapClear() {
    MemMapStore& m = MemMap();
    EnterCriticalSection(&m.cs);
    m.n = 0;
    m.dropped = 0;
    LeaveCriticalSection(&m.cs);
}

/// 'adr' 是否落在已映射窗口内。
/// 没有任何映射时一律回答“是”，否则不用 AG_MEMMAP 的老工程会拒绝所有断点。
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

// 每 10 秒一行“仍在等待”。
// AG_WAITFORBRK 会阻塞到目标停下，日志才能区分“在等”和“卡死”。
struct WaitHeartbeat {
    uint64_t    t0;
    uint64_t    last;
    const char* what;

    explicit WaitHeartbeat(const char* w)
        : t0(GetTickCount64()), last(t0), what(w) {}

    void Tick() {
        const uint64_t now = GetTickCount64();
        if (now - last < 10000ull) return;
        last = now;
        ORB_LOG_INFO("AGDI: %s still waiting (%u s) - the target has not stopped "
                     "and no stop request has arrived",
                     what, (unsigned)((now - t0) / 1000));
    }
};

// 日志放 %TEMP%：ARM\BIN 属于 uVision，可能只读，静默打不开日志更糟。
bool TempDir(wchar_t* out, size_t cch) {
    const DWORD n = GetTempPathW(static_cast<DWORD>(cch), out);
    return n > 0 && n < cch;
}

// 断点表归 uVision 所有（bpHead 指向它的头指针），只读遍历以判断
// 'adr' 上的断点是不是数据观察点、触发类型是什么。
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

// ---- Context ----
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

// 串行化所有访问探针的 AGDI 入口。
// DCRSR/DCRDR 是共享的一对寄存器，寄存器轮询与单步重叠会互相读到残值
// （PC 读回 DHCSR 的值）。TryEnter 拿不到锁就回 AG_NOACCESS，不冻住 IDE。
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
    const uint64_t t0 = GetTickCount64();
    for (;;) {
        if (TryEnterCriticalSection(&g_comLock)) { m_held = true; return; }
        if ((GetTickCount64() - t0) >= timeoutMs) return;
        Sleep(1);
    }
}

ComLockWait::~ComLockWait() {
    if (m_held) LeaveCriticalSection(&g_comLock);
}

bool ComLockWait::Held() const { return m_held; }

// uVision 入口的探针锁等待语义。
// 等 250 ms（≈ worker 时间片的 125 倍，远小于一次擦除）：worker 每 2 ms 抓一次锁，
// 纯非阻塞会让寄存器/内存窗口间歇性拿到 AG_NOACCESS；无限等待会在 Flash 期间挂死。
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

// ---- Trace plumbing ----

// 是否有 trace 源在供数；决定 Trace Data 窗口的“处理中”状态。
static bool TraceSourceActive() {
    return OrbTraceActive() || ItmActive() ||
           (EtbAvailable() && EtbGetStatus().configured);
}

// 窗口“Update”：把源里剩下的取干净（ETB 重读目标缓冲，orbtrace 再排空端点）。
static bool TraceWinRefresh() {
    Context& c = Get();

        // 流式源不在窗口线程上抽干：抽数已移到 worker，这里再抽会把环里积压的
        // 几十万条一次灌进窗口、覆写正在显示的那批。ETB 是停机后读回，仍在此做。
    if (EtbAvailable() && EtbGetStatus().configured) {
        ComLock lock;
        if (lock.Held()) EtbProcess(c.dap);
    }
    return true;
}

// 采集开始/结束的三处同步：选项掩码总开关、窗口“处理中”状态、状态栏一行。
// 合并成一个入口，避免三处各自判断得出不一致结果。
static void TraceSetRunning(bool bState, bool bWinUpdate) {
    const bool active = bState && TraceSourceActive();

        // start 与 stop 共用入口，VTR 内容在函数体内不变，放最前一次覆盖两个方向。
    RefreshTraceClocks();

        // 再发一次 TDI 接口表与窗口配置：窗口在 UV_TRACE_IF_INIT 之后才创建，
        // 注册时紧随其后的 WIN_CONFIG 可能因窗口不存在而被丢弃。
    if (active) orb::agdi::TraceWinReconfigure();

        // 不要提前清 running：TraceWinSetRunning() 有相等早退，会让停止通知被吞掉。

        // 发状态之前先声明“trace 数据展示已启用”（UV4 取数门 app[0xafc] & 8）。
    orb::agdi::TraceWinAssertTraceDisplay(true, active ? "采集开始" : "停止");

        // 停止时先同步视图再发状态：顺序反了 uVision 不知道要看哪一段，就不再取记录。
    if (!bState) orb::agdi::TraceWinSyncViews(TraceWinLastCycle());

        // TraceOpt = 总开关 + 时间戳 + PC 采样 + 6 个事件位 + ETM/Unlimited。
        // 窗口按这些位给记录分类，少一位就不显示那一类。
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

        // 时间列默认关：bHideTimestamps 同时是取数门的一环，打开可能一行都不取。
    orb::agdi::TraceWinSetTimestampsVisible(orb::cfg::Get().tsWin);

        // 上报 trace 配置（AG_CB_CFG_TRC）：ULP2CM3 在启动/停止时发。
        // 结构体：+0x00=1、+0x04/+0x08=两个配置值、+0x0C 位域（时间戳/异常/已启用）。
        // 与窗口不出数无关，只为与官方驱动保持一致。
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
                // 官方这两格取自己的配置全局量；本驱动没有两份独立时钟，统一用 c.tclk。
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
        // 上面只是排队；这里是调试线程，投递放在此处，状态行不必等到下次轮询。
    orb::agdi::TraceWinStatusFlush();

        // 指名显示区间：不指名时 uVision 只问范围就不动，指名后必须取记录才能画。
    if (!bState) {
        const uint64_t last = orb::agdi::TraceWinLastCycle();
        if (last) orb::agdi::TraceWinShowRange(orb::agdi::TraceWinFirstCycle(), last);
        // 停止流程走完后重新确认 bTrcDisp（只置位，不越界）。
        orb::agdi::TraceWinAssertTraceDisplay(true, "停止");
        // 只读地把取数门链的当前值打出来：是哪道门关着，一眼可见。
        orb::agdi::TraceWinReadHostGates("停止");
    }
}

// ---- Session control ----

// 官方 SevereTargetError() 以 SetPlayDead 收尾；本驱动的等价物是“会话不可访问”，
// 所以先上报，再把会话标记为死，之后所有入口一律 AG_NOTARGET。
static void V8mPlayDead(const char* msg) {
    Context& c = Get();
    ORB_LOG_ERROR("AGDI: fatal target error - %s", msg ? msg : "(unknown)");
    StatusMessage(msg ? msg : "Fatal target error - the session was stopped");
    c.connected = false;
}

/// CodeImagePreloadFlash() 用的读函数。
/// 必须在文件作用域：局部类不许有静态成员函数。Connect 阶段无锁竞争，不取探针锁。
static bool ReadMemForCodeImage(void* ctx, uint32_t addr, uint8_t* out, unsigned len) {
    orb::Dap* d = static_cast<orb::Dap*>(ctx);
    return d && d->IsConnected() && d->MemRead(addr, out, len);
}

/// 按配置打开两份落盘文件（trfile= / trrec=，默认都关）。
/// 有两个调用点（Connect 与 reset 清理），必须用同一套规则。
static void TraceFileOpenFromConfig()
{
    const orb::cfg::Config& tc = orb::cfg::Get();
        // 落盘都是 opt-in：trfile= 未给即关；trrec=1 = %TEMP% 下自动命名。
        // 写盘已在后台线程，不再卡抽数；trrecmb= 给上限，默认 2 GB。
    const unsigned capMB = tc.trRecMB > 0 ? static_cast<unsigned>(tc.trRecMB) : 0u;
    const unsigned keep  = tc.trKeep > 0 ? static_cast<unsigned>(tc.trKeep) : 0u;
    orb::agdi::TraceFileOpen(tc.trFile, keep, capMB);
    // 记录日志（P2-2）：内存窗口之外能回看的那一份（紧凑记录，查询不重解）。
    orb::agdi::TraceFileOpenRecords(tc.trRec, keep, capMB);
}

/// 从头开始：作废上一轮的 trace 数据（内存缓冲、周期轴基准、导航分片、落盘缓存）。
/// 顺序必须是先关删文件、再清内存、最后重开，否则重开的文件会被刚删掉。
/// 挂点只有 Connect 与 AG_RESET 两个事件，不对应菜单操作。
static void TraceResetForTarget(const char* why)
{
    const unsigned nFiles = orb::agdi::TraceFileCloseAndDelete(why);
    orb::agdi::TraceWinClear();
        // 清空/复位不是 run 状态变化（SetRunning 会早退），故在这里补一次时钟重发。
    orb::agdi::RefreshTraceClocks();
    TraceFileOpenFromConfig();
    ORB_LOG_INFO("AGDI: trace 已按 reset 语义清空（%s）—— 落盘缓存删掉 %u 个，"
                 "内存缓冲与周期轴已归零",
                 why ? why : "?", nFiles);
}

/// 复位后重新武装目标侧 trace（复位清掉了 TRCENA/TPIU/ITM/DWT/ETMCR 与 TraceStart）。
/// 只重放上次编进目标的配置：不重开采集会话（会丢已收字节），不清记录缓冲。
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

        // DP 已应答 ⇒ 可以跑描述里的调试序列（它们经 AG_MemAccX/AG_DapAcc 访问目标，
        // 未标 connected 会被拒）。所以 connected 指“DP 起来了、可访问”，早于内核 attach。
    c.connected = true;

        // 扩展访问路径按 AP 上下文校验：ACCMX_QUERY_ACC 从此取值，为零会让所有访问
        // 报 ACCMX_ERR_ACC_SIZE。AHB-AP/AXI-AP 都支持字节/半字/字访问。
        // DP_Type 按 SWD 填；只有 JTAG 专用命令依赖它。
    SetDapPortType(false);
    SetApAccSizes(c.dap.DapIndex(), c.dap.ApIndex(),
                  AP_ACCSZ_BYTE | AP_ACCSZ_HWORD | AP_ACCSZ_WORD);

        // 目标识别：一次 CPUID 读出内核名/架构/ETM 变种（表在 ORBAGDI_CpuId.cpp）。
        // v8-M 安全扩展探测也挂在这里；v7-M 判定为非 v8-M 即跳过。
    {
        const orb::agdi::CpuId id = orb::agdi::ReadCpuId(c.dap);
        if (id.valid) {
            ORB_LOG_INFO("AGDI: CPUID=0x%08X PARTNO=0x%03X -> %s (%s, %s)",
                         id.cpuid, id.partNo,
                         id.desc ? id.desc->name : "unknown",
                         orb::agdi::ArchName(id.desc ? id.desc->arch : orb::agdi::TraceArch::Unknown),
                         orb::agdi::EtmVariantName(id.desc ? id.desc->etm : orb::agdi::EtmVariant::None));
        }
        if (id.desc != nullptr && orb::agdi::IsV8M(id.desc->cpu)) {
            const unsigned cpu = id.desc->cpu;
            if (!orb::agdi::V8mDetectSecurityExtensions(c.dap, cpu)) {
                ORB_LOG_WARN("AGDI: v8-M security extension detection failed");
            } else {
                const orb::agdi::V8mStatus vs = orb::agdi::V8mGetStatus();
                ORB_LOG_INFO("AGDI: v8-M CPU %s (part 0x%03X) SecureExt=%d "
                             "CPU=%s SDbgEna=%d NSDbgEna=%d",
                             id.desc->name, id.partNo, vs.secureExt ? 1 : 0,
                             vs.cpuSecure ? "Secure" : "Non-Secure",
                             vs.sDbgEna ? 1 : 0, vs.nsDbgEna ? 1 : 0);
            }
        }
    }

    // SevereTargetError()'s fatal path is delegated to this hook.
    orb::agdi::V8mSetPlayDeadHook(&V8mPlayDead);

        // 器件描述里的调试序列。解锁调试口必须先跑这组：
        // Attach() 要读 CPUID 并停机，端口锁着时两样都会被拒。
    if (orb::pdsc::DebugSeqReady()) {
        ORB_LOG_INFO("AGDI: running the description's debug sequences (%u present)",
                     (unsigned)orb::pdsc::DebugSeqCount());
        if (!orb::pdsc::ConnectDebugPort())
            ORB_LOG_WARN("AGDI: DebugPortSetup sequence failed");
        if (!orb::pdsc::StartDebugPort())
            ORB_LOG_WARN("AGDI: DebugPortStart sequence failed");
                // 失败只上报不致命：多数描述的解锁本来就是空操作，
                // 端口真锁着时下面的 Attach() 会明确报错。
        if (!orb::pdsc::UnlockDevice())
            ORB_LOG_WARN("AGDI: DebugDeviceUnlock sequence failed - attaching anyway");
    }

        // “Connect: without Stop”：attach 后让目标继续跑，下游靠 c.running 判断。
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

        // 掉调试连接后还能活：描述里有 RecoverySupportStart 就跑，断开时 RecoveryStop。
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

        // 三个互斥的 trace 源，优先级 orbtrace > ETB > SWO：
        // orbtrace= 探针自己的 trace 端点，etb= 片上 ETB，swo= SWO 引脚。
        // 启动失败只关 trace，不影响调试。
    HistReset();
    EtbReset();
    EtmTraceReset();
        // 代码镜像的生命周期是一次会话，清在这里。不能放进 EtmTraceReset()：
        // 那个函数每次开流都会调，会清掉刚 Download 存下的镜像。
    CodeImageClear();

        // 连上后立刻预读器件声明的 Flash：此刻探针可用、worker 未起、锁不忙。
        // 这样代码镜像不再取决于 uVision 何时 Download。
    CodeImagePreloadFlash(&ReadMemForCodeImage, &c.dap);
        // 按内核选 ETM 解码引擎（M3/M4→ETMv3.5，M7/M33→ETMv4，M0+/M23→MTB）。
        // 放在 EtmTraceReset() 之后：读不到 CPUID 时保持默认。
    EtmTraceSelectProtocol(c.dap);
        // 记录缓冲规模必须在 TraceWinInit() 之前给：上限决定一次性分配的索引数组。
        // 默认 4 MB/块、上限物理内存 1/4 封顶 1 GB；trmax= 默认 0 = 不限条数。
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

        // SWO 位时钟：显式 swo= 优先，否则由 Trace Clock / Prescaler 除出来。
        // “SWO 引脚是不是选中源”只看 swo=：另两个源会把它写成 0，派生值不代表请求。
        // Trace 总开关关掉时，三个源都不启动。
    const bool orbSource = opt.traceEnable && opt.orbtrace;
    const bool etbSource = opt.traceEnable && opt.etb;
    const bool swoSource = opt.traceEnable && (opt.swoBaud > 0);

        // 同时给多个源时按 orbtrace > ETB > SWO 取值，另两个静默不生效。
        // 设置页造不出这种串，只来自手改的选项串 —— 说一句免得误判“选了没生效”。
    if (static_cast<int>(orbSource) + static_cast<int>(etbSource) +
        static_cast<int>(swoSource) > 1) {
        ORB_LOG_WARN("AGDI: 同时给了多个 trace 源（orbtrace=%d etb=%d swo=%d）—— "
                     "按 orbtrace > ETB > SWO 只启用 **%s**，其余两个不生效",
                     opt.orbtrace ? 1 : 0, opt.etb ? 1 : 0, opt.swoBaud,
                     orbSource ? "orbtrace" : (etbSource ? "ETB" : "SWO"));
    }

        // 派生值仍供 orbtrace 的 SWO 档在 trbaud= 缺省时使用；
        // 那条路走探针 trace 通道，与 RDDI 的 SWO 引擎不是一回事。
    int swoBaud = opt.swoBaud;
    if (swoBaud <= 0) swoBaud = cfg::SwoBaud(opt);

        // 位时钟快于核心时钟 ⇒ 两个值单位不同（本页 kHz，Keil 的框是 ns），差 1e6 倍。
        // 核心时钟取自 Trace 页、不从 tclk 派生，所以仍能抓住 tclk 的笔误。
        // 只在有源真正使用该速率时告警。
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

        // ---- 告诉 pack 目标要怎么配 trace 输出（__traceout）----
        // 真正的寄存器编程由 pack 的 EnableTraceSWO / EnableTraceTPIU 完成；
        // 这里只传参，否则 TraceStart 空转、引脚配置只能靠固件写死。
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

        // ---- 周期轴机制按实际选中的源分两套 ----
        // ITM/SWO 数据量小，轴追墙钟；ETM 数据量太大（实测 4.9 M 条/s），只按解码推进。
        // 判据：源的通道里是否只有 ITM（RDDI SWO、orbtrace 的 SWO 档）。
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

                // 位宽只在并行模式下有意义；SWO 模式带下去会先按 4-bit 配一遍再被覆盖，
                // 中间那步日志会误导排查。
        const int width = (opt.trSwoMode > 0) ? 0 : opt.trWidth;
                // 1 位并口在 formatter 打开的目标上是坏的（实测）：id 位让整条流错位，
                // 通道号散成 20 多个、大量帧当未知 tag 丢弃。
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

        // 描述自己的 trace 设置，只在有源在跑时跑；序列缺失不算失败。
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
                // 这一发复位会把刚编好的 trace 寄存器打回默认，不补一次则整场收不到 trace。
        RearmTargetTraceAfterReset("reset after connect");
    }

    // 抽数工作线程随会话起（官方 `DSMonitor_Thread` 也是 `DSM_StartMonitor` 里
    // 建、会话结束才收）。它自己会在没有源在跑时什么都不做。
    TracePumpStart();
    return true;
}

// ---- 运行期收掉 trace 源（设置页取消 Trace Enable）----
// 源只看运行状态不看配置，不显式收尾就继续每 2 ms 抢探针锁，调试入口只能报 busy。
// 顺序同 Disconnect：窗口归零 → 停源 → 关目标侧硬件；worker 不停，仍负责访问恢复。
bool StopTraceSources(const char* why) {
    Context& c = Get();
    if (!c.connected) return false;

    const bool streaming = OrbTraceActive() || ItmActive();
    const bool etbArmed  = EtbAvailable() && EtbGetStatus().configured;
    if (!streaming && !etbArmed) return false;

    ORB_LOG_INFO("Trace: 收掉运行中的 trace 源（%s）orbtrace=%d itm=%d etb=%d",
                 why ? why : "?", (int)OrbTraceActive(), (int)ItmActive(),
                 (int)etbArmed);

    // 停源要碰探针。拿不到锁就不硬来：断开那一路会无条件收尾，而且此时硬等只会
    // 让设置页按钮看起来卡住。
    ComLockWait lock(500);
    if (!lock.Held()) {
        ORB_LOG_WARN("Trace: 收源时拿不到探针锁（%s），留待 Disconnect 收尾",
                     why ? why : "?");
        return false;
    }

    // 状态通知放在**拿到锁之后**：拿不到锁时干脆别动窗口，否则会留下"窗口说已
    // 停止、源还在跑"的半截状态，而下次 Connect 之前没人能纠正它。
    TraceSetRunning(false, true);
    OrbTraceStop(c.dap);
    ItmStop(c.dap);
    // 目标侧那一半同样要关：只停主机侧的抽取，ETM 仍在全速灌，没人排空就溢出。
    ItmTraceOff(c.dap);
    if (EtbAvailable()) EtbReset();
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
        // 无条件关掉目标侧 trace 硬件。只在 ETB 可用时关，会让 orbtrace/SWO 结束后
        // ETM 继续全速灌 trace 而无人排空，缓冲溢出后数据错乱。
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

        // 必须在 StopDebugPort() 之前：Clear() 要写目标（卸 FPB/DWT 比较器），
        // 排在之后调试口已拆，只会在日志里留下两行失败的 DP 写噪声。
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

// ---- 把 PC 交回 uVision ----
// uVision 不从 AG_AllReg 取 PC，而是由 AG_INITCURPC 给出它自己的变量指针，
// 驱动必须持续写它（同时发布 Thumb 位），否则源码行箭头不跟着走。
void PublishPC(uint32_t pc) {
    Context& c = Get();
        // 每会话记一次：uVision 没发 AG_INITCURPC 时指针为空，下面全是静默空写。
    static bool logged = false;
    if (!logged) {
        logged = true;
        ORB_LOG_INFO("AGDI: AG_INITCURPC pointer %s - publishing PC 0x%08X",
                     c.curPC ? "present" : "MISSING", pc);
    }
    if (c.curPC) *c.curPC = pc;
}

// ---- SetupTmpBrk / UpdatePBStart：临时断点与遍历起点 ----
// “运行到地址”= 插进 uVision 自己列表的临时断点：TBp 挂在表头前、pBStart 指向它，
// 遍历时按地址(&TBp)跳过，所以不报成用户断点命中，只让核停在指定处。
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

// ---- AnalyzeBreaks：这组断点能不能服务 ----
// 每次 Go/Step 前跑，非零结果转成消息框并停住 HLL 单步；否则装不上的断点只是永不触发，
// 和"没跑到"无法区分。0 = OK、1 = 不支持条件断点、2 = 观察点太多、3 = 范围非法。
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
                        // 参考实现按 BP_INFO_WVAL 限制值匹配观察点；本仓库没有该字段，
                        // 且 DWT 比较器只按地址匹配，所以限值检查保留但永远够不着。
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

// ---- BrkWalk：遍历 uVision 自己的断点表 ----
// 表头指针由 AG_INITBPHEAD 给出，只有遍历它才知道命中的是哪个断点及其编号；
// 不遍历就只能报 nBpNum = 0xFFFF。
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

        // 从 pBStart 遍历并跳过临时断点，避免“运行到地址”被报成用户断点命中。
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
                                                // 带命令的断点不停目标：命令执行完继续跑。
                                                // 调用期间临时清 iRun，让命令自己也能访问内存。
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
                                // 观察点条件交 uVision 求值（AG_CB_TRUEXPR），为假就跳过。
                                // 不求值的话条件观察点每次地址匹配都会触发。
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

                                // 读写型观察点在读访问上触发时，官方也报成写访问。
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

        // 整段运行期间持有探针锁；否则插进来的寄存器轮询会在写 FPB 时读 DCRDR。
        // AG_STOPRUN 不走这里，所以 Stop 仍然可用。
    orb::agdi::ComLock comLock;
    if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
        ORB_LOG_WARN("GoStep: run refused, the probe is busy (%u ms 没拿到锁)",
                     orb::agdi::kProbeWaitMs);
        return AG_NOACCESS;
    }

        // 临时断点插进 uVision 自己的断点表，遍历时能看见并跳过；
        // 硬件比较器仍由本驱动的槽位表编程。
    const uint32_t temp = tempBpAddr & ~1u;
    const bool wantTemp = (temp != 0u && temp != 0xFFFFFFFEu);
    SetupTmpBrk(wantTemp ? temp : 0xFFFFFFFFu);

        // 这组断点能不能服务；非零时不能起跑，否则装不上的断点会静默失效。
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

        // 每次起跑打一行：把硬件比较器与本驱动槽位表并排打出来。
        // “没人要求却停了”通常是残留或位置不对的比较器，只有并排才能区分。
    {
        char   hw[192] = {0};
        size_t n = 0;
        for (uint32_t comp = 0; comp < 8u; ++comp) {
            uint32_t v = 0;
            if (!c.dap.MemRead32(0xE0002008u + 4u * comp, v)) break;
            if (v == 0u) continue;
            orb::fmt::Append(hw, sizeof(hw), n, " FP_COMP%u=0x%08X", comp, v);
        }
        uint32_t addrs[32];
        const int cnt = c.bps.Addresses(addrs, 32);
        char   tb[256] = {0};
        size_t m = 0;
        for (int i = 0; i < cnt; ++i) {
            orb::fmt::Append(tb, sizeof(tb), m, " 0x%08X", addrs[i]);
        }
        ORB_LOG_INFO("Bp: before run - hardware:%s | tracked:%s",
                     n ? hw : " (none)", m ? tb : " (none)");
    }

    c.stopReq = 0;
    InterlockedExchange(&c.running, 1);
    TraceSetRunning(true, false);
        // 起跑时清错误状态与同步超时，让本次采集从干净状态开始（官方 Trace_Flush）。
    TraceWinFlush(true);

        // 停在断点上时其比较器仍指着 PC，直接恢复会立刻再触发、永远出不去。
        // 先把比较器摘掉再单步越过；本次运行的临时断点要排除。
    if (c.core.IsHalted()) {
        const uint32_t here = ReadPC();
        if (here != 0u && here != temp && c.bps.Contains(here)) {
                        // 必须先把比较器摘掉：带着它单步会再次命中同一地址，实测原地打转。
            ORB_LOG_INFO("GoStep: breakpoint at 0x%08X - stepping past it with the "
                         "comparator temporarily disabled", here);
            c.bps.SetEnabled(here, false);
            c.core.Step();
            c.bps.SetEnabled(here, true);
        }
    }

        // PC 上是 BKPT 指令（本驱动没有记录的软件断点，如 __breakpoint()）时同理：
        // 读出两字节，为 0xBE00 就把 PC 挪过去，否则恢复即再次触发、零指令就停。
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

        // 同一陷阱，来源是不认识的比较器：停在 PC 上的比较器一恢复就触发，零指令结束。
        // 只扫 FPB 段（0xE0002008+）且只清“正好在 PC 上”的那个，其余与 DWT 不动。
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

        // 趁目标还停着武装 ETB：跑起来就写缓冲，停下再读回。
    const bool etb = EtbAvailable() && EtbGetStatus().configured;
    if (etb) {
        ItmArmForEtb(c.dap);
                // 上次停止可能在 FFCR 留下停止位，先清掉，否则 ETB/TMC 不再开始采集。
        EtbRecovery(c.dap);
        EtbActivate(c.dap);
    }

        // 抽数已移到 worker，从“让目标跑起来”起就不再需要独占探针；
        // 持锁全程会让 worker 饿死（官方同样只在等待循环里 Sleep）。
    const bool resumed = c.core.Resume();
    comLock.Release();

    U32 rc = AG_OK;
    if (!resumed) {
        rc = AG_RDFAILED;
    } else {
        const uint64_t start = GetTickCount64();
        WaitHeartbeat beat("AG_GOFORBRK");
        for (;;) {
            if (InterlockedCompareExchange(&c.stopReq, 0, 0) != 0) {
                                // 未确认停机 = 内核还在跑，下面读到的 PC 只是上次 AP 访问的残值。
                                // 停机要抢探针，所以等一小会儿，放弃等于留下一个还在跑的内核。
                ComLockWait lock(200);
                if (lock.Held() && !c.core.Halt() && rc == AG_OK) {
                    rc = AG_RDFAILED;
                                        // v8-M 上非安全调试器停不下安全态 CPU：官方降级成警告而非判死。
                                        // 返回值仍是 AG_RDFAILED，因为内核确实还在跑。
                    orb::agdi::V8mSevereTargetError(orb::agdi::kSevErrStop);
                }
                break;
            }

                        // 目标在跑时只采样 DHCSR：锁一小段一小段拿，与 worker 交替，谁也不饿死。
            bool halted = false;
            {
                ComLock lock;
                if (lock.Held()) halted = c.core.IsHalted();
            }
            if (halted) break;

            if (timeoutMs != 0 && (GetTickCount64() - start) >= timeoutMs) {
                ComLockWait lock(200);
                if (lock.Held() && !c.core.Halt() && rc == AG_OK) {
                    rc = AG_RDFAILED;
                    orb::agdi::V8mSevereTargetError(orb::agdi::kSevErrStop);
                }
                break;
            }

                        // 取数在 worker 上，这一圈不搬字节：单圈耗时与 trace 速率无关，
                        // Stop 的响应也不再由它决定（留在调试线程时一次抽满 2 MB 要 ~106 ms）。
                        // worker 只把状态栏消息排队，投递放在这里。
            orb::agdi::TraceWinStatusFlush();
            beat.Tick();
            Sleep(1);
        }
    }

    InterlockedExchange(&c.running, 0);
    if (tempIsNew) c.bps.Remove(temp);

        // 停机收尾要碰目标（DFSR/PC/ETB），重新独占探针；worker 时间片只有 2 ms。
    ComLockWait stopLock(500);
    if (!stopLock.Held()) {
        ORB_LOG_WARN("GoStep: 停机收尾没拿到探针锁，worker 未让出 —— 继续，"
                     "但这一轮的 DFSR/PC 可能读到脏值");
    }

        // 停止原因必须读 DFSR，不能靠“PC 是否在断点表里”猜：
        // 猜法会把断点、观察点、fault、lockup 和“被要求停”混成同一个 HIT_NOREASON。
    uint32_t dfsr = 0;
    {
        uint32_t dhcsr = 0;
        c.dap.MemRead32(kDcbDhcsr, dhcsr);
        c.dap.MemRead32(kDcbDfsr, dfsr);
        char   why[192] = {0};
        size_t w = 0;
        if (dfsr & 0x01u) orb::fmt::Append(why, sizeof(why), w, " HALTED");
        if (dfsr & 0x02u) orb::fmt::Append(why, sizeof(why), w, " BKPT");
        if (dfsr & 0x04u) orb::fmt::Append(why, sizeof(why), w, " DWTTRAP");
        if (dfsr & 0x08u) orb::fmt::Append(why, sizeof(why), w, " VCATCH");
        if (dfsr & 0x10u) orb::fmt::Append(why, sizeof(why), w, " EXTERNAL");
        ORB_LOG_INFO("GoStep: stop cause - DHCSR=0x%08X%s%s%s | DFSR=0x%08X (%s)",
                     dhcsr,
                     (dhcsr & (1u << 19)) ? " LOCKUP" : "",
                     (dhcsr & (1u << 18)) ? " SLEEP"  : "",
                     (dhcsr & (1u << 25)) ? " RESET_ST" : "",
                     dfsr, w ? why : " none");

                // 向量捕获 = 停在 fault 处理里，读 fault 状态寄存器才能知道是哪个 fault。
        if (dfsr & 0x08u) {
            uint32_t cfsr = 0, hfsr = 0, bfar = 0, mmfar = 0;
            c.dap.MemRead32(0xE000ED28u, cfsr);    // CFSR (MMFSR | BFSR | UFSR)
            c.dap.MemRead32(0xE000ED2Cu, hfsr);    // HFSR
            c.dap.MemRead32(0xE000ED38u, bfar);    // BFAR
            c.dap.MemRead32(0xE000ED34u, mmfar);   // MMFAR
            ORB_LOG_WARN("GoStep: fault - CFSR=0x%08X HFSR=0x%08X BFAR=0x%08X "
                         "MMFAR=0x%08X", cfsr, hfsr, bfar, mmfar);
        }

                // DFSR 是写 1 清零；不清会把上次的原因带到下一次。
        c.dap.MemWrite32(kDcbDfsr, dfsr);
    }

    // Read out and decode the ETB now that the target has stopped.
    if (etb) EtbProcess(c.dap);

        // 最后一次排空：停机前沿路的缓存，此时调试线程仍独占探针。
    OrbTracePump(c.dap, 5);
    ItmPump(c.dap, 5);
    TraceSetRunning(false, true);

        // PC 只在停机时有意义：DCRDR 保存上次传输的残值（实测读到过 DHCSR 的值），
        // 把残值当停止地址会表现成“停在别处”，所以宁可报运行失败。
    if (stopAddr) {
        if (c.core.IsHalted()) {
            *stopAddr = ReadPC();
                        // 告诉 uVision 停在哪、为什么停：不发布的话寄存器窗口一片空白，
                        // 尽管 AG_AllReg 的值都是对的。
                        // 走 uVision 自己的断点表分类：命中时带它的编号，未命中报 HIT_EXEC。
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
// Register View  -  AG_CB_INITREGV：窗口由 pCbFunc(AG_CB_INITREGV, &dsc) 装下的描述表生成，
// 不装就只有表头。条目 id 用 uVision 的 AR_n*（0x00-0x0F R0-R15、0x100 xPSR、0x11/0x12 MSP/PSP）。
// ===========================================================================
namespace {

RGROUP kRegGroups[] = {
    { 0x00, 0x03, "Core"   },   // ShEx bit0 = expanded, bit1 = bold
    { 0x00, 0x00, "Banked" },
    { 0x00, 0x00, "System" },
        // 只在目标带 FPU 时才随表装上，见 InstallRegView 的 MVFR0 判断。
    { 0x00, 0x00, "FPU"    },
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
        // xPSR 子项：nItem 连续编号表示嵌套，结束嵌套的分隔项必须排在整段之上。
    { 0x01, 0, 0x101, "N",          0, 1, 0 },
    { 0x01, 0, 0x102, "Z",          0, 1, 0 },
    { 0x01, 0, 0x103, "C",          0, 1, 0 },
    { 0x01, 0, 0x104, "V",          0, 1, 0 },
    { 0x01, 0, 0x105, "Q",          0, 1, 0 },
    { 0x01, 0, 0x106, "GE",         0, 0, 0 },   // canChg 0, as the reference has it
    { 0x01, 0, 0x107, "T",          0, 1, 0 },
    { 0x01, 0, 0x108, "IT",         0, 0, 0 },
    { 0x01, 0, 0x109, "ISR",        0, 1, 0 },
        // 分隔项 nItem 用 0x10F 而不是 0。
        // 0 与 R0 撞号，会画出一行无名却带 R0 数值的行，且没能关掉 xPSR 的子项。
    { 0x01, 0, 0x10F, "",           0, 0, 0 },
    { 0x01, 1, 0x11, "MSP",         0, 1, 0 },
    { 0x01, 1, 0x12, "PSP",         0, 1, 0 },
    { 0x01, 2, 0x20, "BASEPRI",     0, 1, 0 },
    { 0x01, 2, 0x21, "PRIMASK",     0, 1, 0 },
    { 0x01, 2, 0x22, "FAULTMASK",   0, 1, 0 },
    { 0x01, 2, 0x23, "CONTROL",     0, 1, 0 },

        // ---- FPU（组 3，随目标有无 FPU 决定装不装）----
        // 条目 id 沿用官方 CM4F 表：0x40-0x5F S0-S31、0x60-0x6F D0-D15、
        // 0x70-0x8F S 的浮点视图、0x90-0x9F D 的双精度视图、0xA00 FPSCR 及其状态位。
        // 0x400/0x600/0x700 是分节标题行，uVision 只在它下面嵌套，不会回调取值。
    { 0x01, 3, 0x400, "S<n>",       0, 0, 0 },
    { 0x01, 3, 0x40,  "S0",         0, 1, 0 },
    { 0x01, 3, 0x41,  "S1",         0, 1, 0 },
    { 0x01, 3, 0x42,  "S2",         0, 1, 0 },
    { 0x01, 3, 0x43,  "S3",         0, 1, 0 },
    { 0x01, 3, 0x44,  "S4",         0, 1, 0 },
    { 0x01, 3, 0x45,  "S5",         0, 1, 0 },
    { 0x01, 3, 0x46,  "S6",         0, 1, 0 },
    { 0x01, 3, 0x47,  "S7",         0, 1, 0 },
    { 0x01, 3, 0x48,  "S8",         0, 1, 0 },
    { 0x01, 3, 0x49,  "S9",         0, 1, 0 },
    { 0x01, 3, 0x4A,  "S10",        0, 1, 0 },
    { 0x01, 3, 0x4B,  "S11",        0, 1, 0 },
    { 0x01, 3, 0x4C,  "S12",        0, 1, 0 },
    { 0x01, 3, 0x4D,  "S13",        0, 1, 0 },
    { 0x01, 3, 0x4E,  "S14",        0, 1, 0 },
    { 0x01, 3, 0x4F,  "S15",        0, 1, 0 },
    { 0x01, 3, 0x50,  "S16",        0, 1, 0 },
    { 0x01, 3, 0x51,  "S17",        0, 1, 0 },
    { 0x01, 3, 0x52,  "S18",        0, 1, 0 },
    { 0x01, 3, 0x53,  "S19",        0, 1, 0 },
    { 0x01, 3, 0x54,  "S20",        0, 1, 0 },
    { 0x01, 3, 0x55,  "S21",        0, 1, 0 },
    { 0x01, 3, 0x56,  "S22",        0, 1, 0 },
    { 0x01, 3, 0x57,  "S23",        0, 1, 0 },
    { 0x01, 3, 0x58,  "S24",        0, 1, 0 },
    { 0x01, 3, 0x59,  "S25",        0, 1, 0 },
    { 0x01, 3, 0x5A,  "S26",        0, 1, 0 },
    { 0x01, 3, 0x5B,  "S27",        0, 1, 0 },
    { 0x01, 3, 0x5C,  "S28",        0, 1, 0 },
    { 0x01, 3, 0x5D,  "S29",        0, 1, 0 },
    { 0x01, 3, 0x5E,  "S30",        0, 1, 0 },
    { 0x01, 3, 0x5F,  "S31",        0, 1, 0 },
    { 0x01, 3, 0x600, "D<n>",       0, 0, 0 },
    { 0x01, 3, 0x60,  "D0",         0, 1, 0 },
    { 0x01, 3, 0x61,  "D1",         0, 1, 0 },
    { 0x01, 3, 0x62,  "D2",         0, 1, 0 },
    { 0x01, 3, 0x63,  "D3",         0, 1, 0 },
    { 0x01, 3, 0x64,  "D4",         0, 1, 0 },
    { 0x01, 3, 0x65,  "D5",         0, 1, 0 },
    { 0x01, 3, 0x66,  "D6",         0, 1, 0 },
    { 0x01, 3, 0x67,  "D7",         0, 1, 0 },
    { 0x01, 3, 0x68,  "D8",         0, 1, 0 },
    { 0x01, 3, 0x69,  "D9",         0, 1, 0 },
    { 0x01, 3, 0x6A,  "D10",        0, 1, 0 },
    { 0x01, 3, 0x6B,  "D11",        0, 1, 0 },
    { 0x01, 3, 0x6C,  "D12",        0, 1, 0 },
    { 0x01, 3, 0x6D,  "D13",        0, 1, 0 },
    { 0x01, 3, 0x6E,  "D14",        0, 1, 0 },
    { 0x01, 3, 0x6F,  "D15",        0, 1, 0 },
    { 0x01, 3, 0x700, "Float",      0, 0, 0 },
    { 0x01, 3, 0x70,  "S0",         0, 1, 0 },
    { 0x01, 3, 0x71,  "S1",         0, 1, 0 },
    { 0x01, 3, 0x72,  "S2",         0, 1, 0 },
    { 0x01, 3, 0x73,  "S3",         0, 1, 0 },
    { 0x01, 3, 0x74,  "S4",         0, 1, 0 },
    { 0x01, 3, 0x75,  "S5",         0, 1, 0 },
    { 0x01, 3, 0x76,  "S6",         0, 1, 0 },
    { 0x01, 3, 0x77,  "S7",         0, 1, 0 },
    { 0x01, 3, 0x78,  "S8",         0, 1, 0 },
    { 0x01, 3, 0x79,  "S9",         0, 1, 0 },
    { 0x01, 3, 0x7A,  "S10",        0, 1, 0 },
    { 0x01, 3, 0x7B,  "S11",        0, 1, 0 },
    { 0x01, 3, 0x7C,  "S12",        0, 1, 0 },
    { 0x01, 3, 0x7D,  "S13",        0, 1, 0 },
    { 0x01, 3, 0x7E,  "S14",        0, 1, 0 },
    { 0x01, 3, 0x7F,  "S15",        0, 1, 0 },
    { 0x01, 3, 0x80,  "S16",        0, 1, 0 },
    { 0x01, 3, 0x81,  "S17",        0, 1, 0 },
    { 0x01, 3, 0x82,  "S18",        0, 1, 0 },
    { 0x01, 3, 0x83,  "S19",        0, 1, 0 },
    { 0x01, 3, 0x84,  "S20",        0, 1, 0 },
    { 0x01, 3, 0x85,  "S21",        0, 1, 0 },
    { 0x01, 3, 0x86,  "S22",        0, 1, 0 },
    { 0x01, 3, 0x87,  "S23",        0, 1, 0 },
    { 0x01, 3, 0x88,  "S24",        0, 1, 0 },
    { 0x01, 3, 0x89,  "S25",        0, 1, 0 },
    { 0x01, 3, 0x8A,  "S26",        0, 1, 0 },
    { 0x01, 3, 0x8B,  "S27",        0, 1, 0 },
    { 0x01, 3, 0x8C,  "S28",        0, 1, 0 },
    { 0x01, 3, 0x8D,  "S29",        0, 1, 0 },
    { 0x01, 3, 0x8E,  "S30",        0, 1, 0 },
    { 0x01, 3, 0x8F,  "S31",        0, 1, 0 },
    { 0x01, 3, 0x90,  "D0",         0, 1, 0 },
    { 0x01, 3, 0x91,  "D1",         0, 1, 0 },
    { 0x01, 3, 0x92,  "D2",         0, 1, 0 },
    { 0x01, 3, 0x93,  "D3",         0, 1, 0 },
    { 0x01, 3, 0x94,  "D4",         0, 1, 0 },
    { 0x01, 3, 0x95,  "D5",         0, 1, 0 },
    { 0x01, 3, 0x96,  "D6",         0, 1, 0 },
    { 0x01, 3, 0x97,  "D7",         0, 1, 0 },
    { 0x01, 3, 0x98,  "D8",         0, 1, 0 },
    { 0x01, 3, 0x99,  "D9",         0, 1, 0 },
    { 0x01, 3, 0x9A,  "D10",        0, 1, 0 },
    { 0x01, 3, 0x9B,  "D11",        0, 1, 0 },
    { 0x01, 3, 0x9C,  "D12",        0, 1, 0 },
    { 0x01, 3, 0x9D,  "D13",        0, 1, 0 },
    { 0x01, 3, 0x9E,  "D14",        0, 1, 0 },
    { 0x01, 3, 0x9F,  "D15",        0, 1, 0 },
    { 0x01, 3, 0xA00, "FPSCR",      0, 1, 0 },
    { 0x01, 3, 0xA01, "N",          0, 1, 0 },
    { 0x01, 3, 0xA02, "Z",          0, 1, 0 },
    { 0x01, 3, 0xA03, "C",          0, 1, 0 },
    { 0x01, 3, 0xA04, "V",          0, 1, 0 },
    { 0x01, 3, 0xA05, "AHP",        0, 1, 0 },
    { 0x01, 3, 0xA06, "DN",         0, 1, 0 },
    { 0x01, 3, 0xA07, "FZ",         0, 1, 0 },
    { 0x01, 3, 0xA08, "RMode",      0, 1, 0 },
    { 0x01, 3, 0xA09, "IDC",        0, 1, 0 },
    { 0x01, 3, 0xA0A, "IXC",        0, 1, 0 },
    { 0x01, 3, 0xA0B, "UFC",        0, 1, 0 },
    { 0x01, 3, 0xA0C, "OFC",        0, 1, 0 },
    { 0x01, 3, 0xA0D, "DZC",        0, 1, 0 },
    { 0x01, 3, 0xA0E, "IOC",        0, 1, 0 },
        // 分隔项：结束 FPSCR 的嵌套，排在整段之后。
    { 0x01, 3, 0xA0F, "",           0, 0, 0 },
};

// uVision 最近一次要的寄存器块。UPR_NORMAL 刷新整块，之后逐项取值都从这里拿。
RgARMCM g_regs;
bool    g_haveRegs  = false;
bool    g_installed = false;
uint64_t g_regsAt   = 0;        // when g_regs was last read from the target

// FPU 单独一份缓存：33 次 DCRSR 传输比核心寄存器贵，只有真被问到才读。
RgARMFPU g_fpu;
bool     g_haveFpu     = false; // 目标有 FPU（MVFR0 判定），决定窗口装不装 FPU 组
bool     g_haveFpuRegs = false;
uint64_t g_fpuAt       = 0;

// (Re)read the whole register file from the target.
bool RefreshRegs() {
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return false;
    g_haveRegs = c.core.ReadRegs(g_regs);
    g_regsAt   = GetTickCount64();
    return g_haveRegs;
}

// FPU 缓存缺失或超龄就重读，口径与核心寄存器一致（100 ms）。
bool FreshFpuRegs() {
    if (g_haveFpuRegs && (GetTickCount64() - g_fpuAt) <= 100u) return true;
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return false;
    g_haveFpuRegs = c.core.ReadFpuRegs(g_fpu);
    g_fpuAt       = GetTickCount64();
    return g_haveFpuRegs;
}

// 有没有 FPU：MVFR0（0xE000EF40）低 4 位是单精度支持，0 表示没有。
// 判错最多是多一组全 0 行或少一组，所以读失败按“没有”处理。
bool DetectFpu(orb::agdi::Context& c) {
    uint32_t mvfr0 = 0;
    if (!c.dap.MemRead32(0xE000EF40u, mvfr0)) {
        ORB_LOG_WARN("RegView: MVFR0 is unreadable - the FPU group stays off");
        return false;
    }
    const bool has = (mvfr0 & 0xFu) != 0u;
    ORB_LOG_INFO("RegView: MVFR0=0x%08X -> %s FPU", mvfr0, has ? "has" : "no");
    return has;
}

// FPSCR 状态位的掩码/位移，顺序与 kRegItems 里 0xA01-0xA0E 一致（官方 bFPSCR[]）。
struct FpscrBit { U32 mask; U32 shft; };
const FpscrBit kFpscrBits[] = {
    { 0x80000000u, 31 },   // N
    { 0x40000000u, 30 },   // Z
    { 0x20000000u, 29 },   // C
    { 0x10000000u, 28 },   // V
    { 0x04000000u, 26 },   // AHP
    { 0x02000000u, 25 },   // DN
    { 0x01000000u, 24 },   // FZ
    { 0x00C00000u, 22 },   // RMode
    { 0x00000080u,  7 },   // IDC
    { 0x00000010u,  4 },   // IXC
    { 0x00000008u,  3 },   // UFC
    { 0x00000004u,  2 },   // OFC
    { 0x00000002u,  1 },   // DZC
    { 0x00000001u,  0 },   // IOC
};

bool ReadRegItem(U16 nItem, U32& out) {
        // FPU 项有自己的缓存，读它们不该顺带把整份核心寄存器再读一遍。
    if (nItem >= 0x40 && nItem <= 0x5F) {          // S0..S31
        if (!FreshFpuRegs()) return false;
        out = g_fpu.Sn[nItem - 0x40];
        return true;
    }
    if (nItem == 0xA00) {                          // FPSCR
        if (!FreshFpuRegs()) return false;
        out = g_fpu.FPSCR;
        return true;
    }
    if (nItem >= 0xA01 && nItem <= 0xA0E) {        // FPSCR 状态位
        if (!FreshFpuRegs()) return false;
        const FpscrBit& b = kFpscrBits[nItem - 0xA01];
        out = (g_fpu.FPSCR & b.mask) >> b.shft;
        return true;
    }

        // 缓存缺失或超龄就刷新。
        // 只靠 UPR_NORMAL 会整个会话不再更新（实测一直显示复位值），
        // 100 ms 的老化是它不来的兜底，代价是每 100 ms 一次寄存器读。
    if (!g_haveRegs || (GetTickCount64() - g_regsAt) > 100u) {
        if (!RefreshRegs()) return false;
    }
    if (nItem <= 0x0Fu) { out = g_regs.Rn[nItem]; return true; }
    switch (nItem) {
    case 0x100: out = g_regs.xPSR;              return true;
        // xPSR 子项，位位置与官方 bPSR[] 一致。
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

// 值变了置 iHigh（uVision 画蓝），没变则清掉。
// 无条件写 0 会让“自动高亮变化的项”永远失效。
void Settle32(RITEM* vp, U32 v) {
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

// FPU 的 D 视图是 64 位，比较与回写都要按 u64 做。
void Settle64(RITEM* vp, U64 v) {
    if (v == vp->v.u64) {
        if (vp->iHigh) {
            vp->iHigh = 0;
            vp->iDraw = 1;
        }
    } else {
        vp->iHigh = 1;
        vp->iDraw = 1;
    }
    vp->v.u64 = v;
}

// uVision's get callback (DbgCM/AGDI.CPP:15877 RegGet).
void RegGet(RITEM* vp, int nR) {
    if (!vp) return;
    const U32 mode = static_cast<U32>(nR) & 0xF0000000u;

        // 留个可见点：确认 uVision 真的会回调它、返回值也真的落地。
        // 装表只建行，值从这里来，此前没有证据证明这里被调用过。
    {
        static volatile LONG s_normal = 0;
        static volatile LONG s_items  = 0;
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
        }
    }

    if (mode == UPR_NORMAL) {
        // One refresh for the whole set; the per-item calls below only format.
        RefreshRegs();
        return;
    }
    if (mode == UPR_HIST) return;      // no execution history in this driver

    vp->iDraw = 0;

        // FPU 的 D 视图和浮点视图不是 32 位十六进制，先分出去；
        // S 的十六进制视图、FPSCR 及其状态位仍走下面的 32 位统一路径。
    if (vp->nItem >= 0x60 && vp->nItem <= 0x6F) {          // D0..D15（十六进制）
        if (!FreshFpuRegs()) return;
        const U32 n = (vp->nItem - 0x60) * 2u;
        const U64 v = static_cast<U64>(g_fpu.Sn[n]) |
                      (static_cast<U64>(g_fpu.Sn[n + 1]) << 32);
        if (v != vp->v.u64 || vp->szVal[0] == 0) {
            _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "0x%016llX",
                        static_cast<unsigned long long>(v));
        }
        Settle64(vp, v);
        return;
    }
    if (vp->nItem >= 0x70 && vp->nItem <= 0x8F) {          // S0..S31（浮点视图）
        if (!FreshFpuRegs()) return;
        const U32 bits = g_fpu.Sn[vp->nItem - 0x70];
        float f = 0.0f;
        memcpy(&f, &bits, sizeof(f));
        if (bits != vp->v.u32 || vp->szVal[0] == 0) {
            _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "%g",
                        static_cast<double>(f));
        }
        Settle32(vp, bits);
        return;
    }
    if (vp->nItem >= 0x90 && vp->nItem <= 0x9F) {          // D0..D15（双精度视图）
        if (!FreshFpuRegs()) return;
        const U32 n = (vp->nItem - 0x90) * 2u;
        const U64 bits = static_cast<U64>(g_fpu.Sn[n]) |
                         (static_cast<U64>(g_fpu.Sn[n + 1]) << 32);
        double d = 0.0;
        memcpy(&d, &bits, sizeof(d));
        if (bits != vp->v.u64 || vp->szVal[0] == 0) {
            _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "%g", d);
        }
        Settle64(vp, bits);
        return;
    }

    U32 v = 0;
    if (!ReadRegItem(vp->nItem, v)) return;

    if (vp->nItem >= 0xA01 && vp->nItem <= 0xA0E) {        // FPSCR 状态位
        if (v != vp->v.u32 || vp->szVal[0] == 0) {
            if (vp->nItem == 0xA08) {                      // RMode 显示助记符
                static const char* const kRMode[] = { "0: RN", "1: RP", "2: RM", "3: RZ" };
                _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "%s",
                            v < 4u ? kRMode[v] : "?");
            } else {
                _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "%u", v);
            }
        }
    } else if (v != vp->v.u32 || vp->szVal[0] == 0) {
        _snprintf_s(vp->szVal, sizeof(vp->szVal), _TRUNCATE, "0x%08X", v);
    }

    Settle32(vp, v);
}

// 写 FPU 条目；不属 FPU 段返回 false，交给后面的核心寄存器 switch。
// D 视图按两个单精度寄存器落盘，与官方一致（不给 DCRSR 造 64 位选择子）。
bool WriteFpuItem(orb::agdi::Context& c, const RITEM* vp, const GVAL* pV, bool& ok) {
    ok = false;
    const U16 id = vp->nItem;

    if (id >= 0x40 && id <= 0x5F) {                    // S0..S31（十六进制输入）
        ok = c.core.WriteRegSel(kSelFpuS0 + (id - 0x40), pV->u32);
        if (ok) g_fpu.Sn[id - 0x40] = pV->u32;
        return true;
    }
    if (id >= 0x60 && id <= 0x6F) {                    // D0..D15（十六进制输入）
        const U32 n  = (id - 0x60) * 2u;
        const U32 lo = static_cast<U32>(pV->u64);
        const U32 hi = static_cast<U32>(pV->u64 >> 32);
        ok = c.core.WriteRegSel(kSelFpuS0 + n, lo) &&
             c.core.WriteRegSel(kSelFpuS0 + n + 1, hi);
        if (ok) { g_fpu.Sn[n] = lo; g_fpu.Sn[n + 1] = hi; }
        return true;
    }
    if (id >= 0x70 && id <= 0x8F) {                    // S0..S31（浮点输入）
            // uVision 对浮点项主要填 f64；用户敲 “0” 时它可能只给 i32。
        const float f = (pV->f64 == 0.0) ? static_cast<float>(pV->i32)
                                         : static_cast<float>(pV->f64);
        U32 bits = 0;
        memcpy(&bits, &f, sizeof(bits));
        ok = c.core.WriteRegSel(kSelFpuS0 + (id - 0x70), bits);
        if (ok) g_fpu.Sn[id - 0x70] = bits;
        return true;
    }
    if (id >= 0x90 && id <= 0x9F) {                    // D0..D15（双精度输入）
        const double d = pV->f64;
        U64 bits = 0;
        memcpy(&bits, &d, sizeof(bits));
        const U32 n  = (id - 0x90) * 2u;
        const U32 lo = static_cast<U32>(bits);
        const U32 hi = static_cast<U32>(bits >> 32);
        ok = c.core.WriteRegSel(kSelFpuS0 + n, lo) &&
             c.core.WriteRegSel(kSelFpuS0 + n + 1, hi);
        if (ok) { g_fpu.Sn[n] = lo; g_fpu.Sn[n + 1] = hi; }
        return true;
    }
    if (id == 0xA00) {                                 // FPSCR
        ok = c.core.WriteRegSel(kSelFpuFpscr, pV->u32);
        if (ok) g_fpu.FPSCR = pV->u32;
        return true;
    }
    if (id >= 0xA01 && id <= 0xA0E) {                  // FPSCR 状态位：读-改-写
        U32 fpscr = 0;
        if (!c.core.ReadRegSel(kSelFpuFpscr, fpscr)) return true;  // 读不到就不写，别造半截值
        const FpscrBit& b = kFpscrBits[id - 0xA01];
        fpscr = (fpscr & ~b.mask) | ((pV->u32 << b.shft) & b.mask);
        ok = c.core.WriteRegSel(kSelFpuFpscr, fpscr);
        if (ok) g_fpu.FPSCR = fpscr;
        return true;
    }
    return false;
}

// uVision's set callback (DbgCM/AGDI.CPP:16408 RegSet).
I32 RegSet(RITEM* vp, GVAL* pV) {
    if (!vp || !pV) return 0;
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return 0;

        // 与 AG_AllReg 同一条守卫：运行中不写寄存器。
        // DCRSR 只接受停机态传输，运行中写入要么丢失要么落在随机时刻。
    if (InterlockedCompareExchange(&c.running, 0, 0) != 0) {
        ORB_LOG_WARN("RegView: RegSet(item 0x%04X) refused while the target runs",
                     vp->nItem);
        return 0;
    }

    U32  sel = 0;
    bool ok  = false;
        // FPU 段是一段 id 区间，switch 里写不出范围，先分出去。
    if (WriteFpuItem(c, vp, pV, ok)) return ok ? 1 : 0;
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

// 把表交给 uVision；幂等，重装只会让它重建一个本来就对的窗口。
void InstallRegView() {
    orb::agdi::Context& c = orb::agdi::Get();
    if (g_installed) return;
    if (!c.cb) {
        ORB_LOG_WARN("RegView: AG_INITCALLBACK not seen - the Registers-window "
                     "layout cannot be installed");
        return;
    }

        // 只有带 FPU 的目标才装 FPU 组，否则 M0/M3 上会多出一整组恒为 0 的行。
    g_haveFpu = DetectFpu(c);

    const I32 nAll = static_cast<I32>(sizeof(kRegItems) / sizeof(kRegItems[0]));
        // FPU 行全在表尾且 nGi == 3，扫出来的就是核心段的条目数，改表不会错位。
    I32 nCore = 0;
    while (nCore < nAll && kRegItems[nCore].nGi != 3) ++nCore;

    REGDSC dsc;
    dsc.nGitems = g_haveFpu ? 4 : 3;
    dsc.GrpArr  = kRegGroups;
    dsc.nRitems = nCore + (g_haveFpu ? nAll - nCore : 0);
    dsc.RegArr  = kRegItems;
    dsc.RegGet  = RegGet;
    dsc.RegSet  = RegSet;

    c.cb(AG_CB_INITREGV, &dsc);
    g_installed = true;
    ORB_LOG_INFO("RegView: installed %d group(s) / %d register item(s) via "
                 "AG_CB_INITREGV, FPU %s", (int)dsc.nGitems, (int)dsc.nRitems,
                 g_haveFpu ? "on" : "off");
}

// ===========================================================================
// Go/Step state：uVision 用 AG_EXECITEM 通告每条运行控制命令，据此置 MultipleGoStep 与
// BkpInstructionSkip（起跑时跳过 BKPT）。原来只是静默接受，BKPT 跳过从来没执行过。
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

// ---- AGDI entry points ----
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

/// 会话开头的横幅：uVision 给的工程信息 + 实际生效的设置。
/// 日志文件留全部细节，这里只回答“芯片/探针/时钟对不对”。
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

        // trace 时钟对（kHz）与派生位时钟：单位与 Keil 的 ns 差 1e6 倍，
        // 打出来才能看见“安静的 ITM 控制台”是算错了还是没数据。
    if (eff.traceClockKhz > 0) {
        orb::ui::Info("  Trace clk : %d kHz core, prescaler %d -> %d Hz bit clock",
                      eff.traceClockKhz,
                      (eff.swoPrescaler > 0) ? eff.swoPrescaler : 1,
                      orb::cfg::SwoBaud(eff));
    }

        // 复位模式直接生效，曾出现枚举与下拉不一致（选 HW 却写 VECTRESET），故打在横幅上。
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

// ---- EnumUvARM7 / DllUv3Cap ----
// AGDI.H 未声明，uVision 按名字解析，用来把 DLL 认成 ARM 目标驱动。
// 下面 qdll 的布局照抄 BOM.H，字段顺序与类型必须完全一致。
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

// uVision 每进程给一次的 debug 块；驱动返回后它仍会被读，所以不能存在栈上。
const void* g_dbgBlk = nullptr;

// debug 块从两个入口来：EnumUvARM7(2) 与 DllUv3Cap(109)，两个都要答。
// 只实现前者会让选项串永远为空（flm=/ram=/clock= 全失效）。
// TargArgs 就是工程里保存、设置页编辑的那串选项。
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
        // dbgext::szTRoot = "<Keil>\ARM"，pack 根由它派生并缓存；
        // 记录晚了会先建出空列表，下载就莫名其妙报“没有算法”。
    orb::cfg::SetToolRoot((d->pDbgX) ? d->pDbgX->szTRoot : nullptr);

        // debug 块里也带 BOM（dbgext::pBom），官方主要走这条路。
        // 两个来源本应同一个实例：本驱动的 pio->* 写只有落在 uVision 读的那个上才可见。
    if (d->pDbgX && d->pDbgX->pBom) {
                // 走 SetBomAlt 只登记备用：这条路的对象与 AG_CB_GETBOMPTR 的 ioc 不是同一个，
                // 而 uVision 取数门读 ioc，ioc 必须始终是主指针。
        orb::pdsc::SetBomAlt(reinterpret_cast<struct bom *>(d->pDbgX->pBom));
    }
    orb::cfg::Parse(d->TargArgs);
        // ULINK 风格的工程把算法写成 -FF0xxx / -FP0(xxx)，没有 '='，Parse() 看不见。
        // 即使本驱动的串为空，debug 块里也有器件、flash 与 RAM，直接 Download 也能用。
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

                // 与官方一致：先采纳 uVision 已知的信息，串为空时退回生效选项，
                // 让对话框预填探测到的 RDDI/时钟而不是空白。
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

        // 109 = 取 debug 块；真实会话走这条，不答就整串选项被丢弃。
    case 109:
        TakeDebugBlock(p, "DllUv3Cap(109)");
        return 0;                 // Keil returns 0 here too: its case 109 only
                                  // breaks out, leaving i at 0 (DbgCM.cpp:790)

        // 100 = 声明支持 Flash 下载。这是个承诺：答 1 后 uVision 就要求
        // AG_INITFLASHLOAD / AG_STARTFLASHLOAD / AG_CB_GETFLASHPARAM 全部实现。
    case 100: return 1;

        // 110 = Utilities 页的 Settings：把结果写回选项串，
        // 这是用户给 Flash 下载指定 FLM 的唯一途径。
    case 110: {
        QdllArgs* q = static_cast<QdllArgs*>(p);
        if (!q || !q->value || q->ValSize <= 0) return 0;

                // 串为空时退回 debug 块采纳的值，对话框直接显示器件隐含的算法。
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

// ---- AG_F_* 能力码的名字，只用于日志 ----
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

// ---- PlayDead —— 官方 ExecPlayDead() ----
// uVision 在“无法访问目标硬件”的恢复对话框里按“停止调试”，目标没在跑时强杀。

// AG_UNINIT 与 PlayDead 共用的收摊动作：谁结束会话，收尾必须一致。
static void SessionTeardown()
{
    orb::agdi::FlashUnInit();
    orb::agdi::Disconnect();
        // 描述属于刚结束的会话，留着会让下一个目标继承到过期的描述。
    orb::pdsc::Shutdown();
        // 访问等级与“复位下恢复”的闩锁同属本次会话，下一个目标不该继承。
    orb::agdi::DebugAccessUninit();
    // The window the reports went to is going away with the session.
    orb::ui::Detach();
}

// 收摊 + MSG_UV2_TERMINATE，位置照官方（AG_Init 末尾）。
// 放在函数末尾而不是 AG_RECOVERY_CONF 分支里，是因为两条路都要兜住。
static void PlayDeadExec()
{
    if (!orb::agdi::PlayDeadPending()) return;

    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.mainFrame || !c.uvMsg) return;      // 没有 uVision 的窗口句柄，投不出去

        // 收摊会回调 uVision，从别的线程做等于在它的恢复对话框里重入；
        // 不在 GUI 线程就留给下一次入口。
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

// ---- AG_Init ----
U32 AG_Init(U16 nCode, void *vp)
{
    orb::agdi::Context& c = orb::agdi::Get();

        // 初始化握手全程记日志：缺哪一项都不会失败，只是依赖它的功能永远不工作，
        // 例如 AG_INITCURPC 缺失时源码箭头不动。

    //暂时屏蔽，26.10.14/19:53
    //ORB_LOG_INFO("Init: nCode=0x%04X vp=0x%p", nCode, vp);

    switch (nCode & 0xFF00) {
    case AG_INITFEATURES:
        InitLogOnce();
                // 每次会话重读描述：uVision 不保证在此之前调过 109/2，而 flash 加载需要器件信息。
        if (g_dbgBlk) orb::cfg::AdoptUVision(g_dbgBlk);
        c.loaded = true;
        c.bpHead = nullptr;
                // 不清 c.curPC：AG_INITCURPC 只发一次，官方也把该指针保留到 DLL 结束，
                // 清掉会让首次读寄存器时无指针可写，源码箭头随之失效。

        memset(&orb::agdi::g_supp, 0, sizeof(SUPP));
        orb::agdi::g_supp.MemAccR = 1;
        orb::agdi::g_supp.RegAccR = 0;
        orb::agdi::g_supp.ResetR  = 1;
                // 数据观察点由 DWT 比较器承担；ExtBrkSupp 让 uVision 通过 AG_BreakFunc 交来断点表。
        orb::agdi::g_supp.ExtBrkSupp = 1;
                // ITM/SWO 控制台 + PC 采样历史 + Trace Data 窗口，只在工程启用了源时声明，
                // 否则窗口不出现、也不占目标引脚。历史是统计采样，不是全指令 trace。
        {
            const orb::cfg::Config& opt = orb::cfg::Get();
            if (opt.swoBaud > 0 || opt.traceClockKhz > 0 || opt.etb || opt.orbtrace) {
                orb::agdi::g_supp.ItmTrace = 1;   // ITM + DWT trace support
                orb::agdi::g_supp.hTrace   = 1;   // trace recording / Trace window
                orb::agdi::g_supp.hPaLyze  = 1;   // statistical PC profile
            }
        }
        if (vp) *static_cast<SUPP *>(vp) = orb::agdi::g_supp;

                // 整组能力码回给 uVision；答错是静默的 —— 依赖它的窗口根本不出现，
                // 就像寄存器窗口在 AG_AllReg 正常时仍然空白那样。
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

                // 连接前先要调试描述：连接组（端口设置/解锁）正是锁死的器件需要的，
                // 要晚了这些序列永远跑不到。调用幂等，下面还会再来一次。
                // Pack 页的 Enable 控制整条 PDSC 路径，关着就退回 FLM。
        if (orb::cfg::Get().pdscEnable) orb::pdsc::Refresh();

        if (!orb::agdi::Connect()) {
                        // uVision 即将对驱动放弃，原因必须先上屏；只上报不弹框，它自己会弹一个。
            orb::ui::Error("Cannot connect to the target. Check the probe, the "
                           "interface selection and the SWJ clock under Options "
                           "for Target / Debug / Settings.");
            return 1;   // non-zero: uVision gives up on this driver
        }

                // 目标起来后再刷新一次：描述可能带只有连上才可用的信息。
                // 没有描述的器件保持 PDSC 不可用、继续用 FLM。
        if (orb::cfg::Get().pdscEnable) orb::pdsc::Refresh();
                // “without Stop”连进入时也不停机，uVision 靠 AG_WAITFORBRK 发现目标在跑。
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
                // 握手：AG_INITFEATURES 里答 0，只有被问到时才答 1 并记住。
                // 答 1 后 uVision 才显示“调试访问受限”，并在恢复流程里发 AG_RECOVERY_CONF。
        case AG_F_DBGRECOVERY: r = orb::agdi::g_supp.DbgRecoverySupp = 1; break;
        case AG_F_SWBREAKCONF: r = orb::agdi::g_supp.SwBreakConfSupp; break;
        case AG_F_ITM_TRACE:   r = orb::agdi::g_supp.ItmTrace;      break;
        default:               r = 0;                               break;
        }
                // uVision 逐个问能力并从答案决定有哪些调试视图；答错是静默的。
        ORB_LOG_INFO("Init: AG_GETFEATURE(%s) -> %u", FeatureName(f), r);
        return r;
    }

    case AG_INITITEM:
        switch (nCode & 0x00FF) {
        case AG_INITCALLBACK:
            c.cb = reinterpret_cast<pCBF>(vp);
                        // 存回调并立刻用它取 BOM（ioc）：那是拿到 PDSC 描述的唯一途径。
                        // 不能加 !HaveBom() 守卫：dbgblk 那条路先跑会让这里整段被跳过，
                        // 而两者不是同一个实例，uVision 的取数门读的是 ioc，必须让 ioc 当主指针。
            if (c.cb) {
                struct bom* ioc = reinterpret_cast<struct bom *>(
                    c.cb(AG_CB_GETBOMPTR, NULL));
                if (ioc) orb::pdsc::SetBom(ioc);
            }
                        // 上报通道三件套之一；另两件是单独两个 AG_INITITEM。
            orb::ui::Attach(c.mainFrame, c.uvMsg, c.cb);
            break;
        case AG_INITPHANDLEP:
                        // 主窗口句柄按值传（直接强转成 HWND），不是指针；
                        // 按 AGDI.H 的字面解引用会立刻访问违例。
            c.mainFrame = reinterpret_cast<HWND>(vp);
            orb::ui::Attach(c.mainFrame, c.uvMsg, c.cb);
            break;
                // 往 uVision 命令/编译窗口送文本用的注册消息；存下来后
                // SendMessage(hMfrm, Uv2Msg, 0x0C 或 0x06, text)。
        case AG_INITUSRMSG:
            c.uvMsg = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(vp));
            orb::ui::Attach(c.mainFrame, c.uvMsg, c.cb);
            break;
        case AG_INITBPHEAD:   c.bpHead = static_cast<AG_BP **>(vp); break;
        case AG_INITCURPC:    c.curPC = static_cast<DWORD *>(vp); break;
                // Flash 下载分两步交给驱动；返回非 AG_OK 会中止会话（"Target DLL has been cancelled"）。
        case AG_INITFLASHLOAD:
            ORB_LOG_INFO("AGDI: AG_INITFLASHLOAD");
            if (!orb::agdi::FlashInitLoad()) {
                orb::agdi::StatusMessage(orb::agdi::FlashLastError());
                return AG_RDFAILED;
            }
            break;
        case AG_STARTFLASHLOAD:
                        // 入口标记：若这是日志最后一行，卡死在 FlashStartLoad（首个动作是回调 uVision）。
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
                        // 这里不清 trace：缓冲只在会话开始（Connect）时清。
            if (orb::agdi::Connect()) {
                c.core.Reset(true);
                                // 复位把目标侧 trace 寄存器打回默认，而它们只在 Connect 编程一次；
                                // 不补这一次，复位后这一轮收不到任何 trace 包。
                                // 必须写限定名：本处在 extern "C" 全局作用域。
                orb::agdi::RearmTargetTraceAfterReset("reset");
                                // 首次打开目标时装寄存器布局（官方在首次复位后、跳过 flash 下载时装）；
                                // 幂等，后续复位是空操作。
                orb::agdi::InstallRegView();
                                // 告知 uVision 复位已完成且目标已停：官方经 AG_CB_EXECHOOK
                                // ("OnStopAfterReset")，不发它 uVision 不知道复位结束了。
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
                        // 官方在此作废缓存的目标数据；本驱动的 DAP 层本就不缓存 SCS，无需处理。
            ORB_LOG_INFO("AGDI: AG_EXECITEM AG_RUNSTOP");
            break;
        case AG_RECOVERY_CONF:  // 0x0060 - Configure Debug Access Recovery
                        // 只有 AG_GETFEATURE(DBGRECOVERY) 握手答过 1，uVision 才会发这一项。
                        // 返回值必须是 AG_RECOVCONF_ERR_*，不能落到末尾的 AG_OK。
            ORB_LOG_INFO("AGDI: AG_EXECITEM AG_RECOVERY_CONF");
            {
                const U32 rc = orb::agdi::ConfigureDebugAccessRecovery(
                    static_cast<const AG_RECOVERY*>(vp));
                                // 官方在同一次调用里就执行 exitDebug 强杀，不等下一次入口；这里照做。
                PlayDeadExec();
                return rc;
            }
        case AG_STEPINTO_START:  // 0x0014 - Step Into starts
        case AG_STEPLINE_START:  // 0x0015 - Step Over (source line) starts
        case AG_OVERSTEP_START:  // 0x0063 - assembly-mode overstepping
                        // 告知即将执行的是 step：PC 上的 BKPT 要被跨过而不是执行。
            orb::agdi::AnnounceStepStart();
            ORB_LOG_INFO("AGDI: AG_EXECITEM %s",
                         ((nCode & 0x00FF) == AG_STEPINTO_START)  ? "AG_STEPINTO_START" :
                         ((nCode & 0x00FF) == AG_STEPLINE_START)  ? "AG_STEPLINE_START"
                                                                  : "AG_OVERSTEP_START");
            break;
        case AG_TRACESETUP_REQ: {
                        // 每会话一次：uVision 交来一个**清零的** TRACE_CONFIG 让驱动填。
                        // 不填窗口就是死的：注册、ClearTrace 都做了，却从不来取记录。
                        // 官方等价动作是 RegisterTraceClock()。结构体就地镜像，不引 TDI 头。
            struct UvTraceConfig { uint32_t nClock; uint32_t nReserved[23]; };
            UvTraceConfig* tc = static_cast<UvTraceConfig*>(vp);
            if (tc) {
                const orb::cfg::Config& opt = orb::cfg::Get();
                                // nClock 必须是**内核时钟**：周期号是内核周期，uVision 按 cycle/nClock 换算。
                                // 报成 trace 时钟（实测 72 vs 48 MHz）会让它请求的区间整个越界，
                                // 表现为窗口只有列没有行、Navigation 停在 0 s。tclk 只管 TPIU 位时钟。
                uint32_t coreHz = static_cast<uint32_t>(opt.cpuKhz) * 1000u;
                if (coreHz == 0) coreHz = 1;            // 0 就是"没配置"，不能报
                                // “Use Core Clock”把 trace 端口时钟钉在核心时钟上；未勾时看 tclk（0 = 跟随）。
                const uint32_t trcHz =
                    (opt.useCoreClock || opt.traceClockKhz <= 0)
                        ? coreHz
                        : static_cast<uint32_t>(opt.traceClockKhz) * 1000u;
                tc->nClock = coreHz;
                // TDI 的 Set/GetTraceConfig 用同一个值，两边保持一致。
                orb::agdi::TraceWinSetCoreClock(coreHz);
                                // CORE_CLK / TRACE_CLK 两个虚拟寄存器：Trace Data 窗口的时间轴要它们。
                orb::agdi::TraceWinRegisterClocks(coreHz, trcHz);
                                // uVision 此刻才正式要 trace，窗口多半已建好，顺手补发接口表与窗口配置。
                orb::agdi::TraceWinReconfigure();
                                // 本驱动唯一的“host 改了 trace 配置”通知口，也是首次发布两个 VTR 的地方。
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

        // 兜住“PlayDead 由别的线程置起”的情况；GUI 线程那条已在 AG_RECOVERY_CONF 分支执行过。
    PlayDeadExec();

    return AG_OK;
}

// ---- AG_MemAtt：内存映射与属性 ----
U32 AG_MemAtt(U16 nCode, UL32 nAttr, GADR *pA)
{
        // 属性流量是会话里最大的一块（实测 ~142000 次 GETMEMATT），也是 uVision
        // 确实在跑逐地址刷新的证据；全记会淹掉日志，只记每种代码首次调用与每 10000 次汇总。
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
                // uVision 告知哪些区间算内存；只记录区间，AG_CADRVALID 要用。
        if (pA) {
            MapMem(pA->Adr, pA->nLen, nAttr);
            pA->ErrAdr = 0;                // 0 == mapping accepted
        }
        return AG_OK;

    case AG_GETMEMATT:
                // 返回值是**属性字指针**：uVision 按 DWORD* 解引用，返回位掩码会被当指针跟着解。
        if (pA) {
            pA->Adr = static_cast<UL32>(
                reinterpret_cast<uintptr_t>(AttrPtr(pA->Adr)));
        }
        return AG_OK;

    case AG_GETATTRSEG:
                // 同一描述符按 64 kB 段取，覆盖率加载/保存要的就是它。
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

// ---- AG_BpInfo：断点查询与通知 ----
// vp 是 GADR*，返回值是该地址的 ATRX_* 属性字（与 CMSIS_AGDI.dll 一致）。
U32 AG_BpInfo(U16 nCode, void *vp)
{
    orb::agdi::Context& c = orb::agdi::Get();

        // 断点协商日志：只记决策点进出，高频的 AG_BPQUERY 故意不记。
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
                // 官方对未知代码走 default：未映射返回 0x80000000，已映射保持 0 ——
                // 所以 0 表示“有效”，非 0 都被当失败哨兵。曾答 1，合法地址全被判无效。
        if (!Mapped(adr)) return 0x80000000u;   // failed: unmapped address
        nA = 0;                                 // valid
        break;

    case AG_CANHANDLEBP:
        if (AttrGet(adr) & (ATRX_BREAK | ATRX_WATCH)) nA = 1;  // can remove
        else if (c.bps.Count() < c.bps.Capacity())    nA = 1;  // can add
        break;

    case AG_BPENABLE:
    case AG_BPSET: {
                // 未映射地址返回失败哨兵 0x80000000，不是驱动错误码。
        if (!Mapped(adr)) return 0x80000000u;

                // 数据观察点：uVision 标成 AG_WBREAK，由 DWT 比较器服务，不走 FPB。
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
                // “全部禁用”不是删除：条目留着，只卸目标侧安装。
                // 以前两种都调 Clear()，禁用看起来像删除，uVision 的标记会闪。
        const int n = c.bps.DisableAll();
        uint32_t addrs[64];
        const int cnt = c.bps.Addresses(addrs, 64);
        for (int i = 0; i < cnt; ++i) {
                        // ATRX_BPDIS 是“已知但未装”的标记；BREAK/WATCH 位必须一起去掉。
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

// ---- AG_BreakFunc：uVision 断点表的钩子 ----
// 表在 uVision 手里，这个钩子只告知节点是什么；原样返回即接受。
// 在这里同步属性缓存，数据观察点（AG_WBREAK）才能被后续查询认出来。
AG_BP * AG_BreakFunc(U16 nCode, U16 n1, GADR *pA, AG_BP *pBp)
{
    orb::agdi::Context& c = orb::agdi::Get();
    (void)n1;
    (void)pA;

        // 同 AG_BpInfo：这是 uVision 交节点（AG_BF_LINK）的地方，也是官方拒绝的地方。
    ORB_LOG_INFO("Bp: AG_BreakFunc(nCode=0x%X, type=%d, adr=0x%08X, enabled=%d)",
                 nCode, pBp ? static_cast<int>(pBp->type) : -1,
                 pBp ? (pBp->Adr & ~1u) : 0u, pBp ? static_cast<int>(pBp->enabled) : -1);

    if (!pBp) return pBp;

    const uint32_t adr = pBp->Adr & ~1u;
    const bool running = (InterlockedCompareExchange(&c.running, 0, 0) != 0);

    switch (nCode) {
    case AG_BF_ACCEPT:
                // 目标运行中只接受地址断点；其余类型装不上，官方返回 0xFFFFFFFF 拒绝。
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
                // AG_BF_TARGET 把地址断点也送进这条路，其他类型只允许观察点。
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
                // uVision 在运行中改断点后必须重同步遍历起点，否则 BrkWalk 走的是旧表。
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

// ---- AG_GoStep：运行控制 ----
U32 AG_GoStep(U16 nCode, U32 nSteps, GADR *pA)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return AG_NOTARGET;

        // 每个运行控制入口先清命中原因，避免 uVision 读到上一次的“命中”。
    orb::pdsc::ClearHitBp();

        // 记录运行控制代码（1 STOPRUN / 2 NSTEP / 3 GOTILADR / 4 GOFORBRK / 5 WAITFORBRK），
        // 否则“箭头不跟步”“窗口空”无法判断是没请求还是请求失败。
    ORB_LOG_INFO("GoStep: code=0x%02X nSteps=%u adr=0x%08X ...", nCode, nSteps,
                 pA ? pA->Adr : 0u);

    switch (nCode) {
    case AG_STOPRUN:
        InterlockedExchange(&c.stopReq, 1);
        ORB_LOG_INFO("GoStep: STOPRUN -> stop request posted");
        return AG_OK;

    case AG_NSTEP: {
                // 单步是新操作，上一次留下的停止请求不能取消它。
                // 不清会让循环第一次就跳出、Step() 从未执行，表现为“按过 Stop 后单步失效”。
        InterlockedExchange(&c.stopReq, 0);
        InterlockedExchange(&c.running, 1);
                // 整段单步持有探针；上面的 AG_STOPRUN 故意在锁外，Stop 键仍能进来。
        orb::agdi::ComLock comLock;
        if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
            InterlockedExchange(&c.running, 0);
            ORB_LOG_WARN("GoStep: NSTEP refused, the probe is busy (%u ms 没拿到锁)",
                         orb::agdi::kProbeWaitMs);
            return AG_NOACCESS;
        }
                // 同 RunAndWait：单步的指令上若装着断点会立刻再触发，单步期间先禁用再恢复。
        const uint32_t atBp = orb::agdi::ReadPC();
        const bool onBp = (atBp != 0u) && c.bps.Contains(atBp);
        if (onBp) {
            ORB_LOG_INFO("GoStep: NSTEP from a breakpoint at 0x%08X - comparator "
                         "temporarily disabled", atBp);
            c.bps.SetEnabled(atBp, false);
        }
                // ---- 跳过 PC 上的软件断点指令 ----
                // 读出两字节，为 BKPT（0xBE00）就把 PC 挪过去并扣一次步数，否则执行它又会停在同一处。
                // uVision 通过 BkpInstructionSkip 声明这一点（见 AG_EXECITEM）。
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

                // 单步循环照官方：每步后若落到断点上就停（BrkWalk(1)），由它给出断点编号；
                // 没有它，多指令单步会穿过断点而不上报。
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
                // 即使没能确认停机也发布 PC：不报会让每次单步都像“什么都没发生”，异常另记日志。
        const uint32_t pc = orb::agdi::ReadPC();
        if (pA) { pA->Adr = pc; pA->ErrAdr = 0; }

                // 告知单步怎么结束的：HIT_EXEC（正常）或 HIT_ESC（被停止）。
                // 不发布的话 uVision 只看到 HIT_NOREASON，无法区分正常结束。
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
                        // 同 RunAndWait：目标在跑时只采样 DHCSR，锁一小段一小段拿。
            bool halted = false;
            {
                orb::agdi::ComLock lock;
                if (lock.Held()) halted = c.core.IsHalted();
            }
            if (halted) break;
                        // 同 RunAndWait：worker 只排队状态消息，投递放在这里。
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

// ---- AG_MemAcc：内存窗口/观察窗口的访问 ----
U32 AG_MemAcc(U16 nCode, UC8 *pB, GADR *pA, UL32 nMany)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!c.connected) return AG_NOTARGET;

        // Flash 菜单的调用 pA == NULL（要求一个操作而不是读内存），
        // 所以两个命令要在下面的空地址检查之前处理。
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

        // uVision 内存/观察/反汇编窗口的流量。以前完全没有日志，读失败不可见；
        // 现在失败必记，首次调用与汇总给出流量形状。
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
                // 取指路径：这里失败就是反汇编窗口只有源码、没有指令。
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

// ---- AG_RegAcc / AG_AllReg：寄存器访问 ----
U32 AG_RegAcc(U16 nCode, U32 nReg, GVAL *pV)
{
    orb::agdi::Context& c = orb::agdi::Get();
    if (!pV) return AG_INVALOP;
    if (!c.connected) return AG_NOTARGET;

    uint32_t sel = 0;
    if (!orb::Core::RegIndexToSel(nReg, sel)) {
                // uVision 认识而本驱动不认识的寄存器；单项失败就足以让整个窗口变空，故记日志。
        ORB_LOG_WARN("RegAcc: register id 0x%02X is not mapped -> AG_INVALOP", nReg);
        return AG_INVALOP;
    }

    if (nCode == AG_READ) {
        uint32_t v = 0;
        if (!c.core.ReadRegSel(sel, v)) {
                        // 逐项读会被高频轮询，只记失败；一个失败的选择器就足以让窗口变空。
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

        // 运行中拒绝寄存器访问（官方同样如此）：运行中的核不响应 DCRSR，
        // 硬读只会拿到 AP 里的残值。
    if (InterlockedCompareExchange(&c.running, 0, 0) != 0) {
        ORB_LOG_WARN("AllReg: refused while the target runs (AG_NOACCESS)");
        return AG_NOACCESS;
    }
        // 传输本身也要持锁，否则另一线程开始的单步会插进来抢 DCRDR。
    orb::agdi::ComLock comLock;
    if (!orb::agdi::LockProbe(comLock, orb::agdi::kProbeWaitMs)) {
        ORB_LOG_WARN("AllReg: refused, the probe is busy (%u ms 没拿到锁, AG_NOACCESS)",
                     orb::agdi::kProbeWaitMs);
        return AG_NOACCESS;
    }

    RgARMCM* r = static_cast<RgARMCM *>(pR);
    if (nCode == AG_READ) {
                // uVision 靠这次调用刷新寄存器窗口并移动源码箭头；失败就是“窗口空、箭头不动”。
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
                // T 位是 xPSR 的 bit24（ARM7 的 CPSR 才是 bit5）。
                // 写错会让 uVision 以为核切到了 ARM 态，后续指令全解错。
        orb::pdsc::SetThumbState((r->xPSR & 0x01000000u) != 0u);
        return AG_OK;
    }

        // FPU 块：M4F 目标每次刷新都会要，回 AG_INVALOP 会让整次刷新失败，
        // 表现为寄存器窗口空、箭头不跟随。
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

        // 未实现的代码要记日志：uVision 只说 “AGDI: invalid operation”，
        // 既不报入口也不报代码，缺处理器曾长期不可见。
    ORB_LOG_WARN("AllReg: unhandled nCode 0x%02X -> AG_INVALOP", nCode);
    return AG_INVALOP;
}

// ---- AG_BreakResAlloc：可分配的断点数 ----
U32 AG_BreakResAlloc(U32 nAlloc, AG_BP *pBp)
{
        // 契约是“成功返回 0，否则错误码”。曾经返回可用断点数，26 被当成错误码，
        // 于是每次设断点都提示 “Cannot set breakpoint(s).”。
    (void)pBp;
    orb::agdi::Context& c = orb::agdi::Get();
    const int avail = c.bps.Capacity() - c.bps.Count();
    const U32 rc = (avail >= static_cast<int>(nAlloc)) ? 0u   /* RES_OK */
                                                        : 19u  /* RES_ERR_USED_BREAK */;
    ORB_LOG_INFO("Bp: AG_BreakResAlloc(%u) -> %u (used %d of %d)",
                 nAlloc, rc, c.bps.Count(), c.bps.Capacity());
    return rc;
}

// ---- AG_Serial：调试器与 DLL 的串口通道 ----
// 官方把串口窗口流量转发成 MSG_UV2_SERBOUT/SERWOUT；本驱动曾一律回 AG_OK 丢数据。
// AG_ITMBOUT 是 ITM 控制台：nSerNo 为激励端口，数据经 AG_CB_ITMDATA 给 printf 视图。
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

// ---- AG_HistFunc：执行历史（回溯窗口）----
// 记录 1 最新、编号越大越旧；运行中条数报 0，避免 uVision 读到半成品。
// 样本来自 ORBAGDI_Itm.cpp 解出的 DWT PC 采样包。
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
// 扩展目标访问：AG_MemAccX / AG_DapAcc（移植自官方 AGDI.CPP 的生效分支）
// 一个分发入口、每命令一个处理器。映射：PlayDead→!connected、iRun→running、
// LinkCom→ComLock、SwitchDP→SetDapIndex、Read/WriteDP|AP→Dap、PDSCDebug_*→orb::pdsc。
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

// ---- AP 支持的访问宽度（定义在 SetApAccSizes 旁，Connect() 要用）----

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

// AP 上下文供 ACCMX_QUERY_ACC 作答，必须在使用前填好；AHB/AXI-AP 都支持 8/16/32 位。
void SetApAccSizes(int dp, int ap, U32 sizes) {
    if (dp < 0 || dp >= DP_NUM || ap < 0 || ap >= AP_NUM) return;
    AP_Context[dp][ap].AccSizes = sizes;
}

// ---- 公共辅助 ----

// 清掉内存访问可能留下的临时错误码（官方同处）。
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

// 官方在此访问 PDSC 调试引擎；本驱动的实现在 orb::pdsc，
// 没有描述时就没什么可问（官方也用 PDSCDebug_IsEnabled() 守着）。
static bool PdscDebugEnabled() { return orb::pdsc::HaveBom(); }

// ---- DAP 访问命令处理器 ----

// DbgCM/AGDI.CPP:17549-17554.
static __inline U32 DapAcc_SetErr(ACCDAP* pIn, U32 err) {
    pIn->bErr = 1;
    pIn->nErr = err;
    return ACCDAP_FAILED;
}

// 读/写调试引脚：pOut8 写值、pIn8 读值、nMask 选引脚、nDelay 稳定延时（微秒）。
// 位序 0 TCK/SWCLK、1 TMS/SWDIO、2 TDI、3 TDO/SWO、4 nTRST、5 nRESET。
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

// 延时执行。经 USB 往返本就不准，单位是微秒。
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
                // RDDI 入口收字节序列，官方传 64 位字，这里按小端拆开。
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

// 写 JTAG 序列（固定 TMS、给 TDI）；SW-DP 下官方直接拒绝。
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
                // SWD 只有一个 DP，内部 id 固定 0；PDSC 的器件 id 只在多 DP 的 JTAG 链里做选择。
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

        // 官方经 PDSCDebug_DebugClockId() 映射；本驱动没有映射表，值就是 Hz。
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

// ---- AG_DapAcc ----
U32 AG_DapAcc(ACCDAP *pIn)
{
    DWORD nE         = 0;
    int   lockStatus = 0;

    orb::agdi::Context& c = orb::agdi::Get();

        // 驱动已断开。官方还允许 PDSC 序列运行期间的访问，本驱动整个序列期间都算已连接。
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

// ---- MemAccX 辅助 ----

// DbgCM/AGDI.CPP:18166-18191.
static U32 MemAccx_SelectDP(ACCMEMX *pIn, U32 *internalDP)
{
    const DWORD nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);

    if (!PdscDebugEnabled()) {
        return MemAccX_SetErr(pIn, nAddr, ACCMX_ERR_NOSUPP_DP);
    }
        // SWD 只有一个 DP，PDSC 器件 id 只有一个合法值 0。
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

// 官方在访问前后查 ReadCache/WriteCache；本驱动的 Dap 层自己在 MemRead/MemWrite 里缓存。
static int MemAccx_MemAcc_DoRead(ACCMEMX *pIn, U32 /*internalDP*/)
{
    orb::agdi::Context& c = orb::agdi::Get();
    const DWORD nAddr = static_cast<DWORD>(pIn->nAddr & 0x00000000FFFFFFFFULL);
    const DWORD nBytes = static_cast<DWORD>(pIn->nMany * pIn->accSize);

        // ACCMX_ANYSZ 让驱动自己选宽度，MemRead/MemWrite 已按地址与长度选，故与分宽度分支同路。
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
                // AP 寄存器访问：字节地址决定寄存器与 bank（bank0/0xF 的 0x00/04/08/0C）。
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

// ---- AG_MemAccX ----
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
                // 返回内存区间的访问属性，官方直接用 AP 上下文的 AccSizes 作答。
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

// ---- AGDIMsgBox：AGDI.H 声明但不导出，用于驱动报错 ----
UINT AGDIMsgBox(HWND hWnd, const char* pText, const char* pCaption,
                UINT uType, UINT uiDefAnswer)
{
        // 回调收的是 AG_MSGBOX 结构而不是字符串；直接 MessageBoxA 只是“无回调”时的兜底。
        // 直接弹框会在 uVision 的工作线程上阻塞，框还可能永远不出现。
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

