<h1 style="color:red">🔴🔴🔴 最高警告（MAX） 🔴🔴🔴</h1>

<h1 style="color:red">任何偏离 <code>AGDI\DbgCM</code> 实现方法的实现皆为风险。</h1>

<h1 style="color:red">PDSC 是 KEIL 核心流程，严禁硬编码，软开关；功能应做就做，行到行兼容。</h1>

---
# bug.md — ORBAGDI 真 bug 清单

**本文件只放这一层解决不了的真 bug。** 判据：

* **Keil / uVision 不支持** —— 需要的接口它不向 AGDI 驱动暴露；
* **RDDI 层未实现** —— 需要的入口在 `ORBAGDI_Rddi.h` 的 API 表里不存在；
* **WONTFIX** —— 有意不做，理由写在条目里。

**不进本文件的**：

* 一切能在代码里改的（含功能缺口、行为偏差、部署项）→ `Todo.md`；
* "已实现但没上机验证" → `Todo.md` 的**待测试**清单，它不是 bug，是待测项。

历史改动记录（原 §A–§D、§G、§H、§J–§P）已移入 `Todo.md`；
本文件保留 §E（实机日志分析）/ §F（参考对照）/ §I（设置界面审计）作为**证据附录**，
因为它们是上面这些结论的原始出处，且被源码注释按名字引用。

状态图例：`OPEN` 未处理 / `WONTFIX` 有意不做 / `BLOCKED` 等外部条件。

---

## R. 真 bug

### R1 `WONTFIX` 运行中内存访问（`AG_F_MEMACCR` = 0）

Cortex-M 的 AHB-AP 在核心运行时访问内存会被总线仲裁拖住甚至锁死。可靠做法是
"暂停—访问—恢复"，但那会破坏实时性，而实时性正是用户开这个选项的原因。
想清楚策略之前不打开，故 `AG_F_MEMACCR` 恒为 0，uVision 也不会在运行中发起访问。

### R2 `BLOCKED` `AG_GoStep` 阻塞期间无事件唤醒，只能轮询

`RunAndWait()` 用 `Sleep(1)` 轮询 `DHCSR.S_HALT`：单次停止最多引入约 1 ms 延迟，
且等待期间一直占着调试线程。

**为什么修不了**：RDDI 接口表（`include\ORBAGDI_Rddi.h`）里**没有任何异步状态或事件
入口** —— 只有 `DAP_ReadReg/WriteReg`、`CMSIS_DAP_*`、`OrbTrace_*`、`TraceStream_*`
这些同步调用。宿主不会主动告诉驱动"目标停了"，所以除了轮询没有别的办法。
（Keil 官方驱动靠自己的 ULINKpro 硬件通路做到，我们走 RDDI，这条路不通。）

### R3 `BLOCKED` 多 DAP / SWD 多 drop / 多器件 JTAG 链无法选择

`RDDI_DAP_CONN_DETAILS` 只有两个字段：

```c
typedef struct _RDDI_DAP_CONN_DETAILS {
    char implementorName[160];
    char connectionDescription[160];
} RDDI_DAP_CONN_DETAILS;
```

**没有 DAP 索引**。`CMSIS_DAP_DetectNumberOfDAPs` / `DetectDAPIDList` 能*数出*链上
有几个 DAP、列出 ID，但 `DAP_ReadReg/WriteReg` 只认一个会话句柄，接口里没有任何
"选中第 N 个 DAP"的入口。所以多 drop / 多 TAP 场景下无法切换，只能操作第一个。

对照：官方 `ULP2CM3.dll` 有 `Multi-Drop SWD not supported.` /
`PDSC: JTAG TAP Index out of bounds` / `JTAG Debug Port IR Length redifinition`
一整套诊断，说明它自己实现了 TAP 链遍历 —— 那是直连硬件的驱动才有的能力。

### R4 `WONTFIX` `AG_F_POWERMEASUREMENT` 恒为 0

ULINKplus 的功耗测量是它自己的硬件（`Granular Power Requester`、
`Prime Cell Timestamp Generator`）。本工程对接的是 CMSIS-DAP 类探针，没有这个量。

### R5 `FIXED` 状态栏 `UV_RTAH_ERRMSG` 通道未接（原判 `WONTFIX` 已撤销）

**原判断（错误）**：官方 `OutMsg()` 走 `pio->Notify(UV_RTAH_ERRMSG, txt)`，而 `pio` 是
UVSOCK 接口对象，uVision **不向 AGDI 驱动暴露**；AGDI 能用的文本出口只有
`SendMessage(mainFrame, Uv2Msg, ...)` 与 `AG_CB_MSGSTRING`，两者都落不到状态栏。

**这个判断与本仓库自己的代码矛盾**：`ORBAGDI_TraceWin.cpp` 一直在用
`orb::pdsc::Bom()->Notify(UV_TRACE_IF_INIT / UV_TRACE_IF_STATUS / UV_TRACE_WIN_CONFIG)`
——**`orb::pdsc::Bom()` 返回的就是 `struct bom*`，也就是官方口中的 `pio`**。
`UV_RTAH_ERRMSG`（`BOM.H:2382 = 0x827`）只是它的又一个请求码，与 TDI 用的那几个
走同一条通道。当初把"拿不到 `pio`"当成事实，是没有把两条路径对上。

**实现**（`ORBAGDI_TraceWin.h/cpp`，逐字对齐官方 `DbgCM/AGDI.CPP:702-743`）：

| 官方 | 本驱动 |
|---|---|
| `OutMsg(char *txt)` | `TraceWinStatusText(const char *text)` |
| `OutTraceMsg(BYTE msg)` | `TraceWinStatusMsg(unsigned msg)` |
| `char *TraceMsg[]`（`Trace.cpp:45-54`） | 同名同内容表（含 `\<clrr>` 红色前导） |
| `TraceOpt`（`Trace.cpp:114`） | `TraceWinSetOptions` / `TraceWinOptions` |
| `T_Secure`（`Trace.cpp:116`） | 同名文件级状态 |

官方检查的三个 BOM 字段 `hwnd` / `hmsg` / `bSecureExt` / `bSTrcEna` 在 `BOM.H`
（`:3713` / `:3714` / `:3995` / `:3999`）里都在，所以守卫条件与安全前缀分支可以照抄；
v7-M 上 `bSecureExt` 恒为 0，该分支不进入。

**顺带对齐了 trace 选项掩码**：官方 `Trace.h:295-310` 的 `TRACE_*` 位（`TRACE_ENABLE`
`TRACE_TIMESTAMP` `TRACE_PCSAMPLE` `TRACE_PC_DATA` `TRACE_EXCTRC` `TRACE_CPI`
`TRACE_EXC` `TRACE_SLEEP` `TRACE_LSU` `TRACE_FOLD` `TRACE_CYCCNT` `PC_SAMPLE`
`TPIU_FORMAT` `ETM_TRACE` `UNLIMITED_TRACE` `TRACE_USE_CORECLK`）与消息码
（`Trace.h:129-136` 的 `T_MSG_COM_ERR`..`T_MSG_RUN`）取值原样引入。官方
`SetTraceRunSynch` 对应本驱动的 `TraceWinSetRunning`——两者都在整体状态变化时才通知。

**采集状态三处合一**：新增 `orb::agdi::TraceSetRunning(bState, bWinUpdate)`，
把官方成对出现的三件事绑在一起（`TraceOpt` 总开关、`SetTraceRunSynch`、`OutTraceMsg`，
见 `DbgCM/AGDI.CPP:2758-2771`）。原先是 4 处调用点各自判断"有没有 trace 源"，
会得出不一致的结果（窗口说在处理、状态栏却是空的）。4 处调用点已全部改走它。

**验证**：`ORB_AGDI.dll` 337,408 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。
**实机待确认**：uVision 是否真的接受来自 AGDI 的 `UV_RTAH_ERRMSG` 并把文本显示在
状态栏——通道本身是通的（与 TDI 同一对象、同一 Notify），但显示行为要上机看。


### R6 `BLOCKED` 核心时钟没有权威来源

