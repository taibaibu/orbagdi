#include "pch.h"

#include <string.h>

namespace orb {

namespace {

// Some RDDI implementations return the DAP/AP register values through an
// "int" pointer; keep the conversion in one place.
inline int   AsReg(uint32_t v) { return static_cast<int>(v); }
inline uint32_t FromReg(int v) { return static_cast<uint32_t>(v); }

/// "Log Debug Accesses" (Debug page).  Deliberately verbose: this is the switch
/// that exists to make a session readable from the log afterwards.
void LogAccess(const char* what, uint32_t a, uint32_t v)
{
    if (!orb::cfg::Get().logAccesses) return;
    ORB_LOG_INFO("Dap: %-10s 0x%08X = 0x%08X", what, a, v);
}

} // namespace

Dap::Dap()
    : m_handle(0), m_dapId(0), m_apIndex(0), m_connected(false),
      m_dpidr(0), m_apidr(0), m_cswCache(0xFFFFFFFFu), m_lastError(0),
      m_cache(nullptr) {}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
bool Dap::Connect(const char* ifaceHint) {
    (void)ifaceHint;
    if (m_connected) return true;

    const orb::cfg::Config& opt = orb::cfg::Get();

    // AP index from the Settings page.  Applied before anything probes the AP so
    // the IDR / CSW accesses below already target the right one (bug.md §I: this
    // option used to be parsed and then ignored).
    SetApIndex((opt.ap >= 0 && opt.ap <= 255) ? opt.ap : 0);

    if (!rddi::Load()) {
        ORB_LOG_ERROR("Dap: RDDI layer not available");
        return false;
    }
    const rddi::Api& api = rddi::Get();

    if (api.Open(&m_handle, nullptr) != 0 || m_handle == 0) {
        m_lastError = rddi::LastErrorCode();
        ORB_LOG_ERROR("Dap: RDDI_Open failed (%d)", m_lastError);
        return false;
    }

    // "Connect:" mode (Debug page).  with Pre-reset / under Reset hold nRESET
    // low across the whole connect sequence, so the target cannot run away while
    // the AP is being brought up.  The pin is released once the DP has answered.
    const bool holdReset = (opt.connect == cfg::kConnectPreReset ||
                            opt.connect == cfg::kConnectUnderReset);
    if (holdReset) ResetPin(true);

    // Select the CMSIS-DAP interface (usually only one exists).
    if (api.CMSIS_DAP_Detect) {
        int numIf = 0;
        if (api.CMSIS_DAP_Detect(m_handle, &numIf) == 0 && numIf > 0) {
            if (api.CMSIS_DAP_Identify) {
                char name[256];
                for (int i = 0; i < numIf; ++i) {
                    memset(name, 0, sizeof(name));
                    if (api.CMSIS_DAP_Identify(m_handle, i, 0, name, sizeof(name)) == 0) {
                        // An empty name is what hosts return when they only fill
                        // the descriptor for a specific idNo; print it as such
                        // rather than as a puzzling blank line.
                        ORB_LOG_INFO("Dap: interface[%d] = \"%s\"",
                                     i, name[0] ? name : "(unnamed)");
                    }
                }
            }
            int useIf = 0;
            if (opt.iface >= 0) {
                if (opt.iface < numIf) {
                    useIf = opt.iface;
                } else {
                    ORB_LOG_WARN("Dap: interface %d out of range (%d found), using 0",
                                 opt.iface, numIf);
                }
            }
            // The third argument is the host's own config string: which port to
            // drive, and whether the SWJ-DP may switch between them.  It is only
            // built when there is something to say, so the plain SWD case keeps
            // making exactly the call this driver has always made.
            // A host that does not understand the string answers non-zero, which
            // is a WARN and nothing more (bug.md §I: the Port combo had no effect
            // because the string was never passed at all).
            // The string comes from rddi::InterfaceConfig so that the settings
            // sheet's AP probe asks the host for exactly the same thing - it did
            // not, and probed a session with no interface bound
            // (COMPAT_ANALYSIS.md §10.3).
            if (api.CMSIS_DAP_ConfigureInterface) {
                char cfg[64];
                char* cfgArg = rddi::InterfaceConfig(cfg, sizeof(cfg));
                if (api.CMSIS_DAP_ConfigureInterface(m_handle, useIf, cfgArg) != 0) {
                    ORB_LOG_WARN("Dap: CMSIS_DAP_ConfigureInterface failed%s",
                                 cfgArg ? " (the host rejected the config string)" : "");
                } else if (cfgArg) {
                    ORB_LOG_INFO("Dap: interface config \"%s\" accepted", cfgArg);
                }
            }

            // DAP_Configure, before the two DAP list calls and before the link -
            // the order ARM's rddi_dap.h requires (ORBMDK COMPAT_ANALYSIS.md §4.6
            // records "No Debug Unit Found" coming out of getting this wrong).
            if (api.DAP_Configure) {
                api.DAP_Configure(m_handle, (opt.port == cfg::kPortJtag) ? "JTAG" : "SWD");
            }

            // The SWJ clock is deliberately NOT programmed here.  The documented
            // order is Connect -> SWJ_Clock, and a JTAG connect caps the clock to
            // 4 MHz by itself, so a rate set before the link is up would only be
            // overwritten.  It moved to just after CMSIS_DAP_Connect, below.
        } else {
            ORB_LOG_WARN("Dap: CMSIS_DAP_Detect found no interface");
        }
    }

    // Discover DAP IDs (0 = the debug port itself).
    int numDaps = 0;
    int dapIds[16] = {0};
    bool haveList = false;
    if (api.CMSIS_DAP_DetectNumberOfDAPs &&
        api.CMSIS_DAP_DetectNumberOfDAPs(m_handle, &numDaps) == 0 && numDaps > 0) {
        size_t n = (numDaps < 16) ? static_cast<size_t>(numDaps) : 16;
        if (api.CMSIS_DAP_DetectDAPIDList &&
            api.CMSIS_DAP_DetectDAPIDList(m_handle, dapIds, n) == 0) {
            haveList = true;
        }
    } else if (api.DAP_GetNumberOfDAPs &&
               api.DAP_GetNumberOfDAPs(m_handle, &numDaps) == 0 && numDaps > 0) {
        size_t n = (numDaps < 16) ? static_cast<size_t>(numDaps) : 16;
        if (api.DAP_GetDAPIDList && api.DAP_GetDAPIDList(m_handle, dapIds, n) == 0) {
            haveList = true;
        }
    }

    // DAP_ReadReg / DAP_WriteReg take a DAP *index*, not an IDCODE: the value
    // lands in the CMSIS-DAP DAP_Transfer index byte, so an IDCODE there is an
    // out-of-range index and every AP access fails (ORBMDK's DAP_GetDAPIDList:
    // "必须是索引而不是 IDCODE").  Hosts disagree on what the ID list holds -
    // ORBMDK returns the index, Keil's CMSIS_DAP.dll returns the real IDCODE
    // (0x2BA01477 on an F1) - so the index is NOT derived from it: DAPs are
    // addressed as 0..n-1, and only the first one is driven here, exactly as
    // m_apIndex does for the AP.  The list is logged for identification only.
    for (int i = 0; i < numDaps && i < 16; ++i) {
        ORB_LOG_INFO("Dap: DAP[%d] ID = 0x%08X", i, static_cast<unsigned>(dapIds[i]));
    }
    (void)haveList;
    m_dapId = 0;
    ORB_LOG_INFO("Dap: %d DAP(s) found, using DAP index %d", numDaps, m_dapId);

    // The link itself.  CMSIS_DAP_Connect is the entry point that performs the
    // SWJ/SWD switch sequence and the first DPIDR read; DAP_Connect is a stub that
    // only fills an implementor string and does no link work at all.  This driver
    // called neither - it went straight to a DP read and relied on the host having
    // linked the target by itself.  ORBMDK's own test program made the same
    // mistake, and COMPAT_ANALYSIS.md §4.4 records the fix.
    if (api.CMSIS_DAP_Connect) {
        int iface = 0;
        const int rc = api.CMSIS_DAP_Connect(m_handle, &iface);
        if (rc == 0) {
            ORB_LOG_INFO("Dap: target linked over %s", (iface == 2) ? "JTAG" : "SWD");
        } else {
            // Not fatal here on its own: the DP probe below is what decides, and
            // its message is the one worth reading.
            ORB_LOG_WARN("Dap: CMSIS_DAP_Connect failed (%d) - no target on the "
                         "port, or the target is not powered", rc);
        }
    } else {
        ORB_LOG_WARN("Dap: the RDDI host has no CMSIS_DAP_Connect - relying on "
                     "the host to link the target by itself");
    }

    // Now that the link is up, the clock - the documented order is
    // Connect -> SWJ_Clock, and this is the first moment the rate can be set
    // without the connect sequence overwriting it.
    if (opt.clockKhz > 0) {
        if (api.CMSIS_DAP_SWJ_Clock) {
            const unsigned int hz = static_cast<unsigned int>(opt.clockKhz) * 1000u;
            const int rc = api.CMSIS_DAP_SWJ_Clock(m_handle, hz);
            if (rc == 0) {
                ORB_LOG_INFO("Dap: SWJ clock = %d kHz", opt.clockKhz);
            } else {
                ORB_LOG_WARN("Dap: SWJ clock %d kHz rejected (%d)", opt.clockKhz, rc);
            }
        } else {
            ORB_LOG_WARN("Dap: host has no CMSIS_DAP_SWJ_Clock, %d kHz ignored",
                         opt.clockKhz);
        }
    }

    m_connected = true;

    // Probe the DP. A successful IDCODE read is the real "connected" proof.
    uint32_t dpidr = 0;
    if (!ReadDP(DAP_DP_REG_IDCODE, dpidr) || dpidr == 0 || dpidr == 0xFFFFFFFFu) {
        m_lastError = rddi::LastErrorCode();
        ORB_LOG_ERROR("Dap: DP IDCODE read failed (0x%08X)", dpidr);
        m_connected = false;
        return false;
    }
    m_dpidr = dpidr;
    ORB_LOG_INFO("Dap: DPIDR = 0x%08X", dpidr);

    if (!PowerUpDebug()) {
        // Not fatal on every host - some RDDI implementations never report the
        // acknowledge bits.  Keep going so the AP probe below can give the real
        // diagnosis instead of one misleading line.
        ORB_LOG_WARN("Dap: DP power-up not acknowledged, continuing to AP probe");
    }

    // Probe the MEM-AP IDR (bank 0xF).  This is the first genuine AP access and
    // therefore the first honest answer to "is the AP reachable at all".
    uint32_t idr = 0;
    // AP IDR lives at address 0xC but must be passed as the encoded id 7
    // (DAP_AP_REG_APIDR); a bare 0xC used to be shifted into a different slot.
    const bool apRead = ReadApReg(0xF, DAP_AP_REG_APIDR, idr);
    if (apRead) {
        m_apidr = idr;
        ORB_LOG_INFO("Dap: AP IDR = 0x%08X", idr);
    } else {
        ORB_LOG_WARN("Dap: AP IDR read failed (err=%d) - AP is not answering",
                     m_lastError);
    }

    // Default MEM-AP configuration (word, single increment).
    const bool apCsw = ConfigureMemAp(AccessSize::U32);
    if (!apCsw) {
        ORB_LOG_WARN("Dap: MEM-AP CSW programming failed (err=%d)", m_lastError);
    }

    // A MEM-AP that answers neither IDR nor CSW cannot serve a single memory
    // access, so the session is over before it began.  Reporting success here is
    // what made uVision print "target attached" and then fail on every access
    // with the real cause far away from the message (bug.md E2).  Only *AP*
    // symptoms are fatal: the DP-only ones above (missing power ACK, halt not
    // confirming) are deliberately tolerated because several RDDI hosts never
    // report those bits on a healthy target.
    // An all-ones IDR is the SWD "no response" pattern, so it counts as dead too.
    if (!apRead || !apCsw || idr == 0xFFFFFFFFu) {
        ORB_LOG_ERROR("Dap: MEM-AP not responding (IDR read %s, IDR = 0x%08X, "
                      "CSW write %s) - the target cannot be used for memory access",
                      apRead ? "ok" : "failed", idr, apCsw ? "ok" : "failed");
        if (holdReset) ResetPin(false);
        Disconnect();   // closes the RDDI handle so a retry starts from scratch
        return false;
    }

    // "with Pre-reset" / "under Reset": the DP answered, so let the target go.
    if (holdReset) ResetPin(false);

    return true;
}

// CMSIS-DAP pin 7 is nRESET (CMSIS-DAP_1.2, SWJ_Pins).
const unsigned char kSwjPinReset = 0x80;

bool Dap::ResetPin(bool assert)
{
    const rddi::Api& api = rddi::Get();
    if (!api.CMSIS_DAP_SWJ_Pins) {
        ORB_LOG_WARN("Dap: host has no CMSIS_DAP_SWJ_Pins - nRESET is not driven");
        return false;
    }
    int res = 0;
    const unsigned char out = assert ? 0x00 : kSwjPinReset;
    const int rc = api.CMSIS_DAP_SWJ_Pins(m_handle, kSwjPinReset, out, &res, 0);
    if (rc != 0) {
        ORB_LOG_WARN("Dap: SWJ_Pins nRESET %s failed (rc=%d)",
                     assert ? "assert" : "release", rc);
        return false;
    }
    ORB_LOG_INFO("Dap: nRESET %s", assert ? "asserted" : "released");
    return true;
}

void Dap::Disconnect() {
    // ⚠ **不要在 `!m_connected` 时直接 return。**
    // `m_connected` 只在"DP IDCODE 读到了"之后才置起，而句柄是**更早**就开出来的
    // （Connect 里 RDDI_Open 之后、DP 探测之前）—— 于是"Connect 失败"那条路上句柄
    // 永远关不掉，下一次 Connect 会再 Open 一个、旧的泄漏。
    // 2026-10-14 的**完整重建**（`_DebugProtocolRecovery` 第 [3/3] 级）正好走这条路：
    // 探针跑飞时句柄是开着的、`m_connected` 是假的 —— 不关掉就重建不起来。
    const rddi::Api& api = rddi::Get();
    const bool had = (m_handle != 0);
    if (m_handle) {
        // Both, in the documented order (ORBMDK Usage.md §2: DAP_Disconnect then
        // RDDI_Close).  The old either/or form skipped DAP_Disconnect entirely
        // whenever the host exported Close - which is every host that matters,
        // so the link was never torn down and only the session handle was.
        if (api.DAP_Disconnect) api.DAP_Disconnect(m_handle);
        if (api.Close) api.Close(m_handle);
    }
    m_handle = 0;
    m_connected = false;
    m_cswCache = 0xFFFFFFFFu;
    CacheFlush();
    if (had) ORB_LOG_INFO("Dap: disconnected");
}

bool Dap::ProbeDap() {
    uint32_t v = 0;
    return ReadDP(DAP_DP_REG_IDCODE, v);
}

// ---------------------------------------------------------------------------
// Raw DP / AP register access
//
// The third argument of DAP_ReadReg / DAP_WriteReg is an *encoded register id*,
// never a byte offset: DP registers are 0..3 and AP registers are 4..7, and the
// AP bank is chosen by writing DP SELECT (id 2).  That encoding lives once, in
// include/ORBAGDI_Rddi.h, and every caller here passes it already encoded
// (DAP_DP_REG_* / DAP_AP_REG_*).
//
// Keil's DbgCM/SWD.cpp:321 receives a *byte address* from its callers and turns
// it into an id with "DAP_REG_DP_0x0 + (adr >> 2)".  An earlier revision of this
// file copied that expression while ALSO being handed the encoded constants, so
// every access shifted the id a second time:
//     CTRL/STAT  id 1 -> 1 >> 2 = 0            -> really read IDCODE, and the
//                                                 power request landed in ABORT,
//                                                 so the DP never powered up
//     SELECT     id 2 -> 2 >> 2 = 0            -> AP bank/APSEL never written
//     AP CSW     id 4 -> 4 + (4 >> 2) = 5      -> CSW writes landed in TAR
//     AP DRW     id 7 -> 4 + (7 >> 2) = 5      -> DRW writes landed in TAR too
// The giveaway in the log was CTRL/STAT reading back exactly the DPIDR that had
// just been read from register 0 (bug.md E7).
// ---------------------------------------------------------------------------
bool Dap::ReadDP(uint32_t regId, uint32_t& value) {
    const rddi::Api& api = rddi::Get();
    if (!api.DAP_ReadReg) return false;
    int v = 0;
    // DAP_ReadReg is itself the direction: DAP_REG_RnW belongs to
    // DAP_RegAccessBlock only and must not be ORed in here (nor does Keil's
    // SWD.cpp:321).
    const int rc = api.DAP_ReadReg(m_handle, m_dapId, static_cast<int>(regId), &v);
    if (rc != 0) {
        m_lastError = rddi::LastErrorCode();
        ORB_LOG_WARN("Dap: DP read reg=%u failed (rc=%d, err=%d)",
                     static_cast<unsigned>(regId), rc, m_lastError);
        m_accessSuspect = true;
        CheckDapStatus(rc);      // 参考 SWD_CheckStatus()：按状态码写 ABORT
        return false;
    }
    value = FromReg(v);
    LogAccess("dp read", regId, value);
    return true;
}

bool Dap::WriteDP(uint32_t regId, uint32_t value) {
    const rddi::Api& api = rddi::Get();
    if (!api.DAP_WriteReg) return false;
    const int rc = api.DAP_WriteReg(m_handle, m_dapId, static_cast<int>(regId), AsReg(value));
    if (rc != 0) {
        m_lastError = rddi::LastErrorCode();
        ORB_LOG_WARN("Dap: DP write reg=%u val=0x%08X failed (rc=%d, err=%d)",
                     static_cast<unsigned>(regId), value, rc, m_lastError);
        m_accessSuspect = true;
        CheckDapStatus(rc);
        return false;
    }
    LogAccess("dp write", regId, value);
    return true;
}

bool Dap::SelectApBank(uint32_t bank) {
    // DP SELECT = APSEL[31:24] | APBANKSEL[7:4]
    const uint32_t sel = (static_cast<uint32_t>(m_apIndex) << 24) | ((bank & 0xF) << 4);
    return WriteDP(DAP_DP_REG_SELECT, sel);
}

bool Dap::ReadApReg(uint32_t bank, uint32_t regId, uint32_t& value) {
    const rddi::Api& api = rddi::Get();
    if (!api.DAP_ReadReg) return false;
    if (!SelectApBank(bank)) return false;
    int v = 0;
    const int rc = api.DAP_ReadReg(m_handle, m_dapId, static_cast<int>(regId), &v);
    if (rc != 0) {
        m_lastError = rddi::LastErrorCode();
        ORB_LOG_WARN("Dap: AP read bank=%u reg=%u failed (rc=%d, err=%d)",
                     bank, static_cast<unsigned>(regId), rc, m_lastError);
        m_accessSuspect = true;
        CheckDapStatus(rc);
        return false;
    }
    value = FromReg(v);
    return true;
}

bool Dap::WriteApReg(uint32_t bank, uint32_t regId, uint32_t value) {
    const rddi::Api& api = rddi::Get();
    if (!api.DAP_WriteReg) return false;
    if (!SelectApBank(bank)) return false;
    const int rc = api.DAP_WriteReg(m_handle, m_dapId, static_cast<int>(regId), AsReg(value));
    if (rc != 0) {
        m_lastError = rddi::LastErrorCode();
        ORB_LOG_WARN("Dap: AP write bank=%u reg=%u val=0x%08X failed (rc=%d, err=%d)",
                     bank, static_cast<unsigned>(regId), value, rc, m_lastError);
        m_accessSuspect = true;
        CheckDapStatus(rc);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Target control
// ---------------------------------------------------------------------------
bool Dap::ClearStickyErrors() {
    // Writes to DP ABORT clear sticky error flags (0x1E = all).
    return WriteDP(DAP_DP_REG_ABORT, 0x0000001Eu);
}

// DP ABORT 寄存器的位（DbgCM/Debug.h:70-74，与参考同名同值）。
const uint32_t kDapAbortBit     = 0x00000001u;  // DAPABORT
const uint32_t kDapAbortStkErr  = 0x00000004u;  // STKERRCLR
const uint32_t kDapAbortWdErr   = 0x00000008u;  // WDERRCLR

// ---------------------------------------------------------------------------
// 参考 SWD_CheckStatus()（DbgCM/SWD.cpp:194-213）
// ---------------------------------------------------------------------------
// 每个 SWD 读写在参考里都包了这一层：按 RDDI 的**具体状态码**做对应的 ABORT 写。
//
//     RDDI_DAP_OPERATION_TIMEOUT (0x100E) -> DAP_WriteReg(ABORT, DAPABORT)
//     RDDI_DAP_DP_STICKY_ERR     (0x100F) -> DAP_WriteReg(ABORT, STKERRCLR|WDERRCLR)
//
// ⚠ **不是线复位。** 参考的 `SWJ_Reset()` 是空函数（"RDDI will handle this
//   automatically, no need to send the sequence again"）；把链路拉回来的是这两个
//   ABORT 写。实测：一次越界访问之后 `DAP_WriteReg` 会持续返回 0x100E，正是这一段
//   要处理的情况；只打日志不处理，就是"错误访问过一次就再也读不出"。
int Dap::CheckDapStatus(int rc) {
    const rddi::Api& api = rddi::Get();
    if (!api.DAP_WriteReg) return kRddiDapError;

    if (rc == kRddiDapOpTimeout) {
        // SWD 事务 WAIT/NO_ACK —— 复位 DP 的传输状态机。
        api.DAP_WriteReg(m_handle, m_dapId, DAP_DP_REG_ABORT, static_cast<int>(kDapAbortBit));
        return kRddiDapErrorMemory;
    }
    if (rc == kRddiDapStickyErr) {
        // DP 粘滞错误 —— 清 STICKYERR / WDATAERR。
        api.DAP_WriteReg(m_handle, m_dapId, DAP_DP_REG_ABORT,
                         static_cast<int>(kDapAbortStkErr | kDapAbortWdErr));
        return kRddiDapErrorMemory;
    }
    if (rc != 0) return kRddiDapError;
    return 0;
}

bool Dap::PowerUpDebug() {
    // CTRL/STAT bit values, taken from DbgCM/Debug.h (the only authoritative
    // source for what this RDDI host expects):
    //   CSYSPWRUPREQ 0x40000000   CDBGPWRUPREQ 0x10000000
    //   CSYSPWRUPACK 0x80000000   CDBGPWRUPACK 0x20000000
    // The ACK mask is NOT the REQ mask.  Using 0x50000000 for both made
    // (value & ack) == ack unsatisfiable - 0xA0000000 & 0x50000000 == 0 - so
    // every session logged "power-up acknowledge timeout" on a perfectly
    // healthy target and the failure was blamed on wiring/speed (bug.md E6).
    const uint32_t req       = 0x50000000u; // CSYSPWRUPREQ | CDBGPWRUPREQ
    const uint32_t ack       = 0xA0000000u; // CSYSPWRUPACK | CDBGPWRUPACK
    const uint32_t maskLane  = 0x00000F00u; // MASKLANE: TRNNORMAL is 0
    const uint32_t stickyClr = 0x0000001Eu; // STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR

    // Precondition the DP: clear whatever sticky error a previous session left
    // behind before the first CTRL/STAT write.  Keil's SWD_CheckStatus does the
    // same DAPABORT recovery, just lazily on the first failure; doing it up
    // front makes the sequence independent of how the last session ended.
    if (!WriteDP(DAP_DP_REG_ABORT, stickyClr)) {
        ORB_LOG_WARN("Dap: initial ABORT sticky clear failed (%d)", m_lastError);
    }

    if (!WriteDP(DAP_DP_REG_CTRL_STAT, req)) {
        ORB_LOG_ERROR("Dap: CTRL/STAT power request failed (%d)", m_lastError);
        return false;
    }

    // Keil waits up to 1 s here; keep the same budget.
    uint32_t v = 0;
    const uint64_t start = GetTickCount64();
    bool powered = false;
    for (;;) {
        if (!ReadDP(DAP_DP_REG_CTRL_STAT, v)) {
            ORB_LOG_ERROR("Dap: CTRL/STAT power poll failed (%d)", m_lastError);
            return false;
        }
        if ((v & ack) == ack) { powered = true; break; }
        if ((GetTickCount64() - start) >= 1000u) break;
        Sleep(1);
    }

    if (powered) {
        ORB_LOG_INFO("Dap: debug power up (CTRL/STAT = 0x%08X)", v);
    } else {
        // Print what the DP actually said.  A bare "timeout" is what let a
        // wrong mask survive three sessions of debugging.
        ORB_LOG_WARN("Dap: power-up acknowledge timeout "
                     "(CTRL/STAT = 0x%08X, wait ACK = 0x%08X)", v, ack);
    }

    // Keil's SWD path follows the power request with a second CTRL/STAT write
    // that adds MASKLANE, then clears the sticky flags via ABORT.  Both belong
    // to the normal connect sequence, not to error recovery.  A DP that never
    // acknowledges still gets these - they are cheap and some targets only
    // report ACK after the lane mask is set.
    if (!WriteDP(DAP_DP_REG_CTRL_STAT, req | maskLane)) {
        ORB_LOG_WARN("Dap: CTRL/STAT lane-mask write failed (%d)", m_lastError);
    }
    if (!WriteDP(DAP_DP_REG_ABORT, stickyClr)) {
        ORB_LOG_WARN("Dap: ABORT sticky clear failed (%d)", m_lastError);
    }
    return powered;
}

// ---------------------------------------------------------------------------
// MEM-AP memory access
// ---------------------------------------------------------------------------
bool Dap::ConfigureMemAp(AccessSize size) {
    uint32_t csw = kCswBase | kCswAddrInc;
    switch (size) {
        case AccessSize::U8:  csw |= kCswSizeU8;  break;
        case AccessSize::U16: csw |= kCswSizeU16; break;
        case AccessSize::U32: csw |= kCswSizeU32; break;
    }
    if (csw == m_cswCache) return true;
    if (!WriteApReg(0, DAP_AP_REG_CSW, csw)) return false;
    m_cswCache = csw;
    return true;
}

// One byte of memory, done as a read-modify-write of the enclosing word.
//
// The MEM-AP's byte lanes are not portable: with CSW.SIZE = byte the datum only
// lands where it belongs when it sits in the lane matching the address, so a
// plain DRW[7:0] write to an odd address stores 0x00 instead.  That is how the
// BKPT sentinel - and every software breakpoint - ended up as 0x0000 while the
// API still reported success (bug.md E13).  Word accesses are the ones the rest
// of this file already proves work, so the byte rides along in one.
bool Dap::MemByte(uint32_t addr, uint8_t* p, bool write) {
    const uint32_t w    = addr & ~3u;
    const uint32_t lane = (addr & 3u) * 8u;

    if (!WriteApReg(0, DAP_AP_REG_TAR, w)) return false;
    uint32_t v = 0;
    if (!ReadApReg(0, DAP_AP_REG_DRW, v)) return false;

    if (write) {
        v = (v & ~(0xFFu << lane)) | ((static_cast<uint32_t>(*p) & 0xFFu) << lane);
        if (!WriteApReg(0, DAP_AP_REG_TAR, w)) return false;
        if (!WriteApReg(0, DAP_AP_REG_DRW, v)) return false;
    } else {
        *p = static_cast<uint8_t>((v >> lane) & 0xFFu);
    }
    return true;
}

bool Dap::MemBulk(uint32_t addr, uint8_t* buf, uint32_t len, bool write) {
    if (!ConfigureMemAp(AccessSize::U32)) return false;

    uint32_t a = addr;
    uint32_t rem = len;
    uint8_t* p = buf;

    // 1) leading bytes until the address is word aligned
    while (rem > 0 && (a & 3u) != 0) {
        if (!MemByte(a, p, write)) return false;
        ++p; ++a; --rem;
    }

    // 2) aligned word run: stream DRW and let the AP auto-increment the address.
    //
    // The increment only spans a 1 KB window.  Past it the address wraps back to
    // the start of the window instead of advancing, so the run re-reads bytes it
    // already transferred - silently, with the API still reporting success.
    // A 1024 byte access never leaves the window, which is why ProgramRange's
    // per-page readback looked perfect while VerifyRange's single 16996 byte
    // read reported bogus bytes from offset 0x1000 on (bug.md E19).  Re-arm TAR
    // at every window boundary; the SS-ADDRINC=0b01 "single increment" in CSW
    // is what the window applies to.
    if (rem >= 4) {
        const rddi::Api& api = rddi::Get();
        const bool bulk = write ? (api.DAP_RegWriteRepeat != nullptr)
                                : (api.DAP_RegReadRepeat != nullptr);
        while (rem >= 4) {
            // One block never crosses a 1 KB auto-increment window.  Same split
            // the reference driver makes, "n = rwpage - (ba & (rwpage - 1))"
            // (DbgCM/Flash.cpp:347); it is what stops a long run from wrapping
            // back to the window start and repeating bytes (bug.md E19).
            uint32_t n = 0x400u - (a & 0x3FFu);
            if (n > rem) n = rem;
            n &= ~3u;
            if (n == 0) n = 4;

            if (!WriteApReg(0, DAP_AP_REG_TAR, a)) return false;

            if (bulk) {
                // Repeat the AP transfer in one RDDI call; the host turns it
                // into a CMSIS-DAP block transfer (DbgCM/SWD.cpp:1247 / :1118).
                // One DAP_WriteReg per word instead costs one full SWD
                // transaction per word and is what made downloads crawl.
                const uint32_t words = n >> 2;
                if (words > 256u) return false;
                uint32_t tmp[256];   // one RWPage worth of words
                if (write) {
                    memcpy(tmp, p, n);
                    if (api.DAP_RegWriteRepeat(m_handle, m_dapId,
                                               static_cast<int>(words),
                                               DAP_AP_REG_DRW,
                                               reinterpret_cast<const int*>(tmp)) != 0) {
                        m_lastError = rddi::LastErrorCode();
                        ORB_LOG_WARN("Dap: block write of %u word(s) at 0x%08X failed (%d)",
                                     words, a, m_lastError);
                        return false;
                    }
                } else {
                    if (api.DAP_RegReadRepeat(m_handle, m_dapId,
                                              static_cast<int>(words),
                                              DAP_AP_REG_DRW,
                                              reinterpret_cast<int*>(tmp)) != 0) {
                        m_lastError = rddi::LastErrorCode();
                        ORB_LOG_WARN("Dap: block read of %u word(s) at 0x%08X failed (%d)",
                                     words, a, m_lastError);
                        return false;
                    }
                    memcpy(p, tmp, n);
                }
            } else {
                // No repeat entry points in this host: one word at a time.
                for (uint32_t off = 0; off < n; off += 4) {
                    if (write) {
                        uint32_t v;
                        memcpy(&v, p + off, 4);
                        if (!WriteApReg(0, DAP_AP_REG_DRW, v)) return false;
                    } else {
                        uint32_t v;
                        if (!ReadApReg(0, DAP_AP_REG_DRW, v)) return false;
                        memcpy(p + off, &v, 4);
                    }
                }
            }
            p += n; a += n; rem -= n;
        }
    }

    // 3) trailing bytes
    while (rem > 0) {
        if (!MemByte(a, p, write)) return false;
        ++p; ++a; --rem;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Read cache - see the declaration in ORBAGDI_Dap.h
// ---------------------------------------------------------------------------
Dap::~Dap() {
    delete[] m_cache;
    m_cache = nullptr;
}

Dap::CacheLine* Dap::CacheLines() {
    if (!m_cache) m_cache = new CacheLine[kCacheLineCount]();
    return m_cache;
}

/// Is a read of this range worth caching?  "Cache Memory" caches everything;
/// "Cache Code" only the on-chip flash window uVision reported, so a data read
/// is never served from a line that could have gone stale through a pointer.
///
/// The System Control Space is excluded unconditionally, whatever the switches
/// say.  Those addresses are not memory: DHCSR changes on its own as the core
/// halts, and DCRDR holds whatever the last DCRSR access selected - so a cache
/// line there is stale the moment it is filled.  Reading the CPU registers goes
/// through exactly this path (MemRead32 -> MemRead), and the DCRSR write in
/// between invalidates DCRSR's line but not DCRDR's, which lands in a different
/// line.  With the default cachemem=1 that made every register read return the
/// value the first one had seen: measured with the AGDI probe, PC / SP / LR /
/// xPSR / MSP all came back as the same flash address, and uVision's register
/// window would have shown one number sixteen times
/// (COMPAT_ANALYSIS.md §10.8).
bool Dap::CacheEnabled(uint32_t addr, uint32_t len) const {
    const cfg::Config& opt = cfg::Get();
    if (len == 0 || len > kCacheMaxBytes) return false;
    // 0xE0000000..0xE00FFFFF: ITM / DWT / FPB / DCB / TPIU / ETM / ROM table.
    if (addr < 0xE0100000u && (addr + len) > 0xE0000000u) return false;
    if (opt.cacheMem) return true;
    if (!opt.cacheCode) return false;
    const cfg::Config& a = cfg::AutoDetected();
    if (a.iromSize == 0) return false;              // code window unknown
    return addr >= a.iromStart && (addr + len) <= (a.iromStart + a.iromSize);
}

bool Dap::CacheLookup(uint32_t addr, void* buf, uint32_t len) const {
    if (!m_cache) return false;
    const CacheLine& l = m_cache[(addr >> 3) & (kCacheLineCount - 1)];
    if (l.len == 0) return false;
    if (addr < l.tag || (addr + len) > (l.tag + l.len)) return false;
    memcpy(buf, l.data + (addr - l.tag), len);
    return true;
}

void Dap::CacheStore(uint32_t addr, const void* buf, uint32_t len) {
    CacheLine& l = CacheLines()[(addr >> 3) & (kCacheLineCount - 1)];
    l.tag = addr;
    l.len = len;
    memcpy(l.data, buf, len);
}

void Dap::CacheInvalidate(uint32_t addr, uint32_t len) {
    if (!m_cache) return;
    for (int i = 0; i < kCacheLineCount; ++i) {
        CacheLine& l = m_cache[i];
        if (l.len == 0) continue;
        if (addr < l.tag + l.len && l.tag < addr + len) l.len = 0;
    }
}

void Dap::CacheFlush() {
    if (!m_cache) return;
    for (int i = 0; i < kCacheLineCount; ++i) m_cache[i].len = 0;
}

bool Dap::MemRead(uint32_t addr, void* buf, uint32_t len) {
    if (!m_connected) return false;
    if (len == 0) return true;

    LogAccess("mem read", addr, len);

    if (CacheLookup(addr, buf, len)) return true;

    if (CacheEnabled(addr, len)) {
        // Round the range out to 8 bytes, so the next read of the same block -
        // which is exactly what disassembly and the memory window do - hits.
        // A few bytes either side are safe inside a mapped region; if the
        // rounded read fails (the range runs off the end of one) the exact read
        // below still gives the caller an honest answer.
        const uint32_t start = addr & ~7u;
        const uint32_t end   = (addr + len + 7u) & ~7u;
        const uint32_t span  = end - start;
        if (span <= kCacheMaxBytes) {
            uint8_t tmp[kCacheMaxBytes];
            if (MemBulk(start, tmp, span, false)) {
                CacheStore(start, tmp, span);
                memcpy(buf, tmp + (addr - start), len);
                return true;
            }
        }
    }

    return MemBulk(addr, static_cast<uint8_t*>(buf), len, false);
}

bool Dap::MemWrite(uint32_t addr, const void* buf, uint32_t len) {
    if (!m_connected) return false;
    if (len == 0) return true;
    LogAccess("mem write", addr, len);
    CacheInvalidate(addr, len);      // a cached read must not outlive the value
    return MemBulk(addr, reinterpret_cast<uint8_t*>(const_cast<void*>(buf)), len, true);
}

} // namespace orb
