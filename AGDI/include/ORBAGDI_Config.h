/**
 * @file ORBAGDI_Config.h
 * @brief 驱动选项：uVision 按工程保存、会话开始时原样回传的 key=value 串。
 * 未知键忽略（新版本写下的工程仍能加载）；默认值不写回，未改过的工程保持干净。
 */
#pragma once

#include <stddef.h>
#include <windows.h>

namespace orb {
namespace cfg {

enum : size_t {
    kPathMax = 260,     // MAX_PATH
    kArgsMax = 1024     // dbgblk::TargArgs
};

enum : unsigned {
    kDefaultRamAddr = 0x20000000u, // SRAM base on every Cortex-M part we support
    kDefaultRamSize = 0x00001000u  // 4 kB is enough for a Keil FLM
};

// ---- Debug page: which port, how to connect, what to do after the reset ----
enum : int {
    kPortSwd = 0,           // "Port:" combo
    kPortJtag = 1,

    // "Connect:" combo，第 4 项是 Keil 的 "without Stop"（连上后让目标继续跑）。
    kConnectNormal = 0,
    kConnectPreReset = 1,   //   with Pre-reset
    kConnectUnderReset = 2, //   under Reset
    kConnectWithoutStop = 3,

    // "Reset:" combo，声明顺序即下拉顺序，选中索引就是值。
    kResetAutodetect = 0,   // use the description's reset when it has one
    kResetHwReset = 1,
    kResetSysResetReq = 2,
    kResetVectReset = 3,
    kResetCoreAndPeripheral = 4
};

// ---- Flash page: the "Download Function" radio group ----
enum : int {
    kEraseFullChip = 0,
    kEraseSectors = 1,
    kEraseNone = 2
};

// ---- Trace page: the "PC Sampling" prescaler combo (index, as shown) ----
enum : int {
    kPcPrescalerCount = 5
};

struct Config {
    // ---- what the driver really acts on ----
    char rddiDll[kPathMax]; // RDDI host DLL; empty = auto-discover
    int  iface;             // CMSIS-DAP interface index; -1 = first available
    int  clockKhz;          // SWJ clock in kHz; 0 = leave it to the host
    int  swoBaud;           // ITM/SWO console: SWO baud in Hz; 0 = off
    int  cpuKhz;            // core clock in kHz（来自 debug 块或 Trace 页）
    char flm[kPathMax];     // flash algorithm (.FLM); empty = no flash download
    unsigned ramAddr;       // RAM window the algorithm is copied into
    unsigned ramSize;
    unsigned clockHz;       // 核心时钟 Hz（0 = 未知），不入工程，每次会话重读

    // ---- Debug page ----
    int  port;              // kPortSwd / kPortJtag
    bool swj;               // SWJ-DP switching enabled (true = the default)
    int  ap;                // AHB-AP index used for memory access
    int  connect;           // kConnect* - "Connect:" combo
    int  reset;             // kReset* - "Reset:" combo
    bool resetAfterConnect; // "Reset after Connect"
    bool stopAfterReset;    // "Stop after Reset"
    bool logAccesses;       // "Log Debug Accesses"
    bool cacheCode;         // "Cache Code"    (shown, this driver has no cache)
    bool cacheMem;          // "Cache Memory"  (shown, this driver has no cache)
    bool downloadToFlash;   // "Download to Flash"

