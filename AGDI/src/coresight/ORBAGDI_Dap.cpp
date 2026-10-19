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
            // 第三个参数是宿主自己的配置串：驱动哪个口、SWJ-DP 是否允许在两口之间切换。只在该说的话
            // 时才拼，纯 SWD 场景保持历来的调用不变。宿主不认这个串会回非零，那只是 WARN。
            // 串统一来自 rddi::InterfaceConfig —— 设置页的 AP 探测必须问同一个东西，否则探的是没绑接口的会话。
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

            // SWJ 时钟故意不在这里编：文档顺序是 Connect → SWJ_Clock，且 JTAG 连接自己会把时钟限到
            // 4 MHz，链路没起来前设的速率只会被覆盖。挪到 CMSIS_DAP_Connect 之后。
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

    // DAP_ReadReg / DAP_WriteReg 收的是 DAP 索引，不是 IDCODE：值落进 CMSIS-DAP 的
    // DAP_Transfer 索引字节，给 IDCODE 就是越界索引、每次 AP 访问都失败。
    // 宿主对 ID 列表含义不一致（ORBMDK 回索引、Keil 的 DLL 回真 IDCODE），故索引不从它推导：
    // DAP 按 0..n-1 编址，只用第一个；列表只记日志供识别。
    for (int i = 0; i < numDaps && i < 16; ++i) {
        ORB_LOG_INFO("Dap: DAP[%d] ID = 0x%08X", i, static_cast<unsigned>(dapIds[i]));
    }
    (void)haveList;
    m_dapId = 0;
    ORB_LOG_INFO("Dap: %d DAP(s) found, using DAP index %d", numDaps, m_dapId);

    // 链路本身。CMSIS_DAP_Connect 才是做 SWJ/SWD 切换序列 + 首次 DPIDR 读的入口，DAP_Connect
    // 只填一个实现串、不碰链路。本驱动原来两者都没调，直接读 DP、指望宿主自己连好。
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

    // MEM-AP 连 IDR 或 CSW 都不应答，就一次内存访问也做不了，会话在开始前已经结束。这里报成功
    // 正是"uVision 说附着成功、之后每次访问都失败"的成因。只有 AP 症状致命：上面那些 DP 症状
    // 故意容忍（多个 RDDI 宿主在健康目标上也不报）。全 1 的 IDR 是 SWD "无响应"，同样算死。
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
    // ⚠ 不要在 !m_connected 时直接 return：m_connected 只在"DP IDCODE 读到了"之后才置起，而句柄
    // 在更早就开出来了（RDDI_Open 之后、DP 探测之前）—— 于是 Connect 失败那条路上句柄永远关不掉、
    // 下次 Connect 再 Open 一个。2026-10-14 的完整重建正好走这条路。
    const rddi::Api& api = rddi::Get();
    const bool had = (m_handle != 0);
    if (m_handle) {
        // 两者都要，按文档顺序（先 DAP_Disconnect 再 RDDI_Close）。原来写成二选一，宿主导出了 Close
        // 就跳过 DAP_Disconnect ⇒ 链路从没被拆，只关了会话句柄。
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
// DP / AP 原始寄存器访问
// 第三个参数是编码后的寄存器 id，不是字节地址：DP 0..3、AP 4..7，AP bank 靠写 DP SELECT
// （id 2）切；编码只在 ORBAGDI_Rddi.h 里存在一次。
// 曾照抄 Keil 的 "DAP_REG_DP_0x0 + (adr >> 2)" 而调用方已给编码 id ⇒ 又移了一次：CTRL/STAT
// 读到 IDCODE、SELECT 写不进、CSW/DRW 落进 TAR（日志里 CTRL/STAT 回读等于刚读的 DPIDR）。
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
// 参考 SWD_CheckStatus()：按 RDDI 的具体状态码做对应的 ABORT 写
//   0x100E（OPERATION_TIMEOUT）→ ABORT=DAPABORT；0x100F（DP_STICKY_ERR）→ STKERRCLR|WDERRCLR
// ⚠ 不是线复位：参考的 SWJ_Reset() 是空函数（"RDDI 自己会处理"），把链路拉回来的是这两个
// ABORT 写。只打日志不处理，就是"错误访问过一次就再也读不出"。
// ---------------------------------------------------------------------------
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
    // CTRL/STAT 位值取自 DbgCM/Debug.h：CSYSPWRUPREQ 0x40000000 / CDBGPWRUPREQ 0x10000000，
    // 对应 ACK 是 0x80000000 / 0x20000000。⚠ ACK 掩码不是 REQ 掩码：两者都用 0x50000000 时
    // (value & ack) == ack 恒不成立，健康目标也报 "power-up acknowledge timeout"。
    const uint32_t req       = 0x50000000u; // CSYSPWRUPREQ | CDBGPWRUPREQ
    const uint32_t ack       = 0xA0000000u; // CSYSPWRUPACK | CDBGPWRUPACK
    const uint32_t maskLane  = 0x00000F00u; // MASKLANE: TRNNORMAL is 0
    const uint32_t stickyClr = 0x0000001Eu; // STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR

    // 先给 DP 打个底：清掉上一次会话留下的粘滞错误，再写 CTRL/STAT。参考的 SWD_CheckStatus 只在
    // 首次失败时做 DAPABORT，前置做则让这条序列与上次会话怎么结束无关。
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

    // 参考的 SWD 路径在 power request 之后还有第二次 CTRL/STAT 写（补 MASKLANE），再用 ABORT 清
    // 粘滞位。两者都属于正常连接序列、不是错误恢复；不应答的 DP 也照样发，很便宜，
    // 有些目标要等 lane mask 置上才回 ACK。
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

// 一个字节 = 读改写所在字。MEM-AP 的字节通道不可移植：CSW.SIZE=byte 时数据只落在与地址匹配的
// 通道上，往奇地址写 DRW[7:0] 实际存 0x00 —— BKPT 哨兵和所有软断点就这样变成 0x0000、
// API 还报成功。字访问是本文件已验证可用的，让字节搭车走字。
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

    // 2) 对齐字：连续写 DRW，让 AP 自增地址。
    // ⚠ 自增只在一个 1 KB 窗口内有效，越界会绕回窗口头、重复读已读过的字节（静默，API 还报成功）。
    // 一次 1024 B 不越界，故 ProgramRange 逐页回读完美，而 VerifyRange 单次 16996 B 从 0x1000 起
    // 全是假数据。每个窗口边界重装 TAR（CSW 的 SS-ADDRINC=0b01 就是按窗口自增）。
    if (rem >= 4) {
        const rddi::Api& api = rddi::Get();
        const bool bulk = write ? (api.DAP_RegWriteRepeat != nullptr)
                                : (api.DAP_RegReadRepeat != nullptr);
        while (rem >= 4) {
            // 一个块绝不跨 1 KB 自增窗口。与参考同样的切分 "n = rwpage - (ba & (rwpage - 1))"，
            // 这是防长跑绕回窗口头重复字节的关键。
            uint32_t n = 0x400u - (a & 0x3FFu);
            if (n > rem) n = rem;
            n &= ~3u;
            if (n == 0) n = 4;

            if (!WriteApReg(0, DAP_AP_REG_TAR, a)) return false;

            if (bulk) {
                // 一次 RDDI 调用重复同一个 AP 传输，宿主会把它变成 CMSIS-DAP 块传输。逐字调 DAP_WriteReg
                // 就是每字一次完整 SWD 事务 —— 下载慢就慢在这。
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

/// 这段区间值不值得缓存？"Cache Memory" 什么都缓存；"Cache Code" 只缓存 uVision 报的片上 flash
/// 窗口，数据读永远不会命中可能被指针改脏的行。
/// 系统控制区无条件排除：DHCSR 随核心停下自己变、DCRDR 保存上次 DCRSR 选中的值，缓存行填上就
/// 过期。寄存器读正是走 MemRead32 → MemRead，中间的 DCRSR 写只失效 DCRSR 的行、不失效 DCRDR
/// 的行 —— 默认 cachemem=1 时实测 PC/SP/LR/xPSR/MSP 全回同一个 flash 地址。
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
        // 按 8 字节取整，让下一次读同一块（反汇编和内存窗口正是这么读）直接命中。多读几个字节在已映射
        // 区内是安全的；取整读失败（跑到区尾）时下面的精确读仍能给调用方一个诚实答案。
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
