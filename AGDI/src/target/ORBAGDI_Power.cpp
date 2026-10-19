/**
 * @file ORBAGDI_Power.cpp
 * @brief orbtrace 的目标供电（Vtref / Vtpwr）。档位见 ORBAGDI_Power.h。
 *
 * 顺序不能错：探针必须先开（vendor 接口绑在 TraceStream_Open 上）→ 档位必须在表内
 * （软件放行 900..5010、档位不放）→ 先电压后使能（反之改档会按旧档带电）。
 */
#include "pch.h"
#include "ORBAGDI_Power.h"

#include <string.h>

namespace orb {
namespace agdi {

const PowerLevel kLevelsVtref[] = { { 1800, L"1.8 V" }, { 3300, L"3.3 V" } };
const PowerLevel kLevelsVtpwr[] = { { 3300, L"3.3 V" }, { 5000, L"5.0 V" } };

const char* const kPowerChName[kPowerChCount] = { "Vtref", "Vtpwr" };

const PowerLevel* OrbPowerLevels(int ch, int& count)
{
    switch (ch) {
    case kPowerChVtref: count = ARRAYSIZE(kLevelsVtref); return kLevelsVtref;
    case kPowerChVtpwr: count = ARRAYSIZE(kLevelsVtpwr); return kLevelsVtpwr;
    default:            count = 0;                       return nullptr;
    }
}

bool OrbPowerLevelValid(int ch, int mv)
{
    int n = 0;
    const PowerLevel* t = OrbPowerLevels(ch, n);
    for (int i = 0; i < n; ++i) {
        if (t[i].mv == mv) return true;
    }
    return false;
}

PowerCaps OrbPowerCaps()
{
    PowerCaps c = { false, false, false, false, nullptr };

    // 1. 宿主：要的是**电压**那两个导出，不是 rddi::HasOrbTrace() 那一组。
    if (!rddi::IsLoaded() && !rddi::Load()) {
        c.why = L"the RDDI host could not be loaded.";
        return c;
    }
    const rddi::Api& api = rddi::Get();
    if (!api.OrbTrace_SetVoltage || !api.OrbTrace_SetVoltageEnable) {
        c.why = L"the RDDI host is not an orbtrace one (no SetVoltage).";
        return c;
    }
    c.host = true;

    if (!api.Open || !api.OrbTrace_Available || !api.OrbTrace_GetInfo) {
        c.why = L"the RDDI host is missing OrbTrace_Available / GetInfo.";
        return c;
    }

    // 2/3. 探针、以及它有没有 Power 接口。**临时句柄，用完就还 —— 这里不打开设备。**
    RDDIHandle h = 0;
    if (api.Open(&h, nullptr) != 0 || h == 0) {
        c.why = L"the RDDI host could not open a session.";
        return c;
    }

    int avail = 0;
    const int rcAvail = api.OrbTrace_Available(h, &avail);
    if (rcAvail != 0 || !avail) {
        if (api.Close) api.Close(h);
        c.why = L"no orbtrace probe was found.";
        return c;
    }
    c.probe = true;

    rddi::RddiOrbTraceInfo info;
    memset(&info, 0, sizeof(info));
    const int rcInfo = api.OrbTrace_GetInfo(h, &info, static_cast<int>(sizeof(info)));
    if (api.Close) api.Close(h);
    if (rcInfo != 0 || !info.present) {
        c.why = L"the probe did not answer the info request.";
        return c;
    }
    if (info.powerIf < 0) {
        c.why = L"this probe has no Power interface (no 0x04 capability).";
        return c;
    }

    c.powerIf = true;
    c.ready   = true;
    return c;
}

namespace {

struct PwState {
    PowerState st;
    RDDIHandle handle = 0;    // 我们自己开的那个句柄（开不出来时留 0）
    bool       open   = false;

