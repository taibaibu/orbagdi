/**
 * @file ORBAGDI_OrbTrace.cpp
 * @brief orbtrace 探针的 trace 采集源（RDDI `OrbTrace_*` / `TraceStream_*`）。
 *
 * 数据链：
 *
 *     TraceStream_Read            (RDDI，USB bulk IN，原始字节)
 *        -> OflowDeframer         (COBS + OFLOW 解帧，orbuculum 格式)
 *           -> tag 1 (ITM)  ->  ItmFeedBytes()  -> AG_CB_ITMDATA + TraceWin
 *           -> tag 2 (ETM)  ->  EtmTraceFeed()  -> TraceWin（ETM 分支）
 *
 * 与 SWO 路径的关键差异：SWO 流里 ITM 解码器靠 48-bit sync 找同步点，而
 * orbtrace 的流从帧边界开始、且每帧自带校验，所以**不能**让 ITM 解码器去搜
 * sync（否则开头的包会被丢掉）。ItmFeedBytes 之前的 ItmResyncDecoder() 就是
 * 为这个。
 *
 * **ITM 与 ETM 在这一点上不一样，别照搬**：ITM 每个包自带边界，所以"帧起点"
 * 可以直接当同步点；而 ETM 的分支地址是**相对**的，必须等到 A-SYNC 或 ISYNC
 * 才有基准地址。所以 ETM 侧**没有**对应的"跳过同步"开关（原来那个
 * `atStreamStart = true` 已经删掉，见 ORBAGDI_Etm.h）。
 *
 * 归属：AGDI 层。RDDI 层只提供"读字节"，不感知 trace 语义
 * （见 COMPAT_ANALYSIS.md §8）。
 */
#include "pch.h"
#include "ORBAGDI_OrbTrace.h"
#include "ORBAGDI_Oflow.h"
#include "ORBAGDI_Itm.h"
#include "ORBAGDI_Etm.h"
#include "ORBAGDI_TraceFile.h"
#include "ORBAGDI_TraceWin.h"

#include <string.h>

namespace orb {
namespace agdi {
namespace {

// RDDI `TraceStream_Read` 的"本轮无数据"返回码（与 StreamingTrace_WaitForEvent
// 同口径，见 orbrddi-main/include/ORBMDK_RDDI.h）。
const int kNoData = 0x204;

// `RddiOrbTraceInfo` 现在在 ORBAGDI_Rddi.h —— ORBAGDI_Power.cpp 也要用它。

struct State {
    bool          active;   // trace 流已打开
    bool          oflow;    // 流是 OFLOW/COBS 成帧的
    RDDIHandle    handle;
    OflowDeframer deframer;
    OrbTraceStats stats;
    bool          logged;   // 设备信息只打一次日志
    bool          sawData;  // 第一批字节只打一次日志

    // 读路径诊断。区分三种"0 字节"：
    //   reads 涨、noData == reads  -> 端点一直是空的（探针没在发）
    //   errors 涨                  -> 读本身出错（不是"没数据"）
    //   reads == 0                 -> 泵根本没跑
    unsigned long long reads;
    unsigned long long noData;
    unsigned long long errors;

    unsigned framesLogged;   // 前几帧打 hex，用于确认通道号与内容

    // 上一次看到的解帧错误计数。两者一变就置 `T_ERR_DATA_ERR`（= 官方的
    // "Data Stream Error"）—— 解帧层原先只写日志，状态栏永远看不到。
    unsigned long long lastBadSum;
    unsigned long long lastCobsErrors;

    // 上一次看到的 RDDI 环丢帧字节数。一变就置 `T_ERR_DATA_OVF`
    // （= 官方的 "Data Overflow"）。
    unsigned long long lastDropped;

    // 不认识的 tag 的直方图（按 tag 索引）。只数帧数看不出丢了**多少字节** ——
    // 实测 tag 6 一次就是 56 MB，而"其它=55663 帧"读起来像几帧噪声。
    unsigned long long tagBytes[256];
    unsigned           tagFrames[256];

