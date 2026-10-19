/*
 * etm_replay - 离线复现 ETM 解码问题（**不需要 Keil、不需要目标板、不需要探针**）
 * ============================================================================
 *
 * 为什么要有它：真机跑一次要开 uVision、连探针、等那个 "Cannot access target
 * hardware" 恢复框，太慢（用户原话："不要再 keil 上，运行一次太慢"）。
 * 而"解出垃圾地址"这件事**只需要一段字节流**就能复现 —— 那段流我们已经落盘了：
 * `trfile=` 的 OFLOW 文件，或 `%TEMP%` 里的 `ORB_trace_*.orb`（注意 `%TEMP%` 里
 * 那个 `ORB_rec_*.trc` 是**紧凑记录**、不是 OFLOW，喂不进这里）。
 *
 * 它做什么：
 *   1. 读 OFLOW 文件（签名 + COBS 解帧）——**用 orbuculum 自己的 `cobs.c`**，
 *      与 `ofl_read.c` 同一条独立通路，所以帧层不会和驱动一起错；
 *   2. 把 `tag=2`（ETM）的载荷喂进**与驱动同一个**解码器（`traceDec/`，orbuculum），
 *      连调用形态都照 `ORBAGDI_Etm.cpp` 的 `OnReport` 写（`changeRecord` 位 + 清零）；
 *   3. 把解出来的地址打出来，标出"不在任何常见映射区"的那些，**并给出它们出现在
 *      第几个 ETM 字节** —— 这样才能反查是哪一帧把流带偏的。
 *
 * ⚠⚠ **本工具最有价值的一点：可以离线 A/B 分支地址编码**（`--alt 0|1`）。
 *    同一个文件跑两遍，哪一遍地址正常就是哪一遍对 —— 比在真机上改选项串重跑
 *    快几个数量级。2026-10-14 的现场就是这样：真机日志里
 *    `0x080183B2` 被解成 `0xA1A183B2` / `0xC7818392`（**低两字节正确、高两字节垃圾**），
 *    那是"绝对地址基准（ISYNC）丢了"的症状，而这个工具能在几秒内判定。
 *
 * ⚠ "越界"只是一个**只读诊断**判据，**不是过滤**：地址白名单在驱动里已被用户
 *   明确否决（"芯片有大量外设区，难以分辨"，Todo §7.3）。本工具也不改任何行为。
 *
 * 用法：
 *     etm_replay.exe <file.orb> [--alt 0|1] [--wild N] [--dump N]
 *
 *     --alt  N   分支地址编码：0 = 标准(STD)，1 = 备用(ALT)。默认 **1**（与驱动默认一致）
 *     --wild N   最多打印多少条越界地址（默认 8）
 *     --dump N   顺便打印前 N 个 ETM 帧的头几个字节
 *     --code lo:hi   **合法指令地址**的区间（十六进制，默认 08000000:0801D9F8 ——
 *                从 map 量出的代码段：Code = 121,384 B）。判据必须是**代码区间**：
 *                "落在某个映射区里"对指令地址是废话（外设区、内核私有区、镜像里代码
 *                之后的 RO 数据区都在映射区内，却都不可能是指令地址）。
 *     --isy  N   打开**解码器内部的 DEBUG 上报**（A-Sync / ISYNC / Branch to …），
 *                每条带 ETM 字节偏移，N = 最多打多少条（默认 200）。
 *                ⚠ 这是判 §13.3「ISYNC 被错过、基准停在垃圾值」的**唯一**手段：
 *                  基准就是 ISYNC 的 4 个原始字节（解码器里零校验），只有把
 *                  "ISYNC 载入了什么"与"随后解出了什么"对齐到**同一个字节偏移**，
 *                  才看得出基准是从哪一条开始错的、以及错之前有没有漏过 ISYNC。
 *
 * 退出码：0 = 没解出越界地址；1 = 有（就是那个 bug）；2 = 参数 / 文件问题。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

// orbuculum 的 COBS（C）。它是 C 代码，从 C++ 里链要 extern "C"。
extern "C" {
#include "cobs.h"
}

// orbuculum 的 trace 解码器（traceDec/，BSD-3-Clause）。**它自带 extern "C"**，
// 所以这里不要再包一层（包了也不报错，但没必要）。
#include "traceDecoder.h"

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
static const char kSig[] = "%%ORBFLOW1.0.0%%";

static unsigned long long gFrames[256];
static unsigned long long gBytes[256];
static unsigned long long gBadSum = 0;
static unsigned long long gShort  = 0;

// ETM 侧
static struct TRACEDecoder gDec;
static unsigned long long  gEtmBytes = 0;   // 已喂进解码器的 ETM 载荷字节
static unsigned long long  gAddrs    = 0;
static unsigned long long  gAtoms    = 0;
static unsigned long long  gExc      = 0;
static unsigned long long  gTs       = 0;
static unsigned long long  gCyc      = 0;
static unsigned long long  gWild     = 0;   // 越界地址条数
static unsigned long long  gTopHist[16];
static uint32_t            gFirst = 0, gLast = 0, gPrev = 0;
static bool                gHaveAny = false;
static unsigned long long  gWildAtEtmByte = 0;  // 第一条越界出现时的 ETM 字节偏移
static uint32_t            gFirstWild = 0, gPrevOfFirstWild = 0;

static int gWildWant = 8;
static int gDumpLeft = 0;

// ---------------------------------------------------------------------------
// 合法判据（--code lo:hi）—— **代码区间**，不是"有没有映射"
// ---------------------------------------------------------------------------
// ⚠⚠ 文件下方那条 Plausible() 只回答"这个地址有没有映射"，对**指令地址**来说那是**废话**：
//     外设区、内核私有区、以及镜像里**代码之后的 RO 数据区**都在映射区内，却都不可能
//     是指令地址。2026-10-14 实测就栽在这里：一整段 `0x08018xxx..0x0801Dxxx` 被判"干净"，
//     而它们的**低位是对的、高位被替换过**（见 OnReport 里的 LOW20-SAME 判据）。
//
// 默认值从 map 量出：`Code = 121,384 B` ⇒ 代码上界 `0x08000000 + 0x1D9F8`。
//   日志旁证：P1-0 在 `0x0801D9D0` 读到 `FE E7`（= `B .`）⇒ 代码确实到那里。
static uint32_t gCodeLo = 0x08000000u;
static uint32_t gCodeHi = 0x0801D9F8u;

static bool InCode(uint32_t a) { return a >= gCodeLo && a < gCodeHi; }

// 统计：不在代码区间 / 其中连映射区都不在 / 其中"低 20 位与上一条好地址相同"
static unsigned long long gOutCode          = 0;
static unsigned long long gOutCodeMapped    = 0;
static unsigned long long gOutCodeSameLow   = 0;
static unsigned long long gOutCodeFirstByte = 0;   // 第一条不在代码区间的 ETM 字节偏移
static uint32_t           gOutCodeFirst     = 0;
static uint32_t           gOutCodeFirstPrev = 0;

// 上一条**在代码区间内**的地址 —— 判"高位被替换"就靠它。
static uint32_t gLastGood = 0;
static bool     gHaveGood = false;

// ---------------------------------------------------------------------------
// 解码器内部上报（--isy）
// ---------------------------------------------------------------------------
// `TRACEDecoderInit` 的第 4 个参数就是它，解码器里 `DEBUG(...)` 全靠它
// （`if (cpu->report)`，传 NULL 时零开销 ⇒ 平时不接）。
// 每条带 **ETM 字节偏移** —— 这是判 §13.3 的关键：把"ISYNC 载入的基准"和
// "随后解出的地址"对齐到同一个位置，才看得出基准是从哪一条开始错的。
static bool                gIsyOn   = false;
static unsigned long long  gIsyLeft = 200;

static void OnDecoderReport(enum verbLevel l, const char* fmt, ...)
{
    (void)l;
    if (!gIsyOn || gIsyLeft == 0) return;
    --gIsyLeft;

    char b[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);

    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;

    // ISYNC 行额外判一次：**它载入的 IADDRESS 在不在代码区间** ——
    // "哪一发 ISYNC 的基准是坏的"当场可见，不用再去猜。
    const char* p = strstr(b, "IADDRESS 0x");
    if (p) {
        const uint32_t v = static_cast<uint32_t>(strtoul(p + 11, nullptr, 16));
        printf("    [ETM+%-9llu] %s   ⇒ %s\n", gEtmBytes, b,
               InCode(v) ? "基准在代码区间内"
                         : "**基准不在代码区间内 ⇒ 这一发 ISYNC 的 IADDRESS 就是坏的**");
        return;
    }
    printf("    [ETM+%-9llu] %s\n", gEtmBytes, b);
}

// ---------------------------------------------------------------------------
// 现场：最近喂进去的 64 个 ETM 字节
// ---------------------------------------------------------------------------
// 第一条越界地址出现时把它打出来 —— "解码器当时看到的是什么"比"解出来的地址"
// 有用得多（同一个思路见驱动侧 P1-0 失败时打那 4 个字节）。
static const unsigned kRingSize = 64;
static uint8_t        gRing[kRingSize];
static unsigned       gRingPos  = 0;
static unsigned       gRingFill = 0;

static void RingPush(const uint8_t* p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        gRing[gRingPos] = p[i];
        gRingPos = (gRingPos + 1) % kRingSize;
        if (gRingFill < kRingSize) ++gRingFill;
    }
}

static void RingDump()
{
    printf("    ETM 现场（最近 %u 字节，旧→新）：", gRingFill);
    for (unsigned i = 0; i < gRingFill; ++i) {
        const unsigned idx = (gRingPos + kRingSize - gRingFill + i) % kRingSize;
        printf(" %02X", gRing[idx]);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// 判据（与驱动侧那条只读诊断同一套）
// ---------------------------------------------------------------------------
// ⚠ 这是**诊断**判据，不是过滤。器件是 STM32F407VE：Flash 0x08000000 + 512 KB。
//
// ⚠⚠ 判据按"**它能不能是一条 PC（分支目标）**"来定，**不是**"这个地址有没有映射"。
//     ETM 报出来的是**指令地址**，所以外设区（0x4000_0000-0x5FFF_FFFF）与内核私有区
//     （>= 0xE000_0000）**永远不可能**是合法值 —— 那里没有可执行代码。
//     原判据把这两段也算"合理"，于是解出来的垃圾**被静默放过**：2026-10-14 实测
//     484,199 条里有 **11 条** bits[31:28]=4（`0x4xxxxxxx`），而工具报的是"0 条越界"。
//     ⇒ **判据太松时"0 条越界"什么也证明不了。** 收严之后它们会以 WILD 打出来。
//     （CCM 在 F4 上其实也不可执行，这里保留宽松，避免换器件时误报。）
static bool Plausible(uint32_t a)
{
    if (a < 0x00080000u)                      return true;  // 启动别名区
    if (a >= 0x08000000u && a < 0x08080000u)  return true;  // Flash 512 KB
    if (a >= 0x10000000u && a <= 0x1000FFFFu) return true;  // CCM
    if (a >= 0x20000000u && a <= 0x3FFFFFFFu) return true;  // SRAM（可执行）
    if (a >= 0x60000000u && a <= 0x9FFFFFFFu) return true;  // 外部 RAM / XIP（可执行）
    return false;                                           // 外设 / 内核私有 ⇒ 不可能
}

// orbuculum 的 TRACEStateChanged() 语义：读一位、清一位（照 ORBAGDI_Etm.cpp:158）。
static bool Changed(struct TRACECPUState* cpu, enum TRACEchanges c)
{
    const bool r = (cpu->changeRecord & (1u << c)) != 0;
    cpu->changeRecord &= ~(1u << c);
    return r;
}

// 引擎报"有事发生"时被调。签名与驱动侧一致（收 void*，就是 TRACECPUState*）。
static void OnReport(void* d)
{
    struct TRACECPUState* cpu = static_cast<struct TRACECPUState*>(d);
    if (!cpu) return;

    if (Changed(cpu, EV_CH_ADDRESS)) {
        const uint32_t a = cpu->addr;
        ++gAddrs;
        if (!gHaveAny) { gFirst = a; gHaveAny = true; }
        gLast = a;
        ++gTopHist[(a >> 28) & 0xF];

        if (!InCode(a)) {
            // ---- 不在代码区间：这才叫"非法指令地址" --------------------------
            // 关键判据：与**上一条好地址**比，低 20 位是否相同。
            //   相同 ⇒ 坏的是 `bits[31:20]`（ETM 的**绝对基准**，来自 ISYNC 的 IADDRESS），
            //          分支包本身是对的 —— 这是"ISYNC 收到了但地址字节坏了"的指纹。
            //   不同 ⇒ 低位也错，那是包本身/错位。
            const bool sameLow = gHaveGood && (((a ^ gLastGood) & 0x000FFFFFu) == 0);
            if (sameLow) ++gOutCodeSameLow;
            if (!Plausible(a)) ++gOutCodeMapped;

            if (gOutCode == 0) {
                gOutCodeFirst       = a;
                gOutCodeFirstPrev   = gHaveGood ? gLastGood : 0u;
                gOutCodeFirstByte   = gEtmBytes;
                RingDump();                 // 现场：解码器当时看到的字节
            }
            if (gOutCode < 12) {
                printf("    OUT-OF-CODE #%-3llu 0x%08X   上一条好地址 0x%08X   %s   ETM 字节偏移 %llu\n",
                       gOutCode + 1, a, gHaveGood ? gLastGood : 0u,
                       sameLow ? "**低 20 位相同 ⇒ 高位(bits[31:20])被替换**" : "（低位也不同）",
                       gEtmBytes);
            }
            ++gOutCode;
        } else {
            gLastGood = a;
            gHaveGood = true;
        }
        if (!Plausible(a)) ++gWild;     // 旧判据：连映射区都不在（更强的信号）
        gPrev = a;
    }

    if (Changed(cpu, EV_CH_ENATOMS) || Changed(cpu, EV_CH_WATOMS)) ++gAtoms;
    if (Changed(cpu, EV_CH_EX_ENTRY) || Changed(cpu, EV_CH_EX_EXIT)) ++gExc;
    if (Changed(cpu, EV_CH_TSTAMP))    ++gTs;
    if (Changed(cpu, EV_CH_CYCLECOUNT)) ++gCyc;

    // 其余位必须清掉，否则下一次回调会把它们当成"新事件"重复处理（照驱动侧）。
    cpu->changeRecord = 0;
}

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------
static void OnFrame(struct Frame* p, void* param)
{
    (void)param;
    if (p->len < 2) { ++gShort; return; }

    const uint8_t  tag = p->d[0];
    const uint8_t  sum = p->d[p->len - 1];
    const unsigned len = p->len - 2;

    uint8_t s = tag;
    for (unsigned i = 0; i < len; ++i) s = static_cast<uint8_t>(s + p->d[1 + i]);
    s = static_cast<uint8_t>(s + sum);
    if (s != 0) { ++gBadSum; return; }

    ++gFrames[tag];
    gBytes[tag] += len;

    if (tag == 2 && len) {          // OFLOW tag 2 = ETM（见 ORBAGDI_Oflow.h）
        RingPush(p->d + 1, len);    // 先留现场，再喂解码器
        if (gDumpLeft > 0) {
            --gDumpLeft;
            printf("    frame tag=2 len=%u :", len);
            for (unsigned i = 0; i < len && i < 12; ++i) printf(" %02X", p->d[1 + i]);
            printf("%s\n", (len > 12) ? " ..." : "");
        }
        TRACEDecoderPump(&gDec, p->d + 1, static_cast<int>(len), &OnReport, &gDec.cpu);
        gEtmBytes += len;
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: etm_replay <file.orb> [--alt 0|1] [--wild N] [--dump N]\n");
        return 2;
    }

    const char* path = argv[1];
    int  alt = 1;                       // 与驱动默认一致（ORBAGDI_Config.cpp: etmalt 默认 1）
    for (int i = 2; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--alt"))  alt = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--wild")) gWildWant = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--dump")) gDumpLeft = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--isy")) {
            gIsyOn   = true;
            gIsyLeft = strtoull(argv[i + 1], nullptr, 0);
        }
        if (!strcmp(argv[i], "--code")) {
            // lo:hi，十六进制。判"合法指令地址"用的就是它。
            const char* s = argv[i + 1];
            const char* c = strchr(s, ':');
            if (c) {
                gCodeLo = static_cast<uint32_t>(strtoul(s, nullptr, 16));
                gCodeHi = static_cast<uint32_t>(strtoul(c + 1, nullptr, 16));
            }
        }
    }

    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL  cannot open %s\n", path); return 2; }

    const size_t sigLen = strlen(kSig);
    char sig[32] = {0};
    if (fread(sig, 1, sigLen, f) != sigLen || memcmp(sig, kSig, sigLen) != 0) {
        printf("FAIL  no OFLOW signature (first %u bytes: \"%.*s\")\n",
               static_cast<unsigned>(sigLen), static_cast<int>(sigLen), sig);
        fclose(f);
        return 2;
    }

    printf("=== 文件 ===\n");
    printf("    %s\n", path);
    printf("    签名 %s\n", kSig);

    // 解码器：**与驱动同一套**，连第三个参数的含义都一样（分支地址编码）。
    // 第 4 个参数只在 --isy 时接上 —— 驱动侧传的是 NULL（热路径上零开销）。
    TRACEDecoderInit(&gDec, TRACE_PROT_ETM35, alt != 0,
                     gIsyOn ? OnDecoderReport : nullptr);

    printf("\n=== 解码（协议 ETM3.5，分支地址编码=%s）===\n",
           alt ? "备用(ALT)" : "标准(STD)");

    struct COBS* c = COBSInit(NULL);
    static uint8_t buf[64 * 1024];
    size_t n;
    unsigned long long fileBytes = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fileBytes += static_cast<unsigned long long>(n);
        COBSPump(c, buf, static_cast<int>(n), OnFrame, NULL);
    }
    const int cobsErr = COBSGetErrors(c);
    COBSDelete(c);
    fclose(f);

    printf("\n=== 帧层（源流属性，应与驱动日志的 校验错/COBS错 对得上）===\n");
    printf("    签名后 %llu 字节；tag1(ITM) %llu 帧 / %llu 字节；"
           "tag2(ETM) %llu 帧 / %llu 字节\n",
           fileBytes, gFrames[1], gBytes[1], gFrames[2], gBytes[2]);
    printf("    坏校验和 %llu；过短 %llu；COBS 错 %d\n", gBadSum, gShort, cobsErr);

    printf("\n=== ETM 解码结果 ===\n");
    printf("    喂入 ETM 载荷 %llu 字节\n", gEtmBytes);
    printf("    地址 %llu；原子 %llu；异常 %llu；时间戳 %llu；周期计数 %llu\n",
           gAddrs, gAtoms, gExc, gTs, gCyc);
    printf("    解码器同步状态 %s\n", TRACEDecoderIsSynced(&gDec) ? "已同步" : "未同步");
    if (gHaveAny) {
        printf("    地址范围 0x%08X .. 0x%08X\n", gFirst, gLast);
        printf("    地址高 4 位直方图：");
        for (int i = 0; i < 16; ++i) {
            if (gTopHist[i]) printf(" %X:%llu", i, gTopHist[i]);
        }
        printf("\n");
    }

    printf("\n=== 不在代码区间内的地址（**只读诊断，不过滤**）===\n");
    printf("    代码区间 0x%08X .. 0x%08X（--code 可改）\n", gCodeLo, gCodeHi);
    if (gOutCode == 0) {
        printf("    无\n");
    } else {
        printf("    共 %llu / %llu 条；其中\n", gOutCode, gAddrs);
        printf("      * **低 20 位与上一条好地址相同** = %llu 条"
               " ⇒ 坏的是绝对基准 bits[31:20]（ISYNC 的 IADDRESS），不是分支包\n",
               gOutCodeSameLow);
        printf("      * 连映射区都不在（旧判据）= %llu 条\n", gOutCodeMapped);
        printf("      * 第一条 0x%08X（上一条好地址 0x%08X）出现在**第 %llu 个 ETM 字节**\n",
               gOutCodeFirst, gOutCodeFirstPrev, gOutCodeFirstByte);
    }

    printf("\n=== 判定 ===\n");
    if (gAddrs == 0) {
        printf("FAIL  一个地址都没解出来 —— 这段流里没有可用的 ETM 数据，"
               "或者编码/协议不对（试试 --alt %d）\n", alt ? 0 : 1);
        return 1;
    }
    if (gOutCode == 0) {
        printf("PASS  %llu 个地址全部落在**代码区间**内 —— 这一段解码是干净的\n", gAddrs);
        return 0;
    }
    printf("FAIL  %llu / %llu 个地址**不在代码区间内**（第一条在第 %llu 个 ETM 字节）\n",
           gOutCode, gAddrs, gOutCodeFirstByte);
    if (gOutCodeSameLow * 2 >= gOutCode) {
        printf("      ⇒ 其中多数是**低 20 位正确、高位被替换** ⇒ 查 ISYNC 的 IADDRESS"
               "（用 --isy 看哪一发基准不在代码区间内）\n");
    } else {
        printf("      ⇒ 低位也错 ⇒ 更像包本身错位/丢字节（不是基准问题）\n");
    }
    printf("      ⇒ 用同一个文件换编码再跑一遍：--alt %d\n", alt ? 0 : 1);
    return 1;
}
