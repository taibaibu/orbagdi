/**
 * @file ORBAGDI_OrbTrace.cpp
 * @brief orbtrace 探针的 trace 采集源（RDDI `OrbTrace_*` / `TraceStream_*`）。
 *
 * 数据链：TraceStream_Read → OflowDeframer（COBS + OFLOW）→ tag1(ITM)→ItmFeedBytes、
 * tag2(ETM)→EtmTraceFeed。与 SWO 的差别：orbtrace 每帧自带边界与校验，ITM 可直接当同步点
 * （ItmResyncDecoder 就是为这个）；ETM 分支地址是相对的，必须等 A-SYNC/ISYNC，没有对应开关。
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

    // 读路径诊断。区分三种"0 字节"：reads 涨且 noData==reads → 探针没在发；errors 涨 → 读本身
    // 出错、不是"没数据"；reads == 0 → 泵根本没跑。
    unsigned long long reads;
    unsigned long long noData;
    unsigned long long errors;

    unsigned framesLogged;   // 前几帧打 hex，用于确认通道号与内容

    // 上一次看到的解帧错误计数。两者一变就置 `T_ERR_DATA_ERR`（= 官方的
    // "Data Stream Error"）—— 解帧层原先只写日志，状态栏永远看不到。
    unsigned long long lastBadSum;
    unsigned long long lastCobsErrors;

    // 本次开流时 BytesIn() 的值。解帧器的计数与"出错帧绝对偏移"都是跨会话累计
    // 的（Reset() 刻意保留统计），日志要报的是**本次会话**里第几个字节出错，
    // 所以减去这个基准。
    unsigned long long errBase;

    // 上一次看到的 RDDI 环丢帧字节数。一变就置 `T_ERR_DATA_OVF`
    // （= 官方的 "Data Overflow"）。
    unsigned long long lastDropped;

    // cache 的累计 QPC 计数。存**原始计数**、只在 OrbTraceGetStats() 里换算一次：
    // 单次 fwrite 常常远不到 1 ms，每块都换算成毫秒会因取整把误差累加成假数。
    unsigned long long writeTicks;

    // 不认识的 tag 的直方图（按 tag 索引）。只数帧数看不出丢了**多少字节** ——
    // 实测 tag 6 一次就是 56 MB，而"其它=55663 帧"读起来像几帧噪声。
    unsigned long long tagBytes[256];
    unsigned           tagFrames[256];

    // ---- 解码器状态的互斥 ---------------------------------------------------
    // 抽 / 解拆成两个线程之后，解码器（deframer + ETM 引擎 + P1-0 补全）仍有第二个
    // 入口：停机收尾时调试线程会走 OrbTracePump()。原来两边"恰好"靠挂起握手错开，
    // 但那只是时序假设；这里显式加锁，粒度为"一块字节"，开销可忽略。
    CRITICAL_SECTION decodeCs;

    // 非解码线程请求"下一次解码前强制 ETM 重同步"。ETM 是相对地址，字节流一旦断开
    // （RDDI 环溢出 / 本层丢块），此后解出的地址全是假的 —— 所以这个请求必须由
    // 解码线程在持 decodeCs 时执行，不能由请求方直接改引擎状态。
    volatile LONG resyncReq;

    // stats() 里漏一个成员就是未初始化值：停止路径在流从没打开过时也会打印这些计数（曾打出
    // 4.5e17 的 ITM 条数），只有 OrbTraceStart() 走过才会被清零。
    State() : active(false), oflow(true), handle(0), stats(), logged(false),
              sawData(false), reads(0), noData(0), errors(0), framesLogged(0),
              lastBadSum(0), lastCobsErrors(0), errBase(0), lastDropped(0),
              writeTicks(0), resyncReq(0)
    {
        memset(tagBytes,  0, sizeof(tagBytes));
        memset(tagFrames, 0, sizeof(tagFrames));
        InitializeCriticalSection(&decodeCs);
    }
};

State& S()
{
    // 故意泄漏（同 ORBAGDI_Itm.cpp 的说明）。
    static State* s = new State();
    return *s;
}

// cache 计时用的时钟。只累计原始计数，见 `State::writeTicks` 的说明。
unsigned long long QpcTicks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<unsigned long long>(t.QuadPart);
}

double QpcTicksToMs(unsigned long long ticks)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (f.QuadPart > 0)
         ? (static_cast<double>(ticks) * 1000.0 / static_cast<double>(f.QuadPart))
         : 0.0;
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
        char   hex[3 * 40 + 4] = {0};
        size_t n = 0;
        const unsigned show = (f.len < 40) ? f.len : 40;
        for (unsigned i = 0; i < show; ++i) {
            orb::fmt::Append(hex, sizeof(hex), n, "%02X ", f.data[i]);
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
        // 不能在这里"强制同步"：ETM 的分支地址是相对的，必须等 A-SYNC / ISYNC 才有基准地址
        // （见 ORBAGDI_Etm.h）。帧边界不等于同步点 —— ITM 里两者恰好重合，ETM 里不重合。
        EtmTraceFeed(f.data, (size_t)f.len);
        break;

    default:
        // 不认识的 tag 也要按 tag 记账：只数"其它=N 帧"看不出丢了多少字节。实测 10 s 采集里
        // tag 6 有 54,823 帧 × 1024 B = 56 MB（占载荷 98.6%），而 tag 2 只有 1,850 B。直方图在关流时打。
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

// 解释一块原始字节：cache → 解帧 → 解码 → 错误折叠。
// 完全不碰探针（代码镜像兜底读默认关闭），所以可以、也必须放在探针锁之外 —— 见头文件"抽/解 分离"。
// 调用者保证：同一块只喂一次、顺序与 OrbTracePullRaw 一致。
void ProcessBytes(const uint8_t* data, unsigned n)
{
    State& s = S();
    if (!data || !n) return;

    // 解码器状态（deframer / ETM 引擎 / P1-0 补全）归持有这把锁的线程所有。
    // 粒度是"一块字节"，相对 64 KB 的 memcpy 与解码成本可忽略。
    EnterCriticalSection(&s.decodeCs);

    // 别的线程请求的重同步（抽数线程发现 RDDI 环溢出、或本层队列丢块）在这里执行 ——
    // 此刻引擎状态才归本线程所有，直接改会和正在跑的 ProcessBytes 撞。
    if (InterlockedExchange(&s.resyncReq, 0) != 0) {
        EtmTraceForceSync(false);
        // 缺口之后解帧器自己也是错位的：让它丢到下一个 0x00 再开始，
        // 否则它会带着半帧继续解，把"下一次对齐"变成又一条 COBS 错。
        s.deframer.DropToDelimiter();
    }

    // cache（P2-1）：orbtrace 交出来的就是 OFLOW 帧（tag + payload + sum，COBS 编码），原样写入
    // 即可（orbuculum 的 -o 就这么做）。写在解帧之前，保证"文件里的字节 = 探针发的字节"。
    const unsigned long long w0 = QpcTicks();
    TraceFileWriteRaw(data, n);
    s.writeTicks += QpcTicks() - w0;

    if (!s.sawData) {
        s.sawData = true;
        ORB_LOG_INFO("orbtrace: 收到第一批 trace 数据 (%u 字节, oflow=%d)",
                     n, s.oflow ? 1 : 0);
    }

    // 本块在"本次会话字节流"里的起点（Pump 之前取，Pump 会加 n）。报错时用它把
    // 出错帧的绝对偏移换算成块内下标，才能把现场那几个字节打出来。
    const unsigned long long blkBase = s.deframer.BytesIn() - s.errBase;

    if (s.oflow) {
            // ⚠ 切片推进，不能一次 Pump 整块（实测一块 64 KB）。
            // 解帧错误（COBS 错 / 校验和错）说明"这个位置起字节不可信"，而 ETM 的地址是
            // **相对**编码：错误点之后的字节若继续当连续流解，会解出一串假地址 ——
            // 日志里的 `ETM: 解出的地址 0x01033080 不在任何常见映射区` 就是它们。
            // 错误检测原先在整块**解完之后**：最多晚 64 KB 才发现，那一整段假地址已经
            // 进了 Trace 窗口和覆盖率段（还会替 0x0103xxxx 这种地址白建 64 KB 段）。
            // 切 512 B 后检测点前移到出错的那一小片：失步次数不变，假地址最多只来自
            // 出错那一片，而不是整块。
        const unsigned kSlice = 512;
        unsigned off = 0;
        while (off < n) {
            const unsigned take = (n - off < kSlice) ? (n - off) : kSlice;
            s.deframer.Pump(data + off, static_cast<int>(take), &OnFrame, nullptr);
            off += take;

            const unsigned long long bs = s.deframer.BadSum();
            const unsigned long long ce = s.deframer.CobsErrors();
            if (bs != s.lastBadSum || ce != s.lastCobsErrors) {
                const unsigned long long dCobs = ce - s.lastCobsErrors;
                const unsigned long long dSum  = bs - s.lastBadSum;
                s.lastBadSum     = bs;
                s.lastCobsErrors = ce;

                    // 把这次缺口**说出来**。只计数的话用户看到的只是"地址比预期少"，
                    // 没法把窗口里的野地址和丢帧对上号；偏移是本块内的，配合关闭时
                    // 那行的累计值就能数清"第几次缺口、在哪儿"。
                // 偏移用解帧器逐帧记的"出错帧起点"，不再用切片边界 `off`：
                // off 永远是 512 的整数倍（实测 512、512、131584），指着检测时机
                // 而不是坏字节，照它去数流里的字节会数错一整段。
                const unsigned long long at    = s.deframer.LastErrorOffset();
                const unsigned long long relAt = (at >= s.errBase) ? (at - s.errBase) : 0;
                ORB_LOG_WARN("orbtrace: 解帧错 @ 流偏移 %llu（本块 %u/%u，COBS=%llu 校验=%llu，"
                             "本次 +%llu）—— ETM 从这一点起失步，缺口之后的字节不再"
                             "按相对地址解，丢弃数见 'ETM: 停止' 那行的 丢弃=",
                             relAt, off, n, ce, bs, dCobs + dSum);

                // 现场字节：出错帧起点前后各 16 字节。这是**唯一**能在不落盘的前提下
                // 看到"坏字节长什么样"的地方 —— 判断是丢了一字节（后续整体错位）还是
                // 某几个字节被改（位翻转），全靠这段。只在错误发生时打一条。
                if (relAt >= blkBase && (relAt - blkBase) < n) {
                    const unsigned pos = static_cast<unsigned>(relAt - blkBase);
                    const unsigned lo  = (pos > 16) ? (pos - 16) : 0;
                    unsigned       hi  = pos + 16;
                    if (hi > n) hi = n;
                    char   hex[3 * 34 + 4];
                    size_t hl = 0;
                    hex[0] = '\0';
                    for (unsigned k = lo; k < hi; ++k) {
                        if (k == pos) orb::fmt::Append(hex, sizeof(hex), hl, "| ");
                        orb::fmt::Append(hex, sizeof(hex), hl, "%02X ", data[k]);
                    }
                    ORB_LOG_WARN("orbtrace: 出错帧起点的现场字节（块内 %u，自 %u 起，"
                                 "| 处即出错帧起点）: %s",
                                 pos, lo, hex);
                }

                    // 解帧层的错误（COBS 错 / 校验和错）→ 官方 "Data Stream Error"
                    // （`DbgCM/Trace.h:149` T_ERR_DATA_ERR）。这两类错误原先只进日志，
                    // 状态栏永远看不到 —— 上报链早已移植，缺的就是这个 producer。
                orb::agdi::TraceWinErrorSet(orb::agdi::kTErrDataErr);

                    // 丢了一帧 = ETM 字节流里有缺口：相对地址的基准断了，解码器接着算
                    // 出来的全是假地址（P0-D）。ITM 不受影响 —— 帧边界就是它的包边界。
                EtmTraceForceSync(false);
                    // 同理：错误点之后直到下一个 0x00 都不可信，先对齐再解。
                s.deframer.DropToDelimiter();
            }
        }
    } else {
        OnRawBytes(data, (int)n);
    }

    LeaveCriticalSection(&s.decodeCs);
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
        // 三种失败原来都打 "no orbtrace device found"，宿主自己拒绝、设备被别的进程占用时是错的。
        // 分别报出来，免得按错的方向查。
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
    st.dropped     = s.lastDropped;
    st.writeMs     = static_cast<unsigned long long>(QpcTicksToMs(s.writeTicks));
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
    // SWO 的 tpui 标志告诉探针怎么解这条流：1 = TPIU 成帧（探针先解包），0 = 裸 ITM 字节。
    // 必须与目标 TPIU_FFCR 的 formatter 状态一致：选错时探针去找同步头 0xFFFFFF7F 永远找不到
    // ⇒ 目标侧寄存器全对、探针一个字节都不出。并口不用这个标志。
    const int  useTPIU = (swo == 0) ? 0 : orb::cfg::Get().trTpui;

    // ---- 打开端点 ----
    // 配置串与显式 setter 表达同一件事，setter 缺失时（旧版宿主）配置串是唯一兜底。
    // ⚠ 并行模式（swo == 0）不能发 swo=：width= 与 swo= 落到同一个 "Set Input Format" 请求上，
    // 后写的赢 ⇒ width=4;swo=uart 变成 NRZ SWO，"流开着、永远 0 字节"。tpui 同理。
    char cfg[96];
    cfg[0] = '\0';
    if (swo > 0) {
        // ⚠ 顺序同样要紧：baud= 与 swo=/tpui= 落到同一个 "Set Input Format" 请求上，后写的赢。
        // 格式必须排在最后，否则被波特率那项覆盖回默认档 0x13（ITM+TPIU），目标不打包就收不到东西。
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

    // ---- 显式配置 trace 源：必须在 TraceStream_Open 之后 ----
    // 宿主这些 setter 第一句都是 if (!opened) return -1，流没开时全部静默返回 8192，日志上和
    // 成功一样。原来"先配置、后打开"，整条路只剩配置串兜底；而配置串表达不了先后，SWO 卡在这。
    if (width > 0) {
        if (api.OrbTrace_SetTraceWidth) {
            ORB_LOG_INFO("orbtrace: SetTraceWidth(%d) -> %d",
                         width, api.OrbTrace_SetTraceWidth(h, width));
        } else {
            ORB_LOG_WARN("orbtrace: 宿主没有 OrbTrace_SetTraceWidth 导出");
        }
    }
    // ⚠ 波特率必须发在 SetTraceSWO 之前：SetSWOBaudrate 会把 input_format 推回默认档 0x13
    // （ITM+TPIU），而 FFCR.EnFCont = 0 才是 formatter 打开；0x13 等不到同步头 0xFFFFFF7F，
    // swo_fifo 读侧永久停滞 ⇒"流开着、永远 0 字节"。原来顺序反了，trtpui=0/1 被覆盖成同一档。
    // 并口模式（swo == 0）整个跳过：并口下 baud 是位时钟，调用它会把 4-bit 并口覆盖成 SWO。
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
    // ⚠ 开流那一刻探针里可能正卡着上一段会话的半帧（退出调试后目标还在跑，ETM
    //   没关）。不清到定界符就开解，会把半帧尾巴当 interval count，一路错到撞出
    //   一次 COBS 错为止 —— 日志里连接后那条 `解帧错 ... COBS=1` 就是它，代价是
    //   ETM 白白失步一段。
    s.deframer.DropToDelimiter();
    s.stats  = OrbTraceStats();
    s.reads  = 0;
    s.noData = 0;
    s.errors = 0;
    // ⚠ 基准取解帧器的**当前**累计值，不能写 0：deframer 的计数跨会话保留
    //   （Reset() 刻意不动它们，见 Oflow.cpp 的说明），写 0 等于把上一场会话的错误数
    //   算进本次的增量 —— 日志里出现过 `COBS=2 ... 本次 +2`、`COBS=3 ... 本次 +3`
    //   这种"增量等于累计"的形状，看着像一次错了两次。
    s.lastBadSum     = s.deframer.BadSum();
    s.lastCobsErrors = s.deframer.CobsErrors();
    s.errBase        = s.deframer.BytesIn();
    s.lastDropped    = 0;
    s.writeTicks     = 0;
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
        char   line[512] = {0};
        size_t n = 0;
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
            orb::fmt::Append(line, sizeof(line), n,
                             "%stag %d: %u frames / %llu B",
                             (n ? "  " : ""), best, s.tagFrames[best], s.tagBytes[best]);
        }
        if (n) {
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

    // 数据由 RDDI 侧异步管线搬进环形缓冲（32 × 64 KB overlapped 读），这里只是 memcpy。
    // 超时用 0（非阻塞）：环空即返回 0x204。一次读的单位是 64 KB，cap 不足一整块则不读 ——
    // 半块留在手里会打乱顺序，下一片再来取。
    const unsigned kChunk = 64u * 1024u;

    // 时间预算，不是字节上限（官方 `Trace_Read(time, ts)` 同口径）。原先的
    // `round < 32` 是隐含的 2 MB 上限，与链路速率无关 —— 换成 USB3/USB4 时它会
    // 先成为硬天花板（Todo.md §六 C2）。
    if (timeMs == 0) timeMs = 1;
    const uint64_t t0 = GetTickCount64();

    unsigned total = 0;

    for (;;) {
        if ((cap - total) < kChunk) break;                     // 装不下下一整块
        if (total && (GetTickCount64() - t0) >= timeMs) break;   // 预算用完（至少读一次）

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

    // RDDI 环溢出（可选导出 TraceStream_DroppedBytes）—— "静默丢数据"的唯一出口：
    // 只报读到多少字节时，分不开"探针没发"和"本层环满丢"。丢帧置 T_ERR_DATA_OVF，
    // 由 TraceWinRead 折叠成状态栏那一行。
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
            // ⚠ 本函数在抽数线程上：这里只**请求**，真正的 ForceSync 由解码线程在
            // 下一块开始时执行（ProcessBytes 持解码锁时），见 OrbTraceRequestResync()。
            InterlockedExchange(&s.resyncReq, 1);
        }
    }
    return total;
}

void OrbTraceDecodeRaw(const uint8_t* data, unsigned n)
{
    ProcessBytes(data, n);
}

void OrbTraceRequestResync()
{
    InterlockedExchange(&S().resyncReq, 1);
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
    const uint64_t t0 = GetTickCount64();

    unsigned total = 0;
    for (;;) {
        const unsigned n = OrbTracePullRaw(dap, buf, (unsigned)sizeof(buf), timeMs);
        if (!n) break;
        total += n;
        OrbTraceDecodeRaw(buf, n);
        if (n < sizeof(buf)) break;                             // 环已排空
        if ((GetTickCount64() - t0) >= timeMs) break;             // 预算用完
    }
    return total;
}

} // namespace agdi
} // namespace orb