    // stats() is not optional: a member left out of this list is default-
    // initialized, and for a plain counter struct that means "indeterminate".
    // The stop path prints those counters even when the stream was never opened
    // (trace off), and did so with a garbage ITM count (4.5e17) until this was
    // added - OrbTraceStart() happens to zero it, which is why it only showed up
    // on a session that never started a trace source.
    State() : active(false), oflow(true), handle(0), stats(), logged(false),
              sawData(false), reads(0), noData(0), errors(0), framesLogged(0),
              lastBadSum(0), lastCobsErrors(0), lastDropped(0)
    {
        memset(tagBytes,  0, sizeof(tagBytes));
        memset(tagFrames, 0, sizeof(tagFrames));
    }
};

State& S()
{
    // 故意泄漏（同 ORBAGDI_Itm.cpp 的说明）。
    static State* s = new State();
    return *s;
}

// 一帧解出来：按 tag 分派。
void OnFrame(const OflowFrame& f, void* /*user*/)
{
    State& s = S();

    // 前几帧原样打出来：探针的 Packetizer 把"通道号"放在每个包的第一个字节，
    // 而我们按 orbuculum 的约定把它当 tag（1 = ITM，2 = ETM）。收到"其它"就说明
    // 通道号对不上，光看计数永远看不出是几 —— 得看原始字节。
    if (s.framesLogged < 3) {
        ++s.framesLogged;
        char hex[3 * 40 + 4];
        int n = 0;
        const unsigned show = (f.len < 40) ? f.len : 40;
        for (unsigned i = 0; i < show && n + 3 < (int)sizeof(hex); ++i) {
            n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", f.data[i]);
        }
        ORB_LOG_INFO("orbtrace: 帧 tag=%u len=%u good=%d : %s%s",
                     f.tag, f.len, f.good ? 1 : 0, hex, (f.len > show) ? "..." : "");
    }

    if (!f.good) {
        // 校验和不对：这一帧不可信。解帧器已经计过数，这里直接丢。
        return;
    }

    switch (f.tag) {
    case kOflowTagItm:
        s.stats.itmBytes += f.len;
        // 走和 SWO / ETB 完全相同的 ITM 解码器与 uVision 回调。
        ItmFeedBytes(f.data, (int)f.len);
        break;

    case kOflowTagEtm:
        s.stats.etmBytes += f.len;
        // **不能**在这里"强制同步"：ETM 的分支地址是相对的，必须等到
        // A-SYNC 或 ISYNC 才有基准地址（见 ORBAGDI_Etm.h 的说明）。
        // 帧边界只是"字节从哪里开始"，不是"同步点"—— 这两件事在 ITM
        // 里恰好重合，在 ETM 里不重合，原来那个 atStreamStart=true 就是
        // 把两者混为一谈。
        EtmTraceFeed(f.data, (size_t)f.len);
        break;

    default:
        // **不认识的 tag 也要记账**：只数"其它=N 帧"看不出丢了多少字节、丢在哪条
        // tag 上。实测（2026-10-13）一次 10 s 采集里 `tag 6` 有 54,823 帧 × 1024 B
        // = **56 MB**（占全部载荷 98.6%），而 tag 2 只有 1,850 B —— 光看
        // "其它=55663" 会以为丢的是几帧噪声。直方图在关流时打（见 OrbTraceStop）。
        ++s.stats.otherFrames;
        s.tagBytes[f.tag]  += f.len;
        s.tagFrames[f.tag] += 1;
        break;
    }
}

// 裸字节（protocol 0x01 TPIU raw）：没有 tag，只能全部当 ITM 处理。
void OnRawBytes(const uint8_t* data, int len)
{
    State& s = S();
    s.stats.itmBytes += (unsigned long long)len;
    ItmFeedBytes(data, len);
}

// 解释一块原始字节：落盘 → 解帧 → 解码 → 错误折叠。
//
// **完全不碰探针**（代码镜像的兜底读默认关闭，见 CodeImage 的 `kMemFallbackEnabled`），
// 所以可以、也必须放在探针锁之外调 —— 见头文件"抽 / 解 分离"。
// 调用者保证：同一块只喂一次、顺序与 `OrbTracePullRaw` 一致。
void ProcessBytes(const uint8_t* data, unsigned n)
{
    State& s = S();
    if (!data || !n) return;

    // 落盘（P2-1）：orbtrace 交出来的**就是** OFLOW 帧（`tag + payload + sum`，
    // COBS 编码），所以**原样**写进文件即可 —— 这正是 orbuculum 的 `-o` 干的事
    // （`Src/orbuculum.c:1005`）。解帧前后写都一样，这里写在解帧**之前**，
    // 保证"文件里有的字节 = 探针发的字节"，与解码成功与否无关。
    TraceFileWriteRaw(data, n);

    if (!s.sawData) {
        s.sawData = true;
        ORB_LOG_INFO("orbtrace: 收到第一批 trace 数据 (%u 字节, oflow=%d)",
                     n, s.oflow ? 1 : 0);
    }

    if (s.oflow) {
        s.deframer.Pump(data, (int)n, &OnFrame, nullptr);
    } else {
        OnRawBytes(data, (int)n);
    }

    // 解帧层的错误（COBS 错 / 校验和错）→ 官方 "Data Stream Error"
    // （`DbgCM/Trace.h:149` T_ERR_DATA_ERR）。这两类错误原先只进日志，
    // 状态栏永远看不到 —— 上报链早已移植，缺的就是这个 producer。
    const OrbTraceStats st = OrbTraceGetStats();
    if (st.badSum != s.lastBadSum || st.cobsErrors != s.lastCobsErrors) {
        s.lastBadSum     = st.badSum;
        s.lastCobsErrors = st.cobsErrors;
        orb::agdi::TraceWinErrorSet(orb::agdi::kTErrDataErr);

        // 丢了一帧 = ETM 字节流里有缺口：相对地址的基准断了，解码器接着算
        // 出来的全是假地址（P0-D）。ITM 不受影响 —— 帧边界就是它的包边界。
        EtmTraceForceSync(false);
    }
}

} // namespace

// ===========================================================================
// 能力 / 状态
// ===========================================================================

bool OrbTraceHostReady()
{
    return rddi::IsLoaded() && rddi::HasOrbTrace();
}

bool OrbTraceActive()
{
    return S().active;
}

bool OrbTraceGetInfo(orb::Dap& dap, OrbTraceInfo& out)
{
    memset(&out, 0, sizeof(out));
    out.traceIf   = -1;
    out.powerIf   = -1;
    out.versionIf = -1;

    if (!OrbTraceHostReady()) {
        ORB_LOG_WARN("orbtrace: 宿主未加载或未导出 OrbTrace_* - 探针 trace 源不可用");
        return false;
    }

    const rddi::Api& api = rddi::Get();
    if (!api.OrbTrace_Available || !api.OrbTrace_GetInfo) {
        ORB_LOG_WARN("orbtrace: 宿主缺少入口 - OrbTrace_Available=%p OrbTrace_GetInfo=%p",
                     reinterpret_cast<const void*>(api.OrbTrace_Available),
                     reinterpret_cast<const void*>(api.OrbTrace_GetInfo));
        return false;
    }

    const RDDIHandle h = dap.Handle();

    int avail = 0;
    const int rcAvail = api.OrbTrace_Available(h, &avail);
    if (rcAvail != 0 || !avail) {
        // The three ways this function fails used to be indistinguishable, and
        // all of them printed "no orbtrace device found" - which is wrong when
        // the host itself said no, or when the device is simply claimed by
        // another process.  Report which one it was.
        ORB_LOG_WARN("orbtrace: OrbTrace_Available -> rc=%d avail=%d (handle=%d) - "
                     "宿主没有可用设备（被其它进程占用 / 未枚举 / 需要重插）",
                     rcAvail, avail, static_cast<int>(h));
        return false;
    }

    rddi::RddiOrbTraceInfo raw;
    memset(&raw, 0, sizeof(raw));
    const int rcInfo = api.OrbTrace_GetInfo(h, &raw, (int)sizeof(raw));
    if (rcInfo != 0) {
        ORB_LOG_WARN("orbtrace: OrbTrace_GetInfo -> rc=%d (handle=%d)",
                     rcInfo, static_cast<int>(h));
        return false;
    }

    out.present       = raw.present != 0;
    out.opened        = raw.opened != 0;
    out.vid           = raw.vid;
    out.pid           = raw.pid;
    out.traceIf       = raw.traceIf;
    out.powerIf       = raw.powerIf;
    out.versionIf     = raw.versionIf;
    out.traceProtocol = raw.traceProtocol;
    out.supportsOflow = raw.supportsOflow != 0;

    strncpy_s(out.product, raw.product, _TRUNCATE);
    strncpy_s(out.serial,  raw.serial,  _TRUNCATE);
    strncpy_s(out.version, raw.version, _TRUNCATE);

    return out.present;
}

OrbTraceStats OrbTraceGetStats()
{
    State& s = S();
    OrbTraceStats st = s.stats;
    st.frames      = s.deframer.Frames();
    st.badSum      = s.deframer.BadSum();
    st.cobsErrors  = s.deframer.CobsErrors();
    st.bytes       = s.deframer.BytesIn();
    return st;
}

// ===========================================================================
// 启动 / 停止
// ===========================================================================

bool OrbTraceStart(orb::Dap& dap, int width, int swo, int baud, bool oflow)
{
    State& s = S();
    if (s.active) return true;

    if (!OrbTraceHostReady()) {
        ORB_LOG_WARN("orbtrace: RDDI 宿主没有 OrbTrace_* / TraceStream_* 导出，"
                     "探针 trace 源不可用");
        return false;
    }
    if (!dap.IsConnected()) {
        ORB_LOG_WARN("orbtrace: 未连接目标，trace 源不启动");
        return false;
    }

    const rddi::Api& api = rddi::Get();
    const RDDIHandle h   = dap.Handle();

    // ---- 设备探测 ---------------------------------------------------------
    OrbTraceInfo info;
    if (!OrbTraceGetInfo(dap, info)) {
        ORB_LOG_WARN("orbtrace: 没有找到 orbtrace 设备");
        return false;
    }
    if (!s.logged) {
        s.logged = true;
        ORB_LOG_INFO("orbtrace: %04X:%04X \"%s\" sn=\"%s\" ver=\"%s\" "
                     "traceIf=%d powerIf=%d verIf=%d protocol=0x%02X OFLOW=%d",
                     info.vid, info.pid, info.product, info.serial, info.version,
                     info.traceIf, info.powerIf, info.versionIf,
                     info.traceProtocol, info.supportsOflow ? 1 : 0);
    }

    // 探针自己报告的协议优先于调用方给的值。
    if (info.supportsOflow)      oflow = true;
    else if (info.traceProtocol == 0x01) oflow = false;   // TPIU raw

    // ---- 配置 trace 源（对应 orbuculum OrbtraceIfSet*）--------------------
    const bool isManch = (swo == 2);
    // SWO 的 tpui 标志告诉探针这条流怎么解：
    //   1 = 流是 TPIU 成帧的，探针要做 TPIU 解包（orbuculum 的 -t U）
    //   0 = 流是裸 ITM 字节，探针直接当 ITM 解（-t u）
    // 它**必须与目标端 TPIU_FFCR 的 formatter 状态一致**：FFCR 的 EnFCont(bit0)
    // 若为 0（STM32 上该位写不进去、恒为 0），TPIU 其实没有打包，此时选 1 会让
    // 探针去找 TPIU 同步头（0xFFFFFF7F）而永远找不到 —— 现象就是"目标侧寄存器全对、
    // 探针一个字节都不出"。并口不用这个标志。
    const int  useTPIU = (swo == 0) ? 0 : orb::cfg::Get().trTpui;

    // ---- 打开端点 ---------------------------------------------------------
    // 配置串与上面的显式 setter 表达同一件事；RDDI 层两者都支持，显式 setter
    // 缺失时（旧版宿主）配置串就是唯一的兜底。
    //
    // ⚠ 并行模式（swo == 0）**不能**发 `swo=`：宿主的 _applyTraceConfig 把
    // `width=` 和 `swo=` 都翻译成同一个 "Set Input Format" 请求，后写的赢 ——
    // 于是 `width=4;swo=uart` 的最终格式是 NRZ SWO 而不是 4-bit 并口，
    // 现象就是"流开着、永远 0 字节"。`tpui` 同理只在 SWO 模式下才有意义。
    char cfg[96];
    cfg[0] = '\0';
    if (swo > 0) {
        // ⚠ 顺序同样要紧：宿主把 `baud=` 和 `swo=`/`tpui=` 都落到同一个
        // "Set Input Format" 请求上，后写的赢。格式必须排在最后，否则会被
        // 波特率那一项覆盖回默认档（0x13 = ITM+TPIU），而目标端不打包 TPIU
        // 时 0x13 收不到任何东西。
        _snprintf_s(cfg, sizeof(cfg), _TRUNCATE,
                    "baud=%d;swo=%s;tpui=%d",
                    (baud > 0) ? baud : 0,
                    isManch ? "manchester" : "uart",
                    useTPIU);
    } else if (width > 0) {
        _snprintf_s(cfg, sizeof(cfg), _TRUNCATE, "width=%d", width);
    }

    if (!api.TraceStream_Open) {
        ORB_LOG_WARN("orbtrace: RDDI 宿主没有 TraceStream_Open");
        return false;
    }
    const int rc = api.TraceStream_Open(h, cfg[0] ? cfg : nullptr);
    if (rc != 0) {
        ORB_LOG_WARN("orbtrace: TraceStream_Open 失败 (rc=0x%X, cfg=\"%s\")",
                     rc, cfg);
        return false;
    }

    // ---- 显式配置 trace 源 ------------------------------------------------
    // ⚠ 必须在 TraceStream_Open **之后**。宿主的这些 setter 第一句都是
    //    `if (!St().opened) return -1;` —— 流没打开时它们**全部返回 -1**，
    //    而不看返回值时，日志上和"配置成功"长得一模一样。
    //    原来的顺序是"先配置、后打开"，于是每一个 setter 都在静默失败
    //    （实测返回 8192 = RDDI_DAP_ERROR），整条路只剩配置串在兜底。
    //    配置串能表达格式和波特率，所以并口一直正常；但它表达不了这两者的
    //    先后关系，SWO 就卡在这里。
    if (width > 0) {
        if (api.OrbTrace_SetTraceWidth) {
            ORB_LOG_INFO("orbtrace: SetTraceWidth(%d) -> %d",
                         width, api.OrbTrace_SetTraceWidth(h, width));
        } else {
            ORB_LOG_WARN("orbtrace: 宿主没有 OrbTrace_SetTraceWidth 导出");
        }
    }
    // ⚠ 波特率必须发在 SetTraceSWO **之前**。
    //
    // 宿主的 SetSWOBaudrate 会把探针的 input_format 一并推回默认档
    // （0x13 = ITM+TPIU）。⚠ 原来这里写"目标 STM32 的 TPIU_FFCR.EnFCont 写不进去、
    // 根本不打包"——**这一位读反了**：ARM 的定义是 **0 = formatter 打开**、1 = bypass
    // （见 ORBAGDI_Config.cpp 里 trtpui 的说明）。真正的现象是 0x13 档要等 TPIU 同步头
    // 0xFFFFFF7F，实测等不到，swo_fifo 读侧就此永久停滞 —— 现象是"流开着、永远 0
    // 字节"，且 trace 灯因写侧持续溢出而长亮红灯。之前 SetTraceSWO 在前、波特率在后，
    // 于是 trtpui=0 和 trtpui=1 被覆盖成同一档，两者结果完全一样。
    //
    // 并口模式（swo == 0）必须整个跳过它：并口下 baud 仍是 tclk/swopresc
    // 算出的位时钟（非零），调用它会把刚设好的 4-bit 并口覆盖成 SWO，
    // 症状同样是"流开着、永远 0 字节"（日志顺序即证据：
    // SetTraceWidth(4) -> 0 / SetSWOBaudrate(25000000) -> 0 / 读路径全程空）。
    if (swo > 0 && baud > 0) {
        if (api.OrbTrace_SetSWOBaudrate) {
            ORB_LOG_INFO("orbtrace: SetSWOBaudrate(%d) -> %d",
                         baud, api.OrbTrace_SetSWOBaudrate(h, baud));
        } else {
            ORB_LOG_WARN("orbtrace: 宿主没有 OrbTrace_SetSWOBaudrate 导出");
        }
    }
    // 格式最后落定，之后不能再被任何 setter 覆盖。
    if (swo > 0) {
        if (api.OrbTrace_SetTraceSWO) {
            ORB_LOG_INFO("orbtrace: SetTraceSWO(manch=%d, tpui=%d) -> %d",
                         isManch ? 1 : 0, useTPIU,
                         api.OrbTrace_SetTraceSWO(h, isManch ? 1 : 0, useTPIU));
        } else {
            ORB_LOG_WARN("orbtrace: 宿主没有 OrbTrace_SetTraceSWO 导出");
        }
    }

    // ---- 解码器就位 -------------------------------------------------------
    s.deframer.Reset();
    s.stats  = OrbTraceStats();
    s.reads  = 0;
    s.noData = 0;
    s.errors = 0;
    s.lastBadSum     = 0;
    s.lastCobsErrors = 0;
    s.lastDropped    = 0;
    s.sawData = false;
    s.oflow = oflow;
    s.handle = h;

    // ITM 解码器放到已知的流边界（同 ETB 路径的理由）。
    ItmResyncDecoder();
    // ETM sink 也从头开始。
    EtmTraceReset();

    s.active = true;
    ORB_LOG_INFO("orbtrace: trace 流已打开 (width=%d swo=%d baud=%d oflow=%d)",
                 width, swo, baud, oflow ? 1 : 0);
    return true;
}

void OrbTraceStop(orb::Dap& dap)
{
    State& s = S();
    if (!s.active) {
        // 也打统计：泵可能已经因为读错误自己停了，那时这里是唯一能看到
        // "到底收了多少字节/帧"的地方 —— 否则现象只剩"一片安静"。
        const OrbTraceStats st0 = OrbTraceGetStats();
        ORB_LOG_INFO("orbtrace: trace 流不在采集 (字节=%llu 帧=%llu ITM=%llu ETM=%llu)",
                     st0.bytes, st0.frames, st0.itmBytes, st0.etmBytes);
        return;
    }
    s.active = false;

    if (rddi::IsLoaded() && rddi::Get().TraceStream_Close) {
        rddi::Get().TraceStream_Close(dap.Handle());
    }

    const OrbTraceStats st = OrbTraceGetStats();
    ORB_LOG_INFO("orbtrace: trace 流已关闭 (字节=%llu 帧=%llu 校验错=%llu "
                 "COBS错=%llu ITM=%llu ETM=%llu 其它=%llu)",
                 st.bytes, st.frames, st.badSum, st.cobsErrors,
                 st.itmBytes, st.etmBytes, st.otherFrames);

    // 不认识的 tag 的直方图。**"其它=N 帧"看不出丢了多少字节** —— 实测一次
    // 10 s 采集里 tag 6 是 54,823 帧 x 1024 B = 56 MB（占载荷 98.6%），而
    // tag 2 只有 1,850 B。按字节数从大到小列前 8 个（只列有载荷的）。
    if (st.otherFrames) {
        bool used[256];
        memset(used, 0, sizeof(used));
        char line[512];
        int  n = 0;
        for (int shown = 0; shown < 8; ++shown) {
            int      best  = -1;
            unsigned long long bestB = 0;
            for (int t = 0; t < 256; ++t) {
                if (used[t] || !s.tagFrames[t]) continue;
                if (s.tagBytes[t] > bestB) { bestB = s.tagBytes[t]; best = t; }
            }
            if (best < 0) break;
            used[best] = true;
            // 格式串保持 ASCII：窄字符串字面量里的非 ASCII 会让 cl 报 C4819
            // （"该文件包含不能在当前代码页中表示的字符"，行号指在这一行）。
            // 注释里的中文不触发，所以这里只有这一处要写成英文。
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                             "%stag %d: %u frames / %llu B",
                             (n ? "  " : ""), best, s.tagFrames[best], s.tagBytes[best]);
            if (n < 0) break;
        }
        if (n > 0) {
            ORB_LOG_WARN("orbtrace: 丢弃的 tag（只认 1=ITM / 2=ETM）—— %s", line);
        }
    }

