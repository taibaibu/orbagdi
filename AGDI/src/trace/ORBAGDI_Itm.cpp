/**
 * @file ORBAGDI_Itm.cpp
 * @brief ITM / SWO 控制台 + DWT PC 采样执行历史，两半都由同一条 SWO 字节流驱动。
 *
 * ① ITM 控制台：RDDI 宿主交上原始 SWO 字节，这里解 ITM 帧，把每个软件激励包经
 *    AG_Serial(AG_ITMBOUT) 转给 uVision（Trace / printf / ITM 视图）。
 * ② 执行历史：DWT 按 POSTCNT/CYCTAP 周期发 PC 采样（source 2，同一条流），存在环形缓冲里，
 *    经 AG_HistFunc 交给 uVision。
 * 解码器移植自 ORBMDK_ITM_Decoder（orbuculum 兼容帧）：48 位同步（5×0x00 + 0x80）、包头
 * bits[1:0] 编码载荷 1/2/4 字节、溢出 0x70、时间戳、全局时间戳、扩展与保留包。
 */
#include "pch.h"
#include "ORBAGDI_Etb.h"
#include "ORBAGDI_Itm.h"
#include "ORBAGDI_Oflow.h"       // kOflowTagItm（cache 编码要用）
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_TraceFile.h"
#include "ORBAGDI_TraceWin.h"
#include "ORBAGDI_CoreSight.h"
#include "ORBAGDI_TraceBehavior.h"

#include <stdio.h>
#include <string.h>

namespace orb {
namespace agdi {

// ===========================================================================
// Target-side trace hardware (ARMv7-M: ITM / TPIU / DWT / DEMCR)
// ===========================================================================
namespace {

enum : uint32_t {
    kItmBase  = 0xE0000000u,  // ITM 基址（LAR = base + 0xFB0，见 UnlockComponent）
    kItmTer0  = 0xE0000E00u,  // stimulus port enables, ports 0..31
    kItmTpr   = 0xE0000E40u,  // stimulus port privilege
    kItmTcr   = 0xE0000E80u,  // ITM control

    // async clock prescaler.  Its input is the target's TPIU async clock; whether
    // that is the core clock or an independent TRACECLKIN is undecided - see
    // `cfg::SwoAcprBaseKhz()`.  (Don't relabel it "SYSCLK" as if that settled it.)
    kTpiuAcpr = 0xE0040010u,
    kTpiuFfcr = 0xE0040304u,  // formatter / flush control
    // SPPR 偏移是 0x0F0 不是 0x0F00 —— 0xE0040F00 是 TPIU 页里的保留空洞，写进去静默无效。
    // 没被发现是因为 SPPR 复位值 2（NRZ）正是 SWO 要的：SWO 靠运气能用，并口永远不行，
    // 因为没人清过它。
    kTpiuSppr = 0xE00400F0u,  // selected pin protocol (0 = parallel, 1 = Manchester, 2 = NRZ)
    kTpiuCspsr = 0xE0040004u, // current port size: one bit per trace pin (parallel only)
    kTpiuBase = 0xE0040000u,  // TPIU 基址（有些器件复位后 TPIU 是锁着的）