SWO 位时钟 = 核心时钟 ÷ TPIU 预分频，所以核心时钟必须是真值。但
`struct dbgblk`（`ComTyp.H:212`）里**只有一个时钟字段**：

```c
DWORD Clock;        // clock frequency
```

它是**晶振**（uVision 工程 Target 页的 "Xtal"），不是核心时钟 ——
§E9 实测 STM32F407 报 12 MHz 而核心跑 168 MHz。

**为什么修不了**：uVision 不提供核心时钟，通用解法不存在。从 PLL 寄存器推算需要
器件相关的 RCC 布局知识（STM32 各系列、nRF、RA 全不同），从器件包读要 PDSC 里有
这个信息（多数没有）。现在的做法是：不再拿晶振冒充（§P2），改用 Trace 页的值与
文档化默认值，并在会话横幅里显示派生出的位时钟，让错误可见。

### R7 `BLOCKED` 指令级 trace 无落盘通路

官方驱动自己写 `%s\isync.td`（ISYNC 文件）供 uVision 做指令级重建，并做 ETM v4
特性协商（`Branch Future Trace` / `Conditional Instruction Trace`）。

**为什么修不了**：`TraceStream_*` 只给原始字节流（`Open/Close/Read/Status`），
没有任何"这条流是什么格式、同步点在哪"的协商入口；ISYNC 的格式与 `.td` 的落盘
时机由 uVision 侧决定，驱动看不到。本工程的 ETM 解码器本身是骨架 ——
那部分**能改，在 `Todo.md`**。

---

## 附录 A. 引用对照（原 §E / §F / §I / §N 已丢失）

2026-10-09：本文件做结构整理时，一次按行范围切割的批量改写把 §E（实机日志分析）、
§F（参考对照）、§I（设置界面审计）、§N（缺口清单）的正文**误删**，且本工程不在
版本控制下，无法恢复。**教训：文档只能用编辑工具逐处精确修改，不要用脚本按正则
切割。**

因此本文件现在只剩 §R。源码里 `bug.md §I` / `§I2` / `§N` 的引用对应的内容
已不在本文件，其结论分别落在：

| 原引用 | 内容去向 |
|---|---|
| `bug.md §I` / `§I2` | 设置界面审计：结论并入 `Todo.md` 第一/二轮记录与 `COMPAT_ANALYSIS.md` §9 |
| `bug.md §N` | 缺口清单：并入 `Todo.md`（含 P0–P3 分级与实机复测项） |
| `bug.md §E*` | 实机日志分析（E1–E20，STM32F103/F407 会话）：**已丢失**，残留结论见 `Todo.md` 的 §E 编号条目 |
| `bug.md §A*` / `§B*` / `§C*` / `§D*` | 功能缺口 / 行为偏差 / 部署 / 待验证：已按新规则分流，见 `Todo.md` |

## 附录 B. 待补的证据

下面这些结论目前只在 `Todo.md` 与源码注释里，原始日志分析已随 §E 丢失，
再遇到相关问题需要重新采一次日志：

* MEM-AP 恢复（原 §E8）：`AP IDR = 0x24770011`、`CTRL/STAT = 0xF0000040`
* 擦除静默空转（原 §E17）：`AddrSector` 是偏移、且是「run」不是单个扇区
* 大块读越过 1 KB 窗口回绕（原 §E19）：verify 误报 mismatch 的真凶
* 取镜像块协议（原 §E14）：`AG_CB_GETFLASHPARAM` 首次必须传 `NULL`


---

### Flash: `no sector of the algorithm covers 0x08030000..0x08040000` —— 假错误（已修）

**现象**：下载时报 `Error: Flash: no sector of the algorithm covers 0x08030000..0x08040000`，而该范围**完全在算法覆盖之内**（STM32F4 的 sector 5 = `0x08020000..0x0803FFFF`，128K）。

**根因**：`ORBAGDI_Flash.cpp` 的 `EraseRange()` 把两件不同的事混在一个计数器里。

```c
    uint32_t erased = 0;
    ...
            // 已被前面段擦过 → 跳过，不计数
            if (s_eraseSize && s >= s_eraseAddr && s < s_eraseAddr + s_eraseSize) {
                continue;
            }
            ... CallAlg(EraseSector) ...
            ++erased;
    ...
    if (erased == 0) {
        Fail("no sector of the algorithm covers 0x%08X..0x%08X", adr, end);
```

`erased` 数的是**实际执行的擦除次数**，但"已被前面段擦过"的扇区走 `continue` 不计数。于是：

1. 前一段覆盖 sector 5 → 擦除 → `s_eraseAddr=0x08020000`, `s_eraseSize=0x20000`
2. 本段 `0x08030000..0x08040000` → 候选只有 sector 5（sector 6 因 `s >= end` 被跳过）→ 去重命中 → `continue`
3. `erased == 0` → **报出"没有扇区覆盖该范围"这个与事实相反的结论**

**关键对照：官方根本没有这条失败路径。**

`DbgCM/Flash.cpp:1141-1196` 的 `EraseFlash()` 对"已擦过"的处理是**直接成功返回**：

```c
    while (nMany) {
        // Check for Last Erased Sector
        if ((nAdr >= ESecAddr) && (nAdr < (ESecAddr + ESecSize))) {
            n = (ESecAddr + ESecSize) - nAdr;
            if (nMany > n) { nAdr += n; nMany -= n; ... }
            else { ...; return (0); }   // Previously Erased  ← 成功
        }
        ...
    }
    return (0);
```

⇒ **这个假错误是我们自己发明的**，不是继承来的。

**修复**：把"表里有候选扇区"（`covered`）与"实际执行了擦除"（`erased`）分开。

- `covered == 0` → 仍然失败（这才是"表里根本没有这个范围"的真问题）
- `covered > 0 && erased == 0` → **成功**，日志记 `already erased (N sector(s))`
- 两者都 > 0 → 日志记 `M of N sector(s) erased`

**验证**：`ORB_AGDI.dll` 341,504 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。

**顺带发现的一个未采纳的改进**（记在这里，供以后决定）：官方在擦之前会先 `BlankCheck()`：

```c
        // Erase Sector (if not already erased)
        if (BlankCheck(adr, sz, FlashDev.valEmpty)) {
            if (EraseSector(adr)) return (1);
        }
```

即"只在扇区非空时才擦"。这比我们的"记住最后一个擦过的扇区"（单槽 `s_eraseAddr`/`s_eraseSize`）**健壮得多**：

- 单槽只能记住**一个**扇区，段序打乱时会重复擦（不错误，只是慢——每次多花一个大扇区的擦除时间）
- `BlankCheck` 是逐扇区判断，与段序无关，而且能顺带省掉"本来就是空的"扇区

官方注释里也点明了这一点（`// Erase Sector (if not already erased)`）。**本次没有改**，因为用户指示是修这个报错；要采纳的话需要接 FLM 的 `BlankCheck` 入口。

---

### 同上 —— **完整采用官方 `EraseFlash`**（上一条写的"本次没改"作废）

上一条只修了假错误，并把官方的 `BlankCheck` 机制记为"未采纳"。**现按用户指示完整实现官方版本**，`EraseRange()` 已按 `DbgCM/Flash.cpp:1141-1196` 逐行重写。

**官方结构的关键：擦除循环的推进机制**

官方不是"遍历范围内的扇区"，而是 `while (nMany)` 里**每轮处理一个扇区**，靠循环顶部的"上一个已擦扇区"检查来推进：

```c
    while (nMany) {
        // Check for Last Erased Sector
        if ((nAdr >= ESecAddr) && (nAdr < (ESecAddr + ESecSize))) {
            n = (ESecAddr + ESecSize) - nAdr;
            if (nMany > n) { nAdr += n; nMany -= n; ... }
            else { ...; return (0); }   // Previously Erased
        }
        // 找 nAdr 所在扇区（不在表首时用前一条 run 的尺寸外推）
        ...
        // Erase Sector (if not already erased)
        if (BlankCheck(adr, sz, FlashDev.valEmpty)) {
            if (EraseSector(adr)) return (1);
        }
        ESecAddr = adr;
        ESecSize = sz;      // ← 下一轮顶部检查就会跳过刚处理的扇区
    }
```