    // ---- Trace page ----
    // Trace 总开关，只回答“要不要 trace”，选哪条源仍看 orbtrace/etb/swoBaud。
    // 默认 1 让只写了 swo=/etb=/orbtrace= 的老工程行为不变；取消勾选写 tren=0。
    bool traceEnable;
    int  traceClockKhz;     // trace port clock in kHz; 0 = follow the core clock
    int  swoPrescaler;      // SWO clock = trace clock / prescaler (>= 1)
    int  swoMode;           // 0 = SWO UART/NRZ, 1 = SWO Manchester
    bool etb;               // 采集进片上 ETB 而不是走 SWO
    bool orbtrace;          // 走 orbtrace 探针的 trace 端点（OFLOW/COBS 分帧），
                            // 优先级高于 etb= 和 swo=
    int  trWidth;           // parallel trace port width in bits: 1 / 2 / 4
    int  trSwoMode;         // 0 = parallel port, 1 = SWO UART/NRZ,
                            // 2 = SWO Manchester
    int  trBaud;            // SWO baud for the probe in Hz; 0 = follow swo=
    bool trOflow;           // 探针用 OFLOW/COBS 分帧（协议 0x10 时强制开）
    bool etmEnable;         // "ETM Trace Enable"：关着时 ProgramEtm() 不写寄存器
    // 分支地址编码：1 = ALT（默认）/ 0 = STD，必须与目标 ETM 实际配置一致。
    // 选错不会报错，只是地址整体错位，所以不做自动判定（固件启动后才配 ETM）。
    bool etmAltAddr;
    bool useCoreClock;      // "Use Core Clock"：trace 时钟跟随核心时钟
    bool unlimitedTrace;    // "Unlimited Trace"：块链本无上限，这里只用于上报
    int  trTpui;            // SWO only: 1 = 探针解 TPIU 帧，0 = 裸 ITM 流。
                            // 必须与目标 TPIU_FFCR 一致，否则永远等不到同步
    bool tsEnable;          // ITM timestamps ("Timestamps: Enable")
    int  tsPrescaler;       // ITM timestamp prescaler (1, 4, 16, 64, 256)
    // 是否让 uVision 显示时间列。默认关：该字段也是 uVision 取数门的一环，
    // 打开可能让 Trace 窗口一行都不取。
    bool tsWin;
    // 记录缓冲规模（块链）。0 = 默认：每块 4 MB，上限物理内存 1/4、封顶 1 GB。
    // 只在第一次 Connect 时生效，改后需重开会话。
    int  trChunkMB;         // trchunk=  每块 MB
    int  trMemMB;           // trmem=    内存上限 MB
    // trmax=：常驻记录条数上限，0 = 不限（默认）。
    // 硬编码条数会在 ETM 下静默裁掉大部分采集，要限大小请用 trmem=。
    int  trMaxRecs;         // trmax=    常驻记录条数上限，0 = 不限
    // trace 缓存文件（P2-1）。空 = 不写；“1”/目录 = 自动命名，否则当作文件名。
    // **不是功能**：只是内存装不下时的临时缓存，新采集段一开始就删掉上一段的，
    // 所以没有“保留几个”这个设置。
    char trFile[kPathMax];
    // 记录 cache（P2-2）：存紧凑记录，查询不必重解帧；与 trfile= 独立。
    char trRec[kPathMax];
    // cache 文件大小上限（MB，0 = 不限，**默认 0**）。默认不限是为了覆盖全程数据量：
    // 到顶后文件冻结在最早那段、内存窗口继续 FIFO 滚动，时间轴中段会变成空洞。
    // 填正值仍然有效（到顶停止追加并弹一次提示框），设置页 Trace Cache 页可改。
    int  trRecMB;
    int  pcPrescaler;       // "PC Sampling: Prescaler" combo index
    bool pcPeriodic;        // "Periodic"
    bool pcDataRw;          // "on Data R/W Sample"
    unsigned itmPorts;      // 32 stimulus-port enable bits
    unsigned itmPriv;       // stimulus-port privilege mask
    // TraceOpt 事件位（TRACE_EXCTRC 0x0010 … TRACE_CYCCNT 0x0400），
    // 与 Trace 页 6 个复选框 1:1，消费端逐位映射到 DWT_CTRL bit16..22。
    unsigned traceEvents;

    // ---- Flash page ----
    int  eraseMode;         // kErase*
    bool program;           // "Program"
    bool verify;            // Flash page "Verify" - the only verify switch
    bool resetAndRun;       // "Reset and Run"

    // ---- Pack page ----
    bool pdscEnable;        // "Enable" (the PDSC debug description)
    bool pdscLog;           // "Log Sequences:"
    char pdscLogFile[kPathMax]; // where those sequences are logged to

    // ---- what uVision reported about the target ----
    // 只由 AdoptUVision() 填，不写回工程：描述的是器件而非驱动。
    char device[64];        // device name, e.g. "STM32F407VETx"
    unsigned iromStart;     // on-chip flash window of that device
    unsigned iromSize;
};

// Process-wide options. Reset() puts every field back to its default.
Config& Get();
void    Reset();

// 本次会话从 debug 块取到的值；与 Get() 分开，保证不会被写回工程文件。
Config& AutoDetected();

// 当前生效的选项：工程值 + 自动探测值补空。
Config Effective();

// uVision 在 debug 块里给的 Keil 根目录（dbgext::szTRoot）。
// pack 根由它派生并缓存，晚了要清缓存；空/NULL 表示清除。
void SetToolRoot(const char* armDir);

// 本机 pack 树根目录，按相关性排序（RTEPATH → CMSIS_PACK_ROOT → 常见位置）。
int  PackRootCount();
bool PackRoot(int index, char* out, size_t cch);

// 解析选项串，整体替换当前值（不合并）；返回是否认出了至少一个键。
bool Parse(const char* args);

// Render the current values into `out` (always NUL-terminated).
void Format(char* out, size_t cch);
void Format(const Config& c, char* out, size_t cch);

// Trace 页最终得到的 SWO 位时钟（trace 时钟 / 分频），没设 trace 时钟时为 0。
int  SwoBaud(const Config& c);

// 目标侧 TPIU 异步分频的基准时钟（kHz）。
// 物理上分的是核心时钟还是独立 TRACECLKIN 尚无定论，先按核心时钟；
// 结论出来后只改这一处。
int  SwoAcprBaseKhz(const Config& c);

// ---- 采纳 uVision 已有的目标信息 ----

// 从 Keil “ULINK 风格”选项串里取 FLM 与 RAM 窗口：
//   -FD/-FC = RAM 窗口，-FF<n><name> / -FP<n>(<path>) = 算法。
// 只填空缺项；找到可用算法时返回 true。
bool ParseKeilFlash(const char* args);

// 把 debug 块里的器件名、flash 设置、片上 RAM 与核心时钟抄进选项。
// 这是“不用打开 Flash 对话框也能下载”的原因；dbgBlk 是 struct dbgblk*，只读不存。
void AdoptUVision(const void* dbgBlk);

// 模态设置页（Debug / Trace / Flash Download / Pack 四页），只差起始页。
// 返回 true 表示用户按了 OK。
bool ShowDialog(HWND parent, const char* inArgs, char* outArgs, size_t outCch);

// Utilities 页的 Settings：同一套页，停在 Flash Download 页。
// prjName 只用于日志，可为 null。
bool ShowFlashDialog(HWND parent, const char* inArgs, char* outArgs,
                     size_t outCch, const char* prjName);

} // namespace cfg
} // namespace orb
