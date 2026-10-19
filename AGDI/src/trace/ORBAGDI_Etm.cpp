/**
 * @file ORBAGDI_Etm.cpp
 * @brief 指令 trace 解码的 AGDI 层接收端 —— orbuculum 解码器与 Trace Data 窗口之间的适配。
 *
 * 解码器来自 orbuculum（src/coresight/traceDec/，BSD-3-Clause）：traceDecoder.c 调度、
 * etm35/etm4/mtb 三种引擎。本文件只把字节喂进引擎（TRACEDecoderPump），再把引擎报出的
 * 状态变化映射到 TraceWin*。引擎是"逐字节 + 事件位"形态，跨帧残包天然能接着解 ——
 * 原来"需要更多字节就 return"会把残包连同已收字节一起丢，每个帧边界都可能错位。
 */
#include "pch.h"
#include "ORBAGDI_Etm.h"
#include "ORBAGDI_TraceWin.h"
#include "ORBAGDI_Dap.h"
#include "ORBAGDI_CodeImage.h"      // P1-0：代码镜像（bit8=0 之后直接分支靠它补）
#include "ORBAGDI_InstrScan.h"      // P1-0：Thumb 顺序扫描
#include "ORBAGDI_CoreSight.h"      // 实现层：寄存器常量
#include "ORBAGDI_TraceBehavior.h"  // 目标识别：CPUID → 内核 / ETM 变种

// orbuculum 的 trace 解码器（traceDec/，BSD-3-Clause）。它自带 extern "C"。
#include "traceDecoder.h"

#include <string.h>

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// 接收端状态
// ---------------------------------------------------------------------------
struct Sink {
    CRITICAL_SECTION   cs;
    struct TRACEDecoder dec;         // 引擎 + CPU 状态都在这里面
    EtmProtocol        proto;        // 想要哪个引擎（dec 未创建时生效）
    bool               created;      // 引擎是否已创建

    // 目标识别结果（EtmTraceSelectProtocol 填），只给"加载"那一行日志用。
    uint32_t           cpuid;
    uint32_t           partno;
    const char*        coreName;

    unsigned long long addresses;    // EV_CH_ADDRESS 次数（= EtmTraceBranchCount）
    unsigned long long atoms;        // P-header 次数
    unsigned long long exceptions;   // 异常进出次数
    unsigned long long timestamps;
    unsigned long long cycles;
    uint64_t           lastCycle;    // 最近一次真实周期计数
    uint64_t           lastTs;       // 最近一次真实时间戳值
    // 本段采集的时间来源：0 = 还没定，1 = 时间戳包，2 = 周期计数包。
    // **ETM 的这两个是互不相同的计数器**（时基不同），一段采集里只挑一个喂给
    // 周期轴 —— 混着推等于把两种时基相加。先到的那个拥有这一段。
    int                timeSrc;
    unsigned long long syncLosses;   // 因字节流缺口被迫重新同步的次数

    // ---- P1-0：全指令 trace（靠代码镜像补直接分支） ------------------------
    uint32_t           lastPc;       // 上一个已知地址 = 扫描起点
    bool               haveLastPc;   // 有起点才谈得上"补"（第一段只能作基准）
    unsigned long long scanInstrs;     // 靠镜像补出来的指令条数
    unsigned long long scanGaps;       // 走到 to 的区间数
    unsigned long long scanBranchOuts; // 因"分支跳走"（BL 调用）收尾的区间数
    unsigned long long scanFails;      // 读不到 / 解不出 / 超上限的区间数
    orb::Dap*          dap;            // 兜底读目标内存用（Connect 时给）

    Sink()
        : proto(ETM_PROTO_ETM35), created(false),
          cpuid(0), partno(0), coreName("?"),
          addresses(0), atoms(0), exceptions(0),
          timestamps(0), cycles(0), lastCycle(0), lastTs(0), timeSrc(0),
          syncLosses(0),
          lastPc(0), haveLastPc(false), scanInstrs(0), scanGaps(0),
          scanBranchOuts(0), scanFails(0),
          dap(nullptr)
    {
        memset(&dec, 0, sizeof(dec));
        InitializeCriticalSection(&cs);
    }
};

// 时间来源的名字（日志用；**不带数字** —— 测试脚本按数字顺序取 ETM 汇总行的字段）。
const char* TimeSrcName(int s)
{
    switch (s) {
    case 1:  return "timestamp";
    case 2:  return "cycle-count";
    default: return "none";
    }
}

