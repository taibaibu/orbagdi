<h1 style="color:red">🔴🔴🔴 最高警告（MAX） 🔴🔴🔴</h1>

<h1 style="color:red">任何偏离 <code>AGDI\DbgCM</code> 实现方法的实现皆为风险。</h1>

<h1 style="color:red">PDSC 是 KEIL 核心流程，严禁硬编码，软开关；功能应做就做，行到行兼容。</h1>

---

# Todo — ORBAGDI 未完成项

**只列待办**（`- [ ]` = 未完成）。已完成事项、实现细节、自查记录、缺陷过程、逆向过程与实测数字
全部在 `COMPAT_ANALYSIS.md` **§11（原 `Todo.md` 全文快照，2026-10-15 冻结）**；
AGDI 逆向 / ABI 结论在 `COMPAT_ANALYSIS.md` §1–§10。

- 每条尽量带**判据**（日志锚点 / 现象）与**出处**（`§11 → 小节名`）。旧编号（`§0.x`、`§一`…`§十六`）
  是归档节里的编号，需要细节就去 §11 找同名小节。
- 上机命令、日志锚点、判据表模板见 `§11 → 0.4 / 0.5`。

---

## 0. 🔬 实机验证清单（只能在真 uVision 里验）

> 探针（`ORBAGDI_AgdiProbe`，假 BOM 宿主）能验的已全部验过并记在 §11。上机前先 `.\build.ps1 -Deploy`。

| # | 验什么 | 判据（日志锚点 / 现象） | 不通过查哪里 |
|---|---|---|---|
| 1 | `DBGMCU_CR` 是否真走 PDSC | 出现 `ITM: DBGMCU_CR 由器件描述的 TraceStart 序列负责，本层不写`；**不再**出现 `ITM: DBGMCU_CR 写 0x…` | 仍出现"写 0x…" ⇒ PDSC 属性没下来，查 `orb::pdsc::Refresh()` 与 Pack 页 Enable |
| 2 | 核心时钟来自 Trace 页 | 在 uVision Trace 页改 Core Clock ⇒ `TraceWin: uVision Trace 页把 CORE_CLK 改为 <N> Hz`；SWO 位时钟按新值重算 | 没这行 ⇒ `CORE_CLK` VTR 没注册成功（`TraceWinRegisterClocks`） |
| 3 | ETM 基址 | 本板应仍是 `ROM 表未列出 ETM，用 ARMv7-M 定义的 0xE0041000`；**换非 F4 器件**应变 `来自 ROM 表` | 换器件后仍兜底 ⇒ 看 `ETB: ROM 组件 … 未识别 (PID=… PN=…)`，补 PN 表 |
| 4 | 时间列（`tswin`） | 打开后 Trace Data 出现真实时间列，Navigation 条时间与墙钟对得上 | ⚠ 打开前看 `§11 → 10.20.1`：`bHideTimestamps` 同时是取数门的一环，窗口空白就关回去 |
| 5 | 探针锁等待语义（P0-A′） | 有 trace 源在跑时，寄存器/内存窗口**不出现**间歇性 `AG_NOACCESS` | 仍出现 ⇒ `LockProbe` 的 250 ms 上限被长持有者顶掉（Flash 下载期间会） |
| 6 | Trace / Performance Analyzer 窗口 | 有数据 | — |
| 7 | A1–A5 其余实机项 | 观察点命中、SWO printf | — |
| 8 | Flash 路径与 worker 抢锁 | 下载期间**不出现** `Flash: … worker` 告警；IDE 不僵住 | 仍僵住 ⇒ `PumpQuiesce`（已改成"拿一次锁确认就放开"） |
| 9 | 拖 Navigation 条回看（P1-1/P1-2/P2-2） | 见 §0.2 | — |
| 10 | Trace Data 有没有行 / Navigation 条有没有范围 | 采集一段后停止：窗口出现记录行、Navigation 范围**不是** `0 s → 0 s` 且可拖动；`TraceWin: [0] BOUNDS` **不再出现** | 仍空 ⇒ 看 `BOUNDS —— 请求 [...]，驱动范围 [...]`（两区间不重叠 = uVision 侧范围没更新） |
| 11 | 记录窗口是否覆盖**整段**（P0，见 §4.3） | `buf(...)` 里 `条数上限=0`、`上限裁掉=0 条`，且 `本段起点=` 与 `[1] GetFirstCycle ->` 报的值一致；`trrec=0` 时拖最左端也画得出数据 | `条数上限` 非 0 ⇒ 选项串里还留着 `trmax=`；`上限裁掉>0` ⇒ 条数上限又被打开了 |
| 12 | **抽数 / 解码瓶颈的分解**（P0 门，见 §3.1 / `§17`；采集 ≥ 30 s 再停） | 停止后出现 `TracePump: 每轮耗时 平均 … ms（等待 … / 抽数 … / 解码 … / 状态折叠 …）最大 … ms（… 轮）`，照 `ORBAGDI_TracePump.cpp:375-381` 的四分法落点定案：**解码占大头 ⇒ 做 §3.1**（解码挪出抽数线程）；**等待占大头 ⇒ 别改架构**，查轮询/链路；**几段都小、平均却大 ⇒ 时间在计时之外**（拿锁、读探针）。配 `orbtrace: 环溢出丢帧=…B` 看缺段量 | **没有这一行** ⇒ 连 `TracePump: 抽数工作线程已停止 (轮=…)` 也没有：worker 没起或停止日志被截断；`探针忙跳过=` 占比大 ⇒ 锁竞争，先看第 5 行；`解码` 占比大但 `ScanTo` 没在跑 ⇒ 看 `InstrScan: 补出=… 扫描失败=…` |

