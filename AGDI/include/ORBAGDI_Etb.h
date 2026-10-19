/**
 * @file ORBAGDI_Etb.h
 * @brief ETB / TMC(ETB 模式) 片上 trace 缓冲。
 * 数据与 SWO 同一条 ITM/ETM 流，全部经 MEM-AP 编程，不依赖 RDDI trace 扩展。
 */
#pragma once

#include <stdint.h>

namespace orb { class Dap; }

namespace orb {
namespace agdi {

// ---- 发现 ----

// 扫 ROM 表找 ETB（或 ETB 模式的 TMC），找到就记下位置。
bool EtbDetect(orb::Dap& dap);

// EtbDetect() 已找到 ETB。
bool EtbAvailable();

// 找到的是 ETB 模式的 TMC（寄存器/指针与老式 ETB 不同）。
bool EtbIsTmc();

// ROM 扫到的 ETM 基址（0 = 表里没有 ETM）。
// 基址是器件事实而非常量，硬编码会让 ETM 放在别处的芯片一个包都收不到。
uint32_t EtbEtmBase();

// 只扫 ROM 表、不动 ETB 状态（ITM/ETM 侧在 ETB 会话进行中也要用它）。
bool EtbScanRom(orb::Dap& dap);

// 从 ETB 流解出的 ETM 分支数（0 = 没见过 ETM 数据）。
unsigned EtbEtmBranchCount();

// ---- 设置 / 控制 ----

// 读 RAM 深度与字宽，锁存 formatter 模式；须在 EtbDetect() 之后、EtbActivate() 之前。
bool EtbSetup(orb::Dap& dap);

// 武装采集，目标恢复运行前调。
bool EtbActivate(orb::Dap& dap);

// 注册 trace 窗口并标记 sink 可用；没有 ETB 时返回 false（官方答 EU32）。
bool EtbInit();

// 手动 flush 并等待完成（STOPFL + FONMAN，最多轮询 10 ms），停采集路径也会调。
bool EtbFlush(orb::Dap& dap);

// ETB 已停、缓冲可安全读取（formatter 的 FTSTOPPED 置位）。
bool EtbCanAccess(orb::Dap& dap);

// 标记缓冲已消费，允许下一次 ETB 更新。
void EtbClear();

// 清掉上次停止留下的 formatter 停止位，才能重新武装；采集期间是空操作。
bool EtbRecovery(orb::Dap& dap);

// 停采集、读完整缓冲、解码并把 ITM 包转给 uVision；返回处理字节数，硬错误 -1。
int EtbProcess(orb::Dap& dap);

// 丢掉全部状态（会话开始）。
void EtbReset();

// ---- 状态 ----

struct EtbStatus {
    bool     present;     // an ETB/TMC was found
    bool     tmc;         // the sink is a TMC in ETB mode
    bool     configured;  // EtbSetup() succeeded
    bool     active;      // capture is currently armed
    uint32_t addr;        // ETB register base address
    uint32_t depth;       // RAM depth in words
    uint32_t width;       // RAM word width in bytes (3 or 4)
    bool     wraparound;  // last read wrapped around (oldest data lost)

    // 流里出现过的 trace 源，解 formatter 帧时锁存。
    bool     itmConnected;
    bool     etmConnected;

    // 武装时写进 ETB_TRG_CNT 的触发延迟。
    uint32_t trigDelay;
};

EtbStatus EtbGetStatus();

} // namespace agdi
} // namespace orb