Sink& S()
{
    // 故意泄漏（同 ORBAGDI_Itm.cpp 的说明）。
    static Sink* s = new Sink();
    return *s;
}

TRACEprotocol MapProto(EtmProtocol p)
{
    switch (p) {
    case ETM_PROTO_ETM4: return TRACE_PROT_ETM4;
    case ETM_PROTO_MTB:  return TRACE_PROT_MTB;
    default:             return TRACE_PROT_ETM35;
    }
}

// 创建引擎。必须在 CS 之外调用（TRACEDecoderInit 只做分配，不回调）。
void CreateDecoder()
{
    Sink& s = S();
    const TRACEprotocol p = MapProto(s.proto);

    // 分支地址编码（第三个参数）必须与目标 ETM 硬件一致：判据是 ETMBASE+0x1E4 的 bit20
    // （orbuculum describeETM 同款）。上游默认 ALT，本驱动原来硬编码标准编码 ⇒ 地址整体错位。
    // 只留 ALT/STD 两档：0x1E4 是 ETM 当前配置态，连接时固件还没初始化 ⇒ 自动判定会安静选错。
    // 默认 1 = ALT（本板实测 bit20=1）。
    const bool alt = orb::cfg::Get().etmAltAddr;
    TRACEDecoderInit(&s.dec, p, alt, nullptr);
    s.created = true;
    // 加载 —— 整个会话只打这一条（新增字段**追加在末尾**，前面的锚点不变）。
    ORB_LOG_INFO("ETM: 解码器加载 —— 协议 %s，目标 %s (CPUID=0x%08X PARTNO=0x%03X)，"
                 "分支地址编码=%s（etmalt=%d）",
                 TRACEDecodeGetProtocolName(p), s.coreName, s.cpuid, s.partno,
                 alt ? "备用(ALT)" : "标准(STD)", alt ? 1 : 0);
}

// orbuculum 的 TRACEStateChanged() 取的是 struct TRACEDecoder*；这里只有
// CPU 状态，所以按它的语义（3 行）就地实现：读一位、清一位。
bool Changed(struct TRACECPUState* cpu, enum TRACEchanges c)
{
    const bool r = (cpu->changeRecord & (1u << c)) != 0;
    cpu->changeRecord &= ~(1u << c);
    return r;
}

// ---------------------------------------------------------------------------
// P1-0：全指令 trace —— 靠代码镜像把"直接分支"那一段补出来
// ---------------------------------------------------------------------------
// ETMCR bit8 = 0 时 ETM 只报间接分支目标，直接分支（B/BL/CBZ）不进流（IHI0014Q Table 3-5），
// 只能 lastPc → 顺序扫到分支 → 接 ETM 报的下一个地址 → 再扫。
// ⚠ 扫描失败不是错误：没有镜像就补不出来，报计数即可 —— 编假地址比缺一段糟得多。

const unsigned kMaxScanInstr = 4096;    // 一个分支区间的硬上限（防镜像不可信时跑飞）

bool CodeImageReadThunk(void* /*ctx*/, uint32_t addr, uint8_t* out, unsigned len)
{
    return CodeImageRead(addr, out, len);
}

// 扫描结果先攒着，确认闭合了才发出去。
// ⚠ ScanTo 遇条件分支时按"没跳"继续走（跳没跳只有 ETM 知道），猜错就走上了没执行的路，
// 那种段一条都不能发 —— 编出来的假地址比缺一段糟得多。
struct ScanBuf {
    unsigned n;
    uint32_t pc[kMaxScanInstr];
};

void CollectInstr(void* ctx, uint32_t pc, const InstrInfo& /*info*/)
{
    ScanBuf* b = static_cast<ScanBuf*>(ctx);
    if (b->n < kMaxScanInstr) b->pc[b->n++] = pc;
}