**`ESecAddr`/`ESecSize` 一个变量干两件事**：
1. **推进循环**——刚擦完的扇区记进去，下一轮 `nAdr` 落在其中，于是跳过；
2. **跨段去重**——上一段擦过的扇区，本段同样跳过。

这解释了为什么官方**没有"无事可做"的失败路径**：整个范围都已擦过时，顶部检查直接 `return (0)` 成功。我们原先按"擦除操作计数"判断，才把这一情形变成了假错误。

**逐行对齐的要点**

| 官方 | 实现 |
|---|---|
| `sectors[i].AddrSector + FlashConf.Dev[SelAlg].Start` | `devBase + d.sectors[i].AddrSector` |
| 搜索循环的外推（`:1168-1177`） | 原样，含 `oldadr`/`oldsz` 逻辑 |
| `BlankCheck(adr, sz, FlashDev.valEmpty)` | `CallAlg(s_alg.fn[kFnBlankCheck], a, sz, d.valEmpty, res)`，**非零 = 非空 → 擦** |
| `ESecAddr = adr; ESecSize = sz;` 无条件 | 原样（未擦的空白扇区也记） |
| `SetHex8(adr)` | 无对应（uVision 的十六进制显示），改为日志 |
| `Counter`/`UpdatePos` | 未接（本函数原本就没有进度回调，属既有差异） |

**我们已经有 `kFnBlankCheck`**（`ORBAGDI_Flash.cpp:92` 的 `AlgFn` 枚举、`:1252` 从 FLM 的 `BlankCheck` 符号解析），所以这一步不用新增基础设施。FLM 没有 `BlankCheck` 入口时退化为无条件擦除（与改动前一致）。

**新增一处官方没有的安全网**：官方的外推在"表的第一条 run 起点高于请求地址"时会**下溢**（`(nAdr - n)` 为负的无符号运算），算出一个巨大地址且永不推进 → **死循环**。已加检查：找到的扇区必须真的包含 `nAdr`，否则 `Fail` 报出表的问题而不是挂住。

**验证**：`ORB_AGDI.dll` 341,504 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。

---

### 烧录慢 —— 日志定案 + `CallAlg` 会话级重构（已修）

**日志分析**（`%TEMP%\ORB_AGDI.log`，1,402 行 / 一次下载 / 11:08:32 → 11:09:14 = **42 秒**）

按被调用的算法函数地址分类（`Flash: call 0xADDR` × `halted at the sentinel after N ms`）：

| 地址 | 次数 | 总耗时 | 平均 | 判定 |
|---|---|---|---|---|
| `0x200000DF` | 295 | 19,818 ms | 67 ms | **ProgramPage** |
| `0x20000093` | 7 | 7,123 ms | 1,018 ms | **EraseSector** |
| `0x20000059` | 4 | 251 ms | 63 ms | Init 类 |
| `0x2000002B` | 3 | 235 ms | 78 ms | Init 类 |

**⇒ `BlankCheck` 一次都没被调用**：`STM32F4xx_512.FLM` 里没有这个符号，`s_alg.fn[kFnBlankCheck]` 为 0，直接跳过。**上一轮那个改动对速度零影响。**

**42 秒的构成**

| 项 | 耗时 | 性质 |
|---|---|---|
| 算法真实执行（27,427 ms 合计） | 27.4 s | 擦除 7.1 s（128K 扇区 ~1 s，真实闪存时间）+ 编程 19.8 s（FLM 自身速度） |
| **`CallAlg` 自身开销** | **~14.6 s** | **我们的开销，可优化** |

**开销定位**：`CallAlg` 每次调用做 ①`ReadRegs(save)` ②8 次单独 `WriteRegSel` 写参数 ③`ReadCoreDebug`+写 DEMCR ④读写 ACTLR ⑤PC 写入校验 ⑥`ReadRegSel(PC)` 校验哨兵 ⑦`WriteRegs(save)` ⑧还原 DEMCR/ACTLR。**约 90 次 MEM-AP 往返 ≈ 47 ms/调用**，与实测差值吻合。

**对照官方 `ExecuteFunction`（`DbgCM/Flash.cpp:142-213`）**

- 用**缓存的全局 `RegARM`**（`SysCallExec(&RegARM)`），**不读回/写回整份寄存器**
- **不重装 DEMCR/ACTLR**
- 轮询循环**无 `Sleep`**，直接 `ReadD32(DBG_HCSR)` 自旋
- 官方 ChangeLog 里有一条：`Bug fix regarding uVision slowdown during flashload in ExecuteFunction().` —— **它自己也踩过这个坑**

---

**修复：把寄存器存取与调试控制提到会话级**

新增 `FlashAlgEnter()` / `FlashAlgLeave()`：

- `FlashAlgEnter()`：**首次调用时**读回寄存器文件 + 装 DEMCR 向量捕获 + 关 ACTLR 写缓冲，此后置标志直接返回
- `FlashAlgLeave()`：还原 ACTLR/DEMCR + 写回寄存器文件，清标志
- `CallAlg` 去掉每次调用的 `ReadRegs`/`WriteRegs` 与 DEMCR/ACTLR 装卸，只保留参数写、PC 校验、Resume、轮询、哨兵校验

**还原点**（避免会话残留）：

- `FlashInitLoad()` 开头调 `FlashAlgLeave()` —— **自愈**：上一次会话在错误路径上结束时会残留状态，下次会话开始时先还原，保证写回的是**应用**的寄存器值而不是上一次算法的残留
- `FlashStartLoad()` 成功收尾处（`ResetAndRunIfAsked()` 之前）
- `CallAlg` 的 `Resume()` 失败路径

**预期收益**：每次调用省下约 64 次往返（寄存器存取）+ 6 次（DEMCR/ACTLR）≈ **40 ms × 309 次 ≈ 12 秒**。42 s → **约 30 s**。

**未采纳**（保留差异，已在代码注释标明）：官方轮询循环无 `Sleep`；本驱动保留 `Sleep(1)`，因为官方那一侧靠 `pio->Notify(UV_DOXEVENTS, 1)` 泵 uVision 消息队列，我们没有对应调用——去掉 `Sleep` 会让下载期间 UI 饿死。

**验证**：`ORB_AGDI.dll` 341,504 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。
**待实测**：新日志里 `halted at the sentinel after N ms` 的**总和应基本不变**（那是算法真实时间），而墙钟总时长应从 42 s 降到 ~30 s。

---

### 擦除期间 UI 卡死 / 进度条停住 —— 缺 uVision 消息泵（已修）

**现象**：擦除进度到 87% 后鼠标转圈、界面卡顿一会儿，然后烧录完成；进度条像是"丢了"。

**日志证据**：相邻日志行的最大时间间隔，**全部落在 `EraseSector` 返回的那一行**：

```
1.97 s  ->  Flash: 0x20000093 -> halted at the sentinel after 1984 ms
1.96 s  ->  Flash: 0x20000093 -> halted at the sentinel after 1984 ms
1.10 s  ->  Flash: 0x20000093 -> halted at the sentinel after 1125 ms
0.34 s  ->  Flash: 0x20000093 -> halted at the sentinel after  360 ms
```

**扇区擦除本身要 1~2 秒**（STM32F4 大扇区，真实闪存时间）。这期间我们**一行日志不发，也不碰 uVision 的消息队列**——而下载是在 **uVision 自己的 UI 线程上**执行的，所以 IDE 无法重绘：光标变沙漏、进度条不动。

**官方正是为此在轮询循环里泵消息队列**（`DbgCM/Flash.cpp:169-208`）：

```c
    do {
        pio->Notify(UV_DOXEVENTS, (void *)1); // no need to use more than one events iteraton
                                              // 11.09.2019: Moved up here to have at least one UV_DOXEVENTS
                                              //   per flash algorithm function call. Otherwise it could be often skipped
                                              //   for fast algorithm functions.
        ... ReadD32(DBG_HCSR, &val) ... if (val & S_HALT) { SysCallRes(&rval); return rval; }
    } while (tick1ms < timeout);
```