### 0.2 拖 Navigation 条回看（P1-1 / P1-2 / P2-2）

**前置**：`bin\ORB_AGDI.dll` 已 `-Deploy`（改过驱动就要重新部署）；固件 = 4 位并行；
选项**必须**写 `trrec=1` 并把内存压小到装不下整段（`trchunk=1 trmem=2`，否则永远没有"文件段"）；
Trace 页使能打开；目标**跑 ≥ 10 s**。

| # | 动作 | 判据 | 项 |
|---|---|---|---|
| 1 | 等 Trace Data 出现记录 | `TraceWin: 记录缓冲 = 块链 …` + `TraceWin: buf(...) 块峰值=k/n` 里 **n > 1** | P1-1 |
| 2 | 拖到**最左（最早）** | `TraceWin: [0] 记录来源 —— 文件段 N 条 / 内存段 M 条` 且 **N > 0**；窗口左端**画得出**那一段 | P2-2 |
| 3 | 拖回**最右（最新）** | 同一条日志变 **文件段 0 条**（全在内存）；刷新 < 数十 ms | P1-1/P1-2 |
| 4 | 左右来回快速拖 | 内容稳定、不闪变；同区间重复取数结果一致 | P1-2 |
| 5 | 会话结束（Disconnect） | `TraceWin: buf(...) 常驻=N 条` 里 **N > 131072**；`TraceFile(...): 记录日志 … 字节，索引 K 条 [周期 a..b]` | P1-1/P2-2 |

**不通过时查哪里**：拖到最左**空白** ⇒ `GetFirstCycle` 没把记录日志那段算进去，或 `trrec=1` 没生效
（看 `TraceFile(…): 无记录日志（选项 trrec= 没开）`）；拖条时**一行都不取** ⇒ `§11 → 10.20.1` 的
`bHideTimestamps` 取数门；`块峰值=1/1` ⇒ `trmem` 太小，退化成单块环、历史被丢。
**P1-2 还原正确性**：拖到最右端时最后一条记录应落在时间轴末端（不是差一大截）。

### 0.3 置后：`trwidth` / 编码的**全档位**回归

- [ ] 固件按档各烧一版（并行 1/2/4 位 + SWO NRZ + SWO Manch，共 5 版），每版跑
      `powershell -File test\trace_matrix.ps1 -SweepAll -FwKind <parallel|swo|manch> -FwBits <n>`；
      判据 = 同档时 `drop% < 10%`。**只改驱动 `trwidth` 而固件不动，样本无效**
      （那是"错位长什么样"，见 `§11 → 0.8`）。现在钉住的只有"必须同档"这一条。

---

## 1. Flash 下载与连接（实机复测）

- [ ] **§E14 重跑一次 Download**：确认"取镜像块协议"修复后不再冻结，日志按
      `Flash: Erase pass` → `Flash: param ...` → `Flash: erase 0x...` → `Flash: call ...`
      正常推进到 `Flash: download finished OK`。
- [ ] **偶发中断待复现**：11 次里 1 次在 `0x08007C00` 页中断（"core was not halted" +
      "halt did not confirm"）。放大它的 `Core::Halt()` 谎报已修；**底层原因未复现** ——
      下次复现看 `0x200000DF` 前那两行日志（看门狗与读缓存均已排除）。
- [ ] **Flash 下载在真 uVision 里的 UI 侧**：不打开任何设置页直接 Download，日志应出现
      `Cfg: pack root = ...`、`Cfg: auto-detected flash algorithm = ...`、`Flash: FLM = ...`，
      且不再有 `cannot parse '$$Device:...'`；再确认进度条与复位收尾。
- [ ] **若出现擦除/编程超时**：`dbgblk::Clock` 给的是晶振（12 MHz）而非内核频率
      （F407 实际 168 MHz），需从 PLL 设置推算（`§11 → §E9 末`）。
- [ ] **P1-1（块链）+ P1-2（紧凑记录）在真 uVision 里的验收**（驱动侧已用假 BOM 宿主实测完，
      剩"窗口能不能按时间浏览"这一眼）—— 判据见 §0.2。⚠ 这条**不需要** `tswin`。
- [ ] **`trwidth=1` 的决策**：根因已定性（id 位把整条流错位，`§11 → 1. 实机复测` 那条），
      驱动现在**只告警**。要不要在连接时**直接拒绝**并说明？"不认识的 tag 只计数不解码"照现状即可。
