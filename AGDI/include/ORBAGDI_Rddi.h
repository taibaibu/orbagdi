/**
 * @file ORBAGDI_Rddi.h
 * @brief RDDI 层（CMSIS_DAP.dll）的动态加载与调用桥。
 * 运行期加载，RDDI DLL 可随意替换而不必重编；签名照抄 ARM RDDI 头。
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
// DAP 寄存器号（DAP_ReadReg / DAP_WriteReg 用）。
//   DP : 0..3  -> DP 地址 0x0 / 0x4 / 0x8 / 0xC
//   AP : 4..7  -> AP 地址 0x0 / 0x4 / 0x8 / 0xC（bank 由 DP SELECT 选）
// 这些已是编码后的 id，不是字节地址，不要再移位。
// 读写方向由调用的函数决定，不要自己 OR 上 DAP_REG_RnW。
// AP bank 通过写 DP SELECT = (apIndex << 24) | (bank << 4) 选择。
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
// 这两个码要按数值特判并写对应 ABORT 后重试（0x100E → DAPABORT，
// 0x100F → STKERRCLR|WDERRCLR）；漏判会把可恢复的瞬时状态当死错误上报。
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
    // 真正“连上目标”的入口：做 SWJ/SWD 切换序列并读第一次 DPIDR。
    // DAP_Connect 只是填个字符串的空壳，不调它 DP 永远起不来。
    // *connectedInterface 仅在可写时回填 1 = SWD / 2 = JTAG。
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

// OrbTrace_GetInfo 回填的结构，与 RDDI 层 ORBMDK_OrbTrace_Info 逐字段对应。
// 放在头里是因为 OrbTrace.cpp 与 Power.cpp 都要用；字段类型必须逐字节对齐，
// 不能拿 bool 顶替 int，也不能 reinterpret_cast 成别的结构。

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

/// CMSIS_DAP_ConfigureInterface 的接口串（端口 SWD/JTAG、是否允许切换）。
/// 返回值即第三个实参，没什么可说时返回 nullptr（纯 SWD 的情况）。
/// 真实连接与设置页 AP 探测共用，两边各写一份会让探测绑不上接口。
char* InterfaceConfig(char* buf, size_t cch);

} // namespace rddi
} // namespace orb
