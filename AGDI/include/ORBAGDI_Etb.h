/**
 * @file ORBAGDI_Etb.h
 * @brief ETB (Embedded Trace Buffer) / TMC-in-ETB-mode support.
 *
 * The ETB is an on-chip trace sink: instead of streaming the trace out of a
 * pin (SWO) it captures it into a RAM block that the debugger reads back
 * through the MEM-AP.  The trace data is exactly the same ITM/ETM stream the
 * SWO console decodes, so the captured ITM bytes are handed to the ITM decoder
 * in ORBAGDI_Itm.cpp and reach uVision through the very same callbacks.
 *
 * Life cycle, mirroring Keil's DbgCM (ETB.cpp):
 *      EtbDetect()    - CoreSight ROM table scan, records the ETB base
 *      EtbSetup()     - read RAM depth/width, latch the formatter mode
 *      EtbActivate()  - arm capture (call before resuming the target)
 *      EtbProcess()   - stop capture, read the RAM, decode (call after halt)
 *      EtbReset()     - forget everything (start of a debug session)
 *
 * All programming is done over the MEM-AP of the Dap; no RDDI trace extension
 * is involved (unlike SWO), so this works on any RDDI host.
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// Scan the CoreSight ROM table for an ETB (or a Trace Memory Controller
// configured in ETB mode).  Records the location on success.  Returns false
// when the device has no usable ETB.
bool EtbDetect(orb::Dap& dap);

// True once EtbDetect() has found an ETB.
bool EtbAvailable();

// True when the found sink is a TMC in ETB mode (registers/pointers differ
// from a plain CoreSight ETB).
bool EtbIsTmc();

// Register base of the ETM found by the ROM scan (0 = the table has no ETM).
//
// The ETM address is a device fact, not a constant: it comes out of the same
// CoreSight ROM table walk that finds the ETB, matched on the part numbers in
// DbgCM/Debug.cpp:284-292 (PN_ETM_CM3 0x924 / CM4 0x925 / SC3 0x926 / CM7 0x975).
// `ORBAGDI_Itm.cpp` used to hardcode 0xE0041000 + offset for six registers, which
// is wrong on any part that puts its ETM elsewhere - and the only symptom is
// "not a single packet arrives" (COMPAT_ANALYSIS.md §10.28 item 4).
uint32_t EtbEtmBase();

// Walk the CoreSight ROM table without touching the ETB state.
//
// `EtbDetect()` resets that state, so it cannot be used from the ITM/ETM side
// where an ETB session may already be running.  This is the entry point that
// resolves `EtbEtmBase()` for a session that only wants instruction trace.
bool EtbScanRom(orb::Dap& dap);

// Number of ETM branches decoded from the ETB stream so far (0 when no ETM data
// was seen).  The decoder is the AGDI-layer ETM decoder (ORBAGDI_Etm.h); the
// decoded branches are kept for a future uVision Trace-window upload path.
unsigned EtbEtmBranchCount();

// ---------------------------------------------------------------------------
// Setup / control
// ---------------------------------------------------------------------------

// Read the ETB properties (RAM depth and word width) and latch the formatter
// mode.  Must be called after EtbDetect() and before EtbActivate().
bool EtbSetup(orb::Dap& dap);

// Arm trace capture.  Call just before the target is resumed.
bool EtbActivate(orb::Dap& dap);

// Register the trace window and mark the sink usable (official `ETB_Init`,
// DbgCM/ETB.cpp:1317).  Returns false when no ETB was found - the official
// answers EU32 ("ETB requested but not available") in that case.
bool EtbInit();

// Manually flush the ETB and wait for the flush to finish (official
// `ETB_Flush`, DbgCM/ETB.cpp:968).  Sets STOPFL, triggers FONMAN and polls for
// up to 10 ms.  Also called by the stop-capture path.
bool EtbFlush(orb::Dap& dap);

// True when the ETB is stopped and its buffer can be safely read (official
// `ETB_CanAccess`, DbgCM/ETB.cpp:1339): the formatter's FTSTOPPED bit is set.
bool EtbCanAccess(orb::Dap& dap);

// Notice that the buffer was consumed, which enables another update from the
// ETB (official `ETB_Clear`, DbgCM/ETB.cpp:1377).
void EtbClear();

// Clear the formatter stop bits a previous stop left behind, so capture can be
// armed again (official `ETB_Recovery`, DbgCM/ETB.cpp:1386).  A no-op while the
// ETB is still capturing.
bool EtbRecovery(orb::Dap& dap);

// Stop capture, read the whole buffer, decode it and forward the ITM packets
// to uVision.  Call after the target has halted.  Returns the number of ETB
// bytes processed (>= 0), or -1 on a hard error.
int EtbProcess(orb::Dap& dap);

// Forget all state (address, properties, buffers).  Start of a session.
void EtbReset();

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
struct EtbStatus {
    bool     present;     // an ETB/TMC was found
    bool     tmc;         // the sink is a TMC in ETB mode
    bool     configured;  // EtbSetup() succeeded
    bool     active;      // capture is currently armed
    uint32_t addr;        // ETB register base address
    uint32_t depth;       // RAM depth in words
    uint32_t width;       // RAM word width in bytes (3 or 4)
    bool     wraparound;  // last read wrapped around (oldest data lost)

    // Trace sources seen in the captured stream (official `ETB_ITMConnected` /
    // `ETB_ETMConnected`, DbgCM/ETB.h:47-48).  Latched while unpacking the
    // formatter frames, so it reflects what the sink actually received.
    bool     itmConnected;
    bool     etmConnected;

    // Trigger delay programmed into ETB_TRG_CNT at activation (official
    // `ETB_TrigDelay`, DbgCM/ETB.h:42).
    uint32_t trigDelay;
};

EtbStatus EtbGetStatus();

} // namespace agdi
} // namespace orb