- [ ] **⚠ 周期轴偶发跳到 1e10 量级（待查）**：实测 `[188241103, 200212836034]`（2.0e11 周期 /
      1098 条记录）、`[143355950, 43260454637]`。**不是 P1-1/P1-2 引入的**。要查
      `TraceWinEtmTime()` 的绝对量（ETM 时间戳 / 周期计数）或 ITM 时间戳乘 `tspresc` 那一步。
      现场证据：`TraceWin: cycle axis(...) source=… cycles=… records=…`。
- [ ] **Trace Funnel（CSTF）—— ETM 侧与参考的最后一个实质缺口**：官方在 `Trace_Setup()`
      **之前**跑 `CSTF_Setup()` 做拓扑检测、选漏斗从端口（`DbgCM/CSTF.cpp:536`）。本驱动**没有**。
      单源无影响；**ETM + ITM 同时汇入 TPIU 时**从端口没配。见 `§11 → 10.28 六`。
- [ ] **A1–A5 其余实机项**：观察点命中、SWO printf、Trace / Performance Analyzer 窗口是否有数据。
- [ ] **整片擦除：`Init(1)` 只发一次（2026-10-19 已改，待真机）**：`FlashInitLoad()` 不再跑
      `Init(1)`，改由擦除 pass / `FlashEraseChip()` 各发一次。判据：Flash→Erase 一次 `Init(1)`
      即成，日志不再出现 `did not stop at the sentinel (Init fnc=1)`；Download 的 pass 序列应是
      `Init(1)…UnInit(1)` / `Init(2)…UnInit(2)` / `Init(3)…UnInit(3)`，且 "Download to Flash off"
      的纯 RAM 写不再有算法 `UnInit` 调用。定性见 `bug.md`。

## 2. 健壮性

- [ ] **§A4 运行中内存访问（`AG_F_MEMACCR`）策略未定**。

## 3. Trace（采集 / 解码 / 窗口）

- [ ] **Exception Trace 窗口**（`AG_CB_EXTRCDATA` / `AG_F_EXCTRC`）与 **Event Counters**
      （`TR_TYPE_EVT` / `TR_EVT_*`）**尚未产出记录** —— Trace 窗口的过滤器已预留这两类。
- [ ] **时间列显示仍是关的**（`bHideTimestamps`，由 `tswin` 控制，默认 0）。原因见 §0 第 4 行。
      **真实时间轴不依赖它**。要开就上机开、盯着窗口看。
- [ ] **`ProgramTraceOff()` 没停 ETM**：每轮跑完后目标仍在全速灌指令 trace。
      应补 `ETMCR` 的 bit10 置位（`0x191E`）。
- [ ] **选项优先级冲突**（同时给 `orbtrace=`/`etb=`/`swo=`）目前是静默按优先级取值，尚未补 WARN。
- [ ] **Manchester 仍未测**（并口 1/2/4 位与 SWO NRZ 已过）。
- [ ] **ETB 复测**：需一块带 ETB/TMC 的目标。
- [ ] **SWO 复测**：Trace / printf / Performance Analyzer 窗口是否有数据。
- [ ] **`0x500` 未决（不许按下去）**：`RegAcc: register id 0x500 is not mapped -> AG_INVALOP`。
      要查：① `AR_n*` 之外还有没有别的 id 段（`0x500` 可能是**分组 id**）；② uVision 在**什么时机**
      问它；③ 先把 **`nCode` 也记进日志**（现在只记 `nReg`），下次上机拿完整现场。
      **警告是"我不知道的东西"，不是噪音** —— 查清它是什么再决定答什么。见 `§11 → 续三`。
- [ ] **P1-0 路径重建待修（三条，按性价比排序）**：① **异常入口分支不要走路径重建**（最该做，
      解码器知道异常包，直接采用其目标地址）；② **镜像范围收窄**（只信"当前下载镜像覆盖的范围"，
      别拿残留字节当代码）；③ 若确认是残留字节，**P1-0 降到 INF** 或加大节流。
      见 `§11 → 十 10.3`。
- [ ] **CPI 事件计数待查（用户点名）**：① "**2 周期/事件、纹丝不动的节拍**"从哪来（真实负载不该这么规整；
      候选：探针/demux 侧重复计数、ITM 包解了两遍、`CPICNT` 语义理解有误）；② **是否按官方那样显式写
      那 5 个剖析计数器**（清零/给重载）。（PC 采样默认值与 `pcTap=` 骗人的日志已修。）见 `§11 → 十 10.4`。
- [ ] **待观察（影响不大）**：Trace Data 纵向滚动条拉到底会超出时间轴分片，Navigation 条上有提示。
      若与"P4 查询加速"的边界逻辑有关，一并看。见 `§11 → 十 10.5`。
- [ ] **第三阶段：raw 级诊断工具** —— 用 `orbrddi-main\test` 那套 + `orbdump` 的原始字节落盘，
      验证"解码器/反汇编器真实效果"（`§11 → 九 9.6`）。