void FillGap(uint32_t from, uint32_t to)
{
    Sink& s = S();

    static ScanBuf buf;                 // 只在抽数 worker 线程上用（16 KB 静态）
    buf.n = 0;

    // ScanToCached：同一段代码在一次采集里会被反复扫（循环、反复调用），按 pc 缓存
    // 指令形状后命中路径不读镜像、不进 capstone —— 这是把解码从 106 ms/64 KB 压下来的
    // 主要手段（见 ORBAGDI_InstrScan.cpp 文件头）。
    const ScanResult r = ScanToCached(&CodeImageReadThunk, nullptr, from, to,
                                      kMaxScanInstr, &CollectInstr, &buf);

    s.scanInstrs += r.instrs;

    if (r.reached) {
        // 闭合了才发：这一段执行流是真的（顺序指令 + 跟进去的函数体 + 分支目标）。
        for (unsigned i = 0; i < buf.n; ++i) TraceWinEtmInstruction(buf.pc[i], true);

        // 两种正常收尾：走到 `to`，或者那次间接分支的目标就是 `to`（函数返回点）。
        if (r.stopPc == to) ++s.scanGaps;
        else                ++s.scanBranchOuts;
        return;
    }

    ++s.scanFails;
    // 限流：前 3 次 + 之后每 4096 次一条。这里在热路径上（每个分支一次），但**只在
    // 补不出来时**才走到，而且这条恰好是"用户需要知道执行流缺了一段"的证据。
    if (s.scanFails <= 3 || (s.scanFails % 4096) == 0) {
        // 把**停下那条指令**的 4 个字节一起打出来 —— 解码器误判时，这是唯一能直接
        // 定性的证据（"目标不一致"既可能是解码错了，也可能是模型不对）。
        uint8_t b[4] = { 0, 0, 0, 0 };
        CodeImageRead(r.stopPc, b, 4);
        ORB_LOG_WARN("ETM: P1-0 代码镜像补不出这一段（0x%08X -> 0x%08X，%s）"
                     "—— 停在 0x%08X，字节 %02X %02X %02X %02X；执行流会缺这一段；"
                     "累计失败 %llu 次",
                     from, to, r.why, r.stopPc, b[0], b[1], b[2], b[3], s.scanFails);
    }
}

// 兜底读：会话里没下载时，代码镜像只能现读目标内存（P1-0 来源 ②）。
// ⚠ 这里不加探针锁：本路径在抽数 worker 上走，而 worker 抽数时已持 ComLock，
// 再拿一次会自锁（CRITICAL_SECTION 不可重入）。
bool ReadTargetMem(void* ctx, uint32_t addr, uint8_t* out, unsigned len)
{
    orb::Dap* dap = static_cast<orb::Dap*>(ctx);
    if (!dap || !dap->IsConnected()) return false;
    return dap->MemRead(addr, out, len);
}

