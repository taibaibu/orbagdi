/**
 * @file ORBAGDI_Config.h
 * @brief Driver options - the string uVision keeps per project for this driver.
 *
 * uVision stores a free-form command line for every target driver and hands it
 * back when the session starts (`dbgblk::TargArgs`, mirrored as `QDLL::value`
 * while the Options dialog is up).  We use a flat "key=value" list:
 *
 *     rddi="C:\Keil_v5\ARM\BIN\CMSIS_DAP.dll" iface=0 clock=1000
 *
 * Unknown keys are ignored, so a project written by a newer build still loads,
 * and an empty string simply means "all defaults".
 *
 * The keys are grouped the way the setup sheet is: the Debug page edits the
 * adapter / port / reset behaviour, the Trace page the SWO and ITM options,
 * the Flash page the algorithm and the download function, the Pack page the
 * PDSC debug description.  Keys whose default is what the driver wants are
 * never written, so an untouched project stays clean.
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

// ---------------------------------------------------------------------------
// Debug page: which port, how to connect, what to do after the reset
// ---------------------------------------------------------------------------
enum : int {
    kPortSwd = 0,           // "Port:" combo
    kPortJtag = 1,

    // "Connect:" combo.  The fourth entry is "without Stop" - connect and leave
    // the target running - which is Keil's own wording and its own semantics
    // (ULP2CM3.dll resource strings, next to Normal / with Pre-reset /
    // under Reset).  It used to be called "Halt after Reset", which named a
    // different thing entirely and was read by no code at all.
    kConnectNormal = 0,
    kConnectPreReset = 1,   //   with Pre-reset
    kConnectUnderReset = 2, //   under Reset
    kConnectWithoutStop = 3,

    // "Reset:" combo.  The declaration order *is* the combo order - and it is the
    // order Keil's own driver lists ("Autodetect / HW RESET / SYSRESETREQ /
    // VECTRESET", ULP2CM3.dll resource strings) - so the selected index is the
    // value.  It used to be declared in a different order than the combo showed,
    // which made "HW RESET" drive AIRCR.VECTRESET and "SYSRESETREQ" pulse nRESET
    // (bug.md §N).
    kResetAutodetect = 0,   // use the description's reset when it has one
    kResetHwReset = 1,
    kResetSysResetReq = 2,
    kResetVectReset = 3,
    kResetCoreAndPeripheral = 4
};

// ---------------------------------------------------------------------------
// Flash page: the "Download Function" radio group
// ---------------------------------------------------------------------------
enum : int {
    kEraseFullChip = 0,
    kEraseSectors = 1,
    kEraseNone = 2
};

// ---------------------------------------------------------------------------
// Trace page: the "PC Sampling" prescaler combo (index, as shown)
// ---------------------------------------------------------------------------
enum : int {
    kPcPrescalerCount = 5
};

struct Config {
    // ---- what the driver really acts on -----------------------------------
    char rddiDll[kPathMax]; // RDDI host DLL; empty = auto-discover
    int  iface;             // CMSIS-DAP interface index; -1 = first available
    int  clockKhz;          // SWJ clock in kHz; 0 = leave it to the host
    int  swoBaud;           // ITM/SWO console: SWO baud in Hz; 0 = off
    int  cpuKhz;            // core clock in kHz (from the debug block or the Trace
                            // page).  Whether it is the target's TPIU prescaler base
                            // is undecided - see `SwoAcprBaseKhz()`.
    char flm[kPathMax];     // flash algorithm (.FLM); empty = no flash download
    unsigned ramAddr;       // RAM window the algorithm is copied into
    unsigned ramSize;
    unsigned clockHz;       // core clock in Hz, taken from uVision's debug block
                            // (0 = unknown).  Not persisted: it is re-read from
                            // the project at every session start.

    // ---- Debug page -------------------------------------------------------
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

    // ---- Trace page -------------------------------------------------------
    // "Trace Enable" —— Trace 页顶部那个总开关（照 ULINK Pro 的设置页布局）。
    //
    // 它只回答"要不要 trace"；**选哪条源**仍由下面三个标志
    // （`orbtrace` / `etb` / `swoBaud`）决定。trace 真正启动的条件是
    // `traceEnable && (orbtrace || etb || swoBaud > 0)`。
    //
    // 默认 **1**：在这个开关存在之前写下的工程只写了 `swo=` / `etb=` /
    // `orbtrace=`，而"有源"本来就等于"开" —— 默认 1 让老工程行为不变。
    // 取消勾选会显式写 `tren=0`（`Format()` 只写非默认值，1 是默认值）。
    bool traceEnable;
    int  traceClockKhz;     // trace port clock in kHz; 0 = follow the core clock
    int  swoPrescaler;      // SWO clock = trace clock / prescaler (>= 1)
    int  swoMode;           // 0 = SWO UART/NRZ, 1 = SWO Manchester
    bool etb;               // capture into the on-chip ETB instead of streaming
                            // over SWO ("etb=1"); the ITM/DWT setup is shared
    bool orbtrace;          // take trace from the orbtrace probe's own trace
                            // endpoint ("orbtrace=1"): the parallel port or SWO,
                            // OFLOW/COBS framed (ORBAGDI_OrbTrace.cpp).
                            // Takes priority over etb= and swo=.
    int  trWidth;           // parallel trace port width in bits: 1 / 2 / 4
    int  trSwoMode;         // 0 = parallel port, 1 = SWO UART/NRZ,
                            // 2 = SWO Manchester
    int  trBaud;            // SWO baud for the probe in Hz; 0 = follow swo=
    bool trOflow;           // the probe frames the stream with OFLOW/COBS
                            // (forced on when the probe reports protocol 0x10)
    // ---- ULINK Pro 的 Trace 页顶上那三个复选框（除 6 个事件复选之外的三项）----
    bool etmEnable;         // "ETM Trace Enable" (`etmen`): 允许配 ETM 指令 trace。
                            // 关着时 `ProgramEtm()` **一个寄存器都不写**。
    // 分支地址编码（`etmalt`）：**1 = 备用(ALT)（默认）/ 0 = 标准(STD)**。
    //
    // **必须与目标 ETM 硬件一致** —— 判据是 `ETMBASE + 0x1E4` 的 bit20。
    // orbuculum 的 GDB 宏 `describeETM` 判的就是这一位（`Support/gdbtrace.init:559,598`），
    // 上游 README（`README.md:766`）原文：解码不对时"用 describeETM 就能知道要不要关掉
    // 备用编码"。本驱动在 `ProgramEtm()` 里把这两个偏移一起打进日志（describeETM 等价物）。
    //
    // ⚠ 上游 orbuculum **默认用备用编码**（`Src/orbmortem.c:1378` 传 `!noAltAddr`，
    //   `orbprofile.c:461` 帮助文本括注 "(on by default)"），而本驱动原来**硬编码标准
    //   编码**（`ORBAGDI_Etm.cpp` 的 `TRACEDecoderInit(..., false, ...)`）。
    //   编码选错 ⇒ 地址字节的位分组错 ⇒ 解出来的地址**整体错位**（跑到 flash 之外），
    //   而"已同步"仍为真、OFLOW 帧校验仍全过（那两者都不看内容）。
    //
    // **2026-10-14 最终形态：两档，由用户选，默认 ALT。**
    //
    // 中途做过一版"寄存器读自动判定"（`0x1E4` bit20），**已按用户指令去掉**：
    //   用户原话："自动识别解码器模式的功能可能有问题，因为固件是启动后执行 etm
    //   初始化，但是驱动是一开始获取的，所以依旧获取到一个错误的值，这个自动功能
    //   去除，按备用/标准选（包括设置界面）"。
    //   ⇒ `0x1E4` 反映的是 ETM 的**当前配置态**，而固件是**启动之后**才配 ETM；
    //     驱动在连接阶段就读，可能读到配置**之前**的值 ⇒ 自动档会**安静地选错**
    //     （错编码不报错，只是地址整体错位）。所以不做"猜"。
    //
    // 默认 **1 = ALT**：上游默认如此，且这颗 STM32F407 实测 `0x1E4` bit20=1（就是 ALT）。
    // ⚠ 与"自动档那一版"不兼容（那版 0 = 自动、2 = STD）；与**最初**的语义一致
    //   （1 = ALT，0 = STD）⇒ 老工程的选项串含义不变。
    bool etmAltAddr;
    bool useCoreClock;      // "Use Core Clock" (`tclkc`): trace 时钟跟随核心时钟，
                            // 忽略 `traceClockKhz`。等价 DbgCM 的
                            // `TRACE_USE_CORECLK`（`AGDI.CPP:4149-4153` 把
                            // `TPIU_Clock` 直接取 `TraceConf.Clk`）。
    bool unlimitedTrace;    // "Unlimited Trace" (`trunlim`): 无上限记录缓冲，
                            // 等价 DbgCM 的 `UNLIMITED_TRACE`。本驱动的块链（P1-1）
                            // 本来就是无上限，这一位的作用是把它报进 TraceOpt。
    int  trTpui;            // SWO only: 1 = the probe unpacks TPIU framing
                            // (orbuculum "-t U"), 0 = the stream is raw ITM
                            // ("-t u").  It has to match the target's
                            // TPIU_FFCR formatter state, otherwise the probe
                            // hunts for a sync that never arrives and delivers
                            // nothing while the target looks perfectly set up.
    bool tsEnable;          // ITM timestamps ("Timestamps: Enable")
    int  tsPrescaler;       // ITM timestamp prescaler (1, 4, 16, 64, 256)
    // 是否让 uVision 显示时间列（TRACE_STATUS.bHideTimestamps = 0）。
    //
    // ⚠⚠ **默认关**。这个字段同时是 uVision 取数门的一环（§10.20.1）：实测
    // `app[0x14] & 0x2000 = 1`，而门链是"该位为 1 且 app[0xbc]（= 本字段）为 0
    // ⇒ 不调 FetchRecords"。也就是说把它打开可能让 Trace 窗口一行都不取。
    // 周期轴（真实时间）**不需要**它 —— 那是 ITM 时间戳包 + TRACE_CLK 的事，
    // 这条只控制窗口里那列时间文字显不显示。要开就上机看着窗口开。
    bool tsWin;
    // 记录缓冲（Trace Data 窗口）的规模 —— 块链（P1-1）。
    //
    // 0 = 用默认值：每块 4 MB；上限 = 物理内存 1/4，封顶 1 GB（Todo.md §九）。
    // 之所以做成选项：默认上限下"到上限摘最旧块"这条路要攒够 **1 GB** 才走得到，
    // 那是"到不了就等于没写"的代码 —— `trchunk=1 trmem=2` 可以让它在几秒内跑到
    // （假 BOM 宿主的验收就是这么做的）。
    //
    // ⚠ 生效时机：只在**第一次 Connect**（`TraceWinInit()` 建索引数组）时应用。
    // 之后改它需要重开会话。见 `TraceWinSetBuffer()`。
    //
    // 用 `KeyType::Int`（十进制）而不是 Uint：Uint 那几把键是按**十六进制**读的
    // （`ports=`/`priv=`/`events=` 是位掩码，见 SetExtra 的说明），写成 Uint 会让
    // `trmem=64` 变成 100 MB。
    int  trChunkMB;         // trchunk=  每块 MB
    int  trMemMB;           // trmem=    内存上限 MB
    // trmax=：常驻**记录条数**上限，**0 = 不限（默认）**。
    //
    // ⚠ 默认必须是不限。原来驱动内部硬编码 1,048,576 条（对齐官方 `cmp 0x100000`），
    //   在 ETM 下等于"只能回看 **0.13 s**"：现场一次 10 s 采集写进 82,658,740 条、
    //   被**静默**裁掉 81,608,143 条，而当时内存只用了 552 MB / 上限 1024 MB ——
    //   整段本来装得下。定性、日志锚点与修法见 `Todo.md §4.3`。
    //   要控窗口大小请用 `trmem=`（字节，精确）；这条只在确实要"卡条数"时用，
    //   而且现在触顶走整块退休（记录日志开着就落盘，没开就报 Data Overflow）。
    // ⚠ 与 trchunk=/trmem= 不同：它只是一个比较值，**随时可改**，不要求重开会话。
    int  trMaxRecs;         // trmax=    常驻记录条数上限，0 = 不限
    // trace 落盘（P2-1，见 ORBAGDI_TraceFile.h）。空 = 不落盘。
    //   "1"        -> 日志所在目录（通常 %TEMP%）下自动命名
    //   "<目录>"    -> 该目录下自动命名
    //   "<名>.orb" -> 就用这个名字
    // 文件名带时间戳、**不带版本号**（`%%ORBFLOW1.0.0%%` 是文件**内容**的签名，
    // 属于格式的一部分，去不掉）。
    // ⚠ 调试用的机器盘小：`trkeep` 默认 3，超出的旧文件在下次落盘时删掉。
    char trFile[kPathMax];
    int  trKeep;            // 同目录最多保留几个 ORB_trace_*.orb（0 = 不删）
    // 记录日志（P2-2，见 ORBAGDI_TraceFile.h）：给"内存窗口之外还能回看"用。
    // 内容是**紧凑记录**（P1-2 的格式），所以查询不必重新解帧/重解 ITM-ETM。
    // 与 trfile= 独立，同样 opt-in（默认不落盘，盘小）。
    char trRec[kPathMax];
    // 落盘文件的大小上限（MB，`trrecmb=`）。0 = 不限。**默认 2048（2 GB）**。
    //
    // 为什么要有个天花板：落盘是 opt-in 的，但一旦开了就是"每块退休追加 4 MB"、
    // 无限增长 —— 不设限就是把用户的盘当缓冲用。到顶后**停止追加**并告警一次，
    // 不是失败（文件里是这段历史**最早**的一段，最近的仍在内存窗口里）。
    int  trRecMB;
    int  pcPrescaler;       // "PC Sampling: Prescaler" combo index
    bool pcPeriodic;        // "Periodic"
    bool pcDataRw;          // "on Data R/W Sample"
    unsigned itmPorts;      // 32 stimulus-port enable bits
    unsigned itmPriv;       // stimulus-port privilege mask
    // "Trace Events"：TraceOpt 的事件位（`TRACE_EXCTRC` 0x0010 … `TRACE_CYCCNT`
    // 0x0400），与 Trace 页那 6 个复选框一一对应。消费端在 `ProgramItmDwt()`：
    // 逐位映射到 DWT_CTRL 的 bit16..bit22（照 `DbgCM/Trace.cpp:1515-1532`）。
    //
    // ⚠ 2026-10-14 更正：这里原来存的是"从 DWT bit17 起整体左移"的自定义掩码，
    //   位序是 CPI 打头、且**表达不了 EXCTRC**（它在 bit16，低于移位基址）。
    //   现在与 Keil 的 `TraceConf.Opt` 同一套位序，6 个复选框 1:1。
    unsigned traceEvents;

    // ---- Flash page -------------------------------------------------------
    int  eraseMode;         // kErase*
    bool program;           // "Program"
    bool verify;            // Flash page "Verify" - the only verify switch
    bool resetAndRun;       // "Reset and Run"

    // ---- Pack page --------------------------------------------------------
    bool pdscEnable;        // "Enable" (the PDSC debug description)
    bool pdscLog;           // "Log Sequences:"
    char pdscLogFile[kPathMax]; // where those sequences are logged to

    // ---- what uVision reported about the target ---------------------------
    // Filled by AdoptUVision() only and never written into the project: they
    // describe the device, not the driver, so they must not travel with the
    // option string.  The Debug and Flash pages show them.
    char device[64];        // device name, e.g. "STM32F407VETx"
    unsigned iromStart;     // on-chip flash window of that device
    unsigned iromSize;
};

// Process-wide options. Reset() puts every field back to its default.
Config& Get();
void    Reset();

// What AdoptUVision() picked up from the debug block during this session. Kept
// apart from Get() so it can never be written back into the project file.
Config& AutoDetected();

// The options in force right now: the project's, with the auto-detected values
// filling every gap. This is what the flash loader and the settings dialogs
// work from.
Config Effective();

// The tool root uVision hands over in its debug block (dbgext::szTRoot, e.g.
// "D:\MDK5\ARM").  It is the authoritative answer to "where is Keil installed";
// every other candidate in ORBAGDI_Config.cpp is a guess at a folder name, and a
// wrong guess stays invisible until a download fails with "no flash algorithm".
// Call it as soon as the debug block arrives - the pack roots are derived from
// it and are cached, so a late answer has to drop that cache.  Empty/NULL clears
// it.  Declared in ORBAGDI_Config.h, defined next to CollectPackRoots().
void SetToolRoot(const char* armDir);

// Roots of the pack trees this machine uses, most relevant first: uVision's
// RTEPATH (TOOLS.INI) first, then CMSIS_PACK_ROOT and the classic locations.
// A current MDK keeps its packs outside <Keil>\ARM\PACK, so anything looking
// for a .FLM has to ask here rather than guess.
int  PackRootCount();
bool PackRoot(int index, char* out, size_t cch);

// "rddi=C:\...\CMSIS_DAP.dll iface=0 clock=1000". Replaces the current values
// (it does not merge). Returns true if at least one key was recognised.
bool Parse(const char* args);

// Render the current values into `out` (always NUL-terminated).
void Format(char* out, size_t cch);
void Format(const Config& c, char* out, size_t cch);

// The SWO bit clock the Trace page ends up with: trace clock / prescaler, or 0
// when no trace clock is set at all.  This is what `swo=` carries.
int  SwoBaud(const Config& c);

// The clock the **target-side** TPIU async prescaler divides by, in kHz.
//
// A third, separate role: `tclk`/`tclkc` feed `SwoBaud()` (the rate the *probe*
// is told to capture at, still pushed in parallel mode - see
// COMPAT_ANALYSIS.md:1194-1198) and the TRACE_CLK VTR (the Trace Data window's
// time axis: the `TRACE_CLK` VTR, created in ORBAGDI_TraceWin.cpp by
// `p_create("TRACE_CLK")`; see the comment block above it).  Which *physical*
// clock the target's ACPR really
// divides - the core clock, or an independent TRACECLKIN - is a device question
// that neither the official source nor our disassembly settles; it needs the A/B
// experiment recorded in Todo.md.  Until that is run this returns the core clock
// (today's shipped behaviour), and it is the single place to change once it is
// answered.
int  SwoAcprBaseKhz(const Config& c);

// ---------------------------------------------------------------------------
// Adopting what uVision already knows about the target
// ---------------------------------------------------------------------------

// Digs the flash algorithm and the RAM window out of a Keil "ULINK style"
// option string, the format DbgCM.cpp:190 (AnalyzeParms) defines:
//
//     -FO7 -FD20000000 -FC800 -FN1 -FF0STM32F10x_128 -FS08000000 -FL020000
//
// i.e. -FD/-FC = RAM window, -FF<n><name> = algorithm without extension (Keil
// looks it up as <Keil>\ARM\Flash\<name>.FLM), -FP<n>(<path>) = full path.
// Only fills what the project did not set; returns true when a usable
// algorithm was found.
bool ParseKeilFlash(const char* args);

// Copies the target description uVision carries in its debug block into the
// options: the device name, the flash settings of the Utilities page, the
// on-chip RAM window and the core clock.  This is what makes a download work
// without the user ever opening the Flash dialog - a project configured for
// ULINK/CMSIS-DAP already names the right algorithm.  `dbgBlk` is a
// `struct dbgblk*` (ComTyp.H); it is only read, never kept.
void AdoptUVision(const void* dbgBlk);

// Modal setup sheet - the Debug sheet's "Settings" button.  Both entry points
// below open the same sheet (Debug / Trace / Flash Download / Pack pages), the
// way Keil's own driver does; they differ only in the page it starts on.
//
// `inArgs` is the string uVision stored, `outArgs` receives the edited one.
// Returns true when the user pressed OK, false on Cancel or if the sheet could
// not be created.
bool ShowDialog(HWND parent, const char* inArgs, char* outArgs, size_t outCch);

// The Utilities sheet's "Settings" button (DllUv3Cap(110), which Keil answers
// with DoFDlgSetup()): the same sheet, opening on the Flash Download page that
// edits the flash algorithm (.FLM) and the RAM window it is copied into.
// `prjName` is QDLL::prjname and is only used for logging; it may be null.
bool ShowFlashDialog(HWND parent, const char* inArgs, char* outArgs,
                     size_t outCch, const char* prjName);

} // namespace cfg
} // namespace orb