### 3.1 抽 / 解 **再**分离：把解码挪出抽数线程（**待用户裁定**）

> **与 `§11 → 16.8 线程架构：结论（别再重开）` 的关系**：那节裁定过"四线程映射不成立、
> **采集并行：不做**"，但同时点出**唯一真收益点是 `FillGap` 展开**，并给了两条路 ——
> **首选**"展开延后到查询侧（带缓存的视图）"、**次选**"展开/预取 worker（前提：**顺序令牌 +
> 有界队列**）"。**本节 = 那次选**，不推翻 16.8，是把它的"前提"逐条落实。
> **细节（证据链 / 代码地图 / 四个陷阱 / 验收判据 / 接手清单）在 `COMPAT_ANALYSIS.md §17`**，
> 本节只留待办。

**为什么现在提**（现场）：ETM + 4 位并行，`orbtrace: RDDI 环溢出` 连打 39 s / 丢 44 MB，
**环长期满**（每 ~0.9 s 丢 ~1.5 MB）⇒ 消费者**稳态速率 < 生产者**。丢在宿主侧（32 MB 环 =
32 × 64 KB 常驻 overlapped 读），粒度不可控。

- [ ] **G0 门（先做，不改任何代码）**：打一次停止日志拿到
      `TracePump: 每轮耗时 平均 …（等待 … / 抽数 … / 解码 … / 状态折叠 …）最大 …`。
      判据就是 `ORBAGDI_TracePump.cpp:375-381` 自己写死的四分法：**解码占大头 ⇒ 做本节**；
      **等待占大头 ⇒ 是轮询/链路问题，别改架构**（16.8 原话）。
      ⚠ 这条日志**至今没拿到过**（上一轮台架 `端点全程为空`、`s.count == 0`）。

**设计要点**（不是"再起一条线程"这么简单）：

1. **单解码线程，不并行解码。** `EtmTraceFeed` 全程持 `Sink::cs`（`ORBAGDI_Etm.cpp:468-475`），
   解码器有状态（同步 / 地址重建 / 推测），按块并行要 checkpoint + 重放 ⇒ **不做**（16.8）。
   `FillGap` 的 `static ScanBuf`（`Etm.cpp:205`）**安全性正来自这把锁** ⇒ 队列化后必须**保持
   同一把锁覆盖"一整段解码"**，否则那句"只在抽数 worker 线程上用"的注释就成了假的。
2. **有界队列 + 顺序令牌。** 抽线程只 `OrbTracePullRaw` → 入队 → 归还；解码线程 FIFO 出队。
   槽沿用 `kStageBytes`（256 KB），槽数 N 决定队列大小（初值 64 槽 = 16 MB，**要可配**）。
   `s_stage`（单缓冲，`:65-72` 自己写着"**只有 worker 用它**"）必须变成 N 槽 + 明确的
   **所有权协议**（抽填 → 解读 → 归还）——**这是本次最容易出错的地方**。
3. **⚠ 停机收尾会插队（现存缺陷，队列化会把它放大）。** `OrbTracePump(c.dap, 5)` 在**调试线程**上
   自己解码（`ORBAGDI_Agdi.cpp:1680`、`:3266`）。它与解码线程靠 `Sink::cs` 互斥，
   **正确、但会乱序** —— 那一小段插到积压**之前**。今天窗口只有一次 decode 那么宽（可忽略），
   队列化后变成**整个积压** ⇒ **这条必须一起修**：停机收尾改成"同步入队 + 等排空到自己这一段"。
4. **挂起 / 停机的顺序要重新规定。** 现有 `TracePumpSuspend`（Flash 的 `PumpQuiesce`，
   `ORBAGDI_Flash.cpp:1679-1695`）与 `TracePumpStop`。队列化后必须是：
   **先停抽 → 排空解 → 再打汇总**。`OrbTraceStop` 里那两行（`orbtrace: 解码结果 …`、
   `EtmTraceLogSummary`）在解码线程排空前打出来，**计数就是错的**。
5. **丢队 = 缺口，必须走同一条上报路。** 队列满时的丢弃**不许静默**：与 RDDI 环溢出同源
   （`TraceWinErrorSet(kTErrDataOvf)` + `EtmTraceForceSync(false)`），并**单独计数**
   （拟 `队列丢弃=… 槽/… MB`）。否则等于把"宿主侧不可控丢失"换成"**本层静默丢失**"——
   正是 §4.3 定性过的那种错。
6. **不提升解码速率。** `FillGap`/capstone 的算力一点没变 ⇒ **稳态带宽不足时照样丢**，
   只是丢在本层、有账。真正收益只有三条：① 环不再溢出（突发被队列吸收，宿主侧不再丢）；
   ② 抽数线程轮次变短可预测 ⇒ Stop 响应与锁交接更好；③ 缺失从"不可控"变"可计量"。
7. **抽数循环的节奏要重算**：`2 ms 片 / 20 ms 预算 / 2 ms 周期`（`kSliceMs`/`kBudgetMs`/`kPeriodMs`）
   有一半是**给解码留的**（"拿下锁再解"那段的历史包袱）。解码搬走后这套数要按新职责重新定，
   只保留"锁持有 ≤ 一片"这一条硬约束。