`UV_DOXEVENTS`（`BOM.H:2388 = 0x832`，注释就是 "Run MessagePump 'n' times"）**一次泵一轮消息队列**。官方注释里还写着：把它移到循环顶部，是为了保证**每次算法调用至少泵一次**，否则快速返回的算法会把它跳过去。

**我们从来没有这个调用。**

**修复**：`CallAlg` 的轮询循环顶部加：

```cpp
        if (bom && bom->Notify) bom->Notify(UV_DOXEVENTS, (void *)1);
```

`bom` 在进入循环前取一次（`orb::pdsc::Bom()`）。`BOM.H` 已加入本文件的 include（`ORBAGDI_Pdsc.h` 只前置声明 `struct bom`，取 `Notify` 与 `UV_DOXEVENTS` 需要完整定义）。

**效果**：长算法执行期间 uVision 能重绘 —— 沙漏光标消失、进度条继续走。

**验证**：`ORB_AGDI.dll` 341,504 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。

**遗留观察**：`CallAlg` 的轮询仍是 `IsHalted()` + `Sleep(1)`，即擦除期间约每毫秒一次 MEM-AP 读（2 秒擦除 ≈ 2000 次往返）。官方是**无 `Sleep` 自旋** + 泵消息。两者总往返量同量级，暂不改为自旋——`Sleep(1)` 让出 CPU 给别的线程，去掉它需要更仔细地确认 uVision 的响应性（官方那一侧靠 `UV_DOXEVENTS` 保证，现在我们也接上了，理论上可以去掉 `Sleep`，但先观察）。

---

### 轮询循环改为官方自旋（去掉 `Sleep`）

用户要求完全按官方。`CallAlg` 的轮询循环现在与 `DbgCM/Flash.cpp:169-208` 一致：

| 项 | 官方 | 现在 |
|---|---|---|
| 消息泵 | `pio->Notify(UV_DOXEVENTS, (void*)1)` 每轮 | 同 |
| 停止检测 | `ReadD32(DBG_HCSR)` 自旋，**无 `Sleep`** | `IsHalted()` 自旋，**无 `Sleep`** |
| 超时 | `while (tick1ms < timeout)` | `kAlgTimeoutMs`（30 s），超时打诊断 |

去掉 `Sleep(1)` 是安全的：`IsHalted()` 本身是一次 MEM-AP 读（USB 往返，约 0.5-1 ms），循环由传输层天然节流，不是 CPU 空转；而让 uVision 保持响应的是 `UV_DOXEVENTS`，不是 `Sleep`。

**收益**：每次检测省 1 ms 延迟。一次 2 秒的扇区擦除原本要付约 2000 次，即最多约 2 秒的纯检测延迟。

**验证**：`ORB_AGDI.dll` 341,504 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。

**仍与官方有差异（未改，逐条列明）**：
- 官方进度条更新 `UpdatePos(100 * tick1ms / timeout)` 每 500 ms 一次，但**只在 `TimeOutTick` 置位时**（即仅 `EraseChip` 路径）。本驱动没有这个标志，且 `EraseRange` 不做进度上报——进度条在擦除阶段的表现为既有差异，不是本次引入。
- 官方 `ExecuteFunction` 用 `RegARM` 全局并只写变化的寄存器；本驱动已改为**会话级**存取（`FlashAlgEnter`/`FlashAlgLeave`），但每次调用仍单独写 8 个参数寄存器。若要进一步逼近，可把参数合并成一次批量写。

---

### 擦除阶段进度条不动 —— 补官方 `Counter`/`TotalCount` 进度（已修）

**新日志核对**（用户实测，改动后）：

| 地址 | 次数 | 总耗时 | 平均 | 上次平均 |
|---|---|---|---|---|
| `0x200000DF` ProgramPage | 295 | **6,717 ms** | **23 ms** | 67 ms |
| `0x20000093` EraseSector | 7 | 6,515 ms | 931 ms | 1018 ms |
| `0x20000059` | 4 | 124 ms | 31 ms | 63 ms |
| `0x2000002B` | 3 | 79 ms | 26 ms | 78 ms |
| **算法总计** | | **13,435 ms** | | **27,427 ms** |

**算法耗时减半**（27.4 s → 13.4 s）。`ProgramPage` 67 ms → 23 ms：`Sleep(1)` 在 Windows 默认定时器精度（15.6 ms）下**实际睡约 15 ms**，去掉后每次省约 44 ms。`EraseSector` 931 ms 是真实闪存时间。

**但最大日志间隔仍是 1.98 s，且仍落在 `EraseSector`** —— 那 2 秒内没有进度上报，进度条不动。

**官方的进度机制在 `EraseFlash` 里，不在轮询循环**（`DbgCM/Flash.cpp:1146-1160`）：

```c
    while (nMany) {
        if ((nAdr >= ESecAddr) && (nAdr < (ESecAddr + ESecSize))) {
            n = (ESecAddr + ESecSize) - nAdr;
            if (nMany > n) {
                nAdr += n; nMany -= n;
                Counter += n;
                UpdatePos(100 * Counter / TotalCount);      // ← 擦除推进时更新进度条
            } else {
                Counter += nMany;
                UpdatePos(100 * Counter / TotalCount);
                return (0);
            }
        }
        ...
```

`Counter`/`TotalCount` 是**全局字节计数器**（`:64-65`），`TotalCount = pF->ActSize`（`:1220`）。**我们 `EraseRange` 里完全没有这个**——这就是擦除阶段进度条不动的原因。

**修复**：

- 新增模块状态 `s_counter` / `s_total` 与 `UpdatePos()`（对应官方同名函数，走 `Progress(PROGRESS_SETPOS, ...)`）
- `EraseRange` 的**两处推进点**加 `s_counter += n; UpdatePos(s_counter);`，与官方 `:1153-1157` 一一对应
- 擦除 pass 开始前先把各段大小求和填入 `s_total`（对应官方 `TotalCount = pF->ActSize`，该趟不碰目标内存），并清零 `s_counter`

**验证**：`ORB_AGDI.dll` 342,016 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。

**与官方仍存的差异（已确认，非本次范围）**：官方 `ExecuteFunction` 用 `RegARM` 全局、只写变化的寄存器；本驱动每次调用单独写 8 个参数寄存器（其中 r9/SB 会话内恒定，理论上可提到 `FlashAlgEnter`，省 1 次往返/调用 ≈ 0.3 s）。

---

### 最后一个差异定位：官方用 `DAP_RegAccessBlock` 块访问，我们逐次单发

**结论先写清楚：这是真差异，但量级只有约 1~1.5 秒。**

**官方机制**（`DbgCM/SWD.cpp:1962` `SWD_SetARMRegs`）：

```c
    regID[0]   = DAP_REG_MATCH_RETRY;  regData[0] = 100;
    regID[1]   = DAP_REG_MATCH_MASK;   regData[1] = 0x00010000;
    regID[2]   = DAP_DP_REG_APSEL;     regData[2] = AP_Sel;
    regID[3]   = DAP_AP_REG_TAR;       regData[3] = DBG_Addr;
    regID[4]   = DAP_DP_REG_APSEL;     regData[4] = AP_Sel | 0x10;
    status = rddi::DAP_RegAccessBlock(rddi::k_rddi_handle, 0, 5, regID, regData);
    // ... 之后按 mask 继续用同一机制批量写 DCRSR/DCRDR ...
```

**`DAP_RegAccessBlock(handle, DAP_ID, numRegs, regIDArray, dataArray)` 把 N 次 DAP 寄存器操作合并成一次 RDDI 调用。**

**我们的现状**：`ORBAGDI_Rddi.h` 的 `Api` 表里**有 `DAP_RegAccessBlock`**（`ORBAGDI_Dap.cpp` 也已在 ETB 的 `EtbReadWords` 里用过 `DAP_RegReadRepeat`），但**算法调用路径一次都没用**——`CallAlg` 用 8 次单独的 `Core::WriteRegSel`，每次 2 个往返（DCRSR + DCRDR）= **16 次往返**。

**量级核算**（基于实测：算法总计 13,435 ms，日志时间跨度 15.4 s）：