    PwState()
    {
        st.known = false;
        for (int i = 0; i < kPowerChCount; ++i) {
            int n = 0;
            const PowerLevel* t = OrbPowerLevels(i, n);
            st.mv[i] = (n > 0) ? t[0].mv : 0;   // 默认落在第一档
            st.on[i] = false;                   // 默认关闭
        }
    }
};

PwState& Pw()
{
    static PwState* s = new PwState();   // 故意泄漏（同 ORBAGDI_OrbTrace.cpp 的说明）
    return *s;
}

/// 保证探针是开着的 —— 供电这条路唯一碰设备的地方。已在跑时再调也安全（Open 幂等、
/// 配置串给 nullptr 不重配），故不必问 trace 层。故意不关：关掉会释放 USB 接口而供电
/// 留在探针里，就成了"界面说关、目标还带电"。断电请用 OrbPowerAllOff。
bool PowerEnsure()
{
    // 宿主与能力由调用方先用 `OrbPowerCaps()` 查过（**并且自己 Load 过宿主** ——
    // 设置页的 bench 探测用完会主动 `Unload()`，只看 `IsLoaded()` 会误判）。
    // 这里只负责"把设备打开"。
    if (Pw().open && Pw().handle) return true;

    const rddi::Api& api = rddi::Get();
    if (!api.Open || !api.TraceStream_Open) {
        ORB_LOG_WARN("power: 宿主缺少 Open / TraceStream_Open，供电控制不可用");
        return false;
    }

    RDDIHandle h = 0;
    if (api.Open(&h, nullptr) != 0 || h == 0) {
        ORB_LOG_WARN("power: RDDI Open 失败，供电控制不可用");
        return false;
    }
    if (api.TraceStream_Open(h, nullptr) != 0) {
        ORB_LOG_WARN("power: TraceStream_Open 失败（探针不在？）—— 供电控制不可用");
        if (api.Close) api.Close(h);
        return false;
    }
    Pw().handle = h;
    Pw().open   = true;
    ORB_LOG_INFO("power: 为供电控制打开了探针");
    return true;
}

} // namespace

PowerState OrbPowerGet() { return Pw().st; }

bool OrbPowerApply(int ch, int mv, bool on)
{
    if (ch < 0 || ch >= kPowerChCount) {
        ORB_LOG_WARN("power: 通道 %d 不存在", ch);
        return false;
    }
    if (!OrbPowerLevelValid(ch, mv)) {
        ORB_LOG_WARN("power: %s 没有 %d mV 这一档（只认固定档位）",
                     kPowerChName[ch], mv);
        return false;
    }
    // 设置页在能力不足时会把整页灰掉，所以正常走不到这里；这是第二道闸
    // （外部脚本可以绕过灰显直接发命令，那时也要给出**说得清的原因**）。
    const PowerCaps caps = OrbPowerCaps();
    if (!caps.ready) {
        ORB_LOG_WARN("power: unavailable - %ls", caps.why ? caps.why : L"unknown");
        return false;
    }
    if (!PowerEnsure()) return false;

    const rddi::Api& api = rddi::Get();
    if (!api.OrbTrace_SetVoltage || !api.OrbTrace_SetVoltageEnable) {
        ORB_LOG_WARN("power: 宿主缺少 SetVoltage / SetVoltageEnable 导出");
        return false;
    }

    const RDDIHandle h = Pw().handle;
    int rc = api.OrbTrace_SetVoltage(h, ch, mv);            // 先电压
    if (rc != 0) {
        ORB_LOG_WARN("power: SetVoltage(%s, %d mV) -> 0x%X", kPowerChName[ch], mv, rc);
        return false;
    }
    rc = api.OrbTrace_SetVoltageEnable(h, ch, on ? 1 : 0);  // 后使能
    if (rc != 0) {
        ORB_LOG_WARN("power: SetVoltageEnable(%s, %d) -> 0x%X",
                     kPowerChName[ch], on ? 1 : 0, rc);
        return false;
    }

    Pw().st.mv[ch] = mv;
    Pw().st.on[ch] = on;
    Pw().st.known  = true;
    ORB_LOG_INFO("power: %s = %d mV, %s", kPowerChName[ch], mv, on ? "ON" : "OFF");
    return true;
}

bool OrbPowerAllOff()
{
    const PowerCaps caps = OrbPowerCaps();
    if (!caps.ready) {
        ORB_LOG_WARN("power: unavailable - %ls", caps.why ? caps.why : L"unknown");
        return false;
    }
    if (!PowerEnsure()) return false;

    const rddi::Api& api = rddi::Get();
    if (!api.OrbTrace_SetVoltageEnable) {
        ORB_LOG_WARN("power: 宿主缺少 SetVoltageEnable 导出");
        return false;
    }
    const int rc = api.OrbTrace_SetVoltageEnable(Pw().handle, kPowerChAll, 0);
    if (rc != 0) {
        ORB_LOG_WARN("power: SetVoltageEnable(ALL, 0) -> 0x%X", rc);
        return false;
    }
    for (int i = 0; i < kPowerChCount; ++i) Pw().st.on[i] = false;
    Pw().st.known = true;
    ORB_LOG_INFO("power: 两路都关 (CH_ALL)");
    return true;
}

} // namespace agdi
} // namespace orb
