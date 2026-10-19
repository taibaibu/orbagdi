/**
 * @file ORBAGDI_TraceFile.h
 * @brief trace 文件：OFLOW 流文件（P2-1）+ 紧凑记录 cache（P2-2）。
 * 内存只能装秒级窗口，更早的历史要靠文件；格式沿用 orbuculum，orbmortem/orbcat 可直接打开。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

// 打开 OFLOW 缓存文件。path："" 不写 / "1" 默认目录自动命名 /
// "<目录>" 该目录自动命名 / "<文件>.orb" 用该名。
// **开新文件前先把目录里更早的同类文件删掉**：这是内存装不下时的临时缓存，
// 不是需要留存的日志，所以没有"保留几个"的旋钮。
// capMB = 大小上限（来自 trrecmb=，默认 0 = 不限）；到顶停止追加并告警一次
// （停而不是环形覆盖，因为文件装的是“更早的历史”，覆盖需要重基全部索引）。
// ⚠ 到顶后本文件**不再算“可用”**（TraceFileRecordsActive() 返回 false）：
//   否则调用方会继续把块送进来、在这里被静默丢掉。
// 返回 false = 没开（路径不合法/建不出文件），不影响 trace 采集。幂等。
bool TraceFileOpen(const char* path, unsigned capMB);

// 关闭并打一行日志（路径/字节/帧/索引数）；幂等，没开时什么都不做。
void TraceFileClose(const char* why);

// 关闭并删除当前文件，返回删掉的文件数。reset 是“从头开始”，不能留上一轮的缓存。
// 调用方随后要重新 Open，否则这段不写 cache。幂等。
unsigned TraceFileCloseAndDelete(const char* why);

bool TraceFileActive();

// 当前 cache 文件路径（UTF-8），没开时返回 ""。
const char* TraceFilePath();

// ---- 记录 cache（P2-2）----
// 与 OFLOW 文件分工：OFLOW 存原始流给外部工具，查询要重解；
// 记录 cache存解码后的紧凑记录，查询零重解，本驱动自己的回看靠它。

// 打开记录 cache，path/capMB 语义同上（默认 capMB = trrecmb=，2 GB）。
// 写盘在后台线程，不在抽数线程上 fwrite，所以开它不卡抽数；默认仍关。
bool TraceFileOpenRecords(const char* path, unsigned capMB);

// 记录 cache是否真的"能接数据"（内存到顶时据此判断能否写 cache，落不了必须报 Overflow）。
// 三种情况返回 false：没开、写失败过、**本段已到大小上限**（到顶后写不进去，
// 再往里送块就是静默丢历史 —— 2026-10-19 的日志里 110 秒的数据就是这么丢的）。
bool TraceFileRecordsActive();

// 本段 cache 到过大小上限（停止追加）后，**第一次**调用返回 true，之后返回 false。
// 供 UI 层弹一次提示框：到顶不是错误，但用户必须知道"再往前的历史没留在盘上"。
bool TraceFileTakeCapNotice();

// 无条件删掉日志目录（%TEMP%，即 ORB_AGDI.log 所在目录）里所有
// `ORB_rec_*.trc` / `ORB_trace_*.orb` —— 上一次会话的残留，不管本次开没开 cache。
// Connect 与 disconnect 各调一次；返回删掉的个数。幂等，删不掉只记日志。
unsigned TraceFilePruneLeftovers(const char* why);

// 追加一块退休块的紧凑记录字节流。
void TraceFileWriteRecords(const uint8_t* p, unsigned n);

// 记一条索引 {文件偏移, 周期号}；调用方在追加之前调。
void TraceFileMarkRecords(uint64_t firstCycle);

// 按周期号定位偏移；没有记录 cache或索引为空时返回 false。
bool TraceFileLocateRecords(uint64_t cycle, uint64_t& ofs, uint64_t& cycleAtOfs);

// 从 ofs 处读最多 max 字节，返回实际字节数（0 = 到头/没开）。
unsigned TraceFileReadRecords(uint64_t ofs, uint8_t* dst, unsigned max);

// 取 afterOfs 之后的下一条索引。
// 记录 cache是各块字节流的拼接，每块首条记录存“差 = 0”，跨块线性解会漂移，
// 必须按索引逐段重锚。
bool TraceFileNextMark(uint64_t afterOfs, uint64_t& ofs, uint64_t& cycleAtOfs);

// 记录 cache覆盖的周期范围；没有记录 cache或没有索引时返回 false。
bool TraceFileRecordsRange(uint64_t& firstCycle, uint64_t& lastCycle);

// 清空可查历史（丢索引，文件是 append-only 不回收）。
// “清空窗口”必须连内存窗口之前的那段一起清，否则清完还能翻出旧记录。
void TraceFileClearRecords();

// 记录 cache现场证据（一行）。
void TraceFileLogRecords(const char* where);

// ---- 三条来源的写入口 ----
// orbtrace 给的已经是 OFLOW 帧，原样写入。
void TraceFileWriteRaw(const uint8_t* p, unsigned n);

// swo=/etb= 是裸字节流，本层编码成 OFLOW 帧再写入（tag 取 kOflowTagItm/Etm）。
void TraceFileWriteStream(uint8_t tag, const uint8_t* p, unsigned n);

// ---- 索引 ----
// 记一条索引，含义是“偏移 ofs 之前已包含周期 ≤ cycle 的记录”。
// 满了就停止添加并告警一次：文件仍完整，只是更早的区域没有索引。
void TraceFileMark(uint64_t cycle);

// 现场证据（一行）：路径/字节/帧/索引数/索引覆盖的周期范围。
void TraceFileLog(const char* where);

} // namespace agdi
} // namespace orb
