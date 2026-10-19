/**
 * @file ORBAGDI_Etb.cpp
 * @brief ETB (Embedded Trace Buffer) implementation.
 *
 * Port of Keil's DbgCM/ETB.cpp onto the RDDI/MEM-AP layer.  The two structural
 * differences to the reference:
 *
 *   - The reference reaches the ETB through its own JTAG/SWD access layer
 *     (ReadD32/WriteD32, AP_Sel juggling, LinkCom locking).  Here every access
 *     goes through orb::Dap's MEM-AP, which serialises its own transfers, so
 *     no locking is needed.
 *
 *   - Reading the ETB RAM uses the RD_DATA port, which advances an *internal*
 *     read pointer on every access.  The MEM-AP must therefore NOT increment
 *     TAR between words (Keil: BLOCK_NADDRINC).  Dap::MemReadFixed() does
 *     exactly that.
 *
 * Decoding is deliberately split: the ETB formatter (ATB 16-byte frames) is
 * unpacked here into a plain ITM byte stream, which is then fed to the ITM
 * decoder in ORBAGDI_Itm.cpp.  That decoder already produces the uVision ITM
 * output and the DWT PC-sample history, so the ETB reuses it unchanged.
 */
#include "pch.h"
#include "ORBAGDI_Etb.h"
#include "ORBAGDI_Etm.h"
#include "ORBAGDI_Itm.h"
#include "ORBAGDI_Dap.h"
#include "ORBAGDI_Oflow.h"       // kOflowTagItm / kOflowTagEtm（落盘编码要用）
#include "ORBAGDI_Rddi.h"
#include "ORBAGDI_TraceFile.h"
#include "ORBAGDI_TraceWin.h"

#include <string.h>

namespace orb {
namespace agdi {
namespace {

// ===========================================================================
// ETB register offsets (standard CoreSight ETB; DbgCM/ETB.h)
// ===========================================================================
enum : uint32_t {
    kOffId      = 0x000,  // ETB ID register
    kOffDepth   = 0x004,  // RAM depth (words)
    kOffWidth   = 0x008,  // RAM width
    kOffStatus  = 0x00C,  // status
    kOffRdData  = 0x010,  // RAM read data port
    kOffRdPtr   = 0x014,  // RAM read pointer
    kOffWrPtr   = 0x018,  // RAM write pointer
    kOffTrgCnt  = 0x01C,  // trigger counter
    kOffCtrl    = 0x020,  // control
    kOffWrData  = 0x024,  // RAM write data port
    kOffFfsr    = 0x300,  // formatter / flush status
    kOffFfcr    = 0x304,  // formatter / flush control

