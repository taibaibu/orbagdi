/**
 * @file ORBAGDI_CodeImage.h
 * @brief 代码镜像：为 ETM 解码补出直接分支（B / BL / CBZ…）的目标地址。
 * ETMCR bit8=0 时 ETM 只播间接分支，直接分支目标只能由代码镜像推出来。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

/// 读目标内存的函数；探针锁由调用方负责（worker 线程上已由抽数方持有）。
/// 返回 false 表示读失败：会计数、停扫描并熔断兜底读。
typedef bool (*CodeImageMemFn)(void* ctx, uint32_t addr, uint8_t* out, unsigned len);

// ---- 来源 ①：留存下载镜像 ----

/// 留存一块下载镜像（AG_CB_GETFLASHPARAM 的 {start, many, image}）。
/// data 会被拷贝；uVision 的 image 指针下载完即失效。与已有块重叠时旧块先被摘掉。
void CodeImageAdd(uint32_t base, const void* data, uint32_t len);

/// 会话开始时按器件声明的 iromStart/iromSize 一次性读入 Flash。
/// 按需现读会占着探针锁做 MEM-AP 读，容易撞 LockProbe 超时，故前置到 Connect。
void CodeImagePreloadFlash(CodeImageMemFn read, void* ctx);

/// 会话开始/结束时丢弃镜像与页缓存。
void CodeImageClear();

/// 当前留存的镜像字节数（日志用）。
unsigned long long CodeImageBytes();

/// 镜像内容版本号：Add / Clear 时递增。用于让按地址缓存"指令形状"的调用方作废缓存
/// （见 ORBAGDI_InstrScan.h 的 ScanToCached）。
unsigned CodeImageGeneration();

// ---- 来源 ②：兜底读目标内存 ----

/// 注册兜底读；fn 传 nullptr 表示取消。
/// 读到未映射地址会让 MEM-AP 报错并卡死整条链路，所以还有两道闸：
/// 地址粗筛（外设/内核私有地址不发事务）+ 一次失败即熔断（memDisabled）。
void CodeImageSetReader(CodeImageMemFn fn, void* ctx);

/// 读代码字节：镜像命中用镜像，否则兜底读 + 页缓存。
/// 返回 false 表示这一段读不到，扫描方应停下。
bool CodeImageRead(uint32_t addr, uint8_t* out, unsigned len);

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

/// 打一条汇总（会话结束时调；where = 调用点名字）。
void CodeImageSummary(const char* where);

} // namespace agdi
} // namespace orb