**判据 / 验收**：

- [ ] **复现现场**（ETM + 4 位并行，目标跑 ≥ 30 s）：`orbtrace: RDDI 环溢出` **应完全消失**；
      代之以队列水位（拟 `TracePump: 队列积压 峰值=… MB 均值=…`）；`环溢出丢帧=0 B`。
- [ ] **解码线程必须有独立计时**（照 `s_tDecode` 的形状）：拟
      `解码线程: 平均 … ms/槽 繁忙度 …%`。**没有这个数就不许说"够用"。**
- [ ] **顺序**：把解码线程**人为拖慢**（调试期插 `Sleep`），Trace 窗口的记录仍严格时间递增，
      且"补出的指令先于其分支记录入轴"这条顺序约束（16.8）仍成立。
- [ ] **停机一致**：`TraceWin: buf(...)` / `EtmTraceLogSummary` 的计数与"解码线程已排空"对得上
      （**不允许**出现"汇总 0 条而窗口有数据"）。
- [ ] **不回归**：`test\run-tests.ps1` 23 项全过；Flash 下载期间 `PumpQuiesce` 仍能拿到探针。

**待用户裁定**：

- [ ] **A：次选（本节）还是首选（`FillGap` 展开延后到查询侧）？** 16.8 的倾向是**首选** ——
      展开的产物是**派生数据、不回流解码器**，能把 CPU 真正从热路径上摘下来。次选的前提
      （1–4 条）**做了不白做**：首选的队列结构与之同形。
- [ ] **B：队列满了丢哪头？** 丢**最新**（跟实时走，中间断）还是丢**最旧**（窗口停住，历史连续）？
      实时调试一般选前者。
- [ ] **C：是否顺带做 P1-0 的三条**（§3 上一条）。解码算力不降，本节只是**把账做实**；
      两者叠加才有"跟得上"的可能。

## 4. 记录缓冲 / 落盘（对官方三层结构）

官方是**字节环 + 4 槽描述符环 + 抽数/落盘交接**三层；本驱动**三层全缺**（只有 worker 直接
`fwrite` 或直接挂内存块链），所以"写盘就卡抽数、内存到顶就丢历史"。

- [ ] **④ `UpdateTraceBuffer` 的语义**：现在只是个 refresh 钩子（重排空一次源）。官方是
      "**按区间把缓冲装满，单次最多 1,000,000 条**" —— 这是 uVision 点播历史的入口，也是"无限"的机制。
      要实现它，缓冲区必须能被**区间点播**（现在只能追加）。**未做**。
- [ ] **⑤ 索引**：官方是 `.tdi` 盘上文件（24 B/项、1024 项分页读）。本驱动是内存 `Chunk** idx`。
      **是否跟**要等 ④ 定下来再决定 —— 若历史主要在盘上，内存索引就没意义了。**未做**。
- [ ] **⑥ 块参数**：官方每块 **32 KB**（`0x8000`）/ 本驱动 **4 MB**。**未跟**。
      **建议保持 4 MB**（`kCkptPerChunk` 的检查点密度、索引数组规模都按它算；官方那 32 KB 是配它
      自己 4 槽交接环的量级，比例关系一致）—— **但要用户拍**。
- [ ] **按三层结构重构（架构性，需用户定）**：改动很大，不是"改几个常量"。见上面 ④⑤⑥。
- [ ] **全仓统一锁名（待用户点头）**：`ORBAGDI_Log.cpp` / `ORBAGDI_Ui.cpp` 的 `g_cs`、
      `ORBAGDI_TraceFile.cpp` 的 `s_cs`、`ORBAGDI_Itm.cpp` 的 `cs` → `xxxLock`。4 个文件的**纯改名**
      diff，⚠ **绝不和功能改动混在一起**（单独一次、只改名、不碰逻辑）。**没点头之前不动。**
- [ ] **文件回放段（P2-2 记录日志）没有游标**：`overflow` 发生在文件回放里时每页仍会重走文件。
      （用户当前会话 `文件段 0`，先不动。）
- [ ] **过滤器不全通过时**：游标能兜住"顺序翻页"，但随机访问（跳着取）仍是 O(区间)。
      要彻底解决得像官方那样按序号建独立索引（4 MB 级），**不建议现在做**。
- [ ] **`上限 0`（只问数量）仍然是全区间扫描**：语义要求的（要精确总数），官方也一样。

### 4.1 只问条数的"索引快路"—— **从未被执行过，别当已修**

- [ ] **待真机验**：`TraceWin: [0] 只问条数 —— 索引快路 N ms，区间 [lo, hi] ⇒ X 条（块索引 K 块 …）`
      （限流，每 16 次一条）。判据：**X 必须等于同区间的 `命中` / `常驻记录=`**（两条独立记账对上 ⇒
      边界块没数错也没重数）；**N 应是 1–5 ms**（老路 266–1453 ms）。上一次台架**一个字节都没采到**
      （`orbtrace: 端点全程为空`），`s.count == 0` 时快路根本不进 ⇒ 该项与改动无关。
      ⚠ 一处**故意的语义变更**：区间落在缓冲跨度内却一条记录都没有时，快路回 `EMPTY`（老路回 `BOUNDS`）。