// 引擎报"有事发生"时被调。`d` 是 TRACECPUState*（就在解码器内部）。
void OnReport(void* d)
{
    struct TRACECPUState* cpu = static_cast<struct TRACECPUState*>(d);
    if (!cpu) return;

    Sink& s = S();

    // ---- 指令地址：Trace Data 窗口真正要的东西（热路径，3 秒 600 万次）----
    // ⚠ 这里不要加日志：ORB_LOG_* 没有限流，一条 INFO 就是一次 fprintf。
    // 要看地址用 EtmTraceLogSummary() 那一条，或临时加带上限的诊断。
    if (Changed(cpu, EV_CH_ADDRESS)) {
        ++s.addresses;

        // ---- 诊断：把"明显不是代码"的地址打出来，只打日志不过滤 ----
        // 记录里唯一带地址的就是本段末尾的 TraceWinEtmInstruction(cpu->addr)，所以 uVision 反汇编
        // 用的垃圾地址只可能来自它；机制是喂进去的字节本身错了（地址白名单已被用户否决）。
        // 只打前 8 条：热路径每秒几十万次，不能刷屏。
        {
            static int s_wildLogged = 0;
            if (s_wildLogged < 8) {
                const uint32_t a = cpu->addr;
                const orb::cfg::Config c = orb::cfg::Effective();
                bool plausible = false;
                if (c.iromSize) {
                    if (a >= c.iromStart &&
                        static_cast<uint64_t>(a) <
                            static_cast<uint64_t>(c.iromStart) + c.iromSize) {
                        plausible = true;                  // 器件声明的 Flash
                    }
                    if (a < c.iromSize) plausible = true;  // 启动别名区
                }
                if (!plausible) {
                    if (a >= 0x10000000u && a <= 0x1FFFFFFFu) plausible = true;  // CCM/系统
                    if (a >= 0x20000000u && a <= 0x3FFFFFFFu) plausible = true;  // SRAM
                    if (a >= 0x40000000u && a <= 0x5FFFFFFFu) plausible = true;  // 外设
                    if (a >= 0x60000000u && a <= 0x9FFFFFFFu) plausible = true;  // 外部 RAM
                    if (a >= 0xE0000000u)                     plausible = true;  // 内核私有
                }
                if (!plausible) {
                    ++s_wildLogged;
                    ORB_LOG_WARN("ETM: 解出的地址 **0x%08X** 不在任何常见映射区"
                                 "（第 %d/8 条）—— 同步=%d，上一条地址=0x%08X，"
                                 "本次会话地址累计=%llu。这是**输入字节有错**的症状"
                                 "（§8.2），不是解码器算错；"
                                 "记录里带地址的只有 ETM 指令这一条",
                                 a, s_wildLogged,
                                 EtmTraceIsSynced() ? 1 : 0,
                                 s.haveLastPc ? s.lastPc : 0u,
                                 s.addresses);
                }
            }
        }

        // ---- P1-0：先把"上一个地址到这条分支之间"的指令补出来 ----
        // bit8=0 时直接分支不进流，只能靠代码镜像顺序扫（FillGap）。
        // ⚠ 只有解码器已同步时才补：未同步解出的是垃圾地址，拿去读内存会让 MEM-AP 报错、DAP 进
        // 错误态；未同步时连起点都不更新。补出来的指令必须先于分支目标记录，时间轴才对。
        const bool synced = EtmTraceIsSynced();
        if (!synced) {
            s.haveLastPc = false;
        } else {
            if (s.haveLastPc) FillGap(s.lastPc, cpu->addr);
            s.lastPc     = cpu->addr;
            s.haveLastPc = true;
        }

        // cpu->addr 已经是"Thumb/ARM 模式已处理过"的地址（ISYNC 的 bit0
        // 决定模式，Thumb 时 bit0 被清掉）—— 这一点自研骨架没有做，
        // 也是窗口里地址全是垃圾的原因之一。
        //
        // ⚠ 只在已同步时上报：未同步解出的不是"少一段"，而是**假地址**
        // （相对地址的基准未知）。头文件那条承诺（"在收到下一个 A-SYNC/ISYNC
        // 之前不上报地址，假地址比没有地址更糟"）原来漏在这里 —— 假地址不只
        // 污染 Trace 窗口，还会让覆盖率替它新建一个 64 kB 段。
        if (synced) TraceWinEtmInstruction(cpu->addr, true);
    }

    // ---- 原子（P-header）：哪几条指令执行了、哪几条没执行 ----------------
    // 自研骨架整个丢掉了这条信息。窗口侧目前只渲染"分支目标"，
    // 逐原子展开留待下一步（需要在记录里带 executed/not-executed 位）。
    if (Changed(cpu, EV_CH_ENATOMS) || Changed(cpu, EV_CH_WATOMS)) {
        ++s.atoms;
    }

    // ---- 异常进出 ---------------------------------------------------------
    if (Changed(cpu, EV_CH_EX_ENTRY) || Changed(cpu, EV_CH_EX_EXIT)) {
        ++s.exceptions;
        // 暂不上报异常记录：窗口的异常表要求 (type << TR_EXC_TYP_POS) |
        // number 的打包格式，而那套宏不在本文件的可见范围里。先只计数，
        // 打包格式确认后再补（见 COMPAT_ANALYSIS.md §10.21 的遗留）。
    }

    // ---- 时间与周期：ETM 自己的时间量 ----
    // ETM 有两个不同的时间计数器，一段采集只挑一个喂周期轴（s.timeSrc 一旦定下就不改）：
    // 时间戳包（ETMCR[28]，非周期精确）与周期计数包（ETMCR[12]，仅器件实现时才有）。
    // 混着推等于把两种时基相加；绝对值→增量、回绕重建基准、与 ITM 的仲裁都在 TraceWin。
    if (Changed(cpu, EV_CH_TSTAMP)) {
        ++s.timestamps;
        s.lastTs = cpu->ts;
        if (s.timeSrc == 0) s.timeSrc = 1;
        if (s.timeSrc == 1) TraceWinEtmTime(cpu->ts, "timestamp");
    }
    if (Changed(cpu, EV_CH_CYCLECOUNT)) {
        ++s.cycles;
        s.lastCycle = cpu->cycleCount;
        if (s.timeSrc == 0) s.timeSrc = 2;
        if (s.timeSrc == 2 && cpu->cycleCount != COUNT_UNKNOWN) {
            TraceWinEtmTime(cpu->cycleCount, "cycle count");
        }
    }

    // 其余事件位（CONTEXTID / TRIGGER / THUMB / SECURE …）本轮不用，
    // 但必须清掉，否则下一次回调会把它们当成"新事件"重复处理。
    cpu->changeRecord = 0;
}

} // namespace