    // TMC (ETB mode) extras
    kOffTmcMode = 0x028,  // TMC mode
    kOffTmcDev = 0xFC8,   // TMC device ID
};

// RAM width
enum : uint32_t {
    kWidthMask  = 0x0000003F,
    kWidth24Bit = (3u << 3),
    kWidth32Bit = (1u << 5),
};

// Status
enum : uint32_t {
    kStatusFull     = (1u << 0),
    kStatusTrigger  = (1u << 1),
    kStatusAcqComp  = (1u << 2),
    kStatusDfEmpty  = (1u << 3),
};

// Control
enum : uint32_t {
    kCtrlCaptEn = (1u << 0),
    kCtrlDemux  = (1u << 1),
    kCtrlSwCtrl = (1u << 2),
};

// Formatter / flush status
enum : uint32_t {
    kFfsrFlushing  = (1u << 0),
    kFfsrFtStopped = (1u << 1),
};

// Formatter / flush control
enum : uint32_t {
    kFfcrEnftc   = (1u << 0),
    kFfcrEnfcont = (1u << 1),
    kFfcrFonflin = (1u << 4),
    kFfcrFontrig = (1u << 5),
    kFfcrFonman  = (1u << 6),
    kFfcrTrigin  = (1u << 8),
    kFfcrTrigeVt = (1u << 9),
    kFfcrTrigfl  = (1u << 10),
    kFfcrStopfl  = (1u << 12),
    kFfcrStoptrig = (1u << 13),
};

// TMC device ID
enum : uint32_t {
    kTmcMemWidthMask = 0x00000700,
    kTmcMemWidthPos  = 8,
    kTmcCfgTypeMask  = 0x000000C0,
    kTmcCfgEtb       = 0x00000000,
    kTmcCfgEtr       = 0x00000040,
    kTmcCfgEtf       = 0x00000080,
};

// ATB source IDs (DbgCM/Debug.h)
enum : uint8_t {
    kAtbIdNull = 0,
    kAtbIdItm  = 1,
    kAtbIdEtm  = 2,
};

// ===========================================================================
// CoreSight ROM table constants (DbgCM/Debug.cpp)
// ===========================================================================
const uint32_t kCidRomTable = 0xB105100Du;
const uint64_t kPidMaskPn   = 0x0000000000000FFFULL;
const uint64_t kPidMaskJep  = 0x0000000F000FF000ULL;
const uint64_t kPidArmJep   = 0x00000004000BB000ULL;

enum : uint32_t {
    kPnEtb = 0x907,  // CoreSight ETB
    kPnTf  = 0x908,  // CoreSight Trace Funnel
    kPnTmc = 0x961,  // CoreSight Trace Memory Controller
    // ETM 的 part number（出处：DbgCM/Debug.cpp:284-292 的 PN_ETM_* 表）。
    // 用来把 ETM 的寄存器基址**从 ROM 表里读出来**，而不是写死 0xE0041000 ——
    // 换一颗器件、ETM 落在别的窗口时，写死的六个地址全错，而现象只是
    // "一个包都收不到"，最难查的那种（COMPAT_ANALYSIS.md §10.28 清单第 4 条）。
    kPnEtmCm3 = 0x924,  // ETM Cortex-M3
    kPnEtmCm4 = 0x925,  // ETM Cortex-M4
    kPnEtmSc3 = 0x926,  // ETM SC300
    kPnEtmCm7 = 0x975,  // ETM Cortex-M7
};

// 组件识别用的 DEVARCH / DEVTYPE 常量（出处：DbgCM/Debug.cpp:311-346）。
//
// 只靠 PIDR 的 part number 认组件是不够的：**v8-M 器件**（Cortex-M23/M33/M35P）
// 与 **ETMv4** 走的是 DEVARCH，而**没有 DEVARCH 的 ETMv3** 只靠 DEVTYPE 就能认出来。
// 参考实现三条路都走（`DebugReadRomTable()`：PIDR / DEVARCH / DEVTYPE），
// 我们原来只有 PIDR 一条 —— 换一颗非 STM32F4 的器件就认不出 ETM。
const uint32_t kDevArchPresentM  = 0x00100000u;  // DEVARCH 寄存器存在位
const uint32_t kDevArchArchM     = 0xFFE00000u;  // Architect ID（JEP106 + PRESENT 位）
const uint32_t kDevArchArchPartM = 0x00000FFFu;  // Architecture Part Number
const uint32_t kDevArchIdArm     = 0x47600000u;  // ARM
const uint32_t kDevArchVerM      = 0x0000F000u;  // Architecture Version
const uint32_t kDevArchVerP      = 12u;
const uint32_t kArchPartEtm      = 0xA13u;       // ARCHID: ETM (v8-M)
const uint32_t kDevTypeTypeM     = 0x000000FFu;  // DEVTYPE 全字段
const uint32_t kDevTypeEtm       = 0x13u;        // DEVTYPE: Trace Source, processor core

// ===========================================================================
// State
// ===========================================================================
const uint32_t kBufSize = 0x10000u;   // 64 KB, like Keil's ETB_BUFFER_SIZE

struct EtbState {
    bool     present;
    bool     tmc;
    bool     configured;
    bool     active;
    uint32_t addr;        // register base
    uint32_t etm;         // ETM component base found by the scan (0 = none)
    uint32_t etmArchVer;  // ETM architecture version from DEVARCH (0 = unknown)
    uint32_t depth;       // RAM depth in words
    uint32_t width;       // bytes per word
    uint32_t fmtMode;     // FFCR formatter bits latched at setup
    uint32_t trigDelay;
    bool     updated;
    bool     wraparound;
    bool     funnelSeen;  // a trace funnel was found during the scan
    bool     itmConnected; // ITM data appeared in the captured stream
    bool     etmConnected; // ETM data appeared in the captured stream

