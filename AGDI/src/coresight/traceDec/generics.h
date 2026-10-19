/**
 * @file generics.h
 * @brief orbuculum `Inc/generics.h` 的替身：只保留 trace 解码子系统实际用到的部分。
 *
 * **为什么不直接复制 orbuculum 的 `Src/generics.c`**：它 include
 * `<sys/time.h>` / `<libgen.h>` / `<unistd.h>`（POSIX，MSVC 没有），并且带着
 * 一整套终端着色、屏幕控制、文件路径逻辑，与本驱动无关。
 *
 * 解码子系统真正依赖的符号是下面这几项 —— 对 7 个源文件逐个 grep 确认过：
 *
 *     EOL · enum verbLevel · symbolMemaddr · symbolMemptr · genericsReportCB
 *     + 两个**函数**：genericsTimestampuS()（msgDecoder.c 用）
 *                     genericsReport()       （msgDecoder.c 用）
 *
 * 注意 `genericsReport` 在 itmDecoder.c / tpiuDecoder.c 里被就地 `#define`
 * 掉了（原本是 GCC 的 `#define genericsReport(x...)` 写法，MSVC 不认，已改成
 * `(...)`），所以那两条路径不依赖这里的实现。
 */
#ifndef _GENERICS_
#define _GENERICS_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOL "\n"

/* 解码器里 `DEBUG(...)` / `REPORT(...)` 宏用的报告等级。顺序必须与 orbuculum
 * 一致（V_ERROR 最低、V_DEBUG 最高），否则 report 回调里的过滤会反。 */
enum verbLevel { V_ERROR, V_WARN, V_INFO, V_DEBUG, V_MAX_VERBLEVEL };

/* orbuculum 的地址/指针类型。Cortex-M 的地址空间是 32 位，够用；
 * `ADDRESS_UNKNOWN`（traceDecoder.h）就定义在这个类型上。 */
typedef uint32_t       symbolMemaddr;
typedef unsigned char* symbolMemptr;

/* 解码器的调试报告回调（`TRACECPUState::report`）。
 * 传 NULL 即完全关闭，`DEBUG()` 宏会跳过，零开销。 */
typedef void (*genericsReportCB)(enum verbLevel l, const char* fmt, ...);

// ---------------------------------------------------------------------------
// 下面两个是 msgDecoder.c 真正会调到的函数。
//
// 集成到驱动时把 genericsReport 接到 ORB_LOG_*（ORBAGDI_Log.h），这样解码器的
// 内部告警（"Unknown Extension Packet Received" 之类）会进同一个日志。
// 这里给一个不依赖任何工程头的默认实现，好让 traceDec/ 能独立编译验证。
// ---------------------------------------------------------------------------

/** @brief 微秒时间戳。orbuculum 用 gettimeofday()，这里用 GetTickCount64()。 */
uint64_t genericsTimestampuS(void);

/** @brief 格式化报告。默认实现什么都不做（见上面的集成说明）。 */
void genericsReport(enum verbLevel l, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