- 剩余开销 ≈ 15.4 − 13.4 = **约 2 s**，全部来自 309 次调用的收发
- 参数寄存器写入 16 次往返 → 1 次块访问，可省约 15 次往返/调用 × 0.5 ms ≈ 7.5 ms × 309 ≈ **约 1.2~1.5 s**
- 即总时长 15.4 s → **约 14 s**

**算法真实时间 13.4 s 是物理下限**（擦除 6.5 s + 编程 6.7 s，都是闪存与 FLM 的速度），改不动。

**待办（下一步实施）**：`CallAlg` 改用 `DAP_RegAccessBlock` 批量写参数寄存器，与 `SWD_SetARMRegs` 同构。需要先读 `Core::WriteRegSel` 的实现，照它的 DAP 访问序列构造 `regID`/`regData` 数组（`TAR=DBG_Addr+4` 选 DCRSR、`DRW` 写选择子、`TAR=DBG_Addr+8` 选 DCRDR、`DRW` 写值），再一次性提交。

---

### 参数寄存器改为块访问（`DAP_RegAccessBlock`）—— 与官方 `SWD_SetARMRegs` 同构

**依据**：`Core::WriteRegSel`（`ORBAGDI_Core.cpp:299-304`）每写一个寄存器是

```cpp
    if (!m_dap->MemWrite32(kDcbDcrdr, value)) return false;              // 往返 1
    if (!m_dap->MemWrite32(kDcbDcrsr, sel | kDcrsrRegWnR)) return false; // 往返 2
    return WaitRegReady();                                               // 往返 3+（轮询 S_REGRDY）
```

8 个参数寄存器 = **24+ 次往返/调用**。

官方的做法（`SWD_SetARMRegs`，`DbgCM/SWD.cpp:1962-2000`）是把整批压进一次 `DAP_RegAccessBlock`，并用

```c
    regID[0] = DAP_REG_MATCH_RETRY; regData[0] = 100;
    regID[1] = DAP_REG_MATCH_MASK;  regData[1] = 0x00010000;  // DHCSR.S_REGRDY
```

**让 RDDI 宿主自己等 S_REGRDY**，因此寄存器之间不需要轮询。

**实现**：新增 `WriteAlgRegsBatched(dap, sel[], val[], n)`：

- 前四项是官方那套头部：`MATCH_RETRY=100`、`MATCH_MASK=0x00010000`、`SELECT=0`（AP0 bank0）、`CSW = kCswBase|kCswSizeU32|kCswAddrInc`
- 每个寄存器四项：`TAR=DCRDR`、`DRW=value`、`TAR=DCRSR`、`DRW=sel|WnR`（与 `Core::WriteRegSel` 同序：先 DCRDR 后 DCRSR）
- 一次 `DAP_RegAccessBlock(handle, 0, k, regID, regData)` 提交，之后 `dap.InvalidateCswCache()`（CSW 被绕过 `Dap` 改写）

**回退**：`CallAlg` 先试块访问，失败则记一条 WARN 并退回原来的逐寄存器路径。宿主没有 `DAP_RegAccessBlock`、或批次被拒时，行为与改动前完全一致，不会让下载失败。

**寄存器集合与官方一致**：xPSR/SB/SP/LR 由"调用方"设置（官方在 `ExecuteFunction:156-159`），A1-A3 与 PC 由具体调用设置（官方 `Init`/`EraseSector`/`ProgramPage` 各自设 `RegARM.A1..A3/PC`）。共 8 个，与改动前相同，只是传输方式变了。

**收益**：24+ 次往返 → 1 次块访问，约省 20 ms/调用 × 309 次 ≈ **1.2~1.5 s**。

**验证**：`ORB_AGDI.dll` 343,040 B / 0 warning；部署到 `D:\MDK5\ARM\BIN`。
**实机待确认**：块访问的 DAP 序列在本驱动 + 本探针上被宿主接受（若被拒会走回退路径，日志出现 `batched register write unavailable`，功能不受影响）。

---

### P1-0 的兜底读把整条 DAP 链路打挂 —— 已默认关闭（2026-10-13 真 uVision）

**现象**：`ORB_AGDI.log` 从 3860 行起刷屏；**停止调试失败**，uVision 反复弹"链路恢复"
（多点几次）；`ORBMDK_RDDI.log` 从 `[19:53:14.581]` 起持续报错。

**证据链**（同一时刻相邻的三行，`ORB_AGDI.log`）：

```
3881 [19:53:14.479][INF] orbtrace: 帧 tag=127 len=26 good=1 : FF FF …（垃圾帧）
3887 [19:53:14.479][INF] ETM: 解码器加载 —— 协议 ETM3.5，目标 Cortex-M4
3893 [19:53:14.578][WRN] Dap: block read of 128 word(s) at 0x08101E00 failed (0)
3895 [19:53:14.581][WRN] Dap: DP write reg=2 val=0x00000000 failed (rc=4110, err=8192)
3897 [19:53:14.581][WRN] ETM: P1-0 代码镜像补不出这一段（0x08101F64 -> 0x08101A2C，读不到指令）
```

**定性**：P1-0 的"现读目标内存兜底"在**抽数 worker 线程**上执行，读的地址来自 ETM 解码器。
流刚打开、解码器**还没同步**时（3881 行是 `tag=127/119` 的垃圾帧）解出的是 **0x0810xxxx**
这种越界地址（Flash 只到 0x080FFFFF）。拿它去读目标内存 ⇒ MEM-AP 报错、**DAP 进错误态**
（`DP write reg=2 … failed`）⇒ 之后**所有** DAP 操作都失败 ⇒ 停止调试失败 ⇒ 宿主弹
"链路恢复"、重连后 worker 又去读、又挂 —— 所以是**反复**弹。

**为什么"一次失败即熔断"不够**：熔断只挡后续，**第一次越界读的伤害已经造成**。
探针环境复现时同样如此（`DHCSR before = <read failed>`，5 项检查挂掉）。

**修法**（三层，已落地）：
1. **默认关闭兜底读**（`ORBAGDI_CodeImage.cpp` 的 `kMemFallbackEnabled = false`）——
   只认下载时留存的镜像，**零探针访问**；没下载就补不出（有日志，`补不出=` 会涨）。
2. **未同步不补**：`OnReport` 里先看 `EtmTraceIsSynced()`，未同步时连扫描起点都不更新
   （垃圾地址正是"刚打开"那段产生的）。
3. 保留**地址粗筛**（外设区 0x4000_0000-0x5FFF_FFFF 与内核私有区 0xE000_0000+ 不发事务）
   + **一次失败即熔断**，作为将来打开兜底读时的兜底。

**验证**：探针环境 `ALL PASSED (42 checks)`，汇总行
`CodeImage(orbtrace): 兜底读 0 B（失败 0，粗筛挡掉 0）—— 兜底读**未启用**`。

**遗留**：P1-0 本身仍未完成（缺**返回栈 / 控制流跟随** —— ETM 只报间接分支，实测 3200 万次
地址里 99.6% 是"同一个返回地址反复"，`FillGap(A, A)` 区间为空），见 `Todo.md` 的 P1-0 行。

---

### Trace Data 窗口只有列、没有行 —— `m_pRecordRange` 指向栈上（2026-10-13 真 uVision）

**现象**：Trace Data 窗口**全空**（只有表头 Time / Address·Port / Instruction·Data）；
Trace Navigation 条 `0 s → 0 s`、拖不动。而驱动侧**有数据**：
`TraceWin: running=0, 缓冲 13431761 条记录 / 23 块`、持有 92 MB。

**证据链**：

```
TraceWin: running=0, 缓冲 13431761 条记录 / 23 块
TraceWin: [0] BOUNDS (请求的周期区间整个在采集范围之外)     ← uVision 要的区间一条都没有
TraceWin: 显示区间 [1, 13431761] -> rc=0                   ← 范围报对了、rc=0
```

**定性**：`TraceWinShowRange()` 里 `TRACE_RECORD_RANGE range` 与 `TRACE_STATUS st` 都是
**栈上的局部变量**，而 `st.m_pRecordRange = &range` 交给了 `bom->Notify()`。uVision 在
`Notify` **返回之后**还会读 `m_pRecordRange`（不是立即拷走 —— 与"BOM 实例取错"同一类
问题），那时 `range` 早已失效 ⇒ uVision 拿到**垃圾区间** ⇒ 请求一个根本不存在的区间 ⇒
驱动回 `BOUNDS` ⇒ 窗口画不出行、Navigation 条的范围也因此是 0。

