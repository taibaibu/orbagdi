/**
 * @file ORBAGDI_Rddi.h
 * @brief Dynamic loader + call bridge for the RDDI layer (CMSIS_DAP.dll).
 *
 * The AGDI DLL never links against the RDDI layer; it loads it at run time so
 * that the RDDI DLL can be swapped (e.g. ORBMDK_RDDI.dll) without rebuilding.
 *
 * Signatures are copied verbatim from the standard ARM RDDI headers
 * (rddi.h / rddi_dap.h / rddi_dap_cmsis.h) and from ORBMDK/include/ORBMDK_RDDI.h.
 * All RDDI entry points use the C default calling convention (__cdecl).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int RDDIHandle;

typedef struct _RDDI_DAP_CONN_DETAILS {
    char implementorName[160];
    char connectionDescription[160];
} RDDI_DAP_CONN_DETAILS;

// ARM order is (context, msg, logLevel) - do not "fix" it.
typedef void (*RDDILogCallback)(void* context, const char* const msg, const int logLevel);

// ---------------------------------------------------------------------------
// DAP register identifiers, as expected by DAP_ReadReg / DAP_WriteReg.
//   DP : 0..3  -> DP address 0x0 / 0x4 / 0x8 / 0xC
//   AP : 4..7  -> AP address 0x0 / 0x4 / 0x8 / 0xC (bank selected via DP SELECT)
// These values are ALREADY encoded ids, not byte addresses: pass them straight
// to DAP_ReadReg / DAP_WriteReg and never shift them again.  Keil's DbgCM gets a
// byte address from its callers and encodes it itself with
// "DAP_REG_DP_0x0 + (adr >> 2)"; copying that expression on top of these
// constants double-shifted every access and broke the whole MEM-AP (bug.md E7).
// The direction is the function you call - DAP_ReadReg vs DAP_WriteReg - so a
// read is NOT "OR in DAP_REG_RnW".  DAP_REG_RnW / DAP_REG_WaitForValue belong
// to DAP_RegAccessBlock alone.
// The AP bank is selected by writing DP SELECT (regID 2) with
//      (apIndex << 24) | (bank << 4)
// ---------------------------------------------------------------------------
enum : int {
    DAP_REG_DP_0x0 = 0,   // IDCODE (read) / ABORT (write)
    DAP_REG_DP_0x4 = 1,   // CTRL/STAT
    DAP_REG_DP_0x8 = 2,   // SELECT
    DAP_REG_DP_0xC = 3,   // RDBUFF
    DAP_REG_AP_0x0 = 4,   // CSW
    DAP_REG_AP_0x4 = 5,   // TAR
    DAP_REG_AP_0x8 = 6,   // bank0: unused ; bank0xF: BASE(ROM)
    DAP_REG_AP_0xC = 7,   // DRW ; bank0xF: IDR
    DAP_REG_MATCH_MASK  = 16,
    DAP_REG_MATCH_RETRY = 17,
    DAP_REG_RnW           = 0x10000,
    DAP_REG_WaitForValue  = 0x20000,
};

// ---------------------------------------------------------------------------
// RDDI-DAP 接口错误码（ARM rddi_dap.h，0x1000 段）
// ---------------------------------------------------------------------------
// Keil 的 AGDI 按**具体数值**识别这两个码并做特殊处理（`DbgCM/SWD.cpp:194-213`
// `SWD_CheckStatus()`）：
//
//   RDDI_DAP_OPERATION_TIMEOUT (0x100E) -> 写 ABORT(DAPABORT) 后重试
//   RDDI_DAP_DP_STICKY_ERR     (0x100F) -> 写 ABORT(STKERRCLR|WDERRCLR) 后重试
//
// 宿主把 SWD 的 FAULT 映射成后者、WAIT/NO_ACK 映射成前者
// （`orbrddi-main/src/ORBMDK_RDDI.cpp:1873-1880`）。匹配不上就会把**可恢复的
// 瞬时状态**当成普通错误直接上报 —— 表现就是"一次越界访问之后链路再也不回来"。
enum : int {
    kRddiDapOpTimeout   = 0x100E,   // RDDI_DAP_OPERATION_TIMEOUT
    kRddiDapStickyErr   = 0x100F,   // RDDI_DAP_DP_STICKY_ERR
    kRddiDapError       = 0x2000,   // RDDI_DAP_ERROR
    kRddiDapErrorMemory = 0x2005,   // RDDI_DAP_ERROR_MEMORY
};

// Named aliases.
enum : int {
    DAP_DP_REG_IDCODE    = DAP_REG_DP_0x0,
    DAP_DP_REG_ABORT     = DAP_REG_DP_0x0,
    DAP_DP_REG_CTRL_STAT = DAP_REG_DP_0x4,
    DAP_DP_REG_SELECT    = DAP_REG_DP_0x8,
    DAP_DP_REG_RDBUFF    = DAP_REG_DP_0xC,
    DAP_AP_REG_CSW       = DAP_REG_AP_0x0,
    DAP_AP_REG_TAR       = DAP_REG_AP_0x4,
    DAP_AP_REG_DRW       = DAP_REG_AP_0xC,
    DAP_AP_REG_ROMADDR   = DAP_REG_AP_0x8,
    DAP_AP_REG_APIDR     = DAP_REG_AP_0xC,
};

namespace orb {
namespace rddi {

// Full function table resolved from the RDDI DLL.
struct Api {
    int (*Open)(RDDIHandle* pHandle, const void* pDetails);
    int (*Close)(RDDIHandle handle);
    int (*GetLastError)(int* pError, char* pDetails, size_t detailsLen);

    int (*DAP_Configure)(const RDDIHandle handle, const char* configFileName);
    int (*DAP_Connect)(const RDDIHandle handle, RDDI_DAP_CONN_DETAILS* pConnDetails);
    int (*DAP_Disconnect)(const RDDIHandle handle);
    int (*DAP_GetNumberOfDAPs)(const RDDIHandle handle, int* noOfDAPs);
    int (*DAP_GetDAPIDList)(const RDDIHandle handle, int* DAP_ID_Array, size_t sizeOfArray);
    int (*DAP_ReadReg)(const RDDIHandle handle, const int DAP_ID, const int regID, int* value);
    int (*DAP_WriteReg)(const RDDIHandle handle, const int DAP_ID, const int regID, const int value);
    int (*DAP_RegAccessBlock)(const RDDIHandle handle, const int DAP_ID, const int numRegs,
                              const int* regIDArray, int* dataArray);
    int (*DAP_RegReadRepeat)(const RDDIHandle handle, const int DAP_ID, const int numRepeats,
                             const int regID, int* dataArray);
    int (*DAP_RegWriteRepeat)(const RDDIHandle handle, const int DAP_ID, const int numRepeats,
                              const int regID, const int* dataArray);
    int (*DAP_SetCommTimeout)(const RDDIHandle handle, int timeoutMs);

    int (*CMSIS_DAP_Detect)(const RDDIHandle handle, int* noOfIFs);
    int (*CMSIS_DAP_Identify)(const RDDIHandle handle, int ifNo, int idNo, char* str, const int len);
    int (*CMSIS_DAP_ConfigureInterface)(const RDDIHandle handle, int ifNo, char* str);
    // The real "connect to the target" entry point, and the one Keil's AGDI uses.
    // It performs the SWJ/SWD switch sequence and the first DPIDR read; DAP_Connect
    // is a stub that only fills an implementor string and does no link work at all
    // (ORBMDK Usage.md §2, COMPAT_ANALYSIS.md §4.4).  Without it the DP is never
    // brought up and every DAP register access fails - which is exactly what the
    // settings sheet's AP probe used to hit.
    //   SWD : DAP_Connect(port=SWD) -> JTAG-to-SWD (0xE79E) -> read DPIDR -> ABORT
    //   JTAG: DAP_Connect(port=JTAG) -> SWD-to-JTAG (0xE73C) -> scan chain -> IDCODE
    // *connectedInterface is written only when it points at writable memory, and
    // receives 1 = SWD / 2 = JTAG; the layer checks writability itself.
    int (*CMSIS_DAP_Connect)(const RDDIHandle handle, int* connectedInterface);
    int (*CMSIS_DAP_DetectNumberOfDAPs)(const RDDIHandle handle, int* noOfDAPs);
    int (*CMSIS_DAP_DetectDAPIDList)(const RDDIHandle handle, int* DAP_ID_Array, size_t sizeOfArray);
    int (*CMSIS_DAP_Commands)(const RDDIHandle handle, int num, unsigned char** request,
                              int* req_len, unsigned char** response, int* resp_len);
    int (*CMSIS_DAP_SWJ_Sequence)(const RDDIHandle handle, int num, unsigned char* request);
    int (*CMSIS_DAP_SWJ_Pins)(const RDDIHandle handle, unsigned char pinselect,
                              unsigned char pinout, int* res, int wait);
    int (*CMSIS_DAP_Delay)(const RDDIHandle handle, int delay_us);
    int (*CMSIS_DAP_ResetTarget)(const RDDIHandle handle);
    int (*CMSIS_DAP_SWJ_Clock)(const RDDIHandle handle, unsigned int clock);

    // Debugger option string ("Trace=SWO-UART;TraceBaudrate=<Hz>;TraceTransport=Read;").
    // The RDDI host remembers the SWO port/transport/baud from this string; the
    // Read-path SWO calls below do not carry them.
    int (*CMSIS_DAP_ConfigureDebugger)(const RDDIHandle handle, const char* config);

    // ---- SWO / ITM streaming trace (optional) -----------------------------
    // The same entry points Keil's own CMSIS_AGDI.dll binds for the ITM console
    // (ORBMDK/include/ORBMDK_RDDI.h).  CMSIS_DAP_SWO_Baudrate takes a pointer:
    // [in] the candidate baud rate in Hz; the host must NOT write it back.
    int (*CMSIS_DAP_SWO_Control)(const RDDIHandle handle, int control);
    int (*CMSIS_DAP_SWO_Status)(const RDDIHandle handle, int* count, int* status);
    int (*CMSIS_DAP_SWO_Baudrate)(const RDDIHandle handle, int* baudrate);
    int (*CMSIS_DAP_SWO_Data)(const RDDIHandle handle, int* num_written, void* buffer, int* status);

    // ---- orbtrace 配置 + raw trace 流（可选）------------------------------
    // ORBMDK 2026-10-08 的扩展：RDDI 层统一管理 orbtrace 复合设备的三个 vendor
    // 接口（Trace 0x54 / Power 0x50 / Version 0x56），AGDI 只取 raw trace 字节流
    // 自行解帧（OFLOW/COBS，见 ORBAGDI_Oflow.h）。宿主没有这些导出时视为
    // "无此能力"，用 HasOrbTrace() 探测。
    int (*OrbTrace_Available)(const RDDIHandle handle, int* available);
    int (*OrbTrace_GetInfo)(const RDDIHandle handle, void* info, int size);
    int (*OrbTrace_SetTraceWidth)(const RDDIHandle handle, int width);
    int (*OrbTrace_SetTraceSWO)(const RDDIHandle handle, int isManchester, int useTPIU);
    int (*OrbTrace_SetSWOBaudrate)(const RDDIHandle handle, int baud);
    int (*OrbTrace_SetVoltage)(const RDDIHandle handle, int channel, int millivolts);
    int (*OrbTrace_SetVoltageEnable)(const RDDIHandle handle, int channel, int on);
    int (*TraceStream_Open)(const RDDIHandle handle, const char* config);
    int (*TraceStream_Close)(const RDDIHandle handle);
    // *numBytes: [in] 缓冲容量, [out] 实际字节数；timeoutMs: 0 = 非阻塞。
    // 返回 0 = 有数据, 0x204 = 本轮无数据, 其他 = 错误。
    int (*TraceStream_Read)(const RDDIHandle handle, int* numBytes, void* buffer, int timeoutMs);
    int (*TraceStream_Status)(const RDDIHandle handle, int* count, int* status);
    // 环溢出丢掉的字节数（可选导出）。宿主没有它时视为"本层不报丢帧" ——
    // 那时上层只能从字节数偏少去推断，分不出是探针没发还是被丢了。
    //   *dropped: [out] 累计丢帧字节数，TraceStream_Open 时清零
    int (*TraceStream_DroppedBytes)(const RDDIHandle handle, unsigned long long* dropped);
};

// ---------------------------------------------------------------------------
// OrbTrace_GetInfo 回填的结构，与 orbrddi-main/include/ORBMDK_OrbTrace.h 的
// ORBMDK_OrbTrace_Info 逐字段对应。
//
// 放在这里（而不是某个 .cpp 的匿名 namespace 里）是因为**两个**翻译单元要用它：
// ORBAGDI_OrbTrace.cpp（设备信息）与 ORBAGDI_Power.cpp（判断有没有 Power 接口）。
// 不直接包含 RDDI 层的头，免得把 ORBMDK 的头拉进 AGDI 的编译单元。
//
// ⚠ 字段类型要和 RDDI 层**逐字节**对齐（那里是 int / uint16_t / char[]），
//   所以不能拿 bool 顶替 int，也不能 reinterpret_cast 成别的结构。
// ---------------------------------------------------------------------------
struct RddiOrbTraceInfo {
    int      present;
    int      opened;
    uint16_t vid;
    uint16_t pid;
    int      traceIf;        // -1 = 无
    int      powerIf;        // -1 = 无 —— 这一条就是"有没有供电硬件"的能力位
    int      versionIf;      // -1 = 无
    int      traceProtocol;  // 0x00 未知 / 0x01 TPIU raw / 0x10 OFLOW v1.0
    int      supportsOflow;
    char     product[128];
    char     serial[64];
    char     version[64];
};

// True when the loaded RDDI host exposes the SWO entry points (needed for the
// ITM console).  Cheap to call; safe before Load().
bool HasSwoTrace();

// True when the loaded RDDI host exposes the orbtrace entry points (needed for
// the probe trace source, ORBAGDI_OrbTrace.cpp).  Cheap; safe before Load().
bool HasOrbTrace();

// Load/Unload the RDDI DLL.
// DLL name: $env:ORBAGDI_RDDI_DLL, else "CMSIS_DAP.dll" next to this DLL.
bool Load();
void Unload();
bool IsLoaded();
HMODULE Module();

// The resolved table (valid only while IsLoaded()).
const Api& Get();

// Last RDDI error code/string (best effort, for the log).
int  LastErrorCode();
void LastErrorText(char* buf, size_t len);

/// The host's own interface config string for CMSIS_DAP_ConfigureInterface:
/// which port to drive (SWD / JTAG) and whether the SWJ-DP may switch between
/// them.  `buf`/`cch` receive the text; the return value is what to pass as the
/// third argument - nullptr when there is nothing to say, which is the plain SWD
/// case, so the call stays exactly the one this driver has always made.
///
/// Shared between the real connect sequence (Dap::Connect) and the settings
/// sheet's AP probe (ProbeAps).  They used to build it separately, and the sheet
/// did not configure the interface at all - so it probed a session with no
/// interface bound and reported "no target answered the AP probe" on a bench
/// where the target was answering fine.
char* InterfaceConfig(char* buf, size_t cch);

} // namespace rddi
} // namespace orb
