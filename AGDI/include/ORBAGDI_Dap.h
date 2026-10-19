/**
 * @file ORBAGDI_Dap.h
 * @brief DAP 层：DP/AP 寄存器访问 + MEM-AP 内存访问。
 * 只用一个 MEM-AP（默认 0，可配），内存访问经 DRW 自增 TAR。
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
    // regId 是 ORBAGDI_Rddi.h 里的编码寄存器号（DP 0..3 / AP 4..7），不是字节地址。
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

    // 强制下次 MEM-AP 访问重写 CSW。
    // ETB 读路径直接写了不自增的 CSW，缓存会失真，不重写会带着旧值跑。
    void InvalidateCswCache() { m_cswCache = 0xFFFFFFFFu; }

    // ---- target control ---------------------------------------------------
    bool ClearStickyErrors();
    bool PowerUpDebug();

    // 把 RDDI 原始返回码翻成语义，并按码写对应的 ABORT 寄存器。
    // 这恢复链路不靠线复位（RDDI 自己处理），靠的就是这两个 ABORT 写。
    int CheckDapStatus(int rc);

    // Drives nRESET through the probe's SWJ pin interface (CMSIS-DAP pin 7).
    // Returns false when the host has no CMSIS_DAP_SWJ_Pins entry point.
    bool ResetPin(bool assert);
    uint32_t Dpidr() const { return m_dpidr; }
    uint32_t ApIdr() const { return m_apidr; }

    int LastError() const { return m_lastError; }

    // 任一次 DP/AP 访问失败都会置起，成功探测后由 DebugAccessDetection() 清掉。
    // 后台监视器只在置起时才做等级探测，健康会话因此不产生额外探针流量。
    bool AccessSuspect() const { return m_accessSuspect; }
    void ClearAccessSuspect() { m_accessSuspect = false; }
    RDDIHandle Handle() const { return m_handle; }

    // ---- read cache (Debug page "Cache Code" / "Cache Memory") ------------
    // 只缓存整段成功的读，不做投机预取；写入重叠或运行状态变化时丢行。
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
