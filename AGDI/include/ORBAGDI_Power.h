/**
 * @file ORBAGDI_Power.h
 * @brief orbtrace 目标供电：Vtref / Vtpwr 两路固定档位输出。
 * 只用 RDDI 的 OrbTrace_SetVoltage / SetVoltageEnable，不碰采集状态。
 */
#pragma once

namespace orb {
namespace agdi {

/// 通道号（与 orbuculum 的 enum Channel 对齐）。
enum {
    kPowerChVtref = 0,
    kPowerChVtpwr = 1,
    kPowerChCount = 2,
    kPowerChAll   = 0xFF,
};

struct PowerLevel {
    int            mv;
    const wchar_t* name;
};

/// 某一路的档位表；ch 越界时返回 nullptr、count = 0。
const PowerLevel* OrbPowerLevels(int ch, int& count);

/// 这个毫伏值是不是该路的档位（界面只能给表里的值，这是最后一道闸）。
bool OrbPowerLevelValid(int ch, int mv);

// ---- 能力探测：设置页据此整页灰显 ----
// 三道闸：宿主导出供电入口、探针在线、探针有 Power 接口。
// 只探测不打开设备，真正打开是 OrbPowerApply 的事。
struct PowerCaps {
    bool           host;     // 宿主有那两个导出
    bool           probe;    // 找到 orbtrace 设备
    bool           powerIf;  // 设备提供 Power 接口（能力位 0x04）
    bool           ready;    // 三条全过
    const wchar_t* why;      // 不 ready 时给用户看的一句话；ready 时为 nullptr
};
PowerCaps OrbPowerCaps();

/// 探针没有回读接口，这里记的是“我们最后成功下发的值”。
struct PowerState {
    int  mv[kPowerChCount];
    bool on[kPowerChCount];
    bool known;          // false = 本进程内一次都没下发过
};
PowerState OrbPowerGet();

/// 下发一路：先设电压再设使能，避免在旧档位上带电一下。
/// 失败返回 false 并记日志；会按需把探针打开。
bool OrbPowerApply(int ch, int mv, bool on);

/// 两路都关（kPowerChAll）。返回 false = 没关掉（探针够不着）。
bool OrbPowerAllOff();

} // namespace agdi
} // namespace orb