    // "0 字节"的三种成因要靠这三个数分开，否则只剩一句"没数据"没法查。
    // 第四个数是**本层丢的**：字节数偏少时，它是"探针没发"与"环满丢了"的分界。
    unsigned long long dropped = 0;
    const rddi::Api& api = rddi::Get();
    if (api.TraceStream_DroppedBytes) {
        api.TraceStream_DroppedBytes(dap.Handle(), &dropped);
    }
    ORB_LOG_INFO("orbtrace: 读路径 调用=%llu 空=%llu 错=%llu 环溢出丢帧=%llu B",
                 s.reads, s.noData, s.errors, dropped);

    // 时间轴（P0-F）的现场证据。orbtrace 的 tag=1 流走的是**同一个** ITM 解码器，
    // 所以计数与 SWO 路径共用一份 —— 只是这条路径不经过 `ItmStop`，得自己打。
    ItmLogTsStats("orbtrace");

    // 解码结果 —— 上面那些字节数只说明"收到了字节"，这两行才说明"解出了东西"。
    // 字节很多而这里为 0，就说明解码器没同步上（例如 ETM 的 ATB ID 撞号时
    // 会看到大量 ETM 字节但一条分支都解不出来）。
    ORB_LOG_INFO("orbtrace: 解码结果  ITM→DWT PC 采样=%u 条, ETM→分支=%u 条",
                 HistCount(), EtmTraceBranchCount());
    // ETM 解码器自己的汇总（协议 / 同步状态 / 各类事件计数）。
    // 整个采集段只这一条 —— 见 ORBAGDI_Etm.h 里关于热路径的说明。
    EtmTraceLogSummary("orbtrace");
    if (s.reads > 0 && s.noData == s.reads) {
        ORB_LOG_INFO("orbtrace: 端点全程为空 —— 探针没有在发数据。目标侧已由本层"
                     "编程（见 'ITM: target trace ->' 一行），所以往'探针输入格式/"
                     "物理接线'方向查；注意探针固件的超帧边界要攒够 750 万字节"
                     "（orbflow.SuperFramer 的 interval 单位是字节），低速率下可能"
                     "很久都不出现边界。");
    }