**同类第二处**：`StatusUpdate()` 的 `TRACE_CLOCK_SAMPLE cs`（`st.m_pClockSample = &cs`）
是同一个坑 —— 本次会话 `真实周期轴=0` 所以没设它，但迟早会踩。

**修法**：两处的 `TRACE_STATUS` / `TRACE_RECORD_RANGE` / `TRACE_CLOCK_SAMPLE` 全改成
**函数内 `static`**（只被 uVision 线程上的 TDI 回调与停止流程访问，不必加锁）。

**顺带加的日志**：`BOUNDS` 那行现在带"**请求的区间 + 驱动手里的范围 + 内存条数**" ——
这次就是靠它定位的，以后再出现一眼可见。

**验证**：探针环境 `ALL PASSED (41 checks)`。**真机复测仍空** ⇒ 它不是窗口空白的**唯一**
原因（见下一条：真正的原因是时钟报错）。这一条本身仍是真缺陷（`Notify` 之后还会读那两个
指针），已修并保留。

---

### Trace Data 窗口空 / Navigation 条 `0 s → 0 s` —— `nClock` 报的是 TRACE_CLK 而不是内核时钟（2026-10-13 真 uVision）

**这才是窗口空白的真正原因**（前两条修的是真实缺陷，但都不是它）。

**证据**（同一时刻的两组数，`ORB_AGDI.log`）：

```
TraceWin: 上报状态 … 真实周期轴=1 来源=ETM cycle count 时钟=72000000 Hz 锚点=有 -> rc=0
TraceWin: 显示区间 [1, 3825814] -> rc=0
TraceWin: [0] BOUNDS —— 请求 [5039998, 5738724]，驱动范围 [1, 3825817]（内存 3825817 条）
```

**算一下就知道差在哪**（目标内核 48 MHz）：

```
驱动范围   3,825,817 / 48 MHz = 0.0797 s   ← 周期号是内核周期
uVision 请求 5,738,724 / 72 MHz = 0.0797 s   ← 它按 72 MHz 换算
```

**同一个真实时间，两套周期数**，比值正是 `72/48`。

**定性**：`AG_TRACESETUP_REQ` 里把 `nClock` 报成了 **`traceClockKhz`（TRACE_CLK = 72 MHz）**，
而周期号（ETM 的 cycle count / ITM 时间戳）是**内核周期（48 MHz）**。uVision 拿 `nClock` 按
`time = cycle / nClock` 换算 ⇒ 它算出的周期数比驱动大 1.5 倍 ⇒ 它请求的区间整个落在驱动
范围之外 ⇒ 驱动按 TDI 语义回 `BOUNDS` ⇒ **窗口只有列没有行、Navigation 条范围恒为 0**
（uVision 收到 BOUNDS 后**不会**重新问范围）。

**修法**（`ORBAGDI_Agdi.cpp` 的 `AG_TRACESETUP_REQ`）：
- `tc->nClock` 与 `TraceWinSetCoreClock()` 改用 **`opt.cpuKhz`（内核时钟）** —— 这才是周期号的时基；
- `TRACE_CLK` 仍按 `traceClockKhz` 通过 `TraceWinRegisterClocks()` 单独报（那边要用它算 SWO 分频），
  与周期号无关；
- 内核时钟为 0 时退回 TRACE_CLK（不能报 0 —— 0 就是"没配置"）。

**⚠ 用户侧同样要对**：uVision **Trace 页的 Core Clock 必须是目标实际主频**（本板 48 MHz）。
那里若写 72 MHz，`cpuKhz` 就是 72 MHz，修完仍然对不上 —— 这正是 `Todo.md` §0.3 #2 的判据。

**验证**：探针环境 `ALL PASSED (41 checks)`。**真 uVision 待复测**（`Todo.md` §0.3 #10）。

---

### SWO 异步分频的基数（ACPR）：**H1/H2 未判别** + 一次改动与回退的复盘（2026-10-14）

**问题**：目标侧 `ACPR+1 = ? / baudHz` 里的"?"是哪个物理时钟 —— HCLK，还是独立的 TRACECLKIN。
我方两个量用两套表达：探针侧位率 `cfg::SwoBaud()`（`Config.cpp`，选择器在 `:1168`）=
`useCoreClock ? cpuKhz : traceClockKhz`（"有效 trace 端口时钟"，并口档也用它推
`SetSWOBaudrate`，`COMPAT_ANALYSIS.md:1194-1198`）；目标侧 ACPR 基数 = `cpuKhz`
（`ItmProgramTarget`：算式 `baseHz / setup.baudHz`、写 `MemWrite32(kTpiuAcpr, presc - 1u)`，
`Itm.cpp:1106-1112` / `:1117`）。两者在
`useCoreClock=0 && traceClockKhz != cpuKhz` 时不等 ⇒ **不可能同时对**。
官方把"探针率 / TRACE_CLK 时间轴 / ACPR 基数"塞进同一个 `TPIU_Clock`
（`AGDI.CPP:4247-4263 RegisterTraceClock()` 头注释即 "Register TPIU Clock to uVision (via
TRACE_CLK)"；`Trace.cpp:1963/:1979` 又拿它当 SWO 率与 ACPR 基数）—— **但官方内部另有旧支用
`TraceConf.Clk`**（`AGDI.CPP:4139`）⇒ 报告里别写"官方一律用 `TPIU_Clock`"。

**为什么源码/反汇编判不了**：两边都只证明"官方按**声明的** `TPIU_Clock` 算"，不证明硬件除的
是哪个时钟 —— 唯一判据是实机 A/B。

**两条器件假设**：
- **H1｜ACPR 的物理输入 = 真实 HCLK**：本板旁证 `COMPAT_ANALYSIS.md:1178-1180`（同步档位率
  就是 HCLK、同步档没有分频器、`TPI_ACPR` 只作用于异步 SWO）+ `:1144-1149`（`PE2=TRACECLK`
  是 MCU 的 AF **输出**脚、板上无独立低速源）⇒ H1 权重上调，**但都不是判据**（`:1178` 只覆盖
  同步/并行档）。H1 下：我方自洽（`cpuHz/(ACPR+1)` 恒 = 探针受理档），**反而官方**在 host 把
  `tclk` 填成 ≠ 真实 HCLK 时偏 `core/trace` 倍。
- **H2｜TRACECLKIN 独立**：我方现状偏 `cpu/trace` 倍（168 M/8 M ⇒ 21 倍）⇒ 静默无 ITM/
  乱码，而 `Connect()` 的 `swoBaud > coreHz` 检查（`Agdi.cpp`，注释 `:817-821`）拦不到
  "差整数倍"。

**过程：两次方向反复（改 → 回退）的复盘，这是本条的重点**
1. **第一次方向（改）**：2026-10-14 先按"两处不同源 ⇒ 单点同源"动手 —— 把 `SwoBaud()` 的
   被除数也改成 `SwoAcprBaseKhz()`，后者返回 `useCoreClock ? cpuKhz : traceClockKhz`。
2. **第二次方向（回退）**：随后**整体回退为"判别前不改行为"** —— `SwoAcprBaseKhz()` 现在
   `return c.cpuKhz;`（`Config.cpp:1148`，理由写在 `:1145-1147`），`SwoBaud()` 恢复自己的探针侧
   选择器（`Config.cpp:1168`，护栏 `:1164-1167`）⇒ 行为与改动前**逐位相同**。