    kDwtCtrl  = 0xE0001000u,  // DWT control (PC sampling)
    kDwtBase  = 0xE0001000u,  // DWT 基址（与 ITM 用同一把 CoreSight 解锁键）
    kDbgmcuCr = 0xE0042004u,  // DBGMCU_CR: TRACE_IOEN (bit5) + TRACE_MODE (bits 7:6)
    kDcbDemcr = 0xE000EDFCu,  // DEMCR (TRCENA = bit 24)
};

// TCR = ITMENA | SYNCENA | TXENA | SWOENA | TraceBusID=1（位定义见 ORBAGDI_CoreSight.h）。
// 原来只有 ITMENA|SWOENA：缺 TXENA（trace 总线输出使能）ITM 根本不往总线上放包，缺 SYNCENA
// 接收端无法定界。实测症状：SWO 下能收到帧，但 ITM 字节恒为 0。
const uint32_t kItmTcrEnable = 0x0001001Du;
const uint32_t kItmTcrEtb    = 0x00010009u;  // ITMENA | DWTENA | TraceBusID = 1
const uint32_t kTpiuSpprNrz  = 0x00000002u;  // 2 = NRZ (UART)
const uint32_t kTpiuSpprMan  = 0x00000001u;  // 1 = Manchester
const uint32_t kTpiuSpprPar  = 0x00000000u;  // 0 = parallel trace port
// TPIU_FFCR = 0x102：连续成帧 + TrigIn。试过 0x103 想额外置 bit0，实测读回仍是 0x102 ——
// 那一位不是软件可控的格式化器使能位，bit1 才是成帧位。记在这里免得再动。
const uint32_t kTpiuFfcrCont = 0x00000102u;  // 连续成帧 + TrigIn

// 同步（并口）trace 的 TCR = ITMENA | SYNCENA | TXENA，bus id 1。
// SWO 路用 kItmTcrEnable（ITMENA|SWOENA）：SWOENA 是 SWO 时间戳时钟，在并口上无意义，
// 真正把包放上总线的是 TXENA。
const uint32_t kItmTcrSync   = 0x0001000Du;

// TCR 时间戳位（TSENA + TSPRESC[9:8]）。
// ETM：ETMCR 的"编程位"是 bit10，只有它置 1 时 TRACEIDR/TECR1 才可写（实测回读恒 0，而能写的
// TRIGGER/TEEVR 恰是在进入编程模式之前写的）。半配置的 ETM 会以 ATB ID 0 灌总线、与 ITM 的
// TraceBusID=1 撞车，现象是"目标侧寄存器全对、探针一个有效包都收不到"。
// ETM 基址由 CoreSight ROM 表给出（见 EtmBase()），偏移常量在 ORBAGDI_CoreSight.h。

/// ETM 寄存器基址：先问 ROM 表（EtbDetect() 扫到的 ETM 组件，按 part number 匹配
/// 0x924/0x925/0x926/0x975），扫不到才退回架构默认。原来是六个写死的 0xE0041000+off 常量：
/// 换一颗器件、ETM 落在别的窗口就全错，而现象只是"一个包都收不到"。
uint32_t EtmBase()
{
    const uint32_t b = EtbEtmBase();
    return b ? b : kEtmBaseArch;
}

// ETMSR bit1 = Progbit 的有效值。IHI0014Q §3.4.3 / Figure 3-3 要求两处轮询：置位后等 1 才能写
// 别的寄存器，清位后等 0。不等就写，硬件有权静默丢弃 —— TRACEIDR/TECR1 回读恒 0 就是这个成因。
const DWORD    kEtmSrPollMs  = 50;           // 轮询上限（正常是下一个总线周期）

// ETMCR 只按位操作，不用整字常量。原来的 0x1D1E/0x151E 三个问题：0x151E 与注释（0x191E 清
// bit10）不符，是抄写错误；两个值在 orbuculum 与官方 DLL 里零命中；bit1/2/3/12 在这颗 ETM 上是
// 保留位（读回恒 0），整字写等于反复写无效位。
// 固件写法是"置 bit10 进编程模式 / 清 bit10 开始采集"，与 DbgCM/Debug.h 的 ETMv3_PROGRAM 一致。

// ETMCR 的另三位（IHI0014Q Table 3-5）本层只读不写 —— 归器件固件（ProgramEtm 是 RMW），
// 但它们是"ETM 模式下有没有时间轴"的直接答案，所以要报出来：
//   bit8  Branch output（分支广播）—— 固件的 TRACE_ETM_BRANCH_OUT
//   bit11 ETMEN（采集使能）—— 不是"端口选择"：固件注释 0x191E 的 bit11、启动写 0x181E、
//         官方 ULP2CM3 的 0x2810，三处证据一致。只读（归 pack 的 TraceStart 序列）。
//   bit12 Cycle-accurate tracing —— 周期计数包开关（本颗 RAZ/WI）
// ETMCR bit28 = 时间戳使能（v3.5），ETM 模式下唯一的时间来源；前提 ETMCCER[22]=1，否则 RAZ/WI。
// 打开后 ETM 会周期性（以及 ISB / 异常进出时）往流里插时间戳包。

// ProgramEtm 是否已经把 ETM 打开 —— ProgramTraceOff 据此决定要不要关。
// 开了就必须关：不关的话每轮跑完目标仍在全速灌 trace 而没人排空（见 §10.28）。
bool g_etmOn = false;

const uint32_t kItmTcrTsEna  = 0x00000002u;
const uint32_t kItmTcrTsMask = 0x00000300u;

// DWT_CTRL 的事件使能位（对应 DbgCM/Debug.h:550-556）。
// ⚠ 位序与 Trace 页那 6 个复选框不同：复选框按 TraceConf.Opt 的事件位排，DWT 那边是 bit16..bit22
// ⇒ 必须逐位映射，不能整体移位。原来 kDwtEvtShift 那版正是因此漏掉 EXCTRCEN（bit16）。
const uint32_t kDwtExcTrcEn  = 0x00010000u;  // DWT_EXCTRCEN   bit16 异常 trace
const uint32_t kDwtEvtCpi    = 0x00020000u;  // DWT_CPIEVTEN   bit17
const uint32_t kDwtEvtExc    = 0x00040000u;  // DWT_EXCEVTEN   bit18
const uint32_t kDwtEvtSleep  = 0x00080000u;  // DWT_SLEEPEVTEN bit19
const uint32_t kDwtEvtLsu    = 0x00100000u;  // DWT_LSUEVTEN   bit20
const uint32_t kDwtEvtFold   = 0x00200000u;  // DWT_FOLDEVTEN  bit21
const uint32_t kDwtEvtCyc    = 0x00400000u;  // DWT_CYCEVTEN   bit22
const uint32_t kDwtEvtMask   = 0x007F0000u;  // bit16..bit22

// DWT_FUNCTION: "ITM Emit Data (R/W) with PC" (DbgCM/Debug.h DWT_ITM_DRW_PC).
const uint32_t kDwtItmDrwPc  = 0x00000003u;

// Hardware source addresses of the DWT's own packets (DbgCM/Trace.cpp:361-380).
const uint8_t  kItmSrcDwtEvt = 0;   // DWT event counters, payload[0] = mask
const uint8_t  kItmSrcExcTrc = 1;   // exception trace
const uint8_t  kItmSrcPcSamp = 2;   // DWT PC sample

// DWT_CTRL: CYCCNTENA + CYCTAP + PCSAMPLENA -> one PC sample per 1024 cycles.
const uint32_t kDwtPcSample = 0x00001201u;

// DWT_CTRL.POSTSET (the POSTCNT reload), bits [4:1] - the same encoding Keil's
// DbgCM/Debug.h uses (DWT_POSTSET / DWT_POSTSET_P).  The tap fires every
// 2^(POSTSET+1) cycles: 0 -> 64, 1 -> 128, 2 -> 256, 3 -> 512, 4 -> 1024.
const uint32_t kDwtPostSet    = 0x0000001Eu;
const int      kDwtPostSetMax = 4;

const uint8_t kItmPortPcSample = kItmSrcPcSamp;

/// ITM_TCR.TSPRESC code for a timestamp prescaler given as its divisor
/// (1 / 4 / 16 / 64).  Anything else falls back to "no division".
uint32_t TsPrescCode(int divisor) {
    switch (divisor) {
    case 4:  return 1;
    case 16: return 2;
    case 64: return 3;
    default: return 0;
    }
}

/// 探针实际接受的 SWO 波特率：从 want 逐次对半试到 115200，每档记一条日志（否则"接受哪几档"
/// 无从得知），返回 0 = 全被拒。官方同样逐档探测，全失败才报 "SWO CLOCK not support"。
/// ⚠ CMSIS_DAP_SWO_Baudrate 第二参数是指针且不许回写：官方包装函数会检查该值是否被改写。
int ProbeSwoBaud(const rddi::Api& api, RDDIHandle h, int want)
{
    if (!api.CMSIS_DAP_SWO_Baudrate) return 0;

    const int kMinSwoBaud = 115200;    // 再低对 printf 没有意义

    int tried[16];
    int n = 0;

    for (int b = want; b >= kMinSwoBaud && n < 16; b /= 2) {
        bool dup = false;
        for (int i = 0; i < n; ++i) {
            if (tried[i] == b) { dup = true; break; }
        }
        if (dup) continue;

        int cand = b;
        const int rc = api.CMSIS_DAP_SWO_Baudrate(h, &cand);
        if (rc == 0) {
            ORB_LOG_INFO("ITM: SWO %d Hz 被接受（请求 %d Hz）", b, want);
            return b;
        }
        ORB_LOG_INFO("ITM: SWO %d Hz 被拒绝 (rc=%d)", b, rc);
        tried[n++] = b;
    }
    return 0;
}

/// 把最后一个 DWT 比较器设成"每次数据读/写都发 PC"：比较器匹配一切（DWT_MASK 忽略低 31 位），
/// 且故意取最后一个 —— 前面的归用户的数据观察点（ORBAGDI_Break.cpp）。
void ArmDataRwSample(orb::Dap& dap)
{
    uint32_t ctrl = 0;
    if (!dap.MemRead32(kDwtCtrl, ctrl)) return;

    const uint32_t n = (ctrl & kDwtNumCompMask) >> kDwtNumCompPos;
    if (n == 0) {
        ORB_LOG_WARN("ITM: no DWT comparator available for data R/W sampling");
        return;
    }
    const uint32_t idx = (n > 16u) ? 15u : (n - 1u);

    dap.MemWrite32(kDwtComp + 16u * idx, 0);
    dap.MemWrite32(kDwtMask + 16u * idx, kDwtMaskMax);
    dap.MemWrite32(kDwtFunc + 16u * idx, kDwtItmDrwPc);
    ORB_LOG_INFO("ITM: DWT comparator %u emits a PC on data R/W", idx);
}

/// Undoes ArmDataRwSample().
void DisarmDataRwSample(orb::Dap& dap)
{
    uint32_t ctrl = 0;
    if (!dap.MemRead32(kDwtCtrl, ctrl)) return;

    const uint32_t n = (ctrl & kDwtNumCompMask) >> kDwtNumCompPos;
    if (n == 0) return;
    const uint32_t idx = (n > 16u) ? 15u : (n - 1u);

    dap.MemWrite32(kDwtFunc + 16u * idx, 0);
    dap.MemWrite32(kDwtMask + 16u * idx, 0);
}

// ===========================================================================
// ITM packet decoder
// ===========================================================================
const uint64_t kSyncMask    = 0x0000FFFFFFFFFFFFULL;  // 6 bytes
const uint64_t kSyncPattern = 0x0000000000000080ULL;  // 5x 0x00 then 0x80
const uint8_t  kMaxPayload  = 5;

enum DecState {
    kUnsynced = 0,
    kIdle,
    kSw,        // software stimulus payload
    kHw,        // hardware source payload
    kTs,        // 时间戳包格式 1（还有续字节）
    kOther,     // 全局时间戳 / 扩展 / 保留（多字节）
};

// kEvTimestamp = 一个时间戳包收齐了（值在 data[]/len 里）。
// ⚠ 与 ETM 时间量无关：ITM 自己的时间戳（TCR.TSENA + TSPRESC）与 ETMCR[28]/ETMCR[12] 是两套
// 互不相干的计数器。实测 ITM 时间戳开着却不推进周期轴、ETM 侧给的是非单调小整数 ⇒ 都不是机器周期。
enum PumpEvent { kEvNone = 0, kEvPacket, kEvTimestamp };

struct Decoder {
    int      state;
    uint64_t syncStat;
    uint8_t  data[8];
    uint8_t  len;
    uint8_t  want;    // payload bytes expected
    uint8_t  src;     // source address (SW port or HW source)
    bool     isSw;

    Decoder() { Reset(); }