    s.deframer.Reset();
    s.handle = 0;
}

// ===========================================================================
// 采集：抽（锁内） / 解（锁外）
// ===========================================================================

unsigned OrbTracePullRaw(orb::Dap& dap, uint8_t* out, unsigned cap, unsigned timeMs)
{
    State& s = S();
    if (!s.active || !out || cap == 0) return 0;

    const rddi::Api& api = rddi::Get();
    if (!api.TraceStream_Read) return 0;

    const RDDIHandle h = dap.Handle();

    // 数据由 RDDI 侧的异步管线搬进环形缓冲（32 个常驻 overlapped 读 × 64 KB，
    // 见 orbrddi-main/src/ORBMDK_OrbTrace.cpp），所以这里只是从环里 memcpy：
    // 超时用 **0（非阻塞）** —— 环空即返回 0x204，不阻塞调用线程。
    // 参数对照与三处修的完整记录见 COMPAT_ANALYSIS.md §10.22 / §10.23。
    //
    // **一次读的单位就是 64 KB**（RDDI 单个传输块）。`cap` 不足一整块时这里
    // **不读**：把半块留在手里会打乱顺序，环里的数据下一片再来取。
    const unsigned kChunk = 64u * 1024u;

    // 时间预算，不是字节上限（官方 `Trace_Read(time, ts)` 同口径）。原先的
    // `round < 32` 是隐含的 2 MB 上限，与链路速率无关 —— 换成 USB3/USB4 时它会
    // 先成为硬天花板（Todo.md §六 C2）。
    if (timeMs == 0) timeMs = 1;
    const DWORD t0 = GetTickCount();

    unsigned total = 0;

    for (;;) {
        if ((cap - total) < kChunk) break;                     // 装不下下一整块
        if (total && (GetTickCount() - t0) >= timeMs) break;   // 预算用完（至少读一次）

        int n  = (int)kChunk;
        const int rc = api.TraceStream_Read(h, &n, out + total, 0);   // 0 = 非阻塞（环里取）
        ++s.reads;

        if (rc == kNoData) { ++s.noData; break; }   // 端点空了
        if (rc != 0) {
            ++s.errors;
            // 读本身失败 = 通信问题，对应官方的 "HW Communication Error"
            // （`DbgCM/Trace.h:148` T_ERR_HW_COM）。
            orb::agdi::TraceWinErrorSet(orb::agdi::kTErrHwCom);
            ORB_LOG_WARN("orbtrace: TraceStream_Read 失败 (rc=0x%X)，停止采集", rc);
            s.active = false;
            break;
        }
        if (n <= 0) break;

        total += (unsigned)n;
        if ((unsigned)n < kChunk) break;                       // 环已经排空
    }

    // RDDI 环溢出（可选导出，`TraceStream_DroppedBytes`）。**这是"静默丢数据"
    // 唯一的出口**：只报"读到了多少字节"的话，少掉的那部分既可能是探针没发、
    // 也可能是本层环满丢的，两者分不开。丢帧置 `T_ERR_DATA_OVF`
    // （= 官方的 "Data Overflow"，`DbgCM/Trace.h:134`），由 `TraceWinRead` 折叠
    // 成状态栏那一行。
    if (api.TraceStream_DroppedBytes) {
        unsigned long long dropped = 0;
        if (api.TraceStream_DroppedBytes(h, &dropped) == 0 && dropped > s.lastDropped) {
            const unsigned long long delta = dropped - s.lastDropped;
            s.lastDropped = dropped;
            ORB_LOG_WARN("orbtrace: RDDI 环溢出，丢了 %llu 字节（累计 %llu）—— "
                         "抽数跟不上，记录里会有缺段", delta, dropped);
            orb::agdi::TraceWinErrorSet(orb::agdi::kTErrDataOvf);

            // P0-D：环回绕 ⇒ 任何关于同步状态的猜测都失效。ETM 的分支地址是
            // 相对的，缺口之后解出来的地址是假的；强制重新同步，等到下一个
            // A-SYNC/ISYNC 才恢复上报。这就是 orbuculum `orbmortem.c:1084` 那一段。
            EtmTraceForceSync(false);
        }
    }
    return total;
}

void OrbTraceDecodeRaw(const uint8_t* data, unsigned n)
{
    ProcessBytes(data, n);
}

unsigned OrbTracePump(orb::Dap& dap, unsigned timeMs)
{
    State& s = S();
    if (!s.active) return 0;

    // 抽 + 解一体，**只给调试线程的一次性收尾用**（停机那 5 ms，调用方自己持锁，
    // 同步做完即可）。抽数 worker 不走这条 —— 它用 PullRaw / DecodeRaw 分开，
    // 让锁只覆盖 memcpy（见头文件的"抽 / 解 分离"）。
    static uint8_t buf[64 * 1024];

    if (timeMs == 0) timeMs = 1;
    const DWORD t0 = GetTickCount();

    unsigned total = 0;
    for (;;) {
        const unsigned n = OrbTracePullRaw(dap, buf, (unsigned)sizeof(buf), timeMs);
        if (!n) break;
        total += n;
        OrbTraceDecodeRaw(buf, n);
        if (n < sizeof(buf)) break;                             // 环已排空
        if ((GetTickCount() - t0) >= timeMs) break;             // 预算用完
    }
    return total;
}

} // namespace agdi
} // namespace orb
