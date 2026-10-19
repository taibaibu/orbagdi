/**
 * @file ORBAGDI_TraceFile.h
 * @brief trace 落盘（P2-1）：把 trace 流写成 orbuculum 能打开的 OFLOW 文件。
 *
 * 内存缓冲只装得下**秒级**窗口（全指令下 1.5 GB 也只有 3.5–6 s，见 Todo.md §二），
 * 所以"整段历史"只能靠落盘。文件格式直接沿用 orbuculum 的 OFLOW 文件格式，好处是
 * `orbmortem` / `orbcat` / `orbdump` 能直接打开它（带符号/源码浏览），不必我们再写
 * 一个浏览器。格式细节与两侧的逐行对照见 `ORBAGDI_TraceFile.cpp` 上方。
 *
 * 文件名：`ORB_trace_YYYYMMDD_HHMMSS.orb` —— **带时间、不带版本号**。版本号
 * （`%%ORBFLOW1.0.0%%`）是**文件内容的签名**，属于格式的一部分，不能去。
 */
#pragma once

#include <stdint.h>

namespace orb {
namespace agdi {

// 打开落盘文件。`path` 的取值：
//   ""            -> 不落盘（默认）
//   "1"           -> 默认目录（日志所在目录，通常 %TEMP%）下自动命名
//   "<目录>"       -> 该目录下自动命名（以 '\' 或 '/' 结尾，或本身是已存在的目录）
//   "<...>.orb"   -> 就用这个名字（不再加时间戳）
// `keep` = 同一目录里最多保留几个 `ORB_trace_*.orb`（按名字排序 = 按时间排序，
// 超出的删掉；0 = 不删）。**调试用的机器盘小，默认就删。**
//
// `capMB` = 文件大小上限（MB），0 = 不限。**到顶就停止追加**并告警一次。
// 为什么是"停"不是"环形覆盖"：这两份文件在系统里的角色是**旧历史**（内存窗口装最近的，
// 文件装更早的，查询先回放文件再走内存），所以"停在顶部"正好等于"只能回溯到这么远"；
// 环形覆盖要把索引里的偏移全部重基，代价大且容易出错。
//
// 返回 false = 没开（路径不合法 / 建不出文件），trace 采集不受影响。
// 幂等：已开着再调只返回 true。
bool TraceFileOpen(const char* path, unsigned keep, unsigned capMB);

// 关闭并打一行日志（路径 / 字节数 / 帧数 / 索引条数）。`why` 只用于日志。
// 幂等；没开时什么都不做。
void TraceFileClose(const char* why);

// **关闭并删除**当前落盘文件，返回删掉的文件个数（0/1/2）。`why` 只用于日志。
//
// 语义是 **reset**（用户 2026-10-14 的裁定）：reset 是"真正意义的从头开始"，
// 所以上一轮的缓存文件必须清干净，**不是**截断 —— 留半截历史只会让人把它误读成
// 本轮的数据。用户原话："reset 清空时也清理掉（因为只是缓存文件，真的有效数据
// 是从 keil 里导出，其他家也是这么搞的）"。
//
// ⚠ 与 `TraceFileClose()` 的区别：那个只是收尾（文件留着给外部工具读），
//   这个把文件从盘上抹掉。调用方紧接着要重新 `TraceFileOpen*()` 开新文件，
//   否则这一段就完全不落盘了。
//
// 幂等；没开过 / 已删过时返回 0。
unsigned TraceFileCloseAndDelete(const char* why);

bool TraceFileActive();

// 当前落盘路径（UTF-8），没开时返回 ""。给日志与设置页用。
const char* TraceFilePath();

// ---------------------------------------------------------------------------
// 记录日志（P2-2）：让"内存窗口之外"的区间能被查回来
// ---------------------------------------------------------------------------
// 和上面那份 OFLOW 文件**分工不同**：
//
//   `trfile=`  OFLOW 文件 —— 给外部工具（`orbmortem`/`orbcat`）用，内容 = 原始流；
//              要查它就得**重新解帧 + 重解 ITM/ETM**（"每次查询都重解一遍"）。
//   `trrec=`   **记录日志** —— 给本驱动自己用，内容 = **紧凑记录字节流**
//              （P1-2 的格式，一块退休时把它的字节流原样追加）。
//              查询直接展开，和内存路径走**同一段解码代码**，零重解。
//
// 两块分工的由来：P1-2 已经证明"存解码后的紧凑记录"比"存原始流"省一半以上，
// 而且查询不用再解一次 —— 所以"能回看整段历史"这件事应该建在记录日志上。
// 两份文件互不影响，各自 opt-in（**默认都不落盘**）。
//
// ⚠ **默认仍然不开，但理由变了**（2026-10-14，P3-1 交接层之后）：落盘**不再**在抽数
//   线程上同步 `fwrite` —— 写满的块交给后台写线程（`ORBAGDI_TraceWin.cpp` 的
//   `RetirePush`，4 槽交接队列，槽数逆向自官方 `0x100EBAAAD` 的 `and eax, 3`），
//   抽数只交指针、不等磁盘。所以现在开它不会卡抽数；`trrec=1` 就是"内存窗口之外
//   还能回看"的那一份。默认关只是"不往机器盘上偷偷写文件"这个**默认策略**。
//
// ⚠ **写盘目录**：`path` 传 `"1"` 就是"日志所在目录下自动命名"，而日志在 `%TEMP%`
//   （`ORB_AGDI.log`，见 ORBAGDI_Log.cpp 的 GetTempPathW）。所以 `trrec=1` 的落点
//   就是 `%TEMP%\ORB_rec_YYYYMMDD_HHMMSS.trc`。
//
// `capMB` = 文件大小上限（MB），0 = 不限；默认由 `trrecmb=` 给（2048 = 2 GB）。
bool TraceFileOpenRecords(const char* path, unsigned keep, unsigned capMB);

// 记录日志是否**真的开着**（`trrec=` 给了路径、且文件确实建出来了）。
//
// 调用方是 `ORBAGDI_TraceWin.cpp` 的"内存到顶"分支：它要据此判断"这块历史能不能
// 落盘"。**装不下、又落不了盘 ⇒ 必须报错**（`kTErrDataOvf` → 状态栏
// "Trace: Data Overflow"），不能静默丢 —— 这正是用户 2026-10-14 的要求：
// *"分配失败触发落盘，如果设置也没启用落盘功能，就触发异常"*。
bool TraceFileRecordsActive();

// 追加一块退休块的紧凑记录字节流（`ORBAGDI_TraceWin.cpp` 摘块/退化时调）。
void TraceFileWriteRecords(const uint8_t* p, unsigned n);

// 索引：`{文件偏移, 周期号}`，含义是"文件里偏移 `ofs` 处就是周期号 `cycle` 那条
// 记录"。调用方在**追加之前**调（先记起点，再写字节流）。
void TraceFileMarkRecords(uint64_t firstCycle);

// 按周期号定位记录日志里的偏移。找到返回 true，`ofs` = 该处字节偏移，
// `cycleAtOfs` = 该处那条记录的周期号。**没有记录日志（或索引为空）时返回 false**
// —— 调用方据此回落到"这段历史确实没有"。
bool TraceFileLocateRecords(uint64_t cycle, uint64_t& ofs, uint64_t& cycleAtOfs);

// 从记录日志的 `ofs` 处读最多 `max` 字节，返回读到的字节数（0 = 到头/没开）。
unsigned TraceFileReadRecords(uint64_t ofs, uint8_t* dst, unsigned max);

// 取 `afterOfs` 之后的**下一条**索引。回放靠它知道"块的边界在哪"：记录日志是
// 各块字节流的**拼接**，而每块的第一条记录存的是"差 = 0"（它的绝对值由索引给出），
// 所以线性跨块解会累积漂移 —— 必须按索引**逐段重锚**。
bool TraceFileNextMark(uint64_t afterOfs, uint64_t& ofs, uint64_t& cycleAtOfs);

// 记录日志覆盖的周期范围（第一条索引的周期号 / 最后一条索引的周期号）。
// 没有记录日志、或一条索引都没有时返回 false。
// 用途：内存窗口为空时，`GetFirstCycle` 用它把"可查范围"的左端报到整段历史的起点。
bool TraceFileRecordsRange(uint64_t& firstCycle, uint64_t& lastCycle);

// 清空记录日志的**可查历史**（丢索引）。`ClearTrace` 与 `TraceWinClear()` 调它 ——
// "清空窗口"要连"内存窗口之前那段还能回看"一起清掉，否则清完还能翻出旧记录。
// ⚠ 文件字节**不回收**（append-only，文件正在被追加）：丢的只是可达性。下一次追加
// 从当前文件位置继续记索引，新旧两段不会串起来。
void TraceFileClearRecords();

// 记录日志的现场证据（一行）。
void TraceFileLogRecords(const char* where);

// ---------------------------------------------------------------------------
// 三条来源的写入口
// ---------------------------------------------------------------------------
// orbtrace：探针交出来的**就是** OFLOW 帧 ⇒ 原样落盘，一个字节不改。
void TraceFileWriteRaw(const uint8_t* p, unsigned n);

// swo= / etb=：裸字节流 ⇒ 本层编码成 OFLOW 帧再落盘。
// `tag` 取 `ORBAGDI_Oflow.h` 的 kOflowTagItm / kOflowTagEtm。
void TraceFileWriteStream(uint8_t tag, const uint8_t* p, unsigned n);

// ---------------------------------------------------------------------------
// 索引（"索引留内存"，P2-1 的后半句）
// ---------------------------------------------------------------------------
// 记一条 `{文件偏移, 周期号}`，含义是"文件里偏移 `ofs` 之前已经包含了周期号 ≤
// `cycle` 的记录"。调用点是"一块历史刚被摘出内存"那一刻（`ORBAGDI_TraceWin.cpp`
// 的 `DropOldest()` 与退化分支）—— 那正是这段历史只剩文件里一份的时刻。
//
// 满了（`kMarkMax` 条）就停止添加并告警一次：文件仍然是**完整**的，只是更早的
// 区域没有索引可查（外部工具照样能从头读）。
void TraceFileMark(uint64_t cycle);

// 现场证据（一行）：路径 / 字节 / 帧 / 索引条数 / 索引覆盖的周期范围。
void TraceFileLog(const char* where);

} // namespace agdi
} // namespace orb
