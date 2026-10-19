/**
 * @file ORBAGDI_Power.h
 * @brief orbtrace 的目标供电：Vtref / Vtpwr 两路固定档位输出。
 *
 * 单独一个文件，没有挂在 `ORBAGDI_OrbTrace.*` 上：它只用 RDDI 的
 * `OrbTrace_SetVoltage` / `OrbTrace_SetVoltageEnable` 两个导出，**不碰 trace 的采集
 * 状态**。所以设置页的测试程序（`test\build.ps1` 只编四个源文件）也链得上它。
 *
 * ⚠ 探针只认**固定档位**，不是任意毫伏。软件层（`orbuculum/Src/orbtraceIf.c` 的
 *    `OrbtraceIfValidateVoltage`、`orbrddi-main/src/ORBMDK_OrbTrace.cpp`）**只做
 *    900..5010 的范围校验** —— 档位是硬件属性，代码里查不到。所以下面这张表是
 *    **规格**（由使用方确认），不是从代码推导出来的：
 *
 *        Vtref : 1.8 V / 3.3 V
 *        Vtpwr : 3.3 V / 5.0 V        ← 两路档位**不一样**，各一张表
 *
 *    界面必须用下拉把档位钉死。让用户填任意毫伏的话，探针会落到最近的档（或直接
 *    拒绝），而界面显示的还是他填的那个数 —— 那是界面在撒谎。
 *
 * ⚠ 这两路是**给目标供电**。任何"顺手也发一下"的调用都是错的：只有用户在设置页上
 *    按下 Apply 才应该走到这里。见 `ORBAGDI_Settings.cpp` 的 Power 页说明。
 */
#pragma once

namespace orb {
namespace agdi {

/// 通道号（与 orbuculum 的 `enum Channel` / `ORBMDK_OrbTrace_Channel` 对齐）。
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

/// 某一路的档位表；`count` 收条目数。`ch` 越界时返回 nullptr、`count = 0`。
const PowerLevel* OrbPowerLevels(int ch, int& count);

/// 这个毫伏值是不是这一路的档位（界面只给得出表里的值，这是最后一道闸）。
bool OrbPowerLevelValid(int ch, int mv);

// ---------------------------------------------------------------------------
// 能力探测 —— 设置页用它决定**整页灰显**
// ---------------------------------------------------------------------------
//
// 三道闸，任何一道不过就整页灰掉。**不是"点了才报错"**：让用户点一个注定失败的
// 按钮、再拿一句话解释，是本工程一直在清的那类东西。
//
//   1. **RDDI 宿主**能加载，并且导出 `OrbTrace_SetVoltage` / `OrbTrace_SetVoltageEnable`
//      —— ⚠ 这两个**不在** `rddi::HasOrbTrace()` 的判据里（它只要
//      `OrbTrace_Available` + `TraceStream_Open|Read|Close`），所以"有 orbtrace"
//      不等于"有供电"。非 orbtrace 的 RDDI 宿主就栽在这里。
//   2. **探针在**（`OrbTrace_Available`）。
//   3. **探针有 Power 接口**（`info.powerIf >= 0`）—— 这一条就是
//      `TraceStream_Status` 的能力位 `0x04`（RDDI 层正是拿 `powerIf >= 0` 合成它的）。
//      没有供电硬件的 orbtrace 栽在这里。
//
// ⚠ 只**探测**，不打开设备：`OrbTrace_Available` / `OrbTrace_GetInfo` 都只枚举 USB，
//    用临时句柄、用完就还。真正打开设备是 `OrbPowerApply` 的事。
struct PowerCaps {
    bool           host;     // 宿主有那两个导出
    bool           probe;    // 找到 orbtrace 设备
    bool           powerIf;  // 设备提供 Power 接口（能力位 0x04）
    bool           ready;    // 三条全过
    const wchar_t* why;      // 不 ready 时给用户看的一句话；ready 时为 nullptr
};
PowerCaps OrbPowerCaps();

/// 探针**没有回读接口**，所以这里记的是"我们最后成功下发的东西"，不是探针的状态。
struct PowerState {
    int  mv[kPowerChCount];
    bool on[kPowerChCount];
    bool known;          // false = 本进程内一次都没下发过
};
PowerState OrbPowerGet();

/// 下发一路：**先设电压、再设使能**（顺序反了会先在旧档位上带电一下）。
/// `on == false` 时电压照样先设 —— 关着的通道改档位无害，而"开"的那一下永远
/// 落在正确的档位上。
///
/// 失败返回 false，原因已进日志。**会按需把探针打开** —— RDDI 层没有"只开电源"
/// 的入口，详见 .cpp 里 `PowerEnsure()` 的说明。
bool OrbPowerApply(int ch, int mv, bool on);

/// 两路都关（`kPowerChAll`）。返回 false = 没关掉（探针够不着）。
bool OrbPowerAllOff();

} // namespace agdi
} // namespace orb