    void Reset() {
        state = kUnsynced;
        syncStat = 0;
        len = 0;
        want = 0;
        src = 0;
        isSw = false;
        memset(data, 0, sizeof(data));
    }
};

// Feeds one byte.  Returns kEvPacket when a complete SW/HW source packet sits
// in the decoder (data/len/src/isSw valid).
int PumpByte(Decoder& d, uint8_t c)
{
    // Sync detection runs on every byte, ahead of the state machine.
    d.syncStat = (d.syncStat << 8) | c;
    if ((d.syncStat & kSyncMask) == kSyncPattern) {
        d.len = 0;
        d.state = kIdle;
        return kEvNone;
    }

    switch (d.state) {
    case kUnsynced:
        break;                       // drop every byte until we see a sync

    case kIdle:
        memset(d.data, 0, sizeof(d.data));
        if (c == 0x00) break;        // padding

        if (c & 0x03) {              // source packet (SW or HW)
            d.want = (uint8_t)((c & 0x03) == 3 ? 4 : (c & 0x03));
            d.len  = 0;
            d.src  = (uint8_t)((c >> 3) & 0x1F);
            d.isSw = (c & 0x04) == 0;
            d.state = d.isSw ? kSw : kHw;
            break;
        }
        if (c == 0x70) break;        // overflow
        if ((c & 0x0F) == 0) {       // timestamp
            d.len = 1;
            d.data[0] = c;
            if (c & 0x80) {          // 格式 1：后面还有续字节
                d.state = kTs;
            } else {                 // 格式 2：值就在 header 里，包已经完整
                d.state = kIdle;
                return kEvTimestamp;
            }
            break;
        }
        if ((c & 0xDF) == 0x94) {    // global timestamp
            d.state = kOther;
            break;
        }
        if (c == 0x08) {             // new sync
            d.len = 0;
            d.want = (uint8_t)(kMaxPayload + 1);
            d.state = kOther;
            break;
        }
        if (c & 0x08) {              // extension
            d.len = 1;
            d.data[0] = c;
            if (!(c & 0x84)) { d.len = 0; d.state = kIdle; }
            else             { d.state = kOther; }
            break;
        }
        d.len = 1;                   // reserved packet
        d.data[0] = c;
        d.state = (c & 0x80) ? kOther : kIdle;
        break;

    case kSw:
    case kHw:
        if (d.len < (uint8_t)sizeof(d.data)) d.data[d.len++] = c;
        if (d.len >= d.want) { d.state = kIdle; return kEvPacket; }
        break;

    case kTs:
        // 时间戳格式 1 的续字节：bits[6:0] 是载荷，bit7 = 还有后续。
        // 上限取 kMaxPayload（5 字节 = header + 4 个续字节），与官方 MAX_PACKET 同口径，
        // 也正是 `_handleTS` 能读到的最大长度。
        if (d.len < (uint8_t)sizeof(d.data)) d.data[d.len++] = c;
        if (!(c & 0x80) || d.len >= kMaxPayload) { d.state = kIdle; return kEvTimestamp; }
        break;

    case kOther:
        if (d.len < (uint8_t)sizeof(d.data)) d.data[d.len++] = c;
        if (!(c & 0x80) || d.len >= kMaxPayload) d.state = kIdle;
        break;
    }
    return kEvNone;
}

// ===========================================================================
// DWT PC-sample execution history (AG_HistFunc)
// ===========================================================================
const unsigned kHistMax = 2048;

struct HistStore {
    CRITICAL_SECTION   cs;
    uint32_t           pc[kHistMax];
    unsigned           head;        // next write slot
    unsigned long long total;       // samples ever pushed

    HistStore() : head(0), total(0) {
        memset(pc, 0, sizeof(pc));
        InitializeCriticalSection(&cs);
    }

    // Samples currently held (the ring saturates at kHistMax).
    unsigned Count() const {
        return (total > (unsigned long long)kHistMax) ? kHistMax : (unsigned)total;
    }
};

HistStore& Hist()
{
    // Deliberately leaked, like the AGDI Context: uVision can still call
    // AG_HistFunc while the CRT is tearing statics down.
    static HistStore* s = new HistStore();
    return *s;
}

void HistPush(uint32_t pc)
{
    if (pc == 0) return;
    HistStore& h = Hist();
    EnterCriticalSection(&h.cs);
    h.pc[h.head] = pc;
    h.head = (h.head + 1u) % kHistMax;
    ++h.total;
    LeaveCriticalSection(&h.cs);
}

// ===========================================================================
// Capture state
// ===========================================================================
struct ItmState {
    bool active;

    // 时间轴统计（P0-F）。只在停止汇总里打一行 —— 时间戳包和普通包同一个量级
    // （实测每秒上万），不能逐条记日志。
    unsigned long long tsPackets;      // 解出的时间戳包数
    unsigned long long tsCycles;       // 它们推进掉的周期数（= 已换算成核心周期）
    int                tsLastStatus;   // 格式 1 的 bits[5:4]；-1 = 只见到格式 2

    // "上一次实际编进目标的那份配置"（`ItmProgramTarget` 落、`ProgramTraceOff` 清）。
    // 复位后 `ItmRearmTarget()` 照它原文重放；不按选项重算的理由见头文件。
    TraceSetup         armed;
    bool               armedValid;

