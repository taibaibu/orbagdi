/**
 * @file ORBAGDI_CodeImage.h
 * @brief 代码镜像（P1-0）—— 固件 bit8=0 之后"直接分支必须靠它补"的那份字节。
 *
 * 背景（IHI0014Q Table 3-5 [8]，见 COMPAT_ANALYSIS.md §10.30）
 * ----------------------------------------------------------
 * `ETMCR` bit8（Branch output）置 1 时 ETM 把**所有**分支地址（含直接分支）都播出来，
 * 手册明说这样"不需要代码镜像就能重建程序流"；清 0 时只出**间接**分支（BX/BLX/LDR PC/
 * POP{PC}…），直接分支（B / BL / CBZ…）的目标必须由**代码镜像**推出来。
 *
 * 本板固件取 bit8=0（P0-E 实测：bit8=1 吃掉 92% 端口带宽、把 ITM 挤成 52 B），
 * 所以执行流看起来"缺一大截"—— 补它的就是这一层。
 *
 * 两个来源（2026-10-13 用户定）
 * ----------------------------
 *   ① **下载时留存的镜像**：`AG_CB_GETFLASHPARAM` 每次给一块 {start, many, image}，
 *      现在只用于烧写（`ORBAGDI_Flash.cpp`）。uVision 那个 `image` 指针**下载完就失效**，
 *      所以必须在这里**拷贝**下来。
 *   ② **会话里没下载**（用户直接点调试）：用注册进来的读函数现读目标内存兜底，
 *      带页缓存、按需读、**不预取全片**。
 *
 * 页缓存（为什么是 512 B）
 * -----------------------
 * 顺序扫描一条分支区间会连着读几十条指令（每条 2/4 B），不做缓存就是把同一条
 * MEM-AP 读重复上百次。512 B/页 × 128 页 = 64 KB，直接映射（`(addr>>9) & 127`），
 * 够覆盖一段循环体，且**不随会话增长**。
 *
 * ⚠ 这一层只在 **ETM 解码的 worker 线程**上用（`OnReport` 里补记录），所以它自己
 *   **不取任何锁**；兜底读的探针锁由注册方负责（见 `CodeImageSetReader`）。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

/// 读目标内存的函数。**调用方负责探针锁**：这一层在 worker 线程上被调时自己不加锁
/// （那时锁已经被抽数方持有）；在 Connect 里被调时也不加（那时 worker 还没起）。
/// 返回 false = 这次读没成功（会被计数，扫描随即停下，并触发熔断）。
typedef bool (*CodeImageMemFn)(void* ctx, uint32_t addr, uint8_t* out, unsigned len);

// ---------------------------------------------------------------------------
// 来源 ①：留存下载镜像
// ---------------------------------------------------------------------------

/// 留存一块下载镜像（`AG_CB_GETFLASHPARAM` 的 {start, many, image}）。
/// `data` 会被**拷贝**；`len` 为 0 或超上限时忽略。
/// **与已有块重叠时，旧块先被摘掉**（否则后写的永远轮不到 —— `MirrorPage()` 取第一个
/// 能装下整页的块，见 .cpp 的说明）。
void CodeImageAdd(uint32_t base, const void* data, uint32_t len);

/// **会话缓存：把器件声明的 Flash 一次性读进来**（P1-0 的主来源）。
///
/// 为什么是"一次性"而不是"用到再读"（2026-10-14 用户定）
/// ----------------------------------------------------
/// 按需现读发生在 **ETM 解码的 worker 线程**上，而那个线程**已持有探针锁** ⇒ 每次页
/// 缓存未命中都要占着锁做一次 MEM-AP 读 ⇒ uVision 侧操作更容易撞 `LockProbe` 超时
/// （实测的"卡顿"来源之一）。而且"有没有镜像"曾经取决于 uVision 什么时候 Download，
/// 那是用户管不着的时序。
///
/// ⇒ 改成：**Connect 时读一次，之后热路径只读内存，零探针访问。**
///
/// 读的范围是 `cfg::Effective()` 的 `iromStart/iromSize` —— **器件描述声明的范围**，
/// 所以每一次读都落在真实映射的内存上（这是 2026-10-13 那次"越界读打挂 DAP"的真正修法：
/// 当时打挂的 `0x08101F64` 落在旧的 256 MB 粗筛里，却超出这颗片子真实的 512 KB）。
///
/// 失败（读不到 / 器件没声明范围）**不致命**：已读到的部分照用，并打日志；下载时
/// `AG_CB_GETFLASHPARAM` 仍会另存一份更准的镜像。
void CodeImagePreloadFlash(CodeImageMemFn read, void* ctx);

/// 会话开始/结束（`AG_UNINIT` 或 `Connect` 时）。丢掉镜像与页缓存。
void CodeImageClear();

/// 当前留存的镜像字节数（日志用）。
unsigned long long CodeImageBytes();

// ---------------------------------------------------------------------------
// 来源 ②：兜底读目标内存
// ---------------------------------------------------------------------------
//
// ⚠ **这一条路是有代价的，代价是整条调试链路。** 2026-10-13 真 uVision 实测：
//   流刚打开、ETM 还没同步时解出了 0x08101F64 这种**越界地址**，拿它去读目标内存
//   ⇒ MEM-AP 报错、DAP 进错误态（`DP write reg=2 val=0 failed (rc=4110)`），
//   **之后所有 DAP 操作都失败** —— 停止调试失败、uVision 弹"链路恢复"、重连后
//   worker 又去读、又挂，反复。
//
// 所以这里有两道闸（见 .cpp 的 `LooksLikeCode()` 与 `FillPage()`）：
//   ① **地址粗筛**：只读"像代码"的区域，明显是外设/内核私有的地址根本不发事务；
//   ② **一次失败即熔断**：任何一次兜底读失败 ⇒ **本会话永久不再现读目标内存**，
//      只认下载镜像。宁可补不出（有日志），不能再挂链路。
//
// 用 `CodeImageStats::memDisabled` 看是否已熔断（会进 `CodeImage(…)` 汇总行）。

/// 注册兜底读。`fn` 传 nullptr 表示取消（例如会话结束、探针已断开）。
void CodeImageSetReader(CodeImageMemFn fn, void* ctx);

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

/// 读代码字节。镜像命中用镜像；否则走兜底读 + 页缓存。
/// 返回 false = 这一段读不到（扫描方应当停，并记一次"镜像不可信"）。
bool CodeImageRead(uint32_t addr, uint8_t* out, unsigned len);

// ---------------------------------------------------------------------------
// 统计（日志）
// ---------------------------------------------------------------------------
struct CodeImageStats {
    unsigned long long mirrorHits;   // 从留存镜像拿到的字节数
    unsigned long long memReads;     // 走兜底读拿到的字节数
    unsigned long long memFails;     // 兜底读失败次数（>0 即已熔断）
    unsigned long long memRejected;  // 被地址粗筛挡掉、根本没发事务的次数
    unsigned long long pageHits;     // 页缓存命中次数
    unsigned long long pageMiss;     // 页缓存未命中次数
    bool               memDisabled;  // 兜底读是否已熔断（本会话）
};

void CodeImageStatsGet(CodeImageStats& out);

/// 打一条汇总（会话结束时调；`where` = 调用点名字）。
void CodeImageSummary(const char* where);

} // namespace agdi
} // namespace orb
