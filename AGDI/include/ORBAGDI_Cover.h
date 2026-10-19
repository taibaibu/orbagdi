// ORBAGDI_Cover - 代码覆盖（AG_F_COVERAGE / supp.hCover）与性能分析
//                 （AG_PROFILEINFO / supp.hPaLyze）的数据面。
//
// 数据来源只有一处：ETM 指令 trace。解码器每解出一条指令就调 OnInstruction()，
// 这里把它记进"64 kB 段"的属性字与计数表 —— 覆盖率窗口读属性字，
// 性能分析器按地址范围查计数。
//
// 为什么不另开一条采样通道：ETM 已经是全指令粒度，再按周期采样只会更差；
// 而这个工程里 ETM 是唯一能给出"执行过的地址"的源（ITM/DWT 只有 PC 采样）。
// 没有 ETM 时覆盖率就是空的 —— 这是数据源限制，不是实现缺口。
#pragma once

#include <stdint.h>

namespace orb {
namespace cover {

/// 一条 ETM 指令（addr 低 bit 是 Thumb 标志，内部会清掉）。
/// 热路径：约 1 M 次/秒，无锁（解码线程是唯一写者）。
void OnInstruction(uint32_t addr, bool executed);

/// 覆盖率属性段的基址（uVision 按 DWORD* 索引 0x4000 项 = 64 kB 地址空间）。
/// 懒分配；段内属性字即 AG_ATR_EXECD / EXECDO 位。
///
/// ⚠ 这个段数组**就是** `AG_GETMEMATT` 返回单项指针的那块内存（见 AttrPtr）。
/// DbgCM 里 MGetAttr / MGetAttr16 / AG_GETATTRSEG 全落在同一份 `acc.atr[]` 上：
/// uVision 的覆盖率窗口是拿 GETMEMATT 的指针逐地址读的，GETATTRSEG 只用于
/// coverage 文件的 load/save（官方注释 "Attribute segment required for
/// coverage load/save"）。两套存储分开做 ⇒ 窗口永远读到 0。
uint32_t* AttrSeg(uint32_t segBase);

/// 单项属性字指针（4 字节粒度），即 AG_GETMEMATT 返回给 uVision 的地址。
/// 与 AttrSeg 同源；段表满返回 nullptr（等于官方"未映射段"，uVision 认这个）。
uint32_t* AttrPtr(uint32_t adr);

/// 读/写单项属性字（断点属性走这条）。
uint32_t AttrGet(uint32_t adr);
void     AttrSet(uint32_t adr, uint32_t v);

/// 只清断点/观察位（AG_BPKILLALL、会话结束），**保留** EXECD/EXECDO 与计数。
void AttrClearBp();

/// AG_MEMMAP 的段初始化（照 DbgCM MMapSeg 的语义：nAttr == 0 清空，否则按位或）。
/// 少了这一步，段属性只有我们标的 EXECD，uVision 侧那些"可执行/只读"位就是空的。
void MapSeg(uint32_t addr, uint32_t len, uint32_t attr);

/// 清零所有段的执行位与计数（AG_CLEARCOVERAGE）。
void Clear();

/// 某地址被执行次数（4 字节粒度，AG_PROF_EXECCOUNT）。
uint64_t ExecCount(uint32_t addr);

/// [start, end) 内累计执行次数 —— 性能分析里当 cycles 用（轴是每条指令 +1）。
uint64_t ExecCountRange(uint32_t start, uint32_t end);

/// 已分配的段数（日志用）。
unsigned SegmentCount();

/// 带执行位（EXECD/EXECDO）的 DWORD 个数 —— 停机时打一行，用来区分
/// "我们没标"（=0）和 "uVision 读了另一条路"（>0 而窗口仍空）。
uint64_t MarkedWords();

} // namespace cover
} // namespace orb