    ItmState() : active(false), tsPackets(0), tsCycles(0), tsLastStatus(-1),
                 armedValid(false) {
        memset(&armed, 0, sizeof(armed));
    }
};

ItmState& State()
{
    static ItmState* s = new ItmState();   // leaked on purpose (see Hist())
    return *s;
}

/// 记下"这一次编进目标的那份配置"（`ItmRearmTarget` 复位后照它重放）。
/// `ItmProgramTarget` 的**两个成功出口**（并口 / SWO）都要走它 —— 少一处，那条
/// 分支在复位后就再也收不到数据，而且症状与"没接好线"一模一样。
static void RememberArmed(const TraceSetup& setup)
{
    ItmState& st = State();
    st.armed      = setup;
    st.armedValid = true;
}

Decoder& Dec()
{
    static Decoder* d = new Decoder();     // leaked on purpose (see Hist())
    return *d;
}

void OnPacket()
{
    Decoder& d = Dec();

    if (d.isSw) {
        // Software stimulus -> uVision's ITM / printf viewer, and the Trace Data
        // window (two independent views of the same packets).
        if (d.len > 0) {
            AG_Serial(AG_ITMBOUT, (U32)d.src, (U32)d.len, (void*)d.data);
            TraceWinItmStimulus((uint32_t)d.src, d.data, (unsigned)d.len);
        }
        return;
    }

    // Hardware source packet.  The source address says which kind it is
    // (DbgCM/Trace.cpp:361-380): 0 = DWT event counters, 1 = exception trace,
    // 2 = PC sample.
    switch (d.src) {
    case kItmSrcDwtEvt:
        // payload[0] is the mask of counters, the values follow one byte each.
        // The Trace window shows the ID, which is what TraceDataTypes.h asks for
        // in a TR_TYPE_EVT record.
        if (d.len >= 1) TraceWinEvent(d.data[0]);
        break;

    case kItmSrcExcTrc: {
        // Little-endian payload, handed over as-is: the window decodes the type
        // and the exception number with its own masks, exactly as Keil does.
        uint32_t v = 0;
        for (uint8_t i = 0; i < d.len && i < 4; ++i) {
            v |= static_cast<uint32_t>(d.data[i]) << (8 * i);
        }
        TraceWinExceptionTrace(v);
        break;
    }

    case kItmSrcPcSamp: {
        if (d.len == 1 && d.data[0] == 0x00) return;   // target sleeping
        uint32_t pc = 0;
        for (uint8_t i = 0; i < d.len && i < 4; ++i) {
            pc |= (uint32_t)d.data[i] << (8 * i);
        }
        HistPush(pc);
        TraceWinPcSample(pc);
        break;
    }

    default:
        break;
    }
}

// 时间戳包 → 周期号。逐行照抄官方 _handleTS（orbuculum msgDecoder.c:189-226）：
//   格式 2（bit7=0）：值 = header bits[6:4]
//   格式 1（bit7=1）：bits[5:4] = 状态；载荷自第 2 字节起各取 bits[6:0]，依次放 [6:0]/[13:7]/[20:14]/[27:21]
// 官方消费端（orbcat / orbzmq）都只做 timeStamp += timeInc，不按 timeStatus 过滤，这里保持一致。
void OnTimestamp()
{
    Decoder& d = Dec();
    if (d.len == 0) return;

    uint32_t stamp  = 0;
    int      status = -1;          // 格式 2 没有状态字段

    if (!(d.data[0] & 0x80)) {
        stamp = static_cast<uint32_t>(d.data[0] >> 4);
    } else {
        status = (d.data[0] & 0x30) >> 4;
        stamp  = static_cast<uint32_t>(d.data[1] & 0x7F);
        if (d.len > 2) stamp |= static_cast<uint32_t>(d.data[2] & 0x7F) << 7;
        if (d.len > 3) stamp |= static_cast<uint32_t>(d.data[3] & 0x7F) << 14;
        if (d.len > 4) stamp |= static_cast<uint32_t>(d.data[4] & 0x7F) << 21;
    }
    if (stamp == 0) return;

    // 时间戳计数器按 tspresc 分频，而周期轴以核心周期为单位 ⇒ 乘回去，
    // TRACE_CLK 仍是核心时钟，uVision 的 time = cycle/clock 直接是秒。
    const int presc = orb::cfg::Get().tsPrescaler;
    uint64_t advance = (presc > 1) ? static_cast<uint64_t>(stamp) * static_cast<uint64_t>(presc)
                                   : static_cast<uint64_t>(stamp);
    if (advance > 0xFFFFFFFFull) advance = 0xFFFFFFFFull;   // 格式 1 最长 28 位

    ItmState& st = State();
    ++st.tsPackets;
    st.tsCycles += advance;
    st.tsLastStatus = status;

    TraceWinAdvanceCycle(static_cast<uint32_t>(advance));
}

// 一个字节进解码器，按事件分派。SWO 泵与 ItmFeedBytes 共用，免得时间戳事件
// 只在其中一条上被处理（orbtrace 的 tag=1 正是带时间戳的那条）。
void PumpOne(uint8_t c)
{
    const int ev = PumpByte(Dec(), c);
    if (ev == kEvPacket)         OnPacket();
    else if (ev == kEvTimestamp) OnTimestamp();
}

// 只改 ETMCR 的 Progbit 并等 ETMSR[1] 跟随（手册 Figure 3-3）。返回 true = 已确认到期望值；
// false 有三种成因（ETMCR/ETMSR 读不到、轮询超时），都不改变"要不要继续"，由调用方判断。
// 为什么必须等：§3.4.3 只允许在 ETMSR[1]=1 时改其他位与寄存器，不等就写会被静默丢弃。
bool EtmSetProgbit(orb::Dap& dap, bool on, const char* what)
{
    const uint32_t etm = EtmBase();

    uint32_t cr = 0;
    if (!dap.MemRead32(etm + kOffEtmCr, cr)) {
        ORB_LOG_WARN("ITM: ETMCR 读不到 —— %s 失败", what);
        return false;
    }

    // 读-改-写：写回去的值与读到的值**只差 Progbit 这一位**，其余原样。
    const uint32_t want = on ? (cr | kEtmCrProgramBit) : (cr & ~kEtmCrProgramBit);
    if (want != cr) dap.MemWrite32(etm + kOffEtmCr, want);

    const uint64_t t0 = GetTickCount64();
    uint32_t sr = 0;
    for (;;) {
        if (!dap.MemRead32(etm + kOffEtmSr, sr)) {
            ORB_LOG_WARN("ITM: ETMSR 读不到 —— %s 无法确认（ETMCR=0x%08X）", what, want);
            return false;
        }
        if (((sr & kEtmSrProgbit) != 0) == on) {
            ORB_LOG_INFO("ITM: %s 握手完成 (ETMCR 0x%08X -> 0x%08X, ETMSR=0x%08X, "
                         "%u ms)", what, cr, want, sr,
                         (unsigned)(GetTickCount64() - t0));
            return true;
        }
        if ((GetTickCount64() - t0) >= kEtmSrPollMs) {
            ORB_LOG_WARN("ITM: %s 后等了 %u ms，ETMSR[1] 仍未变为 %d (ETMSR=0x%08X, "
                         "ETMCR=0x%08X)", what, (unsigned)kEtmSrPollMs, on ? 1 : 0, sr, want);
            return false;
        }
        Sleep(1);
    }
}

// Switch the target's trace hardware back off.  Called on stop and when the
// start sequence fails half-way, so the target is never left generating
// packets nobody drains (a full ITM FIFO would stall the core).
void ProgramTraceOff(orb::Dap& dap)
{
    if (!dap.IsConnected()) return;

    // ---- ETM：我们打开过就必须关 ----
    // 这里原来完全不碰 ETMCR，于是 ProgramEtm 打开 ETM 之后没有任何路径关掉：跑完目标仍在全速灌
    // 指令 trace 而没人排空，缓冲溢出后收到的数据是错乱的（解出假地址，不是"少一段"）。
    // 停法照器件固件：ETM_CR |= 0x400（"Stop tracing"），置 Progbit 同样要走 Figure 3-3 握手。
    if (g_etmOn) {
        uint32_t before = 0;
        dap.MemRead32(EtmBase() + kOffEtmCr, before);
        const bool ok = EtmSetProgbit(dap, true, "停采集");
        if (!ok) {
            ORB_LOG_WARN("ITM: ETM 停采集未确认（ETMCR=0x%08X），ETM 可能仍在采集", before);
        }
        g_etmOn = false;
    }

    dap.MemWrite32(kItmTcr, 0);                       // stop all ITM output

    // PC sampling, the event counters and the data-R/W comparator all go.
    ModifyReg32(dap, kDwtCtrl, kDwtPcSample | kDwtEvtMask, 0u);
    DisarmDataRwSample(dap);

    TraceClockOff(dap);

    // 目标侧已经按我们的要求关掉了 ⇒ "上次那份配置"作废。否则复位路径会重放一份
    // 已经被刻意撤掉的配置（会话收尾之后不该再武装任何东西）。
    State().armedValid = false;
}

/// 按 Trace 页配置 ITM_TPR / ITM_TER / ITM_TCR 与 DWT。tcrBase 由调用方给（SWO 或 ETB 变体），
/// 时间戳位在上面 OR 进来。Trace 页在目标侧的映射全在这里：
///   priv → ITM_TPR、ports → ITM_TER0、ts/tspresc → TCR.TSENA/TSPRESC、
///   pcperiod → DWT_CTRL.PCSAMPLEEN、pcpresc → POSTSET、
///   events → DWT_CTRL 的 EXCTRCEN 与 <counter>EVTEN、pcdrw → 一个 "Emit Data (R/W) with PC" 比较器。
void ProgramItmDwt(orb::Dap& dap, uint32_t tcrBase)
{
    const orb::cfg::Config& opt = orb::cfg::Get();

    uint32_t tcr = tcrBase;
    if (opt.tsEnable) tcr |= kItmTcrTsEna;
    tcr |= (TsPrescCode(opt.tsPrescaler) << 8) & kItmTcrTsMask;

    PrepareItmDwt(dap);                               // unlock ITM + DWT
    dap.MemWrite32(kItmTpr,  opt.itmPriv);            // stimulus port privilege
    dap.MemWrite32(kItmTer0, opt.itmPorts);           // stimulus port enables
    dap.MemWrite32(kItmTcr,  tcr);

    // DWT 有自己的锁，原来从没打开：锁着的 DWT 对总线照常接受 DWT_CTRL 写，但 PC 采样位永不生效
    // ⇒ ITM 没东西可发、TPIU 只发空闲码、探针读到恒为全 1 的总线。（锁已在函数开头随 ITM 打开。）

    uint32_t dwt = 0;
    if (dap.MemRead32(kDwtCtrl, dwt)) {
        // PC sampling: POSTSET picks the tap (0 -> 64 cycles .. 4 -> 1024).
        int post = opt.pcPrescaler;
        if (post < 0) post = 0;
        if (post > kDwtPostSetMax) post = kDwtPostSetMax;

        dwt &= ~(kDwtPostSet | kDwtPcSample | kDwtEvtMask);
        if (opt.pcPeriodic) dwt |= kDwtPcSample;
        dwt |= (static_cast<uint32_t>(post) << 1);

        // "Trace Events"（Trace 页 6 个复选框）→ DWT_CTRL。逐位照 DbgCM/Trace.cpp 与 ITM_Reconfig()：
        // 勾上置位、取消清位。整段先 & ~kDwtEvtMask 清干净，故这里是绝对赋值、不是"只增不减"。
        // EXCTRC 走 bit16（异常 trace 总开关），正是原来移位做法漏掉的那一位。
        const uint32_t evt = static_cast<uint32_t>(opt.traceEvents);
        if (evt & kTraceExcTrc) dwt |= kDwtExcTrcEn;
        if (evt & kTraceCpi)    dwt |= kDwtEvtCpi;
        if (evt & kTraceExc)    dwt |= kDwtEvtExc;
        if (evt & kTraceSleep)  dwt |= kDwtEvtSleep;
        if (evt & kTraceLsu)    dwt |= kDwtEvtLsu;
        if (evt & kTraceFold)   dwt |= kDwtEvtFold;
        if (evt & kTraceCycCnt) dwt |= kDwtEvtCyc;
        dap.MemWrite32(kDwtCtrl, dwt);

        // ⚠ 必须报真实位状态，不能报"按选项算出来的值"：原来打 pcTap=64（64 << post 算出来的），与
        // PCSAMPLENA(bit12) 到底置没置无关 ⇒ 日志看着"PC 采样开着"，实际 pcperiod=0 把它清掉了。
        // 现在报读回来的位：kDwtPcSample = bit0|bit9|bit12 全置才算开。
        const bool pcSampling = (dwt & kDwtPcSample) == kDwtPcSample;
        ORB_LOG_INFO("ITM: ports=0x%08X priv=0x%08X tcr=0x%08X dwt=0x%08X "
                     "PC采样=%s（pcperiod=%d，每 %d 核周期一个采样）ts=%d events=0x%03X",
                     opt.itmPorts, opt.itmPriv, tcr, dwt,
                     pcSampling ? "开" : "关",
                     opt.pcPeriodic ? 1 : 0,
                     pcSampling ? (64 << post) : 0,
                     opt.tsEnable ? 1 : 0, evt);

        if (!pcSampling) {
            ORB_LOG_WARN("ITM: **PC 采样是关的** —— DWT_CTRL 的 PCSAMPLENA(bit12) 为 0。"
                         "Trace Data 窗口里将**只有事件计数器**（Counter Event CPI/LSU…），"
                         "一条 PC 采样都没有。要 PC 采样：选项串加 `pcperiod=1`"
                         "（2026-10-14 起已是默认值，说明本次是显式关掉的）。");
        }
    }

    if (opt.pcDataRw) ArmDataRwSample(dap);
}

/// 配 ETM（指令 trace，ATB ID 2），按 IHI0014Q §3.4.3 / Figure 3-3：解锁 → 置 Progbit 并等
/// ETMSR[1]=1 → 写 TRACEIDR/TECR1/TRIGGER/TEEVR → 清 Progbit 并等 0。不等确认就写会被静默
/// 丢弃（回读恒 0），半配置的 ETM 会以 ATB ID 0 与 ITM 的 TraceBusID=1 撞在同一条 ATB 上。
void ProgramEtm(orb::Dap& dap)
{
    // "ETM Trace Enable"（Trace 页顶上那个复选框，键 `etmen`，照 ULINK Pro 的图）。
    // 没勾就**一个 ETM 寄存器都不写**：半配置的 ETM 会以 ATB ID 0 往总线上灌数据，
    // 和 ITM 撞在一起（见下面的说明），所以"不配"必须是**干净的不配**。
    if (!orb::cfg::Get().etmEnable) {
        ORB_LOG_INFO("ITM: ETM 未使能（Trace 页 'ETM Trace Enable' 没勾），不配 ETM");
        return;
    }

    // ETM 基址是器件事实，不是常量 —— 先让 ROM 表给出答案（§10.28 清单第 4 条）。
    // 已经扫过（ETB 路径扫的，或上一次调用扫的）就不再扫。
    if (EtbEtmBase() == 0) EtbScanRom(dap);
    const uint32_t etm = EtmBase();
    ORB_LOG_INFO("ITM: ETM 基址 0x%08X（%s）", etm,
                 (etm == kEtmBaseArch)
                     ? "ROM 表未列出 ETM，用 ARMv7-M 定义的 0xE0041000"
                     : "来自 ROM 表");

    // ---- describeETM 等价物：这颗 ETM 用哪种分支地址编码？ ----
    // 判据是 ETMBASE+0x1E4（ETMDEVTYPE）的 bit20：1 = 备用(ALT) 编码。上游 README 也说"解码不对就用
    // describeETM 看要不要关掉备用编码"，这是编码格式的唯一权威来源；本驱动原来硬编码标准编码 ⇒
    // 地址位分组错、解出地址整体错位，而"已同步"仍为真、帧校验仍全过。
    // ⚠ 位置必须在 PDSC 门控之前：门控一旦为真本层就一个 ETM 寄存器都不写并直接 return，
    // 诊断读放它后面等于永不执行。ETMCCER 在 0x1E0，0x1E8 是 ETMPDSR（电源状态，不是能力位）。
    {
        uint32_t cap0 = 0, cap4 = 0, cap8 = 0;
        const bool ok0 = dap.MemRead32(etm + kOffEtmCcer, cap0);
        const bool ok4 = dap.MemRead32(etm + 0x1E4u, cap4);
        const bool ok8 = dap.MemRead32(etm + 0x1E8u, cap8);
        const bool hwAlt = ok4 && (((cap4 >> 20) & 1u) != 0);
        const bool tsImpl = ok0 && ((cap0 & kEtmCcerTsImpl) != 0);

        // 解码器用的是**用户选的档**（`etmalt`），不再自动判定 —— 理由见
        // ORBAGDI_Config.h 里 `etmAltAddr` 的说明（固件启动后才配 ETM，驱动早读
        // 会拿到旧值 ⇒ 自动档会安静地选错）。这一读**只用于诊断比对**。
        const bool cfgAlt = orb::cfg::Get().etmAltAddr;

        ORB_LOG_INFO("ITM: ETM 能力寄存器 —— ETMCCER[0x1E0]=0x%08X(%s) bit22=%u(时间戳%s) "
                     "[0x1E4]=0x%08X(%s) [0x1E8]=0x%08X(%s)；"
                     "describeETM 判据 bit20(0x1E4)=%u ⇒ 硬件用 **%s** 分支地址编码；"
                     "etmalt=%d ⇒ 本层解码器用 **%s**",
                     cap0, ok0 ? "读到" : "读不到",
                     ok0 ? ((cap0 >> 22) & 1u) : 0u,
                     tsImpl ? "已实现" : "未实现",
                     cap4, ok4 ? "读到" : "读不到",
                     cap8, ok8 ? "读到" : "读不到",
                     ok4 ? ((cap4 >> 20) & 1u) : 0u,
                     hwAlt ? "备用(ALT)" : "标准(STD)",
                     cfgAlt ? 1 : 0,
                     cfgAlt ? "备用(ALT)" : "标准(STD)");

        if (ok0 && !tsImpl) {
            ORB_LOG_WARN("ITM: ETMCCER[22]=0 —— 这颗 ETM 不实现时间戳，ETMCR[28] 是 RAZ/WI，"
                         "写 TSENA 不会产生时间戳包（ETMCCER=0x%08X）", cap0);
        }

        if (ok4 && (hwAlt != cfgAlt)) {
            ORB_LOG_WARN("ITM: **分支地址编码与硬件不一致** —— 硬件是 %s，解码器按 %s 解。"
                         "这会让解出的地址整体错位（现象：地址跑到 flash 之外，"
                         "而同步标志与帧校验都正常）。把设置页的 `ETM Alt:` 改成 %d"
                         "（或选项串加 `etmalt=%d`）再跑一次。",
                         hwAlt ? "备用(ALT)" : "标准(STD)",
                         cfgAlt ? "备用(ALT)" : "标准(STD)",
                         hwAlt ? 1 : 0, hwAlt ? 1 : 0);
        }
    }

    // ---- PDSC 优先：整段 ETM 配置都归器件描述的 TraceStart 序列 ----
    // 这里原来只把 TraceSeqAvailable() 用在时间戳那一位上，核心配置（Figure 3-3 握手 + TRACEIDR/
    // TECR1/TEEVR）却无条件写 —— 一边承认归 pack，一边去抢同一组寄存器。两处证据：官方
    // PDSCDebug_TraceStart() 只跑序列、一个 ETM 寄存器都不写；器件固件注释也写着由 PDSC 配。
    // **实测代价**：不门控时 pack 已把 ETM 配成采集态（ETMEN=1），本层再置 Progbit 进不去握手、
    // 50 ms 超时，日志留下误导性的"ETMSR[1] 仍未变为 1"。门控后噪音消失，也不再有抢寄存器。
    // ⚠ 序列不存在时才落到本层（兜底路径原样保留）：没有 DFP 描述的老器件/裸片场景。
    if (orb::pdsc::TraceSeqAvailable()) {
        ORB_LOG_INFO("ITM: ETM 配置归器件描述的 TraceStart 序列（PDSC 优先），本层不写 ETM");
        return;
    }

    // **读-改-写，不整字覆盖。** 先读回当前 ETMCR 作基准，之后只动 bit10。
    // 整字写会连带改掉我们不认识的位；官方与器件厂的写法一律是 `Read32 & ~mask | val`。
    uint32_t cr = 0;
    if (!dap.MemRead32(etm + kOffEtmCr, cr)) {
        ORB_LOG_WARN("ITM: ETMCR 读不到（基址 0x%08X），ETM 保持原样（不配）", etm);
        return;
    }

    // 时间轴的前置条件就在这一行里：bit12 = 0 时 ETM **不会**发周期计数包，
    // 于是 ETM 流没有真实时间轴（P0-F′）—— 那是固件的配置，不是驱动没接上。
    // 这一位与 bit8 同属"器件固件拥有"，本层只读（见上面 kEtmCr* 的说明）。
    ORB_LOG_INFO("ITM: ETMCR=0x%08X 周期精确(bit12)=%u 采集使能ETMEN(bit11)=%u "
                 "分支广播(bit8)=%u",
                 cr, (cr & kEtmCrCycleAcc) ? 1u : 0u,
                 (cr & kEtmCrEtmen)      ? 1u : 0u,
                 (cr & kEtmCrBranchOut)  ? 1u : 0u);

    PrepareEtm(dap, etm);                                   // 1) unlock

    // 2) 进编程模式，并等 ETMSR[1] = 1（Figure 3-3 第一次轮询）。
    if (!EtmSetProgbit(dap, true, "进编程模式")) {
        // 确认不了就不配：半配置的 ETM 比不配更糟（会以 ATB ID 0 往总线灌数据）。先还原 Progbit ——
        // §3.4.4 说 Progbit 置起时 trace 端口关闭、FIFO 清空、不再产生 trace，卡在那同样不是用户要的。
        dap.MemWrite32(etm + kOffEtmCr, cr);
        ORB_LOG_WARN("ITM: ETM 未配置（编程模式未确认），ETMCR 已还原为 0x%08X", cr);
        return;
    }

    // 3) 只有在编程模式里这些写入才有效。
    dap.MemWrite32(etm + kOffEtmTraceIdr, kAtbIdEtm);
    // ⚠ 下面三行常量出处不一：ETMTEEVR=0x6F 是手册 §3.5.7 的原文（"set ETMTEEVR to 0x6F (TRUE)"）；
    // ETMTRIGGER=0x406F 没有出处（固件注释与手册示例都没有），已放进 probe 的 --etm-test 对照，
    // 确认前不改行为；ETMTECR1 写 0，与手册配方（只置 bit24 且 ETMTECR2 写 0）是否等价待实测。
    dap.MemWrite32(etm + kOffEtmTecr1,    0);
    dap.MemWrite32(etm + kOffEtmTrigger,  0x406Fu);
    dap.MemWrite32(etm + kOffEtmTeevr,    0x6Fu);

    // 3b) 在编程模式里回读 —— 这是"写入到底有没有被丢弃"的直接证据。旧代码从没
    //     读过 ETMSR，也从没回读，于是"TRACEIDR/TECR1 恒为 0"和"没做握手"这两件
    //     事互为因果、谁也说不清（§10.30 二）。
    uint32_t idr = 0, tecr1 = 0, teevr = 0;
    dap.MemRead32(etm + kOffEtmTraceIdr, idr);
    dap.MemRead32(etm + kOffEtmTecr1,    tecr1);
    dap.MemRead32(etm + kOffEtmTeevr,    teevr);

    // 3c) 时间戳使能（ETMCR bit28）—— ETM 模式下唯一的时间来源。手册 §7.7 原文如此；Table 3-5 [28]
    //     说 ETMCCER[22]=0 时该位 RAZ/WI。打开后 ETM 会周期性及 ISB/异常进出时插时间戳包。
    //     没有它 ETM 流里一个时间量都没有（周期计数包要 bit12，本颗 RAZ/WI）。
    //     时机：§3.4.3 只允许在 ETMSR[1]=1 时改非 Progbit 位，所以必须在清 Progbit 之前、且不能合并
    //     成一次写（实测合并写会让非 Progbit 位被静默丢弃）。读-改-写，只动 bit28。
    //     能走到这里必然"描述里没有这条序列"（开头那道门控管整段 ETM 配置），直接写。
    {
        uint32_t ccer = 0;
        if (dap.MemRead32(etm + kOffEtmCcer, ccer) && (ccer & kEtmCcerTsImpl)) {
            uint32_t crTs = 0;
            if (dap.MemRead32(etm + kOffEtmCr, crTs)) {
                ModifyReg32(dap, etm + kOffEtmCr, 0u, kEtmCrTsEna);

                uint32_t rb = 0;
                dap.MemRead32(etm + kOffEtmCr, rb);
                if ((rb & kEtmCrTsEna) != 0) {
                    ORB_LOG_INFO("ITM: ETM 时间戳已使能 (ETMCR 0x%08X -> 0x%08X；ETMCCER=0x%08X "
                                 "编码=%s 长度=%s)", crTs, rb, ccer,
                                 (ccer & kEtmCcerTsBin) ? "binary" : "gray",
                                 (ccer & kEtmCcerTs64)  ? "64-bit" : "48-bit");
                } else {
                    ORB_LOG_WARN("ITM: ETMCR[28] 写不进去（读回 0x%08X）—— ETM 时间戳没打开",
                                 rb);
                }
            }
        } else {
            ORB_LOG_WARN("ITM: 这颗 ETM 未实现时间戳 (ETMCCER=0x%08X, bit22=%u) —— "
                         "ETMCR[28] 是 RAZ/WI", ccer, (ccer >> 22) & 1u);
        }
    }

    // 3d) A-SYNC 周期（ETMCR[25:24]，IHI0014Q Table 3-5）：00=1024B 01=512B 10=256B 11=128B。
    //     为什么不留给默认值：默认 1024 意味着"使能时那一次 A-SYNC 一旦错过，就要等最多
    //     1 KB 才有下一个基准"。实测代价不是"开头缺几条"，而是整段落相对地址错位 ——
    //     日志里写成"地址清一色 0x902Cxxxx、野地址占 37%"。设成 128 字节一档后，错过
    //     也最多错 128 字节就能自愈；代价约 +6% 带宽（8 字节 A-SYNC / 128 字节）。
    //     时机同 bit28：只能在 ETMSR[1]=1 时改非 Progbit 位，所以必须在清 Progbit 之前。
    //     直写不读：这颗 ETM 上该字段回读恒为 0（实测 6 次全 0），回读判不出成败、
    //     也改不了结果，只会产出一条误导性的"写不进去"告警。
    ModifyReg32(dap, etm + kOffEtmCr, kEtmCrSyncFreqMask, kEtmCrSyncFreq128);

    // 4) 清 Progbit = 开始采集，并等 ETMSR[1] = 0（Figure 3-3 第二次轮询）。
    //    这里**重新读一次 ETMCR**（EtmSetProgbit 内部做的），不用上面那个 cr：
    //    进编程模式之后硬件可能已经改了别的位，拿旧值写回等于把它们改回去。
    const bool started = EtmSetProgbit(dap, false, "退出编程模式");

    if (idr != kAtbIdEtm) {
        ORB_LOG_WARN("ITM: ETMTRACEIDR 回读 0x%X（写入 %u）—— ATB ID 没生效，"
                     "ETM 会以 ID 0 与 ITM 的 TraceBusID=1 撞在同一条 ATB 上",
                     idr, kAtbIdEtm);
    }
    if (!started) {
        ORB_LOG_WARN("ITM: ETM 寄存器已写，但采集启动未确认（ETMSR[1] 未回到 0）");
    }

    g_etmOn = true;
    ORB_LOG_INFO("ITM: ETM 已配 (ETMCR 基准 0x%08X, ATB id %u, 回读 TRACEIDR=0x%X "
                 "TECR1=0x%X TEEVR=0x%X)",
                 cr, kAtbIdEtm, idr, tecr1, teevr);
}

} // namespace

// ===========================================================================
// Public interface
// ===========================================================================

bool ItmHostReady()
{
    return rddi::IsLoaded() && rddi::HasSwoTrace();
}

bool ItmActive()
{
    return State().active;
}

bool ItmProgramTarget(orb::Dap& dap, const TraceSetup& setup)
{
    if (!dap.IsConnected()) {
        ORB_LOG_WARN("ITM: not connected, the target's trace stays unprogrammed");
        return false;
    }

    // Some parts come out of reset with the TPIU locked; the CoreSight unlock
    // key is the one the ITM also uses (orbuculum's gdbtrace.init has the same
    // "unlockComponent" step).
    PrepareTpiu(dap);

    TraceClockOn(dap);

    // ---- DBGMCU_CR：只在器件描述没有 TraceStart 序列时才由本层写 ----
    // 判据与官方一致：PDSCDebug_TraceStart() 跑完序列就返回（"No default implementation"），
    // pack 的 EnableTraceSWO/EnableTraceTPIU 已把 DBGMCU_CR（连引脚/AF）配好，再写就是第二遍。
    // 本层这份是兜底：老器件或整场会话没有 PDSC 时才写；"有没有序列"是静态属性，此刻问得到。
    // TRACE_MODE：00 = 异步(TRACESWO 单引脚)、01/10/11 = 1/2/4-bit 同步并口。这一条以前完全没写，
    // 全靠固件启动设的 0xE0（4-bit 并口）：并口能用、SWO 永远出不来，引脚一直留在并口复用上，
    // TRACESWO(PB3) 根本没接出去 —— 很容易被误判成接线问题。
    if (!orb::pdsc::TraceSeqAvailable()) {
        uint32_t mode = 0;                       // 0 = 异步
        if (setup.mode == TraceSetup::Parallel) {
            const int w0 = (setup.width == 1) ? 1 : (setup.width == 2) ? 2 : 4;
            mode = 1u + ((w0 == 4) ? 2u : (w0 == 2) ? 1u : 0u);
        }
        const uint32_t want = (1u << 5) | (mode << 6);        // TRACE_IOEN | TRACE_MODE

        // 读-改-写，只动 TRACE_IOEN|TRACE_MODE（bits[7:5]）。原来是"先写 0 再写目标值"，理由（锁存位
        // 直接改不生效）既没依据又有害：DBGMCU_CR=0 会连 pack 的 DebugPortSetup 设好的调试冻结位一起
        // 清零；而"清零再置位"之所以显得必要，正是因为 __traceout 以前写死 0、pack 从没配过它。
        // 器件厂（STM32F4xx_DFP 的 EnableTraceTPIU）也是同一个寄存器、同一批位做读-改-写。
        const uint32_t kTraceMask = 0x000000E0u;   // TRACE_IOEN(bit5) | TRACE_MODE(bits7:6)
        uint32_t cur = 0;
        if (dap.MemRead32(kDbgmcuCr, cur)) {
            dap.MemWrite32(kDbgmcuCr, (cur & ~kTraceMask) | (want & kTraceMask));
        } else {
            // 读不到就**不动**：整字写会把别人的位一起清掉，比不配更糟。
            ORB_LOG_WARN("ITM: DBGMCU_CR 读不到，trace 引脚模式保持原样（不写）");
        }

        // 读回并进日志：这一位是否写成功直接决定 SWO 有没有信号，
        // 不看回读就只能靠猜（正是本轮之前一直卡住的原因）。
        uint32_t rb = 0xFFFFFFFFu;
        if (dap.MemRead32(kDbgmcuCr, rb)) {
            ORB_LOG_INFO("ITM: DBGMCU_CR 写 0x%08X -> 读回 0x%08X%s",
                         want, rb, (rb == want) ? "" : "  (没写进去！)");
        }
    } else {
        ORB_LOG_INFO("ITM: DBGMCU_CR 由器件描述的 TraceStart 序列负责，本层不写");
    }

    if (setup.mode == TraceSetup::Parallel) {
        int w = setup.width;
        if (w != 1 && w != 2 && w != 4) {
            ORB_LOG_WARN("ITM: parallel width %d is not 1/2/4, using 4", w);
            w = 4;
        }
        // CSPSR carries one bit per trace pin: 4-bit -> bit 3, 1-bit -> bit 0.
        // Without it the port stays at its reset size and the probe sees a
        // narrower bus than it was told to expect.
        dap.MemWrite32(kTpiuCspsr, 1u << (w - 1));
        dap.MemWrite32(kTpiuSppr,  kTpiuSpprPar);
        dap.MemWrite32(kTpiuAcpr,  0);            // no async prescaler on a sync port
        dap.MemWrite32(kTpiuFfcr,  kTpiuFfcrCont);
        ProgramItmDwt(dap, kItmTcrSync);
        ProgramEtm(dap);

        ORB_LOG_INFO("ITM: target trace -> parallel %d-bit (CSPSR=0x%08X SPPR=0) "
                     "+ ETM (ATB id %u)", w, 1u << (w - 1), kAtbIdEtm);
        RememberArmed(setup);          // 复位后重放这份（见 ItmRearmTarget）
        return true;
    }

    // SWO：单引脚，NRZ 或 Manchester。异步预分频除以目标 TPIU 异步时钟：bit clock = clock/(ACPR+1)。
    // ⚠ 除数是哪个物理时钟未判别（HCLK vs 独立 TRACECLKIN），故保持改动前口径：acprBaseKhz 目前就是
    // 核心时钟，由 cfg::SwoAcprBaseKhz() 单点给出。官方主路径用 TPIU_Clock，但另有 TraceConf.Clk 的
    // 旧支，别写成"官方一律用它"。若 H2 成立，这里偏差是 cpu/trace 倍 ⇒ 静默乱码，而且 Connect() 的
    // swoBaud > coreHz 拦不到"差整数倍"。
    uint32_t presc = 1;
    if (setup.baudHz > 0 && setup.acprBaseKhz > 0) {
        const uint32_t baseHz = static_cast<uint32_t>(setup.acprBaseKhz) * 1000u;
        presc = baseHz / static_cast<uint32_t>(setup.baudHz);
        if (presc == 0)        presc = 1;
        if (presc > 0x10000u)  presc = 0x10000u;
    }

    dap.MemWrite32(kTpiuCspsr, 1u);   // SWO drives a single pin
    dap.MemWrite32(kTpiuSppr,
                   (setup.mode == TraceSetup::SwoManchester) ? kTpiuSpprMan : kTpiuSpprNrz);
    dap.MemWrite32(kTpiuAcpr, presc - 1u);
    dap.MemWrite32(kTpiuFfcr, kTpiuFfcrCont);
    ProgramItmDwt(dap, kItmTcrEnable);

    // SWO 单引脚装不下 ETM 分支流（bit8=1 时 22 MB/s），它只会挤掉 ITM，而这条分支要的就是 ITM。
    // 所以即使 "ETM Trace Enable" 勾着这里也不配 ETM —— 指令 trace 走并口或 ETB 分支。
    // 管不了固件的 ETM_Pin_Init()（发生在内核首次运行、比这里晚），那要靠固件侧 TRACE_ETM_BRANCH_OUT=0。
    ORB_LOG_INFO("ITM: target trace -> SWO %s (prescaler=%u) 不配 ETM",
                 (setup.mode == TraceSetup::SwoManchester) ? "Manchester" : "NRZ",
                 presc);
    RememberArmed(setup);              // 复位后重放这份（含探针实际接受的波特率）
    return true;
}

bool ItmStart(orb::Dap& dap, int baudHz, int acprBaseKhz)
{
    if (State().active) return true;
    if (baudHz <= 0) return false;                 // console disabled by config

    if (!rddi::IsLoaded() || !rddi::HasSwoTrace()) {
        ORB_LOG_WARN("ITM: RDDI host has no SWO entry points, console stays off");
        return false;
    }
    if (!dap.IsConnected()) {
        ORB_LOG_WARN("ITM: not connected, console stays off");
        return false;
    }

    const rddi::Api& api = rddi::Get();
    const RDDIHandle h = dap.Handle();

    // Trace-page options that shape the target-side programming.  Both used to
    // be hard-coded, which is why the corresponding controls in the Settings
    // sheet had no effect at all (bug.md §I).
    const orb::cfg::Config& opt = orb::cfg::Get();

    // 探针实际接受的那一档（原来只试请求值，一拒就整个控制台关掉）。
    const int baud = ProbeSwoBaud(api, h, baudHz);
    if (baud <= 0) {
        // 本路径是 RDDI 宿主的 CMSIS-DAP SWO 引擎。orbtrace 探针不实现该命令族
        // （每档都 RDDI_DAP_ERROR 0x2000）—— 它的 SWO 要走 trswo=1|2。
        ORB_LOG_WARN("ITM: SWO 波特率全被拒，控制台不启动。本路径用宿主的 CMSIS-DAP "
                     "SWO 引擎；orbtrace 探针请改用 orbtrace=1 trswo=1|2 trbaud=<Hz>");
        return false;
    }
    if (baud != baudHz) {
        ORB_LOG_WARN("ITM: 探针拒绝 %d Hz，改用 %d Hz（TPIU 分频随之重算）", baudHz, baud);
    }

    // Read 路径的调用不带 port/transport/baud，宿主只能先从调试器选项串拿到它们
    // （CMSIS_DAP_ConfigureDebugger → Trace= / TraceBaudrate= / TraceTransport=）。
    // 放在探测之后：串里报的必须是实际接受的那一档（SWO_Control 会按它再下发一次）。
    if (api.CMSIS_DAP_ConfigureDebugger) {
        char cfg[96];
        _snprintf_s(cfg, sizeof(cfg), _TRUNCATE,
                    "Trace=SWO-UART;TraceBaudrate=%d;TraceTransport=Read;", baud);
        api.CMSIS_DAP_ConfigureDebugger(h, cfg);
    }

    // ---- 配目标侧 trace 硬件 ----
    // 与 orbtrace 路径共用（ItmProgramTarget）：DEMCR / TPIU / ITM / DWT 两边相同，只有采集侧不同。
    // baudHz 必须是探针实际接受的那一档：TPIU 分频按它算，用请求值算就是乱码。
    TraceSetup setup;
    setup.mode   = (opt.swoMode == 1) ? TraceSetup::SwoManchester : TraceSetup::SwoNrz;
    setup.width  = 0;
    setup.baudHz = baud;
    // acprBaseKhz = ACPR 的除数基数（调用方给 cfg::SwoAcprBaseKhz()，当前返回核心时钟；
    // "HCLK vs 独立 TRACECLKIN"仍是未判别的器件问题）。
    // 72000 只是"调用方什么都没给"时的兜底：数值与 Config.cpp 的 kDefaultCpu 相同，但不带默认时钟
    // 语义。⚠ 它与 ItmProgramTarget 里 acprBaseKhz <= 0 的行为不同（orbtrace 路没有这句兜底）。
    setup.acprBaseKhz = (acprBaseKhz > 0) ? acprBaseKhz : 72000;
    ItmProgramTarget(dap, setup);

    if (api.CMSIS_DAP_SWO_Control(h, 1) != 0) {
        ORB_LOG_WARN("ITM: probe refused the SWO start, console stays off");
        ProgramTraceOff(dap);
        return false;
    }

    Dec().Reset();
    {
        // 时间轴统计随采集段清零（P0-F）。不复位的话上一段的时间戳包数会
        // 混进来，"这一段到底有没有时间戳"就看不出来了。
        ItmState& st = State();
        st.tsPackets    = 0;
        st.tsCycles     = 0;
        st.tsLastStatus = -1;
        st.active       = true;
    }
    ORB_LOG_INFO("ITM: SWO console started (baud=%d Hz, acprbase=%d kHz, transport=Read, "
                 "时间戳=%d 分频=%d)",
                 baud, setup.acprBaseKhz, opt.tsEnable ? 1 : 0, opt.tsPrescaler);
    return true;
}

bool ItmRearmTarget(orb::Dap& dap)
{
    ItmState& st = State();
    if (!st.armedValid) {
        // 这场会话从来没武装过目标（Trace 页关着，或上次武装就失败了）——
        // 没有"上次那份"可重放，静默返回。
        return false;
    }
    if (!dap.IsConnected()) return false;

    // 目标侧这些寄存器全是幂等写：重放不改变语义，只是把复位清掉的那一套补回去（DEMCR.TRCENA 由
    // ItmProgramTarget 内部写，与 ProgramTraceOff 的 &= ~TRCENA 对称，相当于官方 ResetTarget() 末尾
    // 那句 "Enable Trace" 加上 Trace_Recovery()）。
    const bool ok = ItmProgramTarget(dap, st.armed);
    ORB_LOG_INFO("ITM: 复位后重新武装目标侧 trace（%s，宽 %d，波特 %d，acprbase %d kHz）-> %d",
                 (st.armed.mode == TraceSetup::Parallel) ? "并口"
                     : (st.armed.mode == TraceSetup::SwoManchester) ? "SWO Manchester"
                                                                    : "SWO NRZ",
                 st.armed.width, st.armed.baudHz, st.armed.acprBaseKhz, ok ? 1 : 0);

    // SyncFreq（ETMCR[25:24]）只认器件描述给的值：PDSC 有 TraceStart 序列时由序列写
    // （与官方驱动一致 —— 官方对 0xE0041000 只用于内存属性判定，从不写 ETMCR）；没有
    // 序列时 ProgramEtm 已经写成 128 字节。本层不再额外补一发 —— 那一发必须进/出编程
    // 模式，会清 FIFO 并把序列刚使能时发的 A-SYNC 冲掉，代价大于收益。
    return ok;
}

void ItmStop(orb::Dap& dap)
{
    if (!State().active) return;
    State().active = false;

    if (rddi::IsLoaded() && rddi::Get().CMSIS_DAP_SWO_Control) {
        rddi::Get().CMSIS_DAP_SWO_Control(dap.Handle(), 0);
    }
    ProgramTraceOff(dap);

    Dec().Reset();
    ItmLogTsStats("swo");
}

void ItmLogTsStats(const char* where)
{
    const ItmState& st = State();
    ORB_LOG_INFO("ITM: TS 包=%llu 周期=%llu 状态=%d (%s)",
                 st.tsPackets, st.tsCycles, st.tsLastStatus, where ? where : "?");
}

bool ItmArmForEtb(orb::Dap& dap)
{
    if (!dap.IsConnected()) return false;

    // Trace on, then ITM + DWT, exactly as the SWO path does - minus the TPIU
    // and the probe's SWO engine, which are irrelevant when the sink is the
    // on-chip ETB.
    TraceClockOn(dap);

    ProgramItmDwt(dap, kItmTcrEtb);

    // ETB + ETM 是"片上指令 trace"的标准组合：ETM 那条流以 ATB ID 2 进 ETB，由
    // `EtmTraceFeed()` 的 `kAtbIdEtm` 分支接。用与并口路径**同一个**开关
    // （Trace 页的 "ETM Trace Enable"），没勾就只采 ITM/DWT。
    if (orb::cfg::Get().etmEnable) ProgramEtm(dap);

    ORB_LOG_INFO("ITM: target armed for ETB capture");
    return true;
}

void ItmTraceOff(orb::Dap& dap)
{
    ProgramTraceOff(dap);
}

unsigned ItmPump(orb::Dap& dap, unsigned timeMs)
{
    if (!State().active) return 0;

    const rddi::Api& api = rddi::Get();
    if (!api.CMSIS_DAP_SWO_Data) return 0;

    uint8_t buf[1024];
    int overrunLogged = 0;
    unsigned total = 0;

    // 时间预算，不是字节上限（官方 `Trace_Read(time, ts)` 同口径）。0 会变成
    // "一轮都不跑"，所以兜一个最小值。
    if (timeMs == 0) timeMs = 1;
    const uint64_t t0 = GetTickCount64();

    for (;;) {
        int n  = (int)sizeof(buf);
        int st = 0;
        if (api.CMSIS_DAP_SWO_Data(dap.Handle(), &n, buf, &st) != 0) break;
        if (n <= 0) break;

        total += (unsigned)n;

        // cache（P2-1）：SWO 这条拿到的是**裸 ITM 字节**（不是 OFLOW 帧），所以由本层
        // 编码成 OFLOW 帧再写 —— tag 1 = ITM（orbuculum `Inc/nw.h:26`）。
        TraceFileWriteStream(kOflowTagItm, buf, (unsigned)n);

        for (int i = 0; i < n; ++i) PumpOne(buf[i]);

        if (st & 0x80) {                               // bit7 = buffer overrun
            // 探针侧 SWO 缓冲溢出 = 官方的 "HW Buffer Overrun"（T_MSG_HW_BUF / T_ERR_HW_BUF）。
            // 置位掩码，由 TraceWinRead 折叠成状态栏那一行 —— 上报链早已移植，缺的只是这个 producer。
            orb::agdi::TraceWinErrorSet(orb::agdi::kTErrHwBuf);

            // 溢出 = 字节流里有缺口。解码器自己不知道，会继续把数据当包头，
            // 于是 printf 里出现垃圾包 —— 打回未同步，让它重新找 48-bit sync
            // （ITM_TCR 置了 SYNCENA，目标会周期性发同步包，能自愈）。
            ItmLoseSync();

            if (!overrunLogged++) {
                ORB_LOG_WARN("ITM: SWO buffer overrun on the probe, samples were lost");
            }
        }
        if (n < (int)sizeof(buf)) break;               // drained
        if ((GetTickCount64() - t0) >= timeMs) break;    // 预算用完
    }
    return total;
}

void ItmResyncDecoder()
{
    // Start at a known boundary: an ETB capture begins at the start of the ITM
    // stream, so the sync hunt (which would discard everything until a 48-bit
    // sync packet shows up) is both unnecessary and harmful here.
    Decoder& d = Dec();
    d.Reset();
    d.state = kIdle;

    // 这里也是"一段新采集的起点"（orbtrace 开流 / ETB 每次读回），所以按段统计的
    // 时间戳计数跟着重置 —— 不清的话上一段的包数会混进来，`ItmLogTsStats` 那行
    // 就看不出"这一段到底有没有时间戳"。
    ItmState& st = State();
    st.tsPackets    = 0;
    st.tsCycles     = 0;
    st.tsLastStatus = -1;
}

void ItmLoseSync()
{
    // 与 ItmResyncDecoder() 相反：不假设任何边界，退回同步搜索。用在"字节流
    // 有缺口"之后（探针侧 SWO 缓冲溢出），见头文件的说明。
    Decoder& d = Dec();
    d.state    = kUnsynced;
    d.syncStat = 0;
    d.len      = 0;
}

void ItmFeedBytes(const uint8_t* data, int len)
{
    if (!data || len <= 0) return;
    for (int i = 0; i < len; ++i) PumpOne(data[i]);
}

void HistReset()
{
    HistStore& h = Hist();
    EnterCriticalSection(&h.cs);
    h.head  = 0;
    h.total = 0;
    memset(h.pc, 0, sizeof(h.pc));
    LeaveCriticalSection(&h.cs);
}

unsigned HistCount()
{
    HistStore& h = Hist();
    EnterCriticalSection(&h.cs);
    unsigned n = h.Count();
    LeaveCriticalSection(&h.cs);
    return n;
}

bool HistPc(unsigned record, uint32_t& pc)
{
    HistStore& h = Hist();
    EnterCriticalSection(&h.cs);

    const unsigned n = h.Count();
    bool ok = (record >= 1 && record <= n);
    if (ok) {
        // Record 1 = newest; the ring slot just before `head` holds it.
        const unsigned pos = (h.head + kHistMax - record) % kHistMax;
        pc = h.pc[pos];
    }

    LeaveCriticalSection(&h.cs);
    return ok;
}

} // namespace agdi
} // namespace orb