**回退理由**：那个"同源"写法把目标侧的未判别问题绑到**探针语义**上；H1 成立时它正好是官方那个
耦合缺陷的形状（`AGDI.CPP:4247-4263 RegisterTraceClock()` 的"一个量三用"）。
**复盘教训（别再犯）**：① 把"**未判别的器件问题**"当成"**可改的代码错误**"是错的第一步
（判据没跑之前，正确动作只有"建单点 + 保持行为 + 记录判据"）；② 把**探针侧的量**与**目标侧的
基数**合并成"同源"是错的第二步 —— 看着像"两处同源"，实为把一个未判别的问题传播到另一个角色
（护栏句已写进 `Config.cpp:1165-1167`，防后人再合并）。
**责任归属**：两次方向反复都是 team-lead 的判断错，**不是复核方**（`uv4-semantics`/`re-exports`/
`our-gap`）**的错** —— 两轮复核都按当时的分工给出了正确结论（我们的分歧只在"状态：已实施 vs
未实施"，不在机制）。
**⚠ 中间态只存在于台账/消息里**：上一轮那种"`SwoBaud()` 改调 `SwoAcprBaseKhz()`、选择器只留
一份"的代码状态**已不存在于代码**；`Todo.md:920` 里描述它的旧版句子（"已实施"+
"防复发结构（必须保留）"）已由 team-lead 就地改写，**别再引用旧版**。凡引用"当年结论"必须带时点。
**引用约定（本条起用）**：台账里的 `文件:行号` 一律按**时点快照**读（同一晚 `Agdi.cpp`/`Config.h`/
`Config.cpp` 就各漂过 1-5 行）；新写或重写的条目改用**表达式/符号锚点**（如
`setup.acprBaseKhz = (acprBaseKhz > 0) ? acprBaseKhz : 72000`、`p_create("TRACE_CLK")`），
行号只作辅助、不作定位依据。
**第 2 款（机制性理由）**：同一文件里**任何一次插入/删除** ⇒ 该文件中**其下方**的引用
**按段整体移位**（实例 1：`Config.cpp:1125` 插 1 行 ⇒ `:1126` 之后每条 `Config.cpp:NNNN` 都 +1）。
⚠ **多次增删叠加后，位移是分段且方向可不同**（实例 2，同一张表实测：早期组 Δ=−2、晚期组 Δ=+1
⇒ 两组之间有一处 **+3 净插入**，其上方另有 **−2 净删除**）⇒ **任何单点成因（如"删了 3 行"）都
不足以解释，必须按段重算，不能对全文件整体加减**。那次按文件名批量推断出的 9 处"陈旧"，
**被引文件其实一行没漂**。
⇒ "批量替换"必须连带重算同文件**全部**更大号引用，只改被指名的那几处等于制造新错。
⇒ 反向判据：判定"引用失效"只能看**被引行的当前内容**，不能按文件名批量推断。

**已实施（未编译验证）**：
- 新增**单点** `cfg::SwoAcprBaseKhz()`（`Config.h:314` 声明，注释块 `:301-313` / `Config.cpp:1120` 起
  定义，当前 `return c.cpuKhz;` 在 `:1148`）= 唯一的目标侧基数来源；声明块与函数头都写明"判别
  实验登记在 `Todo.md`、判别后**只改这一处**的返回值"。
- `TraceSetup::cpuKhz` → `acprBaseKhz`（`Itm.h:58-61` 字段 / `:36-41` 形参文档；`Itm.cpp` 的算式 / 赋值 / 日志）；
  两个调用点同口径（`Agdi.cpp:905` `setup.acprBaseKhz = SwoAcprBaseKhz(opt)`；
  `:941 ItmStart(..., SwoAcprBaseKhz(opt))`）；日志
  `core=` → `acprbase=`（`Itm.cpp` 末段那行，`:1211`）。
- 口径注释同批改（否则后人按错误侧改回）：`Itm.cpp:47-49`、`Itm.cpp:1095-1099`、
  `Config.cpp:1537-1541`、`Config.h:90-92`。
- **未编译、未跑真机**。因现状 = 改动前算术，**任何配置下新旧行为逐位相同** ⇒ 本次改动**不需要**
  A/B 验收（A/B 解的是器件问题，不是这次改动的对错）。

**判据（唯一，一次实验）**：不给 `swo=`/`trbaud`（让 `SwoBaud()` 生效），A：`tclkc=1 swopresc=4`
（12 M）；B：`tclkc=0 tclk=8000 swopresc=4`（2 M）。**B 解不出 ⇒ H2**（基数应改为用户声明的
trace 时钟）；**B 也解出 ⇒ H1**（保持核心时钟基数）。零成本直证：读 `0xE0040010`（`kTpiuAcpr`）
记录我们写了什么 —— A 档 = `3`；B 档**当前也是 `23`**（现状按 `cpuHz` 算）；读值**不判归属**。
⚠ 早前"改后 B 应写 `3`、`ACPR+1 ≡ swopresc`"的预测**已作废**（回退前那版算术的产物）。

**残留（非本次引入，未改）**：`ItmProgramTarget` 在 `acprBaseKhz <= 0` 时 `presc` 停在 1
⇒ 写 `ACPR = 0` ⇒ 目标按基数全速发、探针按 `baud` 收 ⇒ 静默乱码。可达路径：orbtrace 档给了
`trbaud=` 而 `cpu=` 未填（`Agdi.cpp:905` 传 `SwoAcprBaseKhz()`，但**没有** `ItmStart` 那个
`setup.acprBaseKhz = (acprBaseKhz > 0) ? acprBaseKhz : 72000`（`Itm.cpp:1190`））⇒ 两个调用点
**兜底不对称**。⚠ `72000` 只是"调用方什么都没给"时的兜底（`Itm.cpp:1188-1189` 明写了这句），
它恰好 **=** `Config.cpp:25` 的 `kDefaultCpu`，但**不是**"默认时钟值"，别写成"72000 是默认时钟"。
建议兜底收敛到一处，或
`baudHz > 0 && acprBaseKhz <= 0` 时 `WARN` + `return false`。

**唯一残留行为差（退化路径，非回归；量级已定 35 vs 0；未改代码）**：`cpu=0`（或负值）下，
`swo=2000000 cpu=0` 让两条路写**不同**的 ACPR：
- `ItmStart` 路：显式 `swo=` 使 `baudHz > 0`、控制台照常启动 ⇒ 走兜底
  `setup.acprBaseKhz = (acprBaseKhz > 0) ? acprBaseKhz : 72000`（`Itm.cpp:1190`）⇒ `72000*1000/2M`
  ⇒ **ACPR = 35**。
- orbtrace/并口路：**无兜底**，`acprBaseKhz > 0` 守卫失败 ⇒ `presc` 停在 1 ⇒ **ACPR = 0**。
⇒ 是 **35 vs 0**，不是舍入差。（该配置下 `SwoBaud()` 因 `clkKhz <= 0` 返 0，是被显式 `swo=` 绕过的
—— `Config.cpp:1169`。）
- **可达，但入口只有一条**：选项串的裸 `atoi` —— `Config.cpp:1080-1082` `c.cpuKhz = atoi(val);`
  （`cpu=0`/`cpu=-1` **无下限校验**）；且 `Config.cpp:1210` 只在 `cpuKhz != kDefaultCpu` 时序列化
  ⇒ `0 != 72000`，会被写回、**可复现**。
- **第二写点存在但不产生 0**：`ORBAGDI_TraceWin.cpp` 的 `wrCoreClk()` 是活动配置的第二写点
  （直写 `c.cpuKhz = khz;`），但赋值之前有 `if (hz < 1000u) return;`（`const uint32_t hz =
  g_coreClk->v.ul;`，注释"0 / 异常值：uVision 建表时会读一次"）⇒ VTR 写 0 **到不了赋值**；
  该守卫是**有意偏离**官方 `if (==0) 值=1`（注释明写"别按官方修回去"；`wrTraceClk` 同款口径）。
  **运行中还有一条**：
  `TraceWinIsRunning()` 分支先按 `cpuKhz*1000` 回写、直接 `return` ⇒ **两条守卫都在赋值之前**。
  ⇒ 别写成"VTR 直写也能产生 0"（删掉第二写点也不行 —— 会掩盖"`cpuKhz` 有两个写点"这个事实）。
- **零成本判别**：该配置下读 `0xE0040010`（`kTpiuAcpr`）：`35` = 走了 `ItmStart` 路；`0` = 走了另一条。
- **不作 bug 定性**：`cpu=0` 是用户显式给了不可能的值，两路都不可能得到正确位率；`kDefaultCpu`
  本就是 72000（`Config.cpp:25`），72 MHz 器件下兜底 72000 反而更对。判别前**不改代码**。