### 4.2 打开的问题（**取舍待用户裁定**）

### 4.3 🔴 P0 记录条数硬上限**静默**吃掉整段历史（2026-10-15，已改，待上机复验）

**现场**：ETM + 4 位并行，用户"我就采集了 10 s"，回来发现只有最后一小段能拖出来。

判据 = 停止时那行 `TraceWin: buf(orbtrace) …`（原文照抄）：

```
块峰值=138/255（每块 4 MB）记录峰值=1052673 满块均=0 B/条（0 块）持有=552 MB 上限=1024 MB
摘块=0 分配失败=0 退化=0 丢记录=0 落盘=0 块 等盘=0 次 丢历史=0 块/0 MB
上限裁掉=81608143 条 累计写入(本段)=82658740 条
```

对账：`上限裁掉 81,608,143` + `内存 1,050,597` = `累计写入(本段) 82,658,740`；且
`[1] GetFirstCycle -> 81,608,144` = 裁掉数 **+1**（= 幸存者第一条，不是本段起点）。

**根因**（三处，全在 `ORBAGDI_TraceWin.cpp`）：

1. `kMaxRecs = 1u << 20` **硬编码**的常驻条数上限 ⇒ ETM 下"能回看多久" = 1.05 M 条 ≈ **0.13 s**。
   它与本文件的既有裁定**直接冲突**（`kMemLimitMax` 那段：上限的作用只是"别把 32 位地址空间
   吃光"，**不是**"限制能回看多久"）。
2. 当场内存只用了 **552 MB / 上限 1024 MB** ⇒ **整段 10 s 本来装得下**（82.66 M 条 ≈ 552 MB）。
   条数上限把内存上限**架空**了：`count` 被钉在 1.05 M，字节却一路涨到 552 MB
   （`EnforceRecCap` 只推进游标，裁掉的前缀**留在原地**没回收）。
3. 丢的动作在 `EnforceRecCap` 的**就地裁剪**里：那条路**不落盘、不报警、不置 `T_ERR_DATA_OVF`**
   ⇒ `摘块=0 丢历史=0 丢记录=0`，**一个信号都没有**。（整块退休那条路是会报 ERROR + 置位的，
   所以它日志里那句"要留住它：加 `trrec=1`"在本现场是**错的** —— `TraceFileWriteRecords`
   根本没被调用过。）

**已改**：`trmax=` 默认 **0 = 不限**（窗口只受字节上限 `trmem=` 约束）；上限真触发时改走
**整块退休**（记录日志开着 ⇒ 落盘，一条不丢；没开 ⇒ ERROR + Data Overflow，块接回链头）；
`GetFirstCycle` / `StatusUpdate` 的 `m_pRecordRange` / `TraceWinShowRange` 的显示区间
**一律报"本段起点"**（= `TraceWin: ETM cycle count base = …` 那条日志的位置）。

**第二轮加固（2026-10-15，用户"最初段有效起点，**不得有误**"）**：

1. **哨兵除掉**：原先判据是 `if (s.segFirstCycle && …)` —— 拿 **0** 当"本段起点**未设**"。
   而"本段第一条记录的周期号 = 0"是**合法**的（`AppendRec` 的 `advance` 是入参，将来有人传 0
   就中；ITM 轴模式下 `SyncAxisToWall()` 也可能把 `s.cycle` 留在 0）。一旦真为 0，哨兵式判据
   会**静默退回"最旧可读"** —— 正是本次要修的症状。改为显式标志 `Store::segFirstCycleSet`：
   值与判据彻底解耦（置位在同处 `seq == 1`，清空在 `TraceWinClear()` 与 `segFirstCycle` 同处）。
2. **对账日志**：本段第一条记录进来时打一次
   `TraceWin: 本段起点 = N 周期（本段第一条记录；此后 TDI GetFirstCycle / StatusUpdate 的
   m_pRecordRange / 显示区间一律报它 …）` ⇒ 上机可直接与 `[1] GetFirstCycle -> N` 逐字对账，
   不必再靠 `buf(...)` 反推。整段只打一次（`seq == 1`）。
3. **三出口同源复核结论**（读码确认，非推测）：
   - TDI `GetFirstCycle`：`FirstReportedCycle(s)` 再与**记录日志左端**取更早者；
   - `StatusUpdate()` 的 `g_recRange.nFirstCycle`：`FirstReportedCycle(s)`；
   - `TraceWinFirstCycle()`（停止时 `TraceWinShowRange` 的左端，`ORBAGDI_Agdi.cpp:518`）：
     `FirstReportedCycle(s)`；且 `s.count == 0` 时 `TraceWinLastCycle()` 返回 0 ⇒ 调用点
     的 `if (last)` 挡住，**不会**拿它去钉区间。
   - **边界（等价，非缺陷）**：`s.count == 0`（本段根本没写过记录 / 刚 `TraceWinClear`）时
     TDI 出口走"记录日志左端 / `TDI_ERR_EMPTY`"分支 —— 此时 `segFirstCycleSet` 必为 `false`，
     报不出比它更早的值，两条路等价。内存被退休/裁空 **不会** 走到这里：链上恒留 tail 一块，
     且 `RetirePush` 失败是 `LinkHead()` 接回链头（`count` 不归零）。
