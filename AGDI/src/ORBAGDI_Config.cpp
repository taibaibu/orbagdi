/**
 * @file ORBAGDI_Config.cpp
 * @brief Parsing and formatting of the driver option string.
 */
#include "pch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ORBAGDI_Config.h"

// uVision's own header: the debug block it hands to a target driver carries the
// device name and the flash settings of the Utilities page (ComTyp.H).
#include "COMTYP.H"

namespace orb {
namespace cfg {

namespace {

const int kDefaultIface = -1;
const int kDefaultClock = 0;
const int kDefaultSwo   = 0;      // 0 Hz = ITM/SWO console disabled
const int kDefaultCpu   = 72000;  // 72 MHz, the STM32F1 default; swo only

// ---------------------------------------------------------------------------
// The keys the setup sheet adds on top of the ones the driver acts on.
//
// One table drives both directions, so a key can never be written without
// being read back.  `def` is the value the key has when it is absent from the
// option string - and therefore also the value that is not written, which is
// what keeps a project that was never edited free of clutter.
// ---------------------------------------------------------------------------
enum class KeyType { Int, Bool, Uint };

struct ExtraKey {
    const char* name;
    KeyType     type;
    size_t      offset;     // offsetof(Config, ...)
    int         def;
};

const ExtraKey kExtraKeys[] = {
    // Debug page
    { "port",      KeyType::Int,  offsetof(Config, port),              kPortSwd },
    { "swj",       KeyType::Bool, offsetof(Config, swj),               1 },
    { "ap",        KeyType::Int,  offsetof(Config, ap),                0 },
    { "connect",   KeyType::Int,  offsetof(Config, connect),           kConnectNormal },
    // Default "Autodetect" (= 0), so an untouched project still writes nothing -
    // and now the value the combo shows is the value the driver acts on.
    { "reset",     KeyType::Int,  offsetof(Config, reset),             kResetAutodetect },
    // Default ON.  A flash download has to start from a state in which the CPU
    // can reach the flash controller, and a target left halted inside the
    // application is not one: the STM32 algorithm's first act is a FLASH_KEYR
    // write, which then faults (PRECISERR, BFAR=0x40023C04) and locks the core
    // up - a 30 s timeout with no hint at the cause.  Resetting on connect is
    // what makes a download work out of the box (COMPAT_ANALYSIS.md §10.12).
    { "rstcon",    KeyType::Bool, offsetof(Config, resetAfterConnect), 1 },
    { "stoprst",   KeyType::Bool, offsetof(Config, stopAfterReset),    0 },
    { "logacc",    KeyType::Bool, offsetof(Config, logAccesses),       0 },
    { "cachecode", KeyType::Bool, offsetof(Config, cacheCode),         1 },
    { "cachemem",  KeyType::Bool, offsetof(Config, cacheMem),          1 },
    { "dlflash",   KeyType::Bool, offsetof(Config, downloadToFlash),   0 },
    // Trace page
    // tren: "Trace Enable" 总开关（Trace 页顶部，照 ULINK Pro 的设置页布局）。
    // 默认 1 —— 老工程没这个键时由"有没有源"推出"开"，行为不变；用户取消勾选
    // 才写 `tren=0`（Format() 只写非默认值）。
    { "tren",      KeyType::Bool, offsetof(Config, traceEnable),       1 },
    { "tclk",      KeyType::Int,  offsetof(Config, traceClockKhz),     0 },
    // tclkc: "Use Core Clock" —— trace 时钟跟随核心时钟，忽略 tclk=（等价 DbgCM 的
    // TRACE_USE_CORECLK，AGDI.CPP:4149-4153）。见 ORBAGDI_Config.h 的 useCoreClock。
    { "tclkc",     KeyType::Bool, offsetof(Config, useCoreClock),      0 },
    { "swopresc",  KeyType::Int,  offsetof(Config, swoPrescaler),      1 },
    { "swomode",   KeyType::Int,  offsetof(Config, swoMode),           0 },
    { "etb",       KeyType::Bool, offsetof(Config, etb),               0 },
    // Trace page: orbtrace probe trace source (ORBAGDI_OrbTrace.cpp).
    // trwidth is in BITS (1/2/4); the RDDI setter accepts those values directly.
    { "orbtrace",  KeyType::Bool, offsetof(Config, orbtrace),          0 },
    { "trwidth",   KeyType::Int,  offsetof(Config, trWidth),           4 },
    { "trswo",     KeyType::Int,  offsetof(Config, trSwoMode),         0 },
    { "trbaud",    KeyType::Int,  offsetof(Config, trBaud),            0 },
    { "troflow",   KeyType::Bool, offsetof(Config, trOflow),           1 },
    // trtpui: 让探针按 TPIU 成帧解包（固件 input_format 的 TPIU 档）。
    //
    // ⚠ 2026-10-13 更正：这里原来写"EnFCont 写不进去 ⇒ 目标不打包"，**把这一位读反了**。
    //    ARM 的定义（`TPIU_FFCR.EnFCont`，Cortex-M3/M4 TRM）：**0 = formatter 打开**
    //    （输出 16 字节格式化帧、每字节带 1 个 id 位），**1 = bypass / 连续模式**；
    //    而 bypass 下**只有 ITM/DWT 通过，ETM 被 TPIU 丢弃**。本板 FFCR 读回 0
    //    ⇒ formatter 是**开着**的。
    //    ⇒ 并口路径（固件 input_format 0x01/0x02/0x03）**永远**走探针的 TPIU demux
    //      （`orbtrace/trace/core.py:114-125`），formatter 开着时这是对的 ——
    //      **但 1 位并口不行**：id 位错位，见 ORBAGDI_Agdi.cpp 里 `trwidth=1` 的告警。
    //    ⇒ SWO 路径上 trtpui=0 走的是 bypass（固件 `tpiu.py:226-232` 全打 tag 1），
    //      要带 ETM 必须 trtpui=1；而 trtpui=1 需要探针等到 TPIU 同步头
    //      0xFFFFFF7F，实测等不到（674 vs 2048 PC 采样）。
    { "trtpui",    KeyType::Int,  offsetof(Config, trTpui),            0 },
    // etmen: "ETM Trace Enable" —— 关着时 ProgramEtm() 一个 ETM 寄存器都不写。
    { "etmen",     KeyType::Bool, offsetof(Config, etmEnable),         0 },
    // etmalt: 分支地址编码。**1 = 备用(ALT)（默认）/ 0 = 标准(STD)**。
    // 不做"寄存器读自动判定" —— 理由见 ORBAGDI_Config.h 里 etmAltAddr 的说明
    // （固件启动后才配 ETM，驱动早读会拿到旧值 ⇒ 会安静地选错）。
    // ⚠ 默认 1 = ALT：上游 orbuculum 默认就是 ALT，这颗 STM32F407 实测也是 ALT。
    { "etmalt",    KeyType::Bool, offsetof(Config, etmAltAddr),        1 },
    { "ts",        KeyType::Bool, offsetof(Config, tsEnable),          0 },
    { "tspresc",   KeyType::Int,  offsetof(Config, tsPrescaler),       1 },
    // tswin: 让 uVision 显示时间列。⚠ 同时是取数门的一环，默认关 —— 见
    // ORBAGDI_Config.h 里 tsWin 的说明与 COMPAT_ANALYSIS.md §10.20.1。
    { "tswin",     KeyType::Bool, offsetof(Config, tsWin),             0 },
    // 记录缓冲的规模（块链，P1-1）。0 = 默认（4 MB/块；上限物理内存 1/4 封顶
    // 1 GB）。默认上限下"到上限摘块"要攒够 1 GB 才走得到，测试用这两条把它缩短。
    // ⚠ 必须是 Int（十进制）：Uint 键是按十六进制读的（见 SetExtra）。
    { "trchunk",   KeyType::Int,  offsetof(Config, trChunkMB),         0 },
    { "trmem",     KeyType::Int,  offsetof(Config, trMemMB),           0 },
    // trmax: 常驻**记录条数**上限，**0 = 不限（默认）**。
    // ⚠ 默认必须是不限。原来驱动内部硬编码 1,048,576 条，在 ETM 下等于"只能回看
    //   0.13 s"：现场一次 10 s 采集写进 82,658,740 条、被静默裁掉 81,608,143 条，
    //   而当时内存只用了 552 MB / 上限 1024 MB（整段本来装得下）。定性见 Todo.md §4.3。
    //   要控窗口大小用 trmem=（字节，精确）；这条只在确实要卡条数时用。
    { "trmax",     KeyType::Int,  offsetof(Config, trMaxRecs),         0 },
    // trace 落盘（P2-1）：trfile= 是**字符串**键（在 Parse/Format 里单独处理，
    // 与 rddi=/flm= 同一种做法），trkeep 是普通整数键。
    { "trkeep",    KeyType::Int,  offsetof(Config, trKeep),            3 },
    // trrecmb: 落盘文件大小上限（MB），0 = 不限。默认 2 GB。
    // ⚠ 必须是 Int（十进制）：Uint 键按十六进制读（见 SetExtra），写 2048 会变成 0x2048。
    { "trrecmb",   KeyType::Int,  offsetof(Config, trRecMB),           2048 },
    { "pcpresc",   KeyType::Int,  offsetof(Config, pcPrescaler),       0 },
    // pcperiod: "Periodic"（PC 采样周期使能）。**默认 1（开）**。
    //
    // ⚠ 2026-10-14 由 0 改成 1。原来默认关的后果不是"省点带宽"，而是：
    //   `ProgramItmDwt()` 会**无条件清掉** DWT_CTRL 的 bit0/bit9/bit12（见那里），
    //   而 `pcperiod=0` 就不置回来 ⇒ `PCSAMPLENA` 最终是 0。固件本来是想开的
    //   （`DWT_CTRL = 0x00011201` 含 bit12），但它有 `fwOwnsTrace` 门控 ——
    //   接了调试器它就不写，于是我们的写是唯一的写。
    //   现象：Trace Data 窗口**一条 PC 采样都没有**，只剩 `Counter Event CPI`
    //   这类事件计数器刷屏（用户 2026-10-14 报的"很多 CPI 异常"）。
    //   `test/` 下所有脚本本来就是显式传 `pcperiod=1` 的，默认值反而与它们不一致。
    { "pcperiod",  KeyType::Bool, offsetof(Config, pcPeriodic),        1 },
    { "pcdrw",     KeyType::Bool, offsetof(Config, pcDataRw),          0 },
    // 默认全 1 = "所有 ITM 端口都使能"。`def` 是 int，写 0xFFFFFFFF 会触发
    // C4838 收缩转换（位模式其实一样），这里直接写 -1 让值和类型对得上。
    { "ports",     KeyType::Uint, offsetof(Config, itmPorts),          -1 },
    { "priv",      KeyType::Uint, offsetof(Config, itmPriv),           0x00000008 },
    // trunlim: "Unlimited Trace"（等价 DbgCM 的 UNLIMITED_TRACE）。本驱动的块链
    // （P1-1）本来就是无上限，这一位的作用是把它报进 TraceOpt 给 uVision。
    { "trunlim",   KeyType::Bool, offsetof(Config, unlimitedTrace),    0 },
    // events: "Trace Events" 的 6 个复选框 —— TraceOpt 的事件位
    // （TRACE_EXCTRC 0x0010 … TRACE_FOLD 0x0200；TRACE_CYCCNT 0x0400 没有复选框）。
    // 消费端在 ProgramItmDwt()：逐位映射到 DWT_CTRL 的 bit16..bit22。
    { "events",    KeyType::Uint, offsetof(Config, traceEvents),       0 },
    // Flash page
    { "erase",     KeyType::Int,  offsetof(Config, eraseMode),         kEraseSectors },
    { "program",   KeyType::Bool, offsetof(Config, program),           1 },
    { "fdverify",  KeyType::Bool, offsetof(Config, verify),            0 },
    { "resetrun",  KeyType::Bool, offsetof(Config, resetAndRun),       0 },
    // Pack page
    { "pdsc",      KeyType::Bool, offsetof(Config, pdscEnable),        1 },
    { "pdsclog",   KeyType::Bool, offsetof(Config, pdscLog),           0 },
};

int  ExtraInt(const Config& c, size_t off)  { return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(&c) + off); }
bool ExtraBool(const Config& c, size_t off) { return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(&c) + off); }
unsigned ExtraUint(const Config& c, size_t off) { return *reinterpret_cast<const unsigned*>(reinterpret_cast<const char*>(&c) + off); }

int ExtraValue(const Config& c, const ExtraKey& k) {
    switch (k.type) {
    case KeyType::Int:  return ExtraInt(c, k.offset);
    case KeyType::Bool: return ExtraBool(c, k.offset) ? 1 : 0;
    default:            return static_cast<int>(ExtraUint(c, k.offset));
    }
}

void SetExtra(Config& c, const ExtraKey& k, const char* val) {
    char* base = reinterpret_cast<char*>(&c) + k.offset;
    switch (k.type) {
    case KeyType::Int:
        *reinterpret_cast<int*>(base) = static_cast<int>(strtol(val, nullptr, 0));
        break;
    case KeyType::Bool:
        *reinterpret_cast<bool*>(base) = (strtol(val, nullptr, 0) != 0);
        break;
    default:
        // Uint keys are bit masks (ports / priv / events) and Format() writes
        // them as 0x..., so hex is their native form.  strtoul(...,16) accepts
        // an explicit 0x prefix too, which is what Format() emits.
        // Base 0 here silently turned "ports=FFFFFFF0" into 0, because the
        // digits look decimal until the first 'F' (bug.md §I).
        *reinterpret_cast<unsigned*>(base) = static_cast<unsigned>(strtoul(val, nullptr, 16));
        break;
    }
}

void Defaults(Config& c) {
    c.rddiDll[0] = '\0';
    c.iface      = kDefaultIface;
    c.clockKhz   = kDefaultClock;
    c.swoBaud    = kDefaultSwo;
    c.cpuKhz     = kDefaultCpu;
    c.flm[0]     = '\0';
    c.ramAddr    = kDefaultRamAddr;
    c.ramSize    = kDefaultRamSize;
    c.clockHz    = 0;

    for (const ExtraKey& k : kExtraKeys) {
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", k.def);
        SetExtra(c, k, buf);
    }
    c.pdscLogFile[0] = '\0';
    c.trFile[0]  = '\0';
    c.trRec[0]   = '\0';
    c.device[0]  = '\0';
    c.iromStart  = 0;
    c.iromSize   = 0;
}

// Separator between "key=value" pairs.  Format() writes them space-separated,
// which is also what Keil's own option strings look like
// ("-FO7 -FD20000000 -FC800"), so that is the primary form.  ';' is accepted as
// well: it is how this format tends to be written down by hand, and without it
// the first value swallowed the entire rest of the string, so every key after
// the first was silently dropped (bug.md §I).
bool IsSep(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';';
}

bool KeyEq(const char* tok, size_t len, const char* key) {
    size_t i = 0;
    for (; key[i]; ++i) {
        if (i >= len) return false;
        char a = tok[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != key[i]) return false;
    }
    return i == len;
}

void Append(char* out, size_t cch, const char* piece) {
    if (out[0]) strncat_s(out, cch, " ", _TRUNCATE);
    strncat_s(out, cch, piece, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// Finding a .FLM on disk
//
// uVision names the algorithm either without a path ("-FF0STM32F10x_128", read
// as <Keil>\ARM\Flash\<name>.FLM) or as a pack reference:
//
//     -FP0($$Device:STM32F407VETx$CMSIS\Flash\STM32F4xx_512.FLM)
//
// which means "CMSIS\Flash\... .FLM of the device's own pack".  Resolving that
// needs the pack root, and on a current MDK install the packs are NOT under
// <Keil>\ARM\PACK - uVision keeps its own root in TOOLS.INI:
//
//     RTEPATH="D:\Keil_v5\Packs"
//
// so that is where to look first.  (Learned the hard way: everything else
// resolved to nothing and the loader then tried to parse the literal
// "$$Device:..." text as an ELF file.)
// ---------------------------------------------------------------------------
bool FileExistsA(const char* p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExistsA(const char* p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// True when `dir` looks like a Keil install root and not, say, its UV4 folder:
/// it must carry TOOLS.INI or an ARM directory.
bool LooksLikeKeilRoot(const char* dir) {
    if (!DirExistsA(dir)) return false;
    char probe[MAX_PATH];
    _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s\\TOOLS.INI", dir);
    if (FileExistsA(probe)) return true;
    _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s\\ARM", dir);
    return DirExistsA(probe);
}

/// A Keil install root, e.g. C:\Keil_v5 - the environment uVision sets first,
/// then the usual drives (same policy as the Settings dialog).
bool KeilRootA(char* out, size_t cch) {
    static const char* kEnv[] = { "KEIL_ROOT", "UV4", "KEIL_ARM_ROOT" };
    char cand[MAX_PATH];
    for (size_t i = 0; i < _countof(kEnv); ++i) {
        const DWORD n = GetEnvironmentVariableA(kEnv[i], cand, MAX_PATH);
        if (n == 0 || n >= MAX_PATH || !cand[0]) continue;

        // UV4 points at the executable or at the UV4 folder more often than at
        // the install root, so walk upwards until the directory is one.
        for (int up = 0; up < 3; ++up) {
            if (LooksLikeKeilRoot(cand)) {
                strncpy_s(out, cch, cand, _TRUNCATE);
                return true;
            }
            char* slash = strrchr(cand, '\\');
            if (!slash || slash == cand + 2) break;     // stop at "D:"
            *slash = '\0';
        }
    }

    static const char* kDrive[] = { "C:", "D:", "E:" };
    static const char* kName[]  = { "Keil_v5", "Keil", "Keil_v4", "MDK" };
    char probe[MAX_PATH];
    for (size_t d = 0; d < _countof(kDrive); ++d) {
        for (size_t k = 0; k < _countof(kName); ++k) {
            _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s\\%s\\ARM\\BIN", kDrive[d], kName[k]);
            if (DirExistsA(probe)) {
                _snprintf_s(out, cch, _TRUNCATE, "%s\\%s", kDrive[d], kName[k]);
                return true;
            }
        }
    }
    out[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------
// Pack roots
// ---------------------------------------------------------------------------
const int kMaxPackRoots = 8;

// The tool root uVision handed over (dbgext::szTRoot, "<Keil>\ARM").
char g_toolRoot[MAX_PATH];

// The pack-root list is cached for the process - uVision has to be restarted
// before a newly installed pack shows up anyway - so an answer that arrives
// after the first lookup has to be able to drop the cache.
char g_packRoots[kMaxPackRoots][MAX_PATH];
int  g_packRootCount = -1;

/// Splits a RTEPATH style value ("D:\Keil_v5\Packs", possibly quoted, possibly
/// a ';' separated list) and appends every existing directory in it.
void AddRootList(char roots[][MAX_PATH], int& n, const char* val) {
    if (!val || !*val) return;

    char buf[MAX_PATH * 2];
    strncpy_s(buf, val, _TRUNCATE);

    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ";,\"", &ctx); tok; tok = strtok_s(nullptr, ";,\"", &ctx)) {
        while (*tok == ' ' || *tok == '\t') ++tok;
        size_t l = strlen(tok);
        while (l && (tok[l - 1] == ' ' || tok[l - 1] == '\t' ||
                     tok[l - 1] == '\\' || tok[l - 1] == '/')) {
            tok[--l] = '\0';
        }
        if (!l || n >= kMaxPackRoots || !DirExistsA(tok)) continue;

        bool dup = false;
        for (int i = 0; i < n; ++i) {
            if (_stricmp(roots[i], tok) == 0) { dup = true; break; }
        }
        if (!dup) strncpy_s(roots[n++], MAX_PATH, tok, _TRUNCATE);
    }
}

/// Reads the RTEPATH lines out of a uVision TOOLS.INI.
void ReadIniRtePath(const char* iniPath, char roots[][MAX_PATH], int& n) {
    FILE* f = nullptr;
    if (fopen_s(&f, iniPath, "r") != 0 || !f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, "RTEPATH", 7) != 0) continue;
        char* eq = strchr(p, '=');
        if (eq) AddRootList(roots, n, eq + 1);
    }
    fclose(f);
}

/// Every pack root this machine uses, in the order worth trying.  Cached for
/// the process: uVision must be restarted before a newly installed pack shows
/// up anyway.
int CollectPackRoots(char roots[][MAX_PATH]) {
    char (&cache)[kMaxPackRoots][MAX_PATH] = g_packRoots;
    int& cached = g_packRootCount;

    if (cached < 0) {
        int n = 0;

        // 0. The tool root uVision handed over in its debug block.  <szTRoot> is
        //    "<Keil>\ARM", so the pack root sits directly below it and TOOLS.INI
        //    one level up (on some installs also next to ARM).  This is the only
        //    authoritative answer: the folder-name guesses further down failed on
        //    a "D:\MDK5" install, and the download then reported "no flash
        //    algorithm" with nothing pointing at the cause.
        if (g_toolRoot[0]) {
            char p[MAX_PATH];
            _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\PACK", g_toolRoot);
            AddRootList(cache, n, p);
            _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\TOOLS.INI", g_toolRoot);
            ReadIniRtePath(p, cache, n);
            _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\..\\TOOLS.INI", g_toolRoot);
            ReadIniRtePath(p, cache, n);
        }

        // 1. CMSIS' own variable - independent of uVision.
        char env[MAX_PATH * 2];
        if (GetEnvironmentVariableA("CMSIS_PACK_ROOT", env, sizeof(env)) > 0) {
            AddRootList(cache, n, env);
        }

        // 2. uVision's RTEPATH.  This is the one that is actually in use and
        //    the only place a current MDK keeps its packs.
        char keil[MAX_PATH];
        if (KeilRootA(keil, sizeof(keil))) {
            char ini[MAX_PATH];
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\TOOLS.INI", keil);
            ReadIniRtePath(ini, cache, n);
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\ARM\\TOOLS.INI", keil);
            ReadIniRtePath(ini, cache, n);
            // 3. The classic location, for older installs.
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\ARM\\PACK", keil);
            AddRootList(cache, n, ini);
        }

        // 4. The per-user copy.
        char home[MAX_PATH];
        const DWORD nl = GetEnvironmentVariableA("LOCALAPPDATA", home, MAX_PATH);
        if (nl > 0 && nl < MAX_PATH) {
            char ini[MAX_PATH];
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\Arm\\TOOLS.INI", home);
            ReadIniRtePath(ini, cache, n);
            _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\Arm\\Packs", home);
            AddRootList(cache, n, ini);
        }

        // Worth naming in the log: these are the only places a .FLM is looked
        // for, and getting them wrong is invisible until a download fails.
        if (n) {
            for (int i = 0; i < n; ++i) ORB_LOG_INFO("Cfg: pack root = %s", cache[i]);
        } else {
            ORB_LOG_WARN("Cfg: no Keil pack root found - checked RTEPATH in "
                         "TOOLS.INI, CMSIS_PACK_ROOT and <Keil>\\ARM\\PACK");
        }
        cached = n;
    }

    for (int i = 0; i < cached; ++i) strncpy_s(roots[i], MAX_PATH, cache[i], _TRUNCATE);
    return cached;
}

/// Bounded recursive hunt for an exact file name under `dir`.
bool FindFlmRec(const char* dir, const char* want, int depth, char* out, size_t cch, int& seen) {
    if (depth > 6 || seen > 8000) return false;

    char mask[MAX_PATH];
    _snprintf_s(mask, sizeof(mask), _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(mask, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (++seen > 8000) break;

        char full[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (FindFlmRec(full, want, depth + 1, out, cch, seen)) { found = true; break; }
        } else if (_stricmp(fd.cFileName, want) == 0) {
            strncpy_s(out, cch, full, _TRUNCATE);
            found = true;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/// Resolves a pack relative path such as "CMSIS\Flash\STM32F4xx_512.FLM".
/// The pack is identified by the device, which is only a name, so walk
/// <root>\<vendor>\<pack>\<version>\ - three fixed levels, no recursion.
bool ResolvePackRel(const char* rel, char* out, size_t cch) {
    if (!rel || !*rel) return false;

    char roots[kMaxPackRoots][MAX_PATH];
    const int n = CollectPackRoots(roots);

    for (int i = 0; i < n; ++i) {
        char vendorMask[MAX_PATH];
        _snprintf_s(vendorMask, sizeof(vendorMask), _TRUNCATE, "%s\\*", roots[i]);

        WIN32_FIND_DATAA fv;
        HANDLE hv = FindFirstFileA(vendorMask, &fv);
        if (hv == INVALID_HANDLE_VALUE) continue;
        do {
            if (fv.cFileName[0] == '.' ||
                !(fv.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

            char packMask[MAX_PATH];
            _snprintf_s(packMask, sizeof(packMask), _TRUNCATE, "%s\\%s\\*",
                        roots[i], fv.cFileName);
            WIN32_FIND_DATAA fp;
            HANDLE hp = FindFirstFileA(packMask, &fp);
            if (hp == INVALID_HANDLE_VALUE) continue;
            do {
                if (fp.cFileName[0] == '.' ||
                    !(fp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

                char verMask[MAX_PATH];
                _snprintf_s(verMask, sizeof(verMask), _TRUNCATE, "%s\\%s\\%s\\*",
                            roots[i], fv.cFileName, fp.cFileName);
                WIN32_FIND_DATAA fver;
                HANDLE hver = FindFirstFileA(verMask, &fver);
                if (hver == INVALID_HANDLE_VALUE) continue;
                do {
                    if (fver.cFileName[0] == '.' ||
                        !(fver.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

                    char cand[MAX_PATH];
                    _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s\\%s\\%s\\%s\\%s",
                                roots[i], fv.cFileName, fp.cFileName,
                                fver.cFileName, rel);
                    if (FileExistsA(cand)) {
                        strncpy_s(out, cch, cand, _TRUNCATE);
                        FindClose(hver); FindClose(hp); FindClose(hv);
                        return true;
                    }
                } while (FindNextFileA(hver, &fver));
                FindClose(hver);
            } while (FindNextFileA(hp, &fp));
            FindClose(hp);
        } while (FindNextFileA(hv, &fv));
        FindClose(hv);
    }
    return false;
}

/// Appends ".FLM" unless it is already there.
void EnsureFlmSuffix(char* s, size_t cch) {
    const size_t l = strlen(s);
    if (l < 4 || _strnicmp(s + l - 4, ".FLM", 4) != 0) {
        strncat_s(s, cch, ".FLM", _TRUNCATE);
    }
}

/// Resolves a name uVision stored (with or without .FLM, with or without a
/// path, pack reference or not) to a file that exists.
/// False = the algorithm is not on this machine.
bool ResolveFlm(const char* nameOrPath, char* out, size_t cch) {
    out[0] = '\0';
    if (!nameOrPath || !*nameOrPath) return false;
    if (FileExistsA(nameOrPath)) {
        strncpy_s(out, cch, nameOrPath, _TRUNCATE);
        return true;
    }

    // "$$Device:<Device>$<rel>": <rel> is relative to the device's own pack.
    const char* rel = nullptr;
    if (_strnicmp(nameOrPath, "$$Device:", 9) == 0) {
        const char* dollar = strchr(nameOrPath + 9, '$');
        if (dollar && dollar[1]) rel = dollar + 1;
    }

    char full[MAX_PATH];
    strncpy_s(full, nameOrPath, _TRUNCATE);
    EnsureFlmSuffix(full, sizeof(full));

    const char* src = rel ? rel : nameOrPath;
    const char* slash = strrchr(src, '\\');
    if (!slash) slash = strrchr(src, '/');
    char leaf[MAX_PATH];
    strncpy_s(leaf, slash ? slash + 1 : src, _TRUNCATE);
    if (!leaf[0]) return false;
    EnsureFlmSuffix(leaf, sizeof(leaf));

    // 1. The classic layout: <Keil>\ARM\Flash\<name>.FLM.
    char keil[MAX_PATH];
    if (KeilRootA(keil, sizeof(keil))) {
        char cand[MAX_PATH];
        _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s\\ARM\\Flash\\%s", keil, leaf);
        if (FileExistsA(cand)) {
            strncpy_s(out, cch, cand, _TRUNCATE);
            return true;
        }
    }

    // 2. Exactly where the pack reference points.
    if (rel && ResolvePackRel(rel, out, cch)) return true;

    // 3. Same file name, any pack.
    char roots[kMaxPackRoots][MAX_PATH];
    const int nr = CollectPackRoots(roots);
    for (int i = 0; i < nr; ++i) {
        int seen = 0;
        if (FindFlmRec(roots[i], leaf, 0, out, cch, seen)) return true;
    }

    // 4. Legacy algorithms can also sit in a sub folder of ARM\Flash.
    if (keil[0]) {
        char base[MAX_PATH];
        _snprintf_s(base, sizeof(base), _TRUNCATE, "%s\\ARM\\Flash", keil);
        int seen = 0;
        if (FindFlmRec(base, leaf, 0, out, cch, seen)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Device -> algorithm
//
// The Utilities page only carries the Flash-Download settings of the driver
// that is selected there, so switching a project over to us leaves that page
// without an algorithm for our driver while uVision still knows the device
// perfectly well.  And the device is all that is needed: Keil spells both pack
// and algorithm after the device family
//
//     STM32F407VETx -> Packs\Keil\STM32F4xx_DFP\<ver>\CMSIS\Flash\STM32F4xx_*.FLM
//
// and the flash window of the device picks the right one of the numbered
// variants ("_512" for a 512 KB part).
// ---------------------------------------------------------------------------

char ToUpper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// How far a file name (without its extension) follows the device name.  An 'x'
/// in the file name counts as a wildcard, which is what makes the family
/// spelling work: "STM32F4xx_512" scores 9 against "STM32F407VETx" while
/// "STM32F1xx_128" only reaches 6.
int DeviceStemScore(const char* device, const char* stem) {
    int i = 0;
    for (; device[i] && stem[i]; ++i) {
        const char a = ToUpper(device[i]);
        const char b = ToUpper(stem[i]);
        if (a != b && b != 'X') break;
    }
    return i;
}

/// The best .FLM seen so far for a device.
struct FlmPick {
    int  score;             // -1 = nothing found yet
    int  rank;              // 0 = size matches, then growing mismatch
    char path[MAX_PATH];
};

/// Flash size encoded in an algorithm name: "STM32F4xx_512" -> 512.  A suffix
/// that is not a plain number ("_OTP", "_1024dual") is none, and 0 says so.
/// The name may still carry its extension ("STM32F4xx_512.FLM").
unsigned KbFromName(const char* name) {
    const char* num = strrchr(name, '_');
    if (!num || !num[1]) return 0;

    unsigned kb = 0;
    for (const char* q = num + 1; *q && *q != '.'; ++q) {
        if (*q < '0' || *q > '9') return 0;
        kb = kb * 10u + static_cast<unsigned>(*q - '0');
    }
    return kb;
}

/// Offers every *.FLM in `dir` to `best`.
void ConsiderFlmDir(const char* dir, const char* device, unsigned needKb, FlmPick& best) {
    char mask[MAX_PATH];
    _snprintf_s(mask, sizeof(mask), _TRUNCATE, "%s\\*.FLM", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(mask, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char stem[MAX_PATH];
        strncpy_s(stem, fd.cFileName, _TRUNCATE);
        if (char* dot = strrchr(stem, '.')) *dot = '\0';

        const int score = DeviceStemScore(device, stem);
        if (score < 5) continue;            // never fall back on another family

        const unsigned kb = KbFromName(stem);

        int rank = 0;
        if (needKb) {
            if (kb == needKb)      rank = 0;                // 512 KB part -> _512
            else if (kb > needKb)  rank = 1000 + static_cast<int>(kb - needKb);
            else if (kb)           rank = 2000 + static_cast<int>(needKb - kb);
            else                   rank = 3000;             // no size in the name
        }

        // The flash size decides, the name only breaks ties.  The other way
        // round "STM32F40xxx_41xxx_OPT" beat "STM32F4xx_512" - its two wildcards
        // match one character more of the device name (bug.md E11).
        const bool better = best.score < 0 || rank < best.rank ||
                            (rank == best.rank && score > best.score);
        if (!better) continue;

        best.score = score;
        best.rank  = rank;
        _snprintf_s(best.path, MAX_PATH, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------------------
// Device -> algorithm, the way uVision itself does it: the pack description
// (PDSC) declares the algorithms of every device and marks one as the default.
//
//   <device Dname="STM32F407VE">
//     <algorithm name="CMSIS/Flash/STM32F4xx_512.FLM" default="1"/>
//     <algorithm name="CMSIS/Flash/STM32F40xxx_41xxx_OPT.FLM"/>
//     <algorithm name="CMSIS/Flash/STM32F4xx_OTP.FLM"/>
//
// "STM32F4xx_512" with the pack-relative path is exactly what uVision writes
// into the Utilities page ("-FF0STM32F4xx_512 ... -FP0($$Device:...)"), so this
// reproduces Keil's own choice instead of guessing from file names.
// ---------------------------------------------------------------------------

/// Reads a whole file into a malloc'ed, NUL terminated buffer.
bool ReadWholeFile(const char* path, char** data, size_t* len) {
    *data = nullptr;
    *len = 0;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 32 * 1024 * 1024) { fclose(f); return false; }

    char* p = static_cast<char*>(malloc(static_cast<size_t>(n) + 1));
    if (!p) { fclose(f); return false; }
    const size_t got = fread(p, 1, static_cast<size_t>(n), f);
    fclose(f);
    if (got != static_cast<size_t>(n)) { free(p); return false; }
    p[n] = '\0';
    *data = p;
    *len = static_cast<size_t>(n);
    return true;
}

bool IsXmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Value of `attr="..."` in the tag that ends at `end`.  The name has to start
/// a word, otherwise "name" would also answer for "Dname".
bool TagAttr(const char* tag, const char* end, const char* attr, char* out, size_t cch) {
    const size_t alen = strlen(attr);
    for (const char* p = tag; p + alen < end; ++p) {
        if (p > tag && !IsXmlSpace(p[-1]) && p[-1] != '<') continue;
        if (_strnicmp(p, attr, alen) != 0) continue;

        const char* q = p + alen;
        while (q < end && IsXmlSpace(*q)) ++q;
        if (q >= end || *q != '=') continue;
        while (++q < end && IsXmlSpace(*q)) {}
        if (q >= end || (*q != '"' && *q != '\'')) continue;

        const char quote = *q++;
        const char* v = q;
        while (q < end && *q != quote) ++q;

        size_t n = static_cast<size_t>(q - v);
        if (n >= cch) n = cch - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

/// Match of a PDSC device name ("STM32F407VE") against the device uVision
/// reports ("STM32F407VETx").  uVision appends package/temperature letters that
/// the pack name does not carry, so the pack name may stop early; an 'x' on
/// either side is a wildcard.  -1 = different parts.
int DeviceMatchScore(const char* device, const char* dname) {
    int i = 0;
    for (; device[i] && dname[i]; ++i) {
        const char a = ToUpper(device[i]);
        const char b = ToUpper(dname[i]);
        if (a != b && a != 'X' && b != 'X') break;
    }
    if (dname[i]) return -1;                        // pack name is longer
    if (i < 5) return -1;
    return device[i] ? i : i + 1000;                // exact spelling wins
}

/// Offers one <algorithm .../> tag of `device` to `best`.
void ConsiderAlgTag(const char* tag, const char* end, const char* packDir,
                    unsigned needKb, int devScore, FlmPick& best) {
    char rel[MAX_PATH] = "";
    if (!TagAttr(tag, end, "name", rel, sizeof(rel)) &&
        !TagAttr(tag, end, "file", rel, sizeof(rel))) {
        return;
    }
    if (!rel[0]) return;

    char dflt[16] = "";
    TagAttr(tag, end, "default", dflt, sizeof(dflt));
    const bool isDefault = dflt[0] == '1' || _stricmp(dflt, "true") == 0 ||
                           _stricmp(dflt, "yes") == 0;

    const char* leaf = rel;
    for (const char* q = rel; *q; ++q) {
        if (*q == '/' || *q == '\\') leaf = q + 1;
    }
    const unsigned kb = KbFromName(leaf);

    // Default first, then the size in the name.  "CMSIS/Flash/x.FLM" is relative
    // to the pack folder, which is where the PDSC itself lives.
    int rank = isDefault ? 0 : 100000;
    if (needKb) {
        if (kb == needKb)      rank += 0;
        else if (kb > needKb)  rank += 1000 + static_cast<int>(kb - needKb);
        else if (kb)           rank += 2000 + static_cast<int>(needKb - kb);
        else                   rank += 3000;        // no size in the name
    }

    char full[MAX_PATH];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", packDir, rel);
    for (char* q = full; *q; ++q) {
        if (*q == '/') *q = '\\';
    }
    if (!FileExistsA(full)) return;

    const bool better = best.score < 0 || devScore > best.score ||
                        (devScore == best.score && rank < best.rank);
    if (!better) return;

    best.score = devScore;
    best.rank  = rank;
    strncpy_s(best.path, MAX_PATH, full, _TRUNCATE);
}

/// Every <algorithm> of the devices `device` matches in one PDSC.
void ScanPdsc(const char* pdscPath, const char* packDir, const char* device,
              unsigned needKb, FlmPick& best) {
    char* buf = nullptr;
    size_t len = 0;
    if (!ReadWholeFile(pdscPath, &buf, &len)) return;

    const char* p = buf;
    while ((p = strstr(p, "<device")) != nullptr) {
        const char* t = p + 7;
        if (!IsXmlSpace(*t)) { p = t; continue; }   // <devices> is not a device

        const char* tagEnd = strchr(t, '>');
        if (!tagEnd) break;

        char dname[128] = "";
        const int score = (TagAttr(p, tagEnd, "Dname", dname, sizeof(dname)) && dname[0])
                              ? DeviceMatchScore(device, dname)
                              : -1;
        if (score >= 5 && tagEnd[-1] != '/') {
            // The body of the element holds one <algorithm> per algorithm the
            // pack offers for this very device.
            const char* close = strstr(tagEnd, "</device>");
            const char* bodyEnd = close ? close : buf + len;
            const char* a = tagEnd + 1;
            while (a < bodyEnd && (a = strstr(a, "<algorithm")) != nullptr && a < bodyEnd) {
                const char* aEnd = strchr(a, '>');
                if (!aEnd || aEnd > bodyEnd) break;
                ConsiderAlgTag(a, aEnd, packDir, needKb, score, best);
                a = aEnd + 1;
            }
            p = close ? close + 9 : bodyEnd;
            continue;
        }
        p = tagEnd + 1;
    }

    free(buf);
}

/// Walks the installed packs for the algorithms their descriptions give the
/// device.  Stops as soon as the default algorithm of the right size was found.
bool ResolveByPdsc(const char* device, unsigned needKb, char* out, size_t cch) {
    FlmPick best = { -1, 0, "" };

    char roots[kMaxPackRoots][MAX_PATH];
    const int n = CollectPackRoots(roots);

    for (int i = 0; i < n && best.rank != 0; ++i) {
        char vendorMask[MAX_PATH];
        _snprintf_s(vendorMask, sizeof(vendorMask), _TRUNCATE, "%s\\*", roots[i]);

        WIN32_FIND_DATAA fv;
        HANDLE hv = FindFirstFileA(vendorMask, &fv);
        if (hv == INVALID_HANDLE_VALUE) continue;
        do {
            if (fv.cFileName[0] == '.' ||
                !(fv.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

            char packMask[MAX_PATH];
            _snprintf_s(packMask, sizeof(packMask), _TRUNCATE, "%s\\%s\\*",
                        roots[i], fv.cFileName);
            WIN32_FIND_DATAA fp;
            HANDLE hp = FindFirstFileA(packMask, &fp);
            if (hp == INVALID_HANDLE_VALUE) continue;
            do {
                if (fp.cFileName[0] == '.' ||
                    !(fp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

                char verMask[MAX_PATH];
                _snprintf_s(verMask, sizeof(verMask), _TRUNCATE, "%s\\%s\\%s\\*",
                            roots[i], fv.cFileName, fp.cFileName);
                WIN32_FIND_DATAA fver;
                HANDLE hver = FindFirstFileA(verMask, &fver);
                if (hver == INVALID_HANDLE_VALUE) continue;
                do {
                    if (fver.cFileName[0] == '.' ||
                        !(fver.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

                    // <root>\<vendor>\<pack>\<version> is the pack folder - both
                    // its *.pdsc and the algorithm paths inside it start here.
                    char dir[MAX_PATH];
                    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s\\%s\\%s",
                                roots[i], fv.cFileName, fp.cFileName, fver.cFileName);

                    char pdscMask[MAX_PATH];
                    _snprintf_s(pdscMask, sizeof(pdscMask), _TRUNCATE, "%s\\*.pdsc", dir);
                    WIN32_FIND_DATAA fd;
                    HANDLE h = FindFirstFileA(pdscMask, &fd);
                    if (h == INVALID_HANDLE_VALUE) continue;
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        char pdsc[MAX_PATH];
                        _snprintf_s(pdsc, sizeof(pdsc), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
                        ScanPdsc(pdsc, dir, device, needKb, best);
                    } while (best.rank != 0 && FindNextFileA(h, &fd));
                    FindClose(h);
                } while (best.rank != 0 && FindNextFileA(hver, &fver));
                FindClose(hver);
            } while (best.rank != 0 && FindNextFileA(hp, &fp));
            FindClose(hp);
        } while (best.rank != 0 && FindNextFileA(hv, &fv));
        FindClose(hv);
    }

    if (best.score < 0) return false;
    ORB_LOG_INFO("Cfg: flash algorithm \"%s\" is what the device's pack "
                 "description declares (default=%s)", best.path,
                 best.rank < 100000 ? "yes" : "no");
    strncpy_s(out, cch, best.path, _TRUNCATE);
    return true;
}

/// Algorithm of `device` (with `flashSize` bytes of flash) as found in the
/// packs installed on this machine.  False = no pack here describes the device.
bool ResolveByDevice(const char* device, unsigned flashSize, char* out, size_t cch) {
    if (!device || !*device) return false;

    const unsigned needKb = flashSize ? ((flashSize + 1023u) / 1024u) : 0u;
    if (!needKb) {
        ORB_LOG_WARN("Cfg: flash size of \"%s\" unknown - picking the algorithm "
                     "by name alone", device);
    }

    // Best source by far: the device's pack description says which algorithms
    // belong to it and which one is the default - no guessing involved.
    if (ResolveByPdsc(device, needKb, out, cch)) return true;

    FlmPick best = { -1, 0, "" };

    char roots[kMaxPackRoots][MAX_PATH];
    const int n = CollectPackRoots(roots);

    for (int i = 0; i < n; ++i) {
        // <root>\<vendor>\<pack>\<version>\{CMSIS\Flash|Flash}\*.FLM - three
        // fixed levels, the same shape ResolvePackRel walks.
        char vendorMask[MAX_PATH];
        _snprintf_s(vendorMask, sizeof(vendorMask), _TRUNCATE, "%s\\*", roots[i]);

        WIN32_FIND_DATAA fv;
        HANDLE hv = FindFirstFileA(vendorMask, &fv);
        if (hv == INVALID_HANDLE_VALUE) continue;
        do {
            if (fv.cFileName[0] == '.' ||
                !(fv.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

            char packMask[MAX_PATH];
            _snprintf_s(packMask, sizeof(packMask), _TRUNCATE, "%s\\%s\\*",
                        roots[i], fv.cFileName);
            WIN32_FIND_DATAA fp;
            HANDLE hp = FindFirstFileA(packMask, &fp);
            if (hp == INVALID_HANDLE_VALUE) continue;
            do {
                if (fp.cFileName[0] == '.' ||
                    !(fp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

                char verMask[MAX_PATH];
                _snprintf_s(verMask, sizeof(verMask), _TRUNCATE, "%s\\%s\\%s\\*",
                            roots[i], fv.cFileName, fp.cFileName);
                WIN32_FIND_DATAA fver;
                HANDLE hver = FindFirstFileA(verMask, &fver);
                if (hver == INVALID_HANDLE_VALUE) continue;
                do {
                    if (fver.cFileName[0] == '.' ||
                        !(fver.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

                    char dir[MAX_PATH];
                    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s\\%s\\%s\\CMSIS\\Flash",
                                roots[i], fv.cFileName, fp.cFileName, fver.cFileName);
                    ConsiderFlmDir(dir, device, needKb, best);

                    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s\\%s\\%s\\Flash",
                                roots[i], fv.cFileName, fp.cFileName, fver.cFileName);
                    ConsiderFlmDir(dir, device, needKb, best);
                } while (FindNextFileA(hver, &fver));
                FindClose(hver);
            } while (FindNextFileA(hp, &fp));
            FindClose(hp);
        } while (FindNextFileA(hv, &fv));
        FindClose(hv);
    }

    // Older installs keep their algorithms next to uVision itself.
    char keil[MAX_PATH];
    if (KeilRootA(keil, sizeof(keil))) {
        char dir[MAX_PATH];
        _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\ARM\\Flash", keil);
        ConsiderFlmDir(dir, device, needKb, best);
    }

    if (best.score < 0) return false;
    strncpy_s(out, cch, best.path, _TRUNCATE);
    return true;
}

} // namespace

Config& Get() {
    // Leaked on purpose: uVision may still call us while the CRT tears statics
    // down (same reasoning as agdi::Get()).
    static Config* c = nullptr;
    if (!c) {
        c = new Config();
        Defaults(*c);
    }
    return *c;
}

void Reset() {
    Defaults(Get());
}

bool Parse(const char* args) {
    Config& c = Get();
    Defaults(c);
    if (!args) return false;

    bool any = false;
    const char* p = args;
    while (*p) {
        while (IsSep(*p)) ++p;
        if (!*p) break;

        const char* key = p;
        while (*p && *p != '=' && !IsSep(*p)) ++p;
        const size_t klen = static_cast<size_t>(p - key);
        if (*p != '=') continue;        // bare token: skip it
        ++p;                            // past '='

        // Values may be quoted, which is how a path with spaces survives.
        char val[320];
        size_t vlen = 0;
        if (*p == '"') {
            ++p;
            while (*p && *p != '"' && vlen + 1 < sizeof(val)) val[vlen++] = *p++;
            if (*p == '"') ++p;
        } else {
            while (*p && !IsSep(*p) && vlen + 1 < sizeof(val)) val[vlen++] = *p++;
        }
        val[vlen] = '\0';

        if (KeyEq(key, klen, "rddi")) {
            strncpy_s(c.rddiDll, val, _TRUNCATE);
            any = true;
        } else if (KeyEq(key, klen, "iface")) {
            c.iface = atoi(val);
            any = true;
        } else if (KeyEq(key, klen, "clock")) {
            c.clockKhz = atoi(val);
            any = true;
        } else if (KeyEq(key, klen, "swo")) {
            c.swoBaud = atoi(val);
            any = true;
        } else if (KeyEq(key, klen, "cpu")) {
            c.cpuKhz = atoi(val);
            any = true;
        } else if (KeyEq(key, klen, "flm")) {
            // Flash algorithm. Kept in the same option string as everything
            // else so the Flash-Download dialog (DllUv3Cap(110)) and the Debug
            // dialog share one source of truth.
            strncpy_s(c.flm, val, _TRUNCATE);
            any = true;
        } else if (KeyEq(key, klen, "ram")) {
            // ram=<addr>,<size>, both hexadecimal - the same syntax
            // orb::agdi::FlashOptions() accepts, so either parser agrees.
            unsigned a = 0, s = 0;
            if (sscanf_s(val, "%x,%x", &a, &s) == 2 && s > 0) {
                c.ramAddr = a;
                c.ramSize = s;
                any = true;
            }
        } else if (KeyEq(key, klen, "pdsclogfile")) {
            strncpy_s(c.pdscLogFile, val, _TRUNCATE);
            any = true;
        } else if (KeyEq(key, klen, "trfile")) {
            strncpy_s(c.trFile, val, _TRUNCATE);
            any = true;
        } else if (KeyEq(key, klen, "trrec")) {
            strncpy_s(c.trRec, val, _TRUNCATE);
            any = true;
        } else {
            // Everything the setup sheet adds beyond what the driver acts on.
            for (const ExtraKey& k : kExtraKeys) {
                if (!KeyEq(key, klen, k.name)) continue;
                SetExtra(c, k, val);
                any = true;
                break;
            }
        }
    }
    return any;
}

int SwoAcprBaseKhz(const Config& c) {
    // 目标侧 ACPR 的基数 = **TPIU 异步输入的那个物理时钟**。它与下面两个量是**不同
    // 角色**，不要因为"名字都像 trace 时钟"就合并：
    //   * `SwoBaud()`（本文件下方）= **探针侧**采集率（并口档也用它推
    //     `SetSWOBaudrate`，`COMPAT_ANALYSIS.md:1194-1198`）；
    //   * TRACE_CLK VTR = Trace Data 窗口的**时间轴**（`ORBAGDI_TraceWin.cpp` 里
    //     `p_create("TRACE_CLK")` 那一段）。
    // 官方把这三者塞进同一个 `TPIU_Clock`（`RegisterTraceClock()`，
    // `DbgCM/AGDI.CPP:4247-4263` 头注释就写 "Register TPIU Clock to uVision (via
    // TRACE_CLK)"；`Trace.cpp:1963 SWV_Setup(TPIU_Clock / ((SWV_Pre & 0x1FFF)+1))`、
    // `:1979 WriteD32(TPIU_ASYNCLKPRES, ...)` 又拿它当 ACPR 基数）—— "一个量三用"
    // 只在"用户填的就是目标 TPIU 输入时钟"这个前提下自洽。
    //
    // ⚠ **该物理时钟是 HCLK 还是独立 TRACECLKIN，源码/反汇编层面判不了**（官方源码
    //   与我们的反汇编都只证明"官方按声明的 `TPIU_Clock` 算"），只能靠一次 A/B 实测
    //   （`Todo.md` 已登记：不给 `swo=`，`tclkc=1 swopresc=4` 与
    //   `tclkc=0 tclk=8000 swopresc=4` 各跑一次，**B 静默 ⇒ TRACECLKIN 独立**）。
    //   * **H1｜ACPR 输入 = 真实 HCLK**（本板实测方向支持：同步档位率 = HCLK、
    //     `COMPAT_ANALYSIS.md:1144-1149` PE2=TRACECLK 是 MCU 的 AF 输出脚、板上无独立
    //     低速源）⇒ `cpuHz/(ACPR+1)` 使物理位率**恒等于** `baudHz` = 探针受理档 ⇒
    //     返回核心时钟即自洽；此时 host 若把 `tclk` 填成 ≠ 真实 HCLK，**官方按
    //     `TPIU_Clock` 配 ACPR 反与物理位率差 `core/trace` 倍** ⇒ 出错方是官方。
    //   * **H2｜TRACECLKIN 独立** ⇒ 本函数应改成"用户声明的 trace 时钟"
    //     （`useCoreClock ? cpuKhz : traceClockKhz`），否则偏 `cpu/trace` 倍
    //     （168 M/8 M ⇒ 21 倍）⇒ 静默/乱码。
    //   ⇒ **判别之前不改行为**：保持现状 = 核心时钟（`cpuHz` 恰好是"改动前"那个
    //     被除数，行为逐位不变）。这样既不给 H1 引入偏差，也不把"时间轴/探针语义"
    //     当成目标侧基数 —— 后者正是 H1 下官方那个耦合缺陷的形状。
    return c.cpuKhz;
}

int SwoBaud(const Config& c) {
    // The Trace page names a trace port clock and a prescaler; the bit clock the
    // RDDI host has to be told is their quotient.  A prescaler of 0 would divide
    // by zero, so anything below 1 counts as 1.
    //
    // The unit is kHz - a frequency, not the period in ns Keil's own Trace Clock
    // box uses.  That difference is a factor of a million, so the value is kept
    // explicit everywhere it is shown and Connect() checks the result against the
    // core clock rather than trusting it.
    // "Use Core Clock"（`tclkc`）把 trace 端口时钟直接取核心时钟 —— 与 DbgCM 的
    // `TRACE_USE_CORECLK` 同一件事（`AGDI.CPP:4149-4153` 里 `TPIU_Clock =
    // TraceConf.Clk`）。勾上之后 `tclk=` 就不再参与。
    //
    // ★ 这是**探针侧**的量（并口档也用它推 `SetSWOBaudrate`，
    //   `COMPAT_ANALYSIS.md:1194-1198`），与"目标侧 ACPR 基数"是不同角色。
    //   **不要**为了"两处同源"改成 `SwoAcprBaseKhz()` —— 那是把目标侧的未判别问题
    //   绑到探针语义上（H1 下正好是官方那个耦合缺陷的形状）。
    const int clkKhz = c.useCoreClock ? c.cpuKhz : c.traceClockKhz;
    if (clkKhz <= 0) return 0;
    const int presc = (c.swoPrescaler > 0) ? c.swoPrescaler : 1;

    // 64 bit: a hand-edited option string can put a kHz figure past what
    // kHz*1000 fits in an int (2.1 GHz), and a silent overflow here is a wrong
    // SWO baud - which shows up as "no ITM output" and nothing else.
    const int64_t hz = (static_cast<int64_t>(clkKhz) * 1000) / presc;
    if (hz <= 0) return 0;
    // No SWO link runs anywhere near this; the ceiling only exists so the value
    // cannot come back as a negative int.
    const int64_t kCeiling = 200000000;   // 200 MHz
    return static_cast<int>(hz > kCeiling ? kCeiling : hz);
}

void Format(const Config& c, char* out, size_t cch) {
    if (!out || cch == 0) return;
    out[0] = '\0';

    // Only what differs from the default is written, so an untouched driver
    // leaves the project file clean - and Parse() restores the defaults for
    // every key that is absent.
    if (c.rddiDll[0]) {
        char piece[kPathMax + 16];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "rddi=\"%s\"", c.rddiDll);
        Append(out, cch, piece);
    }
    if (c.iface != kDefaultIface) {
        char piece[32];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "iface=%d", c.iface);
        Append(out, cch, piece);
    }
    if (c.clockKhz != kDefaultClock) {
        char piece[32];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "clock=%d", c.clockKhz);
        Append(out, cch, piece);
    }
    if (c.swoBaud != kDefaultSwo) {
        char piece[32];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "swo=%d", c.swoBaud);
        Append(out, cch, piece);
    }
    if (c.cpuKhz != kDefaultCpu) {
        char piece[32];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "cpu=%d", c.cpuKhz);
        Append(out, cch, piece);
    }
    if (c.flm[0]) {
        char piece[kPathMax + 16];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "flm=\"%s\"", c.flm);
        Append(out, cch, piece);
    }
    if (c.ramAddr != kDefaultRamAddr || c.ramSize != kDefaultRamSize) {
        char piece[40];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "ram=%X,%X", c.ramAddr, c.ramSize);
        Append(out, cch, piece);
    }

    // The keys the setup sheet adds.  Only what differs from the default is
    // written; Parse() restores the default for every key that is absent.
    for (const ExtraKey& k : kExtraKeys) {
        const int v = ExtraValue(c, k);
        if (v == k.def) continue;
        char piece[64];
        if (k.type == KeyType::Uint) {
            _snprintf_s(piece, sizeof(piece), _TRUNCATE, "%s=0x%X", k.name, static_cast<unsigned>(v));
        } else {
            _snprintf_s(piece, sizeof(piece), _TRUNCATE, "%s=%d", k.name, v);
        }
        Append(out, cch, piece);
    }
    if (c.pdscLogFile[0]) {
        char piece[kPathMax + 20];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "pdsclogfile=\"%s\"", c.pdscLogFile);
        Append(out, cch, piece);
    }
    if (c.trFile[0]) {
        char piece[kPathMax + 16];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "trfile=\"%s\"", c.trFile);
        Append(out, cch, piece);
    }
    if (c.trRec[0]) {
        char piece[kPathMax + 16];
        _snprintf_s(piece, sizeof(piece), _TRUNCATE, "trrec=\"%s\"", c.trRec);
        Append(out, cch, piece);
    }
}

void Format(char* out, size_t cch) {
    Format(Get(), out, cch);
}

// ---------------------------------------------------------------------------
// Keil's own flash spelling - see DbgCM.cpp:190 (AnalyzeParms)
// ---------------------------------------------------------------------------

/// Reads the flash settings out of a Keil style option string and resolves the
/// algorithm to a file that exists.  `haveRam` reports whether -FD/-FC were
/// present at all.
bool ScanKeil(const char* args, char* flm, size_t cch,
              unsigned& ramAddr, unsigned& ramSize, bool& haveRam) {
    flm[0] = '\0';
    ramAddr = 0;
    ramSize = 0;
    haveRam = false;
    if (!args || !*args) return false;

    char name[MAX_PATH] = "";
    char path[MAX_PATH] = "";

    const char* p = args;
    while (*p) {
        while (IsSep(*p)) ++p;
        if (!*p) break;

        const char* tok = p;
        while (*p && !IsSep(*p)) ++p;
        const size_t tlen = static_cast<size_t>(p - tok);

        // Only "-F..." tokens carry flash information.  The index digit that
        // follows -FF/-FS/-FL/-FP is skipped (one algorithm is enough for us).
        if (tlen < 4 || tok[0] != '-' || tok[1] != 'F') continue;
        const char* v = tok + 3;

        switch (tok[2]) {
        case 'D':                                   // RAM window start
            ramAddr = static_cast<unsigned>(strtoul(v, nullptr, 16));
            break;
        case 'C':                                   // RAM window size
            ramSize = static_cast<unsigned>(strtoul(v, nullptr, 16));
            break;
        case 'F':                                   // algorithm name (no path)
            if (*v && !name[0]) strncpy_s(name, v + 1, _TRUNCATE);
            break;
        case 'P': {                                 // algorithm, full path
            const char* l = strchr(v, '(');
            const char* r = l ? strchr(l, ')') : nullptr;
            if (l && r && r > l + 1) {
                size_t n = static_cast<size_t>(r - l - 1);
                if (n >= sizeof(path)) n = sizeof(path) - 1;
                memcpy(path, l + 1, n);
                path[n] = '\0';
            }
            break;
        }
        default:                                    // -FO/-FN/-FS/-FL: not needed
            break;
        }
    }

    // Resolve the algorithm.  -FP<n>(...) is tried first, but that value is
    // normally a pack reference ("$$Device:<Device>$CMSIS\Flash\x.FLM") which
    // only turns into a path once the pack root is known - so the plain
    // -FF<n><name> is tried as well, and finally the bare file name.
    bool have = false;
    char res[MAX_PATH];

    if (path[0] && ResolveFlm(path, res, sizeof(res))) {
        strncpy_s(flm, cch, res, _TRUNCATE);
        have = true;
    } else if (name[0] && ResolveFlm(name, res, sizeof(res))) {
        strncpy_s(flm, cch, res, _TRUNCATE);
        have = true;
    } else {
        // Nothing on disk matches.  Handing the raw reference to the loader
        // would only produce "cannot parse '$$Device:...' as an ELF32 image",
        // which sends the reader off in the wrong direction - say what is
        // actually missing, and leave `flm` empty so the caller can try its
        // next source.
        const char* held = path[0] ? path : name;
        if (held[0]) {
            if (_strnicmp(held, "$$Device:", 9) == 0) {
                char dev[64] = "";
                const char* s = held + 9;
                const char* dollar = strchr(s, '$');
                if (dollar) {
                    size_t n = static_cast<size_t>(dollar - s);
                    if (n >= sizeof(dev)) n = sizeof(dev) - 1;
                    memcpy(dev, s, n);
                    dev[n] = '\0';
                }
                ORB_LOG_ERROR("Cfg: flash algorithm \"%s\" is not installed - the "
                              "device pack for \"%s\" is missing", held, dev);
            } else {
                ORB_LOG_ERROR("Cfg: flash algorithm \"%s\" is not installed on "
                              "this machine", held);
            }
        }
    }
    haveRam = (ramSize != 0);
    return have;
}

bool ParseKeilFlash(const char* args) {
    char flm[kPathMax];
    unsigned ramStart = 0, ramLen = 0;
    bool haveRam = false;
    const bool have = ScanKeil(args, flm, sizeof(flm), ramStart, ramLen, haveRam);

    Config& c = Get();
    if (have) strncpy_s(c.flm, flm, _TRUNCATE);
    // An explicit ram= in our own string always wins over Keil's -FD/-FC.
    if (haveRam && c.ramAddr == kDefaultRamAddr && c.ramSize == kDefaultRamSize) {
        c.ramAddr = ramStart;
        c.ramSize = ramLen;
    }
    return have;
}

// ---------------------------------------------------------------------------
// What uVision already knows about the target
// ---------------------------------------------------------------------------
void AdoptUVision(const void* dbgBlk) {
    if (!dbgBlk) return;
    const dbgblk* d = static_cast<const dbgblk*>(dbgBlk);

    // Detected values are kept apart from the project's option string: they
    // must never leak into Format(), or an absolute .FLM path would end up in
    // the project file and break as soon as Keil moves.
    Config& a = AutoDetected();
    a.flm[0] = '\0';
    a.ramAddr = 0;
    a.ramSize = 0;
    a.clockHz = 0;
    a.device[0] = '\0';
    a.iromStart = 0;
    a.iromSize  = 0;

    // The device itself, for the setup sheet and for the flash-size based
    // algorithm lookup below.
    strncpy_s(a.device, d->Device, _TRUNCATE);
    a.iromStart = d->Irom.nStart;
    a.iromSize  = d->Irom.nSize;

    // 1. The Flash-Download settings of the Utilities page.  The entry of a
    //    different driver (ULINK2/CMSIS-DAP) still describes *this* device, so
    //    it is a perfectly good source while our own string has none.
    if (d->pDbgX && d->pDbgX->pOpUtil) {
        int entries = 0;
        for (const XOPT* o = d->pDbgX->pOpUtil; o; o = o->next) {
            ++entries;
            if (!o->name || !*o->name) continue;

            // uVision keeps one entry per driver and only a flash carrying one
            // can help here.  Printing them is what tells "the page has nothing
            // for us" and "the reference did not resolve" apart in the log.
            if (!strstr(o->name, "-F")) continue;
            ORB_LOG_INFO("Cfg: Utilities entry \"%s\" = \"%s\"",
                         o->key ? o->key : "?", o->name);

            char flm[kPathMax];
            unsigned rAddr = 0, rSize = 0;
            bool haveRam = false;
            if (ScanKeil(o->name, flm, sizeof(flm), rAddr, rSize, haveRam)) {
                strncpy_s(a.flm, flm, _TRUNCATE);
                if (haveRam) { a.ramAddr = rAddr; a.ramSize = rSize; }
                ORB_LOG_INFO("Cfg: flash algorithm auto-detected from the Utilities "
                             "page (entry \"%s\")", o->key ? o->key : "?");
                break;
            }
        }
        ORB_LOG_INFO("Cfg: the Utilities page offers %d option entry(s)", entries);
    }

    // 2. The device's own pack.  Independent of the Utilities page and the only
    //    source left when that page holds no algorithm for this driver.
    if (!a.flm[0] && d->Device[0]) {
        unsigned flash = d->Irom.nSize;
        if (!flash && d->pDbgX && d->pDbgX->uIROM1) flash = d->pDbgX->rOCR[3].nSize;

        char res[MAX_PATH];
        if (ResolveByDevice(d->Device, flash, res, sizeof(res))) {
            strncpy_s(a.flm, res, _TRUNCATE);
            ORB_LOG_INFO("Cfg: flash algorithm auto-detected from the device \"%s\" "
                         "(flash 0x%X)", d->Device, flash);
        } else if (ResolveFlm(d->Device, res, sizeof(res))) {
            strncpy_s(a.flm, res, _TRUNCATE);
            ORB_LOG_INFO("Cfg: flash algorithm auto-detected from the device \"%s\"",
                         d->Device);
        }
    }

    // 3. The on-chip RAM the algorithm is copied into and runs from.
    if (!a.ramSize && d->Iram.nSize) {
        a.ramAddr = d->Iram.nStart;
        a.ramSize = d->Iram.nSize;
    }

    // 4. Core clock: only the algorithm's timing loops care, but they need it.
    if (d->Clock) a.clockHz = d->Clock;

    ORB_LOG_INFO("Cfg: uVision target: \"%s\" / \"%s\", IRAM 0x%08X+0x%X, "
                 "IROM 0x%08X+0x%X, clock %u Hz",
                 d->Device, d->Vendor, d->Iram.nStart, d->Iram.nSize,
                 d->Irom.nStart, d->Irom.nSize, d->Clock);
    if (a.flm[0]) ORB_LOG_INFO("Cfg: auto-detected flash algorithm = %s", a.flm);
}

// ---------------------------------------------------------------------------
// The tool root uVision reports (dbgext::szTRoot)
// ---------------------------------------------------------------------------
// Everything else in this file is a guess at where Keil was installed; this is
// the one answer that comes from uVision itself, so it is recorded before the
// first pack lookup and the cached pack-root list is dropped whenever it
// changes.
void SetToolRoot(const char* armDir) {
    char v[MAX_PATH];
    strncpy_s(v, (armDir && *armDir) ? armDir : "", _TRUNCATE);

    // uVision's value carries a trailing separator ("D:\MDK5\ARM\"), and every
    // use below appends "\something" - keeping it produced the double separator
    // seen in the log as "D:\MDK5\ARM\\PACK".  Win32 tolerates it, but it makes
    // the log harder to read and would break an exact-string comparison.
    size_t n = strlen(v);
    while (n && (v[n - 1] == '\\' || v[n - 1] == '/')) v[--n] = '\0';

    if (_stricmp(g_toolRoot, v) == 0) return;
    strncpy_s(g_toolRoot, v, _TRUNCATE);
    g_packRootCount = -1;      // the cached list was built without this answer
}

int PackRootCount() {
    char roots[kMaxPackRoots][MAX_PATH];
    return CollectPackRoots(roots);
}

bool PackRoot(int index, char* out, size_t cch) {
    if (out && cch) out[0] = '\0';
    if (!out || !cch) return false;

    char roots[kMaxPackRoots][MAX_PATH];
    const int n = CollectPackRoots(roots);
    if (index < 0 || index >= n) return false;

    strncpy_s(out, cch, roots[index], _TRUNCATE);
    return true;
}

Config& AutoDetected() {
    // Leaked on purpose, like Get().
    static Config* a = nullptr;
    if (!a) {
        a = new Config();
        Defaults(*a);
    }
    return *a;
}

Config Effective() {
    Config r = Get();
    const Config& a = AutoDetected();
    if (!r.flm[0] && a.flm[0]) strncpy_s(r.flm, a.flm, _TRUNCATE);
    if (r.ramAddr == kDefaultRamAddr && r.ramSize == kDefaultRamSize && a.ramSize) {
        r.ramAddr = a.ramAddr;
        r.ramSize = a.ramSize;
    }
    // clockHz is the target's *oscillator*.  That is what the flash algorithm's
    // timing loops want, and it is all uVision tells us: dbgblk::Clock is the
    // only clock field in the whole structure (ComTyp.H).
    if (r.clockHz == 0) r.clockHz = a.clockHz;

    // cpuKhz is a *different quantity* - the core clock.  It used to be filled
    // from that same oscillator value, which is a category error: on an STM32F407
    // the debug block reports 12 MHz while the core runs at 168, so every SWO
    // divider came out ~14x wrong and the ITM console stayed silent with nothing
    // to point at.  There is no core clock in the debug block to use instead, so
    // the Trace page's own value (and its documented default) is what stands -
    // and Connect() now says so out loud when the bit clock derived from it is
    // impossible.
    //
    // ⚠ "SWO 分频除哪个时钟"**未判别**（目标侧 ACPR 的基数是 HCLK 还是独立
    // TRACECLKIN），见 `SwoAcprBaseKhz()`。别据此把 `SwoBaud()` 的被除数改成 `tclk`
    // （那是探针侧的量），也别把这里锁死成"必须是核心时钟" —— 两条假设与判别实验
    // 都记在 `Todo.md`。
    // "Use Core Clock"（`tclkc`）把它钉在核心时钟上，不管 tclk= 写了什么；
    // 没勾时 0（= 没填）也表示"跟随核心时钟"。
    if (r.useCoreClock || r.traceClockKhz == 0) r.traceClockKhz = r.cpuKhz;
    if (!r.device[0]) strncpy_s(r.device, a.device, _TRUNCATE);
    if (!r.iromSize) { r.iromStart = a.iromStart; r.iromSize = a.iromSize; }
    return r;
}

} // namespace cfg
} // namespace orb
