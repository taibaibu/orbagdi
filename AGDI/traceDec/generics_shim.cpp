/**
 * @file generics_shim.cpp
 * @brief `generics.h` 替身里那两个函数的实现（orbuculum 的 `generics.c` 不能复制）。
 *
 * orbuculum 的 `Src/generics.c` include `<sys/time.h>` / `<libgen.h>` /
 * `<unistd.h>`（POSIX），MSVC 没有；而且它带着终端着色与文件路径逻辑，与驱动
 * 无关。解码子系统实际只用到这里这两个函数：
 *
 *   genericsTimestampuS()  —— msgDecoder.c（给消息打时间戳）
 *                             tpiuDecoder.cpp（包间隔看门狗）
 *   genericsReport()       —— msgDecoder.c（"Unknown Extension Packet" 之类告警）
 *
 * ⚠ **不要顺手把 genericsReport 接到 ORB_LOG_\***。见下面那个函数的说明 ——
 * 它会被逐包调用，而 ORB_LOG_* 没有限流。要接必须先加限流。
 *
 * 这里给一个不依赖任何工程头的实现，好让 `traceDec/` 能独立编译验证。
 */
#include "generics.h"

#include <windows.h>

uint64_t genericsTimestampuS(void)
{
    /* orbuculum 用 gettimeofday()。GetTickCount64() 是毫秒，换算成微秒。
     * 消费方是 tpiuDecoder 的"包间隔超时"看门狗（阈值 TPIU_TIMEOUT_US，
     * 百微秒量级），毫秒精度够用。 */
    return static_cast<uint64_t>(GetTickCount64()) * 1000ull;
}

void genericsReport(enum verbLevel l, const char* fmt, ...)
{
    /* 什么都不做 —— **这是有意的，不是漏了实现**。
     *
     * msgDecoder.c 会在每个"未知/保留包"上调它。畸形流、或 ATB ID 撞号时
     * 那是**逐包**触发；而 ORB_LOG_* 本身没有限流（ORBAGDI_Log.h 只是转发
     * 到 orb::log::Write），接上去就是每包一条 fprintf，几秒钟就能把日志
     * 撑到几百 MB。
     *
     * 要接必须先加限流，例如"每类消息每会话 N 条"或"每秒最多 N 条"。 */
    (void)l;
    (void)fmt;
}
