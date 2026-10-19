/**
 * @file ORBAGDI_Dap.h
 * @brief Debug Access Port layer: DP/AP register access + MEM-AP memory access.
 *
 * Everything below uses a single MEM-AP (default index 0, configurable).
 * Memory accesses auto-increment TAR through the AP's DRW register.
 */
#pragma once

#include "ORBAGDI_Rddi.h"
#include <stdint.h>

namespace orb {

// AHB-AP CSW base (matches the ARM/SWD host reference and elaphureLink):
//   CSW_RESERVED | CSW_MSTRDBG | CSW_HPROT_PRIV | CSW_DBGSTAT
// = 0x01000000   | 0x20000000  | 0x02000000     | 0x00000040
// Access is completed with | CSW_SADDRINC (0x10) | size(0..2).
enum : uint32_t {
    kCswBase     = 0x23000040u,
    kCswAddrInc  = 0x00000010u,
    kCswSizeU8   = 0u,
    kCswSizeU16  = 1u,
    kCswSizeU32  = 2u,
};

enum class AccessSize { U8, U16, U32 };

class Dap {
public:
    Dap();
    ~Dap();

    // ---- connection -------------------------------------------------------
    // Opens the RDDI DLL (if needed), opens the RDDI handle, picks the
    // interface, connects to the target and probes the DP IDCODE.
    bool Connect(const char* ifaceHint = nullptr);
    void Disconnect();
    bool IsConnected() const { return m_connected; }

    // DAP/AP selection.
    void SetDapIndex(int dapId) { m_dapId = dapId; }
    int  DapIndex() const { return m_dapId; }
    void SetApIndex(int ap) { m_apIndex = ap; }
    int  ApIndex() const { return m_apIndex; }

    // ---- raw DP / AP register access -------------------------------------
    // regId is the *encoded* RDDI register id from ORBAGDI_Rddi.h - DP 0..3
    // (DAP_DP_REG_*), AP 4..7 (DAP_AP_REG_*) - NOT a byte address.  Passing a
    // byte address here is what made CTRL/STAT read back the DPIDR (bug.md E7).
    bool ReadDP(uint32_t regId, uint32_t& value);
    bool WriteDP(uint32_t regId, uint32_t value);
    // AP register access, bank 0 (CSW/TAR/DRW) or 0xF (IDR/BASE); regId is a
    // DAP_AP_REG_* constant.
    bool ReadApReg(uint32_t bank, uint32_t regId, uint32_t& value);
    bool WriteApReg(uint32_t bank, uint32_t regId, uint32_t value);
    bool SelectApBank(uint32_t bank);

    // ---- MEM-AP memory access --------------------------------------------
    bool MemRead(uint32_t addr, void* buf, uint32_t len);
    bool MemWrite(uint32_t addr, const void* buf, uint32_t len);
    bool MemRead8 (uint32_t addr, uint8_t&  v) { return MemRead (addr, &v, 1); }
    bool MemRead16(uint32_t addr, uint16_t& v) { return MemRead (addr, &v, 2); }
    bool MemRead32(uint32_t addr, uint32_t& v) { return MemRead (addr, &v, 4); }
    bool MemWrite8 (uint32_t addr, uint8_t  v) { return MemWrite(addr, &v, 1); }
    bool MemWrite16(uint32_t addr, uint16_t v) { return MemWrite(addr, &v, 2); }
    bool MemWrite32(uint32_t addr, uint32_t v) { return MemWrite(addr, &v, 4); }

    // Force the next MEM-AP access to reprogram CSW.  The ETB read path writes
    // a non-incrementing CSW directly (the RD_DATA port advances its own
    // pointer, so TAR must not move), which leaves m_cswCache stale; without
    // this the following MemRead would skip the CSW write and run with the
    // non-incrementing value still in the AP.
    void InvalidateCswCache() { m_cswCache = 0xFFFFFFFFu; }

    // ---- target control ---------------------------------------------------
    bool ClearStickyErrors();
    bool PowerUpDebug();

    // 参考 `SWD_CheckStatus()`（DbgCM/SWD.cpp:194-213）：把 RDDI 的原始返回码翻成
    // 语义，并按码做**对应的 ABORT 寄存器写**。
    //
    // ⚠ 不是线复位。参考的 `SWJ_Reset()` 是空函数（"RDDI will handle this
    //   automatically, no need to send the sequence again"）；把链路拉回来的是这两个
    //   ABORT 写。返回 0 或 `rddi::kRddiDap*` 的语义化结果。
    int CheckDapStatus(int rc);

    // Drives nRESET through the probe's SWJ pin interface (CMSIS-DAP pin 7).
    // Returns false when the host has no CMSIS_DAP_SWJ_Pins entry point.
    bool ResetPin(bool assert);
    uint32_t Dpidr() const { return m_dpidr; }
    uint32_t ApIdr() const { return m_apidr; }

    int LastError() const { return m_lastError; }

    // 任一次 DP/AP 访问失败都会置起它。后台监视器据此决定要不要做等级探测 ——
    // **健康的会话因此完全不产生额外探针流量**，P0-A 的"无源不碰锁"保证不受影响；
    // 只有真出过错才会去探。成功探测后由 `DebugAccessDetection()` 清掉。
    bool AccessSuspect() const { return m_accessSuspect; }
    void ClearAccessSuspect() { m_accessSuspect = false; }
    RDDIHandle Handle() const { return m_handle; }

    // ---- read cache (Debug page "Cache Code" / "Cache Memory") ------------
    // uVision re-reads the same code and data constantly - disassembly, the
    // memory window, the source view - and every read is a MEM-AP round trip.
    // Only *successful whole reads* are cached, never speculative fetches, so a
    // hit can never touch an address the caller did not ask for.  A write that
    // overlaps a line drops it, and so does a run-control change.
    void CacheFlush();

private:
    enum { kCacheLineCount = 128, kCacheMaxBytes = 256 };

    struct CacheLine {
        uint32_t tag;                  // address the line was read from
        uint32_t len;                  // 0 = empty
        uint8_t  data[kCacheMaxBytes];
    };

    bool ProbeDap();
    bool ConfigureMemAp(AccessSize size);
    bool MemBulk(uint32_t addr, uint8_t* buf, uint32_t len, bool write);
    bool MemByte(uint32_t addr, uint8_t* p, bool write);

    CacheLine* CacheLines();
    bool CacheEnabled(uint32_t addr, uint32_t len) const;
    bool CacheLookup(uint32_t addr, void* buf, uint32_t len) const;
    void CacheStore(uint32_t addr, const void* buf, uint32_t len);
    void CacheInvalidate(uint32_t addr, uint32_t len);

    RDDIHandle m_handle;
    int  m_dapId;
    int  m_apIndex;
    bool m_accessSuspect = false;   // 见 AccessSuspect()
    bool m_connected;
    uint32_t m_dpidr;
    uint32_t m_apidr;
    uint32_t m_cswCache;   // last CSW value programmed
    int  m_lastError;
    CacheLine* m_cache;    // lazily allocated; see CacheLines()
};

} // namespace orb