4. **未改动**（不在本次范围，另记）：`Store::headFirstCycle` 的注释仍写"绝不能是 0"；它现在
   只作 `FirstReportedCycle` 的一个输入，且 `!first` 时由 `segFirstCycle` 兜底 ⇒ 不再是对外
   值，注释留待合并 `headOfs` 死前缀机制时一并收敛。

- [ ] **上机复验（ETM + 4 位并行，`trrec=0`，目标跑 ≥ 10 s，选项串里不要有 `trmax=`）**：
      `buf(...)` 里 `条数上限=0`、`上限裁掉=0 条`、`本段起点=1`（或本段第一条记录的周期号）；
      `[1] GetFirstCycle ->` 必须 ≈ `本段起点`（**不再**是 81,608,144 那种"幸存者第一条"）；
      拖 Navigation 条**最左端**画得出数据。
      逐字对账那条新日志：`TraceWin: 本段起点 = N 周期（本段第一条记录；…）` ⇒
      **N 必须与 `[1] GetFirstCycle ->` 完全相等**（采集段内只应出现**一次**；出现两次以上
      = 本段被当成两段，说明某处调了 `TraceWinClear()`）。
- [ ] **上机复验（响亮这条路）**：`trmax=1048576` + `trrec=0` 再跑一段 ⇒ 必须出现
      `TraceWin: 记录条数上限（trmax=…）到顶，而**记录日志没开**` 的 ERROR + 状态栏 Data Overflow；
      **不允许**再出现"没有任何日志、数据却少了"。
- [ ] **回归（上限 + 落盘同时开）**：`trmax=1048576 trrec=1 trchunk=1 trmem=2` ⇒
      `TraceFile(...): 记录日志 … 索引 K 条 [周期 a..b]` 里的 `a` 应回到本段起点（裁掉的那段接上了）。

## 5. 设置界面

- [ ] **设置界面实机复测**：在 uVision 里分别从 Debug 页与 Utilities 页打开设置页，确认
      **七个页签**布局不被裁剪、改完 OK 后选项串正确回写。
- [ ] **§M5 实机复测**：确认 `SendMessage(mainFrame, Uv2Msg, 0x0C, text)` 在真 uVision 会话里确实落到
      命令窗口（落错的表现是"文本没出现"而不是崩溃），以及 `AG_INITUSRMSG` 是否每个会话都被投递。
- [ ] **§O5-2/3/4 仍需真 uVision 会话**：`Refresh()` 前移到连接之前（uVision 未连接时是否拒绝
      `UV_PDSCDBG_GET_PROPERTIES`）、`DebugDeviceUnlock` 在真锁器件上的效果、`ResetSetCatch` 与 PDSC
      复位的先后。`ORBAGDI_AgdiProbe.exe` 提供不了 `AG_CB_GETBOMPTR`，这三项在无 IDE 宿主上测不了。
- [ ] **§K3/§K6 实机复测**：真探针 + 真目标上确认 serial / firmware / AP IDR 能拿到，以及
      **活动会话中再开一个 RDDI 句柄**是否会被宿主拒绝（被拒时按设计退回未探测列表）。
- [ ] **§J 实机复测**：需要真目标板确认寄存器确实按预期写入、读缓存确实没返回陈旧数据。
- [ ] **§L2 实机复测（flashos，优先）**：① 页装配 —— 确认 `Flash: page 0x... programmed (N bytes)`
      的地址都页对齐、长度不超过页大小，且校验通过；② `CheckSectorTable` 是否误拒真实 FLM
      （第 4 条比 Keil 严；被拒时该**放宽**而不是硬擦）；③ `Flash → Erase` 与 `Flash → Run` 在 uVision
      里的实际调用形态。

## 6. 待测试（已实现，但没有实机无法确认）

- [ ] Flash 下载端到端：`AG_INITFLASHLOAD` → `AG_STARTFLASHLOAD` → 擦/写/校验 → 进度 → 复位收尾。
- [ ] PDSC 序列路（第二条下载路径）：`FlashSeqReady()` 为真时走描述而非 FLM。
- [ ] ITM / SWO 控制台：`AG_Serial(AG_ITMBOUT)` → uVision 的 printf 窗口。
- [ ] 执行历史 / Trace：`AG_HistFunc` + Trace Data 窗口（DWT PC 采样，非指令级）。
- [ ] 扩展断点与观察点：`ATRX_WATCH` 显示是否正确、FPB/DWT 比较器数量的实际探测。
- [ ] 端到端联调收尾：`AG_GoStep` 的停止/单步时序、CoreSight 解锁、观察点命中。
- [ ] §O / §P 的实机复测项（连接顺序改动影响每一次连接、`without Stop`、trace clock 单位、
      复位五种模式按新语义重测）。