**邻接项**：`Connect()` 的 kHz/ns 合理性检查（`Agdi.cpp`，注释 `:817-821`）**不动** —— 它防的是单位混用
（差 10^6），与"差整数倍"无关；严格说其参照应是"有效 trace 时钟"而非 core（`traceClock<core`
时偏松、漏报），但目的不受影响。

**三条必记**：① **未编译、未跑真机**；② 现状与改动前**逐位相同**（H1 下不给 H1 引入偏差，
H2 下也没修好 —— 等实测）；③ 早前写进 `ItmProgramTarget()` SWO 分支上方那条
"**有意保留的行为差异 / 别改成跟随 `tclk`**"注释**是错的、已删除**，全仓不得再引用它。

**已撤回、禁止再用的措辞**：① "我方更自洽"；② "官方会把目标位率配成 `TPIU_Clock/(pre+1)`、
与探针受理档错位"（官方 `SWV_Check` 自动探测会收到探针能收为止，`Trace.cpp:1544-1561`）；
③ "本次改动把 `ACPR+1` 变成 ≡ `swopresc`"（回退前那版的产物）。

**（文件位置说明）**：本文件头部写"能在代码里改的进 `Todo.md`"，但本文件既有的同类记录都是
`（已修）` 条目；本条按 team-lead 裁定落在这里，**待办部分在 `Todo.md` §待测试**（"ACPR 基数：
H1/H2 未判别，待一次实机 A/B"）。头部那句话与新记录口径不一致，将来若有人整理，以"过程/定性进
`bug.md`、待办进 `Todo.md`"为准。

---

### 跨断点 Run 后 `DHCSR.C_MASKINTS` 无人清 ⇒ 目标带中断屏蔽运行（已修，2026-10-19）

**现场**：目标停在断点上，再 Run（`ORBAGDI_Agdi.cpp` 的 `RunAndWait()` 里
`GoStep: breakpoint at 0x... - stepping past it...` 那条路）之后，目标不再命中断点；
固件的 `main` 在等 SysTick 的 tick，而 SysTick 一次都不进 ⇒ 自旋。

**根因（两段，缺一不可）**：

1. `Core::Step()` 按 Keil 的序列两次写 DHCSR 都带 `C_MASKINTS`
   （`armed = DBGKEY|C_DEBUGEN|C_HALT|C_MASKINTS`、`step = DBGKEY|C_DEBUGEN|C_STEP|C_MASKINTS`），
   而**没有任何一处清它** —— `C_MASKINTS` 不会自己掉。
2. `Core::Resume()` 写的是 `DBGKEY|C_DEBUGEN`（`C_HALT=0`）。ARMv7-M 规定
   `C_MASKINTS` **只能在 `C_HALT=1` 的同一次写里改变**，`S_HALT` 之外那次写被硬件忽略，
   所以"Resume 会把它写成 0"是错的 —— 掩码原样活过了 Resume。
   ⇒ 目标以"中断全屏蔽"运行：SysTick/外设中断不响应，等 tick 的循环永不退出。

**修法**（`ORBAGDI_Core.h/.cpp`）：

- 新增 `Core::ClearStepMaskInts()`：先读 DHCSR（`0xE0000000..0xE00FFFFF` 不参与读缓存，
  是真读）；`C_STEP|C_MASKINTS` 都没置时直接返回（Resume 在 Flash 下载里要跑几百次，
  省两次 MEM-AP 往返）；否则**读改写**为 `DBGKEY|C_DEBUGEN|C_HALT`（保住 halt、两位都不写），
  然后**读回校验** `C_MASKINTS` 已清。
- `Step()` 尾：`WaitForHalt` 成功后调用并校验；失败打 ERROR 并返回 false（不许把
  "halt 着但中断还被屏蔽"当成功交回去）。`WaitForHalt` 超时那条失败路径也补一次尽力清，
  避免失败路径把掩码留在目标上。
- `Resume()` 头：先调一次，失败则 **拒绝运行** 并打
  `run refused - C_MASKINTS could not be cleared`，不再"带着掩码起跑"。
  原那次 `C_HALT=0` 的写保留不动（它本来就是"起跑"语义）。

**验证**：`.\build.ps1` 通过，`ORB_AGDI.dll` 6,791,680 B，`ORBAGDI_Core.cpp` 0 warning
（其余 warning 是既有的 `ATR_*` 宏重定义与 `Config.cpp:140` 收窄转换）。**未上机**。

---

### 整片擦除报 `did not stop at the sentinel (Init fnc=1)`：`Init(1)` 被连调两次（已修，2026-10-19）

**现场**：Flash→Erase（整片擦除）失败，错误为
`flash algorithm did not stop at the sentinel (Init fnc=1)` —— 算法没跑到 BKPT 哨兵。
**不是**链路/复位问题：同一算法在 Download 的擦除 pass 里跑得动。

**根因**：`Init(1)` 在一次会话里被发了**两次**，第二次是对一个**已经处于擦除模式**的目标：

1. `FlashInitLoad()`（`AG_INITFLASHLOAD`）在装载完 FLM 后直接 `CallAlg(Init, DevAdr, clk, 1)`
   —— 即官方的 `LoadFlashDevAlg()` 里**没有**的那一步。
2. `FlashEraseChip()` 自己又调 `FlashInitFnc(1)`。

官方三条路径都是「`LoadFlashDevAlg` 只装载 + 每个 pass 只 `Init` 一次 + 收尾 `UnInit(fnc)`」：
`EraseFull`/`EraseAll` = `Init(1)..UnInit(1)`（`DbgCM/Flash.cpp:1249/1266`）、
`WriteAll` = `Init(2)..UnInit(2)`（`:1459/:1483`）、`VerifyAll` = `Init(3)`（`:1523`）。
另外装载期的这次 `Init(1)` 还留下一个**不平账**：`FlashStartLoad()` 的擦除 pass 只调
`FlashUnInitFnc(1)` 而不调 `Init(1)`（它以为装载时已经 Init 过），所以「UnInit 没有对应的 Init」；
"Download to Flash off" 的纯 RAM 路径同样会去调算法的 `UnInit`，而那次 Init 从没被打开过。

**修法**（`ORBAGDI_Flash.cpp`）：

- `FlashInitLoad()` 去掉 `Init(1)`，**只装载**（保留 `orb::pdsc::Init()` 这层 PDSC 钩子）。
- `FlashStartLoad()` 的 `doErase` pass 开头补显式 `FlashInitFnc(1)`，与结尾的 `FlashUnInitFnc(1)` 配平。
- `doProg` 的 `FlashInitFnc(2)`、`doVerify` 的 `FlashInitFnc(3)`、`FlashEraseChip()` 的
  `FlashInitFnc(1)` 全部保留 —— 每个 pass 各一次，各自与同 fnc 的 `UnInit` 配对。
- 新增 `s_algInitDone`：`FlashInitFnc()` 成功后置位，`FlashUnInitFnc()` 只在置位时下发并清位。
  `FlashUnInit()`（`AG_UNINIT`）因此只对"某个 pass 开了 Init 却没关"（错误路径 / 取消）补发 `UnInit(0)`，
  正常下载收尾时不再多发一次。
- 新增 `FlashUnInitRam()`：`!downloadToFlash` 的纯 RAM 写只关会话状态（`pdsc::Uninit`、清
  `s_pdscSeq`/`s_algInitDone`/`s_alg.ok`），**不碰算法 UnInit**。

**验证**：`.\build.ps1` 通过，`ORBAGDI.dll` 6,791,680 B，`ORBAGDI_Flash.cpp` 只剩既有的 `ATR_*`
宏重定义 warning。**未上机**：判据是 Flash→Erase 一次 `Init(1)` 即成、日志里不再出现
`did not stop at the sentinel (Init fnc=1)`；Download 的 pass 序列应为
`Init(1)…UnInit(1)` / `Init(2)…UnInit(2)` / `Init(3)…UnInit(3)`。