    EtbState()
        : present(false), tmc(false), configured(false), active(false),
          addr(0), etm(0), etmArchVer(0), depth(0), width(4), fmtMode(0), trigDelay(0),
          updated(false), wraparound(false), funnelSeen(false),
          itmConnected(false), etmConnected(false) {}
};

EtbState& St()
{
    // Deliberately leaked: uVision may still call into the driver while the
    // CRT tears its statics down (same reasoning as ORBAGDI_Itm's Hist()).
    static EtbState* s = new EtbState();
    return *s;
}

uint8_t* Buf()
{
    static uint8_t* b = new uint8_t[kBufSize];
    return b;
}

uint8_t* ItmScratch()
{
    static uint8_t* b = new uint8_t[kBufSize];
    return b;
}

uint8_t* EtmScratch()
{
    static uint8_t* b = new uint8_t[kBufSize];
    return b;
}

// ===========================================================================
// ETM decode
//
// The ETB formatter separates the ITM and ETM streams; the ETM part goes to the
// AGDI-layer shared ETM sink (ORBAGDI_Etm.cpp: EtmTraceFeed), which owns the
// decoder instance, keeps the branch ring and reports every branch to uVision's
// Trace Data window.  The orbtrace probe path (ORBAGDI_OrbTrace.cpp) feeds the
// very same sink, so both sources land in one stream of records.
// ===========================================================================

// ===========================================================================
// Register helpers
// ===========================================================================
inline bool EtbRd(orb::Dap& dap, uint32_t off, uint32_t& v)
{
    return dap.MemRead32(St().addr + off, v);
}

inline bool EtbWr(orb::Dap& dap, uint32_t off, uint32_t v)
{
    return dap.MemWrite32(St().addr + off, v);
}

// ===========================================================================
// CoreSight ROM table scan
// ===========================================================================

// CID: four 32-bit registers at +0xFF0, only the low byte of each is defined.
bool ReadCid(orb::Dap& dap, uint32_t adr, bool r32, uint32_t& cid)
{
    uint8_t b[16];
    if (r32) {
        if (!dap.MemRead(adr, b, 16)) return false;
    } else {
        for (uint32_t n = 0; n < 16; n += 4) {
            if (!dap.MemRead(adr + n, &b[n], 1)) return false;
        }
    }
    cid = (uint32_t)b[0] | ((uint32_t)b[4] << 8) |
          ((uint32_t)b[8] << 16) | ((uint32_t)b[12] << 24);
    return true;
}

// PID: eight 32-bit registers at +0xFD0 (PIDR4..PIDR7, PIDR0..PIDR3).
bool ReadPid(orb::Dap& dap, uint32_t adr, bool r32, uint64_t& pid)
{
    uint8_t b[32];
    if (r32) {
        if (!dap.MemRead(adr, b, 32)) return false;
    } else {
        for (uint32_t n = 0; n < 32; n += 4) {
            if (!dap.MemRead(adr + n, &b[n], 1)) return false;
        }
    }
    uint64_t hi = (uint64_t)b[0] | ((uint64_t)b[4] << 8) |
                  ((uint64_t)b[8] << 16) | ((uint64_t)b[12] << 24);
    uint64_t lo = (uint64_t)b[16] | ((uint64_t)b[20] << 8) |
                  ((uint64_t)b[24] << 16) | ((uint64_t)b[28] << 24);
    pid = (hi << 32) | lo;
    return true;
}

// Recursive walk.  `ptr` is the ADIv5 ROM table base (or an entry address).
bool RomScan(orb::Dap& dap, uint32_t ptr, int level)
{
    if (level == 0) {
        if (ptr == 0xFFFFFFFFu) return false;
        if ((ptr & 0x2u) == 0) ptr |= 0x3u;   // force ADIv5 format
    }
    if (level >= 10) return false;
    if ((ptr & 0x1u) == 0) return true;       // no entry here

    const bool     r32 = (ptr & 0x2u) != 0;
    const uint32_t adr = ptr & 0xFFFFF000u;

    uint32_t cid = 0;
    uint64_t pid = 0;
    if (!ReadCid(dap, adr + 0xFF0u, r32, cid)) return false;
    if (!ReadPid(dap, adr + 0xFD0u, r32, pid)) return false;

    EtbState& st = St();

    if (cid == kCidRomTable) {
        const uint32_t cnt = r32 ? 960u : 240u;
        for (uint32_t n = 0; n < cnt; ++n) {
            uint32_t val = 0;
            if (r32) {
                if (!dap.MemRead32(adr + 4u * n, val)) return false;
            } else {
                uint8_t b[4];
                if (!dap.MemRead(adr + 16u * n + 0,  &b[0], 1)) return false;
                if (!dap.MemRead(adr + 16u * n + 4,  &b[1], 1)) return false;
                if (!dap.MemRead(adr + 16u * n + 8,  &b[2], 1)) return false;
                if (!dap.MemRead(adr + 16u * n + 12, &b[3], 1)) return false;
                val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                      ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            }
            if (val == 0) return true;          // end-of-table marker
            if (!RomScan(dap, adr + val, level + 1)) return false;
        }
        return true;
    }

    // A component: its base is below the entry (4 KB count from PIDR4).
    const uint32_t n4k  = ((1u << (uint32_t)((pid >> 36) & 0xFu)) - 1u) * 4096u;
    const uint32_t comp = adr - n4k;

    switch (pid & (kPidMaskJep | kPidMaskPn)) {
    case (kPidArmJep | kPnEtb):
        if (st.addr == 0) { st.addr = comp; st.tmc = false; }
        break;

    case (kPidArmJep | kPnTmc): {
        if (st.addr == 0) {
            uint32_t devid = 0;
            if (dap.MemRead32(comp + kOffTmcDev, devid)) {
                if ((devid & kTmcCfgTypeMask) == kTmcCfgEtb) {
                    st.addr = comp;
                    st.tmc  = true;
                }
            }
        }
        break;
    }

    case (kPidArmJep | kPnTf):
        st.funnelSeen = true;
        break;

    case (kPidArmJep | kPnEtmCm3):
    case (kPidArmJep | kPnEtmCm4):
    case (kPidArmJep | kPnEtmSc3):
    case (kPidArmJep | kPnEtmCm7):
        // ETM 的基址。`ProgramEtm()` 用它，而不是写死 0xE0041000。
        if (st.etm == 0) st.etm = comp;
        break;

    default:
        // 诊断：把没认出来的组件连 PN 一起打出来。找"这颗器件的 ETM 用哪个 part
        // number"就靠它 —— 表里漏了哪个 PN，日志会直接指出来。
        ORB_LOG_INFO("ETB: ROM 组件 0x%08X 未识别 (PID=0x%016llX PN=0x%03X)",
                     comp, (unsigned long long)pid, (unsigned)(pid & kPidMaskPn));
        break;
    }

    // ---- ETM 的另外两条识别路（v8-M 与 v7-M）-----------------------------
    //
    // 只认 PIDR 是不够的：**v8-M** 器件（Cortex-M23/M33/M35P）与 **ETMv4** 走
    // DEVARCH，而**没有 DEVARCH 的 ETMv3** 只看 DEVTYPE 就能认出来。参考实现三条路
    // 都走（`DebugReadRomTable()`，DbgCM/Debug.cpp:710-796）：DEVARCH 在 `adr+0xFBC`、
    // DEVTYPE 在 `adr+0xFCC`，基址一律 `adr - n`。
    //
    // ⚠ 参考把这段放在 `if (cpupn)` 里 —— 只对 v8-M 的 **CPU 组件**求值。这里对每个
    //   组件都求值：目的是把 ETM 认出来，而 v8-M 的 ETM 未必就是 CPU 那一个组件。
    if (st.etm == 0) {
        uint32_t dev = 0;
        if (dap.MemRead32(adr + 0xFBCu, dev) && (dev & kDevArchPresentM) != 0) {
            // v8-M：DEVARCH 在，Architect ID + Part Number 说明这是哪个组件。
            if ((dev & (kDevArchArchM | kDevArchArchPartM)) ==
                (kDevArchIdArm | kArchPartEtm)) {
                st.etm        = comp;
                st.etmArchVer = (dev & kDevArchVerM) >> kDevArchVerP;
                // ETMv3 在 DEVARCH 里报 ARCHVER = 2（DbgCM/Debug.cpp:752）。
                if (st.etmArchVer == 2) st.etmArchVer = 3;
            }
        } else if (dap.MemRead32(adr + 0xFCCu, dev) &&
                   (dev & kDevTypeTypeM) == kDevTypeEtm) {
            // v7-M：没有 DEVARCH，靠 DEVTYPE。ETM 没有 DEVARCH ⇒ 必是 ETMv3。
            st.etm        = comp;
            st.etmArchVer = 3;
        }
    }

    return true;
}

// ===========================================================================
// RAM read (RD_DATA port, non-incrementing MEM-AP access)
// ===========================================================================
int EtbReadWords(orb::Dap& dap, uint8_t* dst, int dstMax, uint32_t nWords)
{
    const EtbState& st = St();
    const rddi::Api& api = rddi::Get();
    if (!api.DAP_ReadReg) return 0;

    // The RD_DATA port advances the ETB's own read pointer on every access, so
    // TAR must stay put: program CSW without the address-increment bit.
    const uint32_t csw = kCswBase | kCswSizeU32;
    if (!dap.WriteApReg(0, DAP_AP_REG_CSW, csw)) return 0;
    if (!dap.WriteApReg(0, DAP_AP_REG_TAR, st.addr + kOffRdData)) return 0;

    uint32_t tmp[256];
    uint32_t done = 0;
    int      out  = 0;

    while (done < nWords) {
        uint32_t n = nWords - done;
        if (n > 256u) n = 256u;

        if (api.DAP_RegReadRepeat) {
            if (api.DAP_RegReadRepeat(dap.Handle(), dap.DapIndex(),
                                      (int)n, DAP_AP_REG_DRW,
                                      reinterpret_cast<int*>(tmp)) != 0) {
                break;
            }
        } else {
            bool ok = true;
            for (uint32_t i = 0; i < n; ++i) {
                int v = 0;
                if (api.DAP_ReadReg(dap.Handle(), dap.DapIndex(),
                                    DAP_AP_REG_DRW, &v) != 0) { ok = false; break; }
                tmp[i] = (uint32_t)v;
            }
            if (!ok) break;
        }

        for (uint32_t i = 0; i < n; ++i) {
            if (out + (int)st.width > dstMax) { done = nWords; break; }
            const uint32_t w = tmp[i];
            dst[out++] = (uint8_t)(w & 0xFF);
            dst[out++] = (uint8_t)((w >> 8) & 0xFF);
            dst[out++] = (uint8_t)((w >> 16) & 0xFF);
            if (st.width == 4) dst[out++] = (uint8_t)((w >> 24) & 0xFF);
        }
        done += n;
    }

    // CSW is now non-incrementing behind Dap's back: make it reprogram.
    dap.InvalidateCswCache();
    return out;
}

// ===========================================================================
// Formatter unpack (ATB 16-byte frames -> ITM byte stream)
// ===========================================================================
void DecodeFormatter(const uint8_t* buf, uint32_t len,
                     uint8_t* itmOut, int& itmLen, int itmMax,
                     uint8_t* etmOut, int& etmLen, int etmMax)
{
    itmLen = 0;
    etmLen = 0;

    uint8_t frame[16];
    int     frameIdx = 0;
    uint8_t packetId = 0;

    for (uint32_t k = 0; k < len; ++k) {
        frame[frameIdx] = buf[k];
        if (++frameIdx < 16) continue;
        frameIdx = 0;

        for (int i = 0; i < 16; i += 2) {
            const uint8_t bit = (uint8_t)((frame[15] >> (i >> 1)) & 1);

            if (frame[i] & 1) {
                // Even byte carries an ATB source id.
                uint8_t id = packetId;
                packetId = (uint8_t)(frame[i] >> 1);
                if (i == 14) break;
                if (bit == 0) id = packetId;   // id change applies immediately

                if (id == kAtbIdItm && itmLen < itmMax) {
                    itmOut[itmLen++] = frame[i + 1];
                } else if (id == kAtbIdEtm && etmLen < etmMax) {
                    etmOut[etmLen++] = frame[i + 1];
                }
            } else {
                // Even byte carries data (its LSB is the frame flag).
                if (packetId == kAtbIdItm) {
                    if (itmLen < itmMax) itmOut[itmLen++] = (uint8_t)((frame[i] & 0xFE) | bit);
                    if (i == 14) continue;
                    if (itmLen < itmMax) itmOut[itmLen++] = frame[i + 1];
                } else if (packetId == kAtbIdEtm) {
                    if (etmLen < etmMax) etmOut[etmLen++] = (uint8_t)((frame[i] & 0xFE) | bit);
                    if (i == 14) continue;
                    if (etmLen < etmMax) etmOut[etmLen++] = frame[i + 1];
                }
            }
        }
    }
}

// ===========================================================================
// Flush / stop
// ===========================================================================
// EtbFlush() 本身是公开接口（官方 ETB_Flush），定义在文件末尾的公开段；这里只
// 保留停止采集的编排。
bool EtbStopCapture(orb::Dap& dap)
{
    EtbState& st = St();
    if (!st.active) return true;

    EtbFlush(dap);

    EtbWr(dap, kOffCtrl, 0);                  // disable capture
    st.active = false;

    // Wait for acquisition complete, then for the formatter pipe to drain.
    uint32_t val = 0;
    uint64_t t = GetTickCount64();
    do {
        if (!EtbRd(dap, kOffStatus, val)) return false;
        if (val & kStatusAcqComp) break;
    } while ((GetTickCount64() - t) < 10);

    if ((val & kStatusDfEmpty) == 0) {
        t = GetTickCount64();
        do {
            if (!EtbRd(dap, kOffStatus, val)) return false;
            if (val & kStatusDfEmpty) break;
        } while ((GetTickCount64() - t) < 10);
    }

    EtbWr(dap, kOffFfcr, 0);
    return true;
}

} // namespace

// ===========================================================================
// Public interface
// ===========================================================================

bool EtbDetect(orb::Dap& dap)
{
    EtbState& st = St();
    st.present    = false;
    st.addr       = 0;
    st.etm        = 0;
    st.etmArchVer = 0;
    st.tmc        = false;
    st.funnelSeen = false;

    if (!dap.IsConnected()) {
        ORB_LOG_WARN("ETB: not connected");
        return false;
    }

    // The ROM table base lives in the MEM-AP's ROM_ADDR register (bank 0xF);
    // fall back to the architectural default when the target does not report it
    // (absent register reads back as 0 or all-ones).
    uint32_t romBase = 0;
    if (!dap.ReadApReg(0xF, DAP_AP_REG_ROMADDR, romBase) ||
        romBase == 0 || romBase == 0xFFFFFFFFu) {
        romBase = 0xE00FF000u;
    }

    if (!RomScan(dap, romBase, 0)) {
        ORB_LOG_WARN("ETB: ROM table scan failed (base 0x%08X)", romBase);
        return false;
    }

    if (st.addr == 0) {
        ORB_LOG_INFO("ETB: no ETB/TMC in the ROM table (base 0x%08X)", romBase);
        return false;
    }

    st.present = true;
    ORB_LOG_INFO("ETB: found %s at 0x%08X (funnel=%d, ETM=%s)",
                 st.tmc ? "TMC(ETB mode)" : "ETB", st.addr,
                 st.funnelSeen ? 1 : 0,
                 st.etm ? "扫到" : "表里没有");
    return true;
}

bool EtbAvailable()
{
    return St().present;
}

bool EtbIsTmc()
{
    return St().tmc;
}

uint32_t EtbEtmBase()
{
    return St().etm;
}

bool EtbScanRom(orb::Dap& dap)
{
    if (!dap.IsConnected()) return false;

    // Same ROM_ADDR source as EtbDetect(), and the same architectural fallback.
    uint32_t romBase = 0;
    if (!dap.ReadApReg(0xF, DAP_AP_REG_ROMADDR, romBase) ||
        romBase == 0 || romBase == 0xFFFFFFFFu) {
        romBase = 0xE00FF000u;
    }

    // Deliberately **not** the EtbDetect() path: that one resets the ETB state, and
    // this is called from the ITM/ETM side where an ETB session may already be live.
    if (!RomScan(dap, romBase, 0)) {
        ORB_LOG_WARN("ETB: ROM 表扫描失败 (base 0x%08X)", romBase);
        return false;
    }
    return true;
}

unsigned EtbEtmBranchCount()
{
    return EtmTraceBranchCount();
}

bool EtbSetup(orb::Dap& dap)
{
    EtbState& st = St();
    if (!st.present) return false;
    if (!dap.IsConnected()) return false;

    uint32_t id = 0, depth = 0, width = 0;
    if (!EtbRd(dap, kOffId, id)) {
        ORB_LOG_WARN("ETB: ID read failed at 0x%08X", st.addr);
        return false;
    }
    if (!EtbRd(dap, kOffDepth, depth)) return false;
    if (!EtbRd(dap, kOffWidth, width)) return false;

    if (st.tmc) {
        uint32_t devid = 0;
        if (EtbRd(dap, kOffTmcDev, devid)) {
            st.width = 1u << ((devid & kTmcMemWidthMask) >> kTmcMemWidthPos);
        } else {
            st.width = 4;
        }
    } else {
        switch (width & kWidthMask) {
        case kWidth24Bit: st.width = 3; break;
        case kWidth32Bit:
        default:          st.width = 4; break;
        }
    }
    if (st.width != 3 && st.width != 4) st.width = 4;

    st.depth = depth;

    // The formatter is enabled unconditionally: it is what turns the raw ATB
    // stream into the 16-byte frames DecodeFormatter() expects, so enabling it
    // and decoding frames stays self-consistent even for a single source.
    st.fmtMode = kFfcrEnftc | kFfcrEnfcont;

    st.configured = true;
    st.updated    = false;
    st.wraparound = false;

    ORB_LOG_INFO("ETB: setup id=0x%08X depth=%u words width=%u bytes fmt=0x%X",
                 id, depth, st.width, st.fmtMode);
    return true;
}

bool EtbActivate(orb::Dap& dap)
{
    EtbState& st = St();
    if (!st.configured) return false;
    if (st.active) return true;

    // Clear any stop bits a previous session left behind.
    uint32_t val = 0;
    if (EtbRd(dap, kOffFfcr, val)) {
        if (val & (kFfcrStopfl | kFfcrStoptrig)) {
            val &= ~(kFfcrStopfl | kFfcrStoptrig);
            EtbWr(dap, kOffFfcr, val);
        }
    }

    // Flush on trigger, stop after the flush, formatter as latched.
    val = kFfcrStopfl | kFfcrFontrig | st.fmtMode;
    if (!EtbWr(dap, kOffFfcr, val)) return false;

    // Reset pointers and arm capture.
    EtbWr(dap, kOffRdPtr, 0);
    EtbWr(dap, kOffWrPtr, 0);
    EtbWr(dap, kOffTrgCnt, st.trigDelay);
    if (!EtbWr(dap, kOffCtrl, kCtrlCaptEn)) return false;

    st.active     = true;
    st.updated    = false;
    st.wraparound = false;
    return true;
}

// ---------------------------------------------------------------------------
// ETB_Flush / ETB_CanAccess / ETB_Clear / ETB_Recovery / ETB_Init
// ---------------------------------------------------------------------------

// 官方 `ETB_Flush`（DbgCM/ETB.cpp:968-1045）。读 FFSR；若格式化器尚未停止，先置
// STOPFL（ETB/TMC 真正停下来的必要条件，官方注释 21.11.2018），再置 FONMAN 触发
// 一次手动冲刷，然后轮询 FONMAN 清零，超时 10 ms（官方超时返回 EU53）。
bool EtbFlush(orb::Dap& dap)
{
    uint32_t ffsr = 0;
    if (!EtbRd(dap, kOffFfsr, ffsr)) return false;
    if (ffsr & kFfsrFtStopped) return true;   // already stopped

    uint32_t ffcr = 0;
    if (!EtbRd(dap, kOffFfcr, ffcr)) return false;

    ffcr |= kFfcrStopfl;
    if (!EtbWr(dap, kOffFfcr, ffcr)) return false;

    ffcr |= kFfcrFonman;                      // trigger a manual flush
    if (!EtbWr(dap, kOffFfcr, ffcr)) return false;

    const uint64_t t0 = GetTickCount64();
    do {
        if (!EtbRd(dap, kOffFfcr, ffcr)) return false;
        if ((ffcr & kFfcrFonman) == 0) return true;
    } while ((GetTickCount64() - t0) < 10);

    ORB_LOG_WARN("ETB: flush did not complete in time");
    return false;
}

// 官方 `ETB_CanAccess`（DbgCM/ETB.cpp:1339-1375）：只要格式化器停了，缓冲区就可以
// 安全读——官方注释 "ETB can be accessed as soon as its formatter is stopped"。
bool EtbCanAccess(orb::Dap& dap)
{
    const EtbState& st = St();
    if (!st.configured || st.addr == 0) return false;

    uint32_t val = 0;
    if (!EtbRd(dap, kOffFfsr, val)) return false;

    return (val & kFfsrFtStopped) != 0;
}

// 官方 `ETB_Clear`（DbgCM/ETB.cpp:1377-1383）。
void EtbClear()
{
    EtbState& st = St();
    // Noticing the clear enables another update from the ETB.
    st.updated    = false;
    st.wraparound = false;
}

// 官方 `ETB_Recovery`（DbgCM/ETB.cpp:1386-1460）：低功耗等场景后恢复。还在采集就
// 直接返回；否则清掉上一次停止留下的停止位。
bool EtbRecovery(orb::Dap& dap)
{
    EtbState& st = St();
    if (st.addr == 0) return false;

    uint32_t val = 0;
    if (!EtbRd(dap, kOffCtrl, val)) return false;
    if (val & kCtrlCaptEn) return true;       // ETB is capturing, nothing to do

    if (!EtbRd(dap, kOffFfcr, val)) return false;
    if (val & (kFfcrStopfl | kFfcrStoptrig)) {
        val &= ~(kFfcrStopfl | kFfcrStoptrig);
        if (!EtbWr(dap, kOffFfcr, val)) return false;
    }
    return true;
}

// 官方 `ETB_Init`（DbgCM/ETB.cpp:1317-1334）。官方在此置 `pio->bTrcDisp = 1` 并先
// InitTraceInterface() 再 TraceWinInit()；本驱动的 TraceWinInit() 自己会置
// bTrcDisp 并完成注册，所以这里只要保证窗口已注册。
bool EtbInit()
{
    if (St().addr == 0) {
        // ETB requested but not available
        return false;
    }
    return TraceWinInit();
}

int EtbProcess(orb::Dap& dap)
{
    EtbState& st = St();
    if (!st.configured) return 0;
    if (st.updated) return 0;                 // already read since activation

    EtbStopCapture(dap);

    // 官方只在其格式化器停止后才读缓冲区（ETB_CanAccess，DbgCM/ETB.cpp:1339）。
    // 这里复查一次，但**只告警不放弃**：本驱动的停止路径已经等过
    // STATUS.ACQCOMP / DFEMPTY，而 FTSTOPPED 在部分目标上不会置起——把它升级成
    // 硬失败会打断本来能用的目标。差异已在代码里标明。
    if (!EtbCanAccess(dap)) {
        ORB_LOG_WARN("ETB: formatter reports not stopped, reading anyway");
    }

    uint32_t status = 0;
    if (!EtbRd(dap, kOffStatus, status)) return -1;
    st.wraparound = (status & kStatusFull) != 0;

    uint32_t rdPtr = 0, wrPtr = 0;
    if (!EtbRd(dap, kOffRdPtr, rdPtr)) return -1;
    if (!EtbRd(dap, kOffWrPtr, wrPtr)) return -1;

    uint32_t nWords = 0;
    uint32_t start  = 0;
    if (st.wraparound) {
        nWords = st.depth;                    // full buffer; oldest starts at WR
        start  = wrPtr;
    } else {
        // Standard ETB pointers count words, TMC pointers count bytes.
        nWords = st.tmc ? (wrPtr / st.width) : wrPtr;
        start  = 0;
    }
    if (nWords > (kBufSize / st.width)) nWords = kBufSize / st.width;
    if (nWords == 0) {
        st.updated = true;
        return 0;
    }

    if (!EtbWr(dap, kOffRdPtr, start)) return -1;

    // Ensure the RAM is reachable over the debug port (not AHB).
    uint32_t ctrl = 0;
    if (EtbRd(dap, kOffCtrl, ctrl)) EtbWr(dap, kOffCtrl, ctrl & ~kCtrlSwCtrl);

    const int bytes = EtbReadWords(dap, Buf(), (int)kBufSize, nWords);
    st.updated = true;

    // The capture starts at the beginning of the stream, so decode from a known
    // boundary (no 48-bit sync hunt); Keil's ETB ITM decoder does the same.
    ItmResyncDecoder();

    if (st.fmtMode) {
        int itmLen = 0, etmLen = 0;
        DecodeFormatter(Buf(), (uint32_t)bytes,
                        ItmScratch(), itmLen, (int)kBufSize,
                        EtmScratch(), etmLen, (int)kBufSize);
        if (itmLen > 0) {
            // 落盘（P2-1）：ETB 读回来的是裸字节，本层按 tag 编码成 OFLOW 帧再写。
            // 格式化器已经把 ITM / ETM 分开，正好对应 OFLOW 的两个 tag。
            TraceFileWriteStream(kOflowTagItm, ItmScratch(), (unsigned)itmLen);
            ItmFeedBytes(ItmScratch(), itmLen);
            st.itmConnected = true;
        }

        // ⚠ 这里**不**跳过同步搜索。ETM 的分支地址是相对的：必须等到 A-SYNC
        // （5×0x00 + 0x80）或 ISYNC 才有基准地址。ETB 读出来的是一段"字节"，
        // 不是"一个同步点"—— 原来那句 `atStreamStart = true` 把两者混为一谈，
        // 于是解码器在没有任何基准地址的情况下就开始上报（见 ORBAGDI_Etm.h）。
        if (etmLen > 0) {
            // 回绕 = 缓冲区从流中间开始，缺口之后相对地址没有基准 ⇒ 先强制
            // 重新同步（P0-D，等价 orbuculum orbmortem.c:1084）。
            if (st.wraparound) EtmTraceForceSync(false);

            EtmTraceFeed(EtmScratch(), (size_t)etmLen);
            TraceFileWriteStream(kOflowTagEtm, EtmScratch(), (unsigned)etmLen);
            st.etmConnected = true;
        }
        ORB_LOG_INFO("ETB: read %d bytes, %d ITM / %d ETM bytes, %u branches (wrap=%d)",
                     bytes, itmLen, etmLen, EtbEtmBranchCount(), st.wraparound ? 1 : 0);
        // ETM 解码器自己的汇总（协议 / 同步状态 / 各类事件计数）。
        EtmTraceLogSummary("etb");
    } else {
        if (bytes > 0) {
            TraceFileWriteStream(kOflowTagItm, Buf(), (unsigned)bytes);
            ItmFeedBytes(Buf(), bytes);
            st.itmConnected = true;
        }
        ORB_LOG_INFO("ETB: read %d raw bytes (wrap=%d)", bytes, st.wraparound ? 1 : 0);
    }

    // 官方在每次 ETB 处理完后折叠错误掩码并上报状态栏（DbgCM/ETB.cpp:959）。
    TraceWinTraceStatus();
    return bytes;
}

void EtbReset()
{
    EtbState& st = St();
    st.present    = false;
    st.tmc        = false;
    st.configured = false;
    st.active     = false;
    st.addr       = 0;
    st.depth      = 0;
    st.width      = 4;
    st.fmtMode    = 0;
    st.trigDelay  = 0;
    st.updated    = false;
    st.wraparound = false;
    st.funnelSeen = false;
}

EtbStatus EtbGetStatus()
{
    const EtbState& st = St();
    EtbStatus s;
    s.present    = st.present;
    s.tmc        = st.tmc;
    s.configured = st.configured;
    s.active     = st.active;
    s.addr       = st.addr;
    s.depth      = st.depth;
    s.width      = st.width;
    s.wraparound = st.wraparound;
    s.itmConnected = st.itmConnected;
    s.etmConnected = st.etmConnected;
    s.trigDelay    = st.trigDelay;
    return s;
}

} // namespace agdi
} // namespace orb