- [ ] **Debug Recovery 实机复测**（实现已全部落地）：真 uVision 里制造一次链路丢失（或从恢复对话框
      勾"复位下恢复"），确认日志按 `DbgAcc: 访问等级 N，开始逐级恢复` → `DbgAcc: 访问已恢复 (kAccNormal)`
      推进，且窗口在恢复后被刷新。
- [ ] **ARMv8-M / TrustZone**：Cortex-M23/M33 的安全/非安全寄存器别名（选择子 ≥ 0x10）未处理。
- [ ] **SWO 异步分频的基数（ACPR）：H1/H2 未判别，待一次实机 A/B**（2026-10-14 起）
  - **待判别**：目标 `ACPR+1` 该除以哪个物理时钟 —— **H1** 真实 HCLK，还是 **H2** 独立 TRACECLKIN。
    我方**现状 = 核心时钟**（`cfg::SwoAcprBaseKhz()` 当前 `return c.cpuKhz;`）。
  - **危害（只在 H2 成立时）**：`tclkc=0 && tclk≠cpu` 时目标位率与探针受理档差 `cpu/tclk` 倍
    （168 M/8 M ⇒ 21 倍）⇒ 静默无 I​TM 输出/乱码，而 `Connect()` 的 `swoBaud > coreHz` 检查拦不到"差整数倍"。
  - **判据（本条验收）**：见 `§11 → 八 8.5` —— 不给 `swo=`，A：`tclkc=1 swopresc=4`、
    B：`tclkc=0 tclk=8000 swopresc=4`；**B 解不出 ⇒ H2**，**B 也解出 ⇒ H1**。
  - **判别前不改行为**：基数走**单点** `cfg::SwoAcprBaseKhz()`；H2 成立时**只改这一处**的返回值。
    **实测出结果前不要再动 `SwoAcprBaseKhz()` 的返回值。**
  - **⚠ 残留（非本次引入，未改）**：`ItmProgramTarget` 在 `acprBaseKhz <= 0` 时 `presc` 停在 1
    ⇒ 写 `ACPR = 0` ⇒ 目标按基数全速发、探针按 `baud` 收 ⇒ 静默乱码。可达路径：orbtrace 档给了
    `trbaud=` 而 `cpu=` 未填（`Agdi.cpp:905` 传 `SwoAcprBaseKhz()`，但**没有** `ItmStart` 那个 `72000`
    兜底）⇒ **两个调用点兜底不对称**。建议兜底收敛到一处，或 `baudHz > 0 && acprBaseKhz <= 0` 时
    `WARN` + `return false`。
  - 邻接项 `Connect()` 的 kHz/ns 合理性检查（`Agdi.cpp` 注释 `:817-821`）**按原样不动** —— 它防的是
    单位混用（差 10^6），与"差整数倍"无关。

## 7. 待实现（能改，尚未动手）

- [ ] **`AG_F_TARGETMAP`**：**先实现** `AG_EXECITEM/AG_TARGETMAP` 处理，**再**广告该能力位。
      反过来做就是撒谎 —— uVision 会立刻开始发 `AG_TARGETMAP_ITEM`，不处理即失败。
      `AG_F_DBG_UNIT_TIMER` / `AG_F_TP_*` 同理。
- [ ] **CoreSight ROM 表递归 + AP 分类**（`(Unsupported AP)` / `(Unsupported CPU)`），
      决定多 AP / 多核器件能否选对 AP。
- [ ] **§N P1 BKPT 被外部改写检测**（`Warning: BKPT instruction at 0x%08X externally modified!`）
      —— "断点莫名失效"的根因检测。
- [ ] **§N P1 Flash 多算法** + `Flash Algorithms not in Thumb Code` 校验 + RAM 算法大小上限校验。
- [ ] **Flash 多算法（`flm=` 只存一条路径，`FlashConf.Dev[NFlash]` 那套）**；内置 `BlankCheck_ARM` /
      `CRC32_ARM` 代码植入；CRC 校验路径。
- [ ] **§N P3 指令级 trace**（ISYNC 落盘 + 流式解码 + trace 状态上报）—— 独立子系统。
- [ ] **§N P3 多器件 JTAG 链 / SWD 多 drop。**
- [ ] **部署**：`TOOLS.INI` 需同时改 `TDRV` 与 `CPUDLLn`，`[ARM]` / `[ARMADS]` 两段要同步；
      **uVision 5.43 读哪一段未确认。**

## 8. 设置页技术栈（剩余判断，非待办）

- 没有已知待办项。**不建议**换 `.rc` + `rc.exe`（只解决静态那一半，接口/AP 列表、页高都是运行时算的）
  或 WebView2（DLL 跑在 uVision 进程里，静态 CRT `/MT` 就是为了不依赖外部运行时）。