// ===========================================================================
// 协议选择
// ===========================================================================
void EtmTraceSelectProtocol(orb::Dap& dap)
{
    if (!dap.IsConnected()) return;

    // 目标识别归行为层：CPUID → 架构 / PARTNO / 内核名 / ETM 变种。
    TargetProfile prof;
    if (!ProbeTarget(dap, prof)) {
        ORB_LOG_WARN("ETM: CPUID 读不到，协议保持 %s", EtmTraceProtocolName());
        return;
    }

    const uint32_t cpuid = prof.cpuid;
    const uint32_t part  = prof.partNo;
    const char*    core  = prof.coreName;

    EtmProtocol p;
    switch (prof.etm) {
    case EtmVariant::V4:  p = ETM_PROTO_ETM4;  break;
    case EtmVariant::Mtb: p = ETM_PROTO_MTB;   break;
    case EtmVariant::V35: p = ETM_PROTO_ETM35; break;
    default:
        // M0 / M1 本来就没有 ETM（表里 etm=None）；只有 PARTNO 认不出时才值得警告，
        // 表里给的是"没有 trace 出口"而不是"我们不知道"。
        if (prof.desc == nullptr) {
            ORB_LOG_WARN("ETM: PARTNO 0x%03X (CPUID=0x%08X) 不在内核表里 —— 按 ETMv3.5 处理",
                         part, cpuid);
        }
        p = ETM_PROTO_ETM35;
        break;
    }

    // 结果记在 Sink 里，"加载"那一行日志会带上；这里不再单独打（异常情况
    // 上面已经用 WARN 报过了）。
    Sink& s = S();
    s.cpuid    = cpuid;
    s.partno   = part;
    s.coreName = core;

    // P1-0 的来源 ②：会话里没下载时的兜底读。注册在这里是因为它是"连接之后、
    // 开流之前"唯一一个稳定经过的点（`EtmTraceReset()` 在**每次开流**时也会调，
    // 所以镜像的清理不能放那儿 —— 见 ORBAGDI_Agdi.cpp 的 Connect）。
    s.dap = &dap;
    CodeImageSetReader(&ReadTargetMem, &dap);

    EtmTraceSetProtocol(p);
}

void EtmTraceSetProtocol(EtmProtocol p)
{
    Sink& s = S();
    // 协议没变就什么都不做。
    if (s.proto == p) return;

    EnterCriticalSection(&s.cs);
    if (s.created) {
        s.dec.engine->destroy(s.dec.engine);
        s.created = false;
    }
    s.proto = p;
    memset(&s.dec, 0, sizeof(s.dec));
    LeaveCriticalSection(&s.cs);
}

const char* EtmTraceProtocolName()
{
    return TRACEDecodeGetProtocolName(MapProto(S().proto));
}

// ===========================================================================
// 数据与状态
// ===========================================================================
void EtmTraceFeed(const uint8_t* data, size_t len)
{
    if (!data || len == 0) return;

    Sink& s = S();
    if (!s.created) CreateDecoder();

    EnterCriticalSection(&s.cs);

    // TRACEDecoderPump 的签名要 uint8_t*（非 const）。引擎只读它，所以
    // const_cast 安全；不拷贝是为了避免每个 OFLOW 帧都多一次 memcpy。
    TRACEDecoderPump(&s.dec, const_cast<uint8_t*>(data), static_cast<int>(len),
                     &OnReport, &s.dec.cpu);

    LeaveCriticalSection(&s.cs);
}

unsigned EtmTraceBranchCount()
{
    return static_cast<unsigned>(S().addresses);
}

void EtmTraceForceSync(bool isSynced)
{
    Sink& s = S();
    if (!s.created) return;

    bool lost = false;
    unsigned long long nth = 0;

    EnterCriticalSection(&s.cs);
    const bool was = TRACEDecoderIsSynced(&s.dec);
    TRACEDecoderForceSync(&s.dec, isSynced);
    if (was && !isSynced) {
        lost = true;
        ++s.syncLosses;
        nth = s.syncLosses;
    }
    LeaveCriticalSection(&s.cs);

    // 只在第一次打 WARN：缺口可能连着来（丢帧往往是成串的），这条不能刷屏。
    if (lost && nth == 1) {
        ORB_LOG_WARN("ETM: 字节流出现缺口 —— 解码器强制重新同步，在收到下一个 "
                     "A-SYNC/ISYNC 之前不上报地址（假地址比没有地址更糟）");
    }
}

bool EtmTraceIsSynced()
{
    Sink& s = S();
    return s.created && TRACEDecoderIsSynced(&s.dec);
}

void EtmTraceReset()
{
    Sink& s = S();

    EnterCriticalSection(&s.cs);
    if (s.created) {
        s.dec.engine->destroy(s.dec.engine);
        s.created = false;
    }
    memset(&s.dec, 0, sizeof(s.dec));
    s.addresses = 0;
    s.atoms = 0;
    s.exceptions = 0;
    s.timestamps = 0;
    s.cycles = 0;
    s.lastCycle = 0;
    s.lastTs = 0;
    s.timeSrc = 0;
    s.syncLosses = 0;

    // P1-0：解码器重建了，地址序列要重新建立基准（第一段只能当起点）。
    // ⚠ 代码镜像不在这里清：本函数每次开流都会调，而镜像可能是刚 Download 存下来的，
    // 在这儿清等于每次开流都退化成现读目标内存。镜像生命周期是"一次会话"。
    s.lastPc         = 0;
    s.haveLastPc     = false;
    s.scanInstrs     = 0;
    s.scanGaps       = 0;
    s.scanBranchOuts = 0;
    s.scanFails      = 0;

    LeaveCriticalSection(&s.cs);
}

// ===========================================================================
// 停止时的汇总 —— 一个采集段只打这一条
// ===========================================================================
void EtmTraceLogSummary(const char* where)
{
    Sink& s = S();
    // 每个计数都写成 `名字=值`：日志的其他行（orbtrace 那几行）都是这个形状，
    // 测试脚本按 `=(\d+)` 取数。写成"地址 N 条"那种自然语言形式看着顺眼，
    // 但解析不了 —— `test\trace_stream.ps1` 就靠这一行判 P0-D。
    ORB_LOG_INFO("ETM: 停止(%s) 地址=%llu 原子=%llu 异常=%llu 时间戳=%llu "
                 "周期=%llu 同步=%d 缺口重同步=%llu 协议=%s 时间源=%s",
                 where ? where : "?",
                 s.addresses, s.atoms, s.exceptions,
                 s.timestamps, s.cycles,
                 EtmTraceIsSynced() ? 1 : 0, s.syncLosses,
                 EtmTraceProtocolName(), TimeSrcName(s.timeSrc));

    // 周期轴的现场证据（P0-F′）：ETM 模式的时间轴到底接没接通、是谁在推、
    // 推了多少。`source=ETM cycle count` + `bTS=1` 就是"纯 ETM 流也有真实
    // 时间轴"的判据（另一行是 `cycle axis source = ...`）。
    TraceWinCycleAxisLog(where);

    // P1-0 的产出与代价：`补出=` 是靠代码镜像扫出来的指令条数（bit8=0 后流里没有的那部分）；
    // 三种收尾：闭合=走到 ETM 给的地址、跳走=遇无条件分支跳走（指令是真的，缺跳出去那段）、
    // 补不出=读不到/解不出/超上限。判据：有镜像时 `补出` 应远大于 `地址`，`补不出` 接近 0。
    ORB_LOG_INFO("ETM: P1-0 全指令 地址=%llu 补出=%llu 闭合=%llu 跳走=%llu 补不出=%llu",
                 s.addresses, s.scanInstrs, s.scanGaps, s.scanBranchOuts, s.scanFails);

    // 指令形状缓存的命中率：P1-0 解码成本几乎全在这里。命中率高而解码仍慢 ⇒
    // 瓶颈已经不在 ScanTo（去查 TraceWin 的落记录）；命中率低 ⇒ 工作集超过 512 KB
    // 镜像（RAM 里跑的函数会撞槽），或者镜像在采集期间被反复重建（flushes 大）。
    {
        InstrScanCacheStats cs = {};
        InstrScanGetCacheStats(cs);
        const double tot = static_cast<double>(cs.hits + cs.misses);
        ORB_LOG_INFO("InstrScan: 指令缓存 命中=%llu 未命中=%llu 命中率=%.1f%% "
                     "作废=%llu 槽=%u",
                     cs.hits, cs.misses,
                     tot > 0.0 ? 100.0 * static_cast<double>(cs.hits) / tot : 0.0,
                     cs.flushes, cs.slots);
    }

    CodeImageSummary(where);
}

} // namespace agdi
} // namespace orb
