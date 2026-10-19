# NewTodo — 2026-10-19 会话

依据：`%TEMP%\ORB_AGDI.log`（63 s 会话，`orbtrace: RDDI 环溢出，丢了 161087488 字节`，
吞吐 566 KB/s，解码 106.72 ms/64 KB = 抽数的 382.9 倍）。

本次三件事都做了，`build.ps1` 通过（`ORB_AGDI.dll`）。

---

## 一、已完成

### 1. 解码从抽数线程里拆出去（两条线程 + 有界队列）

`src\trace\ORBAGDI_TracePump.cpp`

- `DrainBody()`：只拿探针锁搬字节，`OrbTracePullRaw()` 直接写进队列槽（不再经过 `s_stage`
  中转），然后 `QueueCommit()`。锁持有时间仍然是「一个 2 ms 时间片」。
- `DecodeBody()`：`QueueFront()` → 写 cache / 解帧 / P1-0 补全 → `QueuePop()`。
  **不看挂起位** —— 挂起是为了让调试线程独占探针 / 让 Flash 放锁，解码器不碰探针；
  反过来，挂起期间必须能继续排空，否则收尾会等死。
- 队列：SPSC，`ORBAGDI_TRACE_QUEUE_SLOTS`（8..256，默认 **64 槽 × 256 KB = 16 MB**，Todo §3.1
  要求可配）。写指针只由抽数线程改、读指针只由解码线程改，无锁。
- 队列满 ⇒ **照样抽**（抽到 `s_stage` 后丢弃，不把数据留在探针环里）＋
  `TraceWinErrorSet(kTErrDataOvf)`＋`s_gapPending`＋按字节记账（`丢弃=N 片 / X MB`），
  解码线程下一块之前 `OrbTraceRequestResync()`。丢的位置从「RDDI 内部环（看不见、会让
  COBS 解帧错位、产生假地址）」变成「本层有界队列（字节数可计、能主动标记缺口）」。
  ⚠ 第一版写的是「满了就不抽」——那只把丢失推回环里，等于没解决，已改掉。
- 队列分配 / 解码线程创建失败 ⇒ `s_inlineDecode = true`，退回「抽完就地解」老路径。

### 2. P1-0 解码加速：按 pc 缓存指令形状

`src\trace\ORBAGDI_InstrScan.cpp` / `.h`、`src\trace\ORBAGDI_Etm.cpp`、`src\trace\ORBAGDI_CodeImage.cpp`

- 新增 `ScanToCached()`：`pc → {len, 分支形状, target}` 直接映射表（`1<<18` 槽 = 4 MB，
  正好覆盖 512 KB Flash 的每 2 字节，零冲突）。命中时**不读镜像、不进 capstone**，
  原来每条指令的两次加锁 + 两次 memcpy + `cs_disasm_iter` 全省掉。
- 失效：`CodeImageGeneration()` 每次 `CodeImageAdd()`/`CodeImageClear()` 递增；
  版本变化时整表 `memset`（约 0.3 ms，只发生在下载/清空之后第一次扫描）。
- `FillGap()` 改用 `ScanToCached()`；`EtmTraceLogSummary` 增加一行命中率：
  `InstrScan: 指令缓存 命中=… 未命中=… 命中率=…% 作废=… 槽=…`。
- 只缓存**成功**解码的结果（读不到字节 / 解不出来不缓存，否则会污染）。

### 3. 日志里两处误报

`src\app\ORBAGDI_Agdi.cpp`

- 横幅 `0 kB flash at 0x00000000`：改用 `eff`（`Effective()` 已合并 `AutoDetected()` 的
  device / iromStart / iromSize），不再是 uVision 没填的 `raw`。
- `GoStep: fault`：`DFSR.VCATCH` 只是停止原因的一个位，**不等于**发生过 fault。现在只有
  `CFSR|HFSR` 非零才打 `WARN`；否则打 INFO 并说明 BFAR/MMFAR 无效。

### 4. 配套：解码器状态加锁 + 收尾顺序

`src\trace\ORBAGDI_OrbTrace.cpp`

- `State::decodeCs`：`ProcessBytes()` 全程持有（粒度为一块字节）。抽/解拆线程后解码器
  有两个入口（解码线程、停机收尾的调试线程），原来只靠挂起握手的时序假设。
- `State::resyncReq` + `OrbTraceRequestResync()`：`OrbTracePullRaw()` 在抽数线程上，
  不能再直接改 ETM 引擎状态；改为置标志，由解码线程在 `ProcessBytes()` 持锁时执行
  `EtmTraceForceSync(false)`。
- `TracePumpSync(ms)`：等队列深度归零。用在
  `StopTraceSources()`（打汇总之前）、`RunAndWait()` 停机后的最后一次排空、`AG_Stop` 的
  第二处。**不再在调试线程上直接 `OrbTracePump()`**（会插到积压之前，解码器状态乱序，
  Todo §3.1 要点 3）。

---

## 二、待实机验证（没有这些数不能说「够用」）

- [ ] **30 s 以上 ETM 采集**（4 位并行）：`orbtrace: RDDI 环溢出` 应完全消失；
      看 `TracePump: 队列积压 峰值=… 均值=… MB`，`因队列满放弃=0 轮`。
- [ ] **解码吞吐**：`P1-0` 的 `ms/64 KB` 应从 106.72 降到 20 ms 以下（3.1 MB/s 才不丢）。
      同时看 `InstrScan: … 命中率` —— 命中率高而仍慢 ⇒ 瓶颈已转到 TraceWin 落记录；
      命中率低 ⇒ 工作集超出 512 KB 镜像（RAM 里跑的函数会撞槽）或镜像被反复重建。
- [ ] **顺序性**：人为拖慢解码线程后，Trace 窗口记录仍严格时间递增，
      「补出的指令先于其分支记录入轴」仍成立（§16.8）。
- [ ] **停机一致**：`TraceWin: buf(...)` / `EtmTraceLogSummary` 的计数与「解码线程已排空」
      对得上，不允许「汇总 0 条而窗口有数据」。
- [ ] **不回归**：`test\run-tests.ps1` 23 项；Flash 下载期间 `PumpQuiesce` 仍能拿到探针
      （挂起后解码线程仍在跑，这是有意的 —— 它不碰探针）。
- [ ] **多线程自检**：`OrbTraceDecodeRaw` 现在有三个潜在入口（抽数线程 inline 退路、
      解码线程、调试线程收尾）。确认没有「两处同时解码」的场景：
      `TracePumpRunning()` 为真时收尾一律走 `TracePumpSync()`。

---

## 三、待你裁定

- [ ] **队列满了丢哪一头？** 现在选的是「丢最新」＝这一片照样从环里抽出来、但不入队，
      直接丢弃并记账、置缺口。好处是不破坏队列里的连续性、实现最简；代价是积压期间
      画面停在旧数据上（但**不会有假地址**，缺口气标记会强制重同步）。
      另一头「丢最旧」＝窗口跟实时走、历史中断，但要把整个队列前移，代价大。
      这条 Todo §3.1 也列着「待用户裁定」。

---

## 三·五、2026-10-19 22:38 那次 uVision 崩溃（已修）

**现象**：日志停在 `22:38:41 AG_GOFORBRK still waiting (10 s)`，之后没有任何输出；
`UV4` 进程不存在，`CrashDumps` 空，WER 里没有 uVision 的 AppCrash 条目（只有 Edge/lghub）。

**根因**：`AppendRec`（`ORBAGDI_TraceWin.cpp`）**故意不取锁** —— 原注释写着"热路径不取锁：
3.96–7.80 M 条/s"，它把原子性交给了调用方手里的 `ComLock`。拆线程之前，两个追加入口
（ITM 的 `TraceWinItmByte`、ETM 的 `TraceWinEtmInstruction`）确实都在 ComLock 里（worker 的
`OrbTracePump` 也在锁内），所以这条"约定"成立。**解码线程不持 ComLock** 之后，它和
调试线程的 `TryTraceGetRecord / GetFirstCycle`、抽数线程的 ITM 追加同时改
`s.tail / s.usedBytes / s.count / s.recSeq / c->nRecs` ⇒ 记录链被写坏 ⇒ uVision 读到半条
记录后直接消失。

**修法**：`AppendRec` 开头加 `StoreGuard`。已核对不会死锁：

- `NewWriteChunk()` 内部同样取 `StoreGuard` —— 临界区可重入，同线程不自锁；
- `RetirePush()` 等槽时持着 storeLock，但写线程**腾槽**只走 `retireLock`（`SetEvent` 在
  还块之前），需要 storeLock 的只有"还块"，而 `RetirePush` 不等它还块 ⇒ 不互等；
- 锁序始终 `ComLock → storeLock`，没有反向路径。

代价：ETM 约 1.18 M 条/s，每条一次无竞争 Enter/Leave ≈ 2–4% 单核。真正的新代价是
"UI 线程读记录时会挡住解码线程"，属锁竞争固有，换掉的是一条必崩的路径。

**同类但未改的**（只影响计数/错误位，不会崩，暂不动）：

- `TraceWinErrorSet()` 的 `g_tErr |= mask` 与 `TraceWinTraceStatus()` 的读清都不是原子的；
- `TraceWinAdvanceCycle()` 的 `S().itmCycles += delta`；
- `TracePumpSync(1000)` 超时时解码线程仍在写，与 `TraceWinSetRunning(false)` 并发 ——
  现在被 storeLock 兜住不会崩，但可能出现"停止后又冒出一条记录"。

**如果还崩**，下一步按这个顺序取证：

1. 给 `UV4.exe` 开 LocalDumps（`HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\UV4.exe`，
   `DumpType=2`），重现一次；
2. 用 `ORBAGDI_TRACE_QUEUE_SLOTS=8` 缩小队列内存、先不开 `trrec=1`，区分"我们的缓冲"与
   "cache 写线程"；
3. 若 dump 指向 `ORB_AGDI.dll`，看调用栈落在 `AppendRec` / `NewWriteChunk` / `TraceWinRead` 哪一段。

---

## 三·六、2026-10-19 22:41 第二次崩溃：不是竞态，是产量

**现象**：用户反馈"流畅度很高，但还是崩溃"。日志时间线：

- `22:41:26.068` 抽数 + 解码两条线程启动（本次会话唯一的启动日志）
- `22:41:42.064` `GetLastCycle -> 94146810（内存 94146810 条）`
- `22:41:57.833` `AG_EXECITEM AG_RUNSTART`（第二次 RunStart；`TracePumpStart` 因
  `s_threadDrain` 已存在而早退，没有重复建线程 —— 查过日志，全文件只有一条"两条线程已启动"）
- `22:41:57.839` **最后一条日志**：`TraceWin: running=1, 缓冲 103047588 条记录 / 172 块`
- 之后**一秒内零输出**。此前 `GetLastCycle` 每 1–2 s 必有日志，所以崩溃发生在
  `TraceWinSetRunning(true)` 返回、uVision 开始按这个条数取数的那一刻。

**根因（规模，不是并发）**：

- ETM 把分支之间的每条指令展开成记录，实测 **550 万条/s**（3.1 MB/s ÷ 7 B ≈ 44 万
  个 ETM 包/s × 平均 10 条指令）。30 秒 = 1.03 亿条，172 块 × 4 MB = **688 MB**。
- 上限是 `kMemLimitMax = 1 GB`（物理内存 1/4 封顶），`recCap`（`trmax=`）默认 0 = 不限，
  所以缓冲会一路涨到 1 GB 才滑动。
- uVision 是 32 位进程，1.03 亿条 × `sizeof(TRACE_RECORD_64)` = 16 B ≈ **1.6 GB**，
  取数即死。
- **旧版没暴露**：解码慢（106 ms/64 KB）+ 探针环溢出丢包，记录数被压在几百万。
  抽/解分离之后一条不漏，产量问题才浮出来。所以"流畅度很高"和"崩"是同一件事的两面。

**改法**：`kMemLimitMax` 1 GB → **256 MB**（≈3800 万条，uVision 侧 ~600 MB），
新增环境变量 `ORBAGDI_TRACE_MEM_MB`（16..2048）覆盖默认值，`trmem=` 仍优先。
到上限后走既有的整块退休：丢最旧、状态栏 `Data Overflow`、日志一条 ERROR（不静默）。

**副作用（已知、可接受）**：256 MB 下历史约 7 s（3800 万 ÷ 550 万条/s）。要更长历史得
先开 `trrec=1`（记录 cache 到 `%TEMP%`），再调 `ORBAGDI_TRACE_MEM_MB`。

**验证顺序**（如果 256 MB 还崩，用这个二分，别猜）：

1. `ORBAGDI_TRACE_MEM_MB=96` → 若跑 10 分钟不崩，规模假设成立；
2. 若 96 也崩 → 回退到"不是规模"的判断，按 §三·五 第 2、3 步抓 dump；
3. 日志里新增 `TraceWin: 内存窗口上限被环境变量覆盖 = N MB` 可以确认变量真的生效。

---

## 四、已知残留（本次没动）

- **稳态带宽不足时仍会丢。** 队列只是把丢失变成可计量、可标记缺口；要真正不丢，解码必须
  ≥ 探针输出速率。缓存是第一步，若实测仍不够，下一步是 §3.1 的「首选」方案：
  P1-0 展开延后到查询侧（派生数据不回流解码器）。
- **缓存只对镜像内代码有效。** RAM 里运行/自修改的代码会命中慢路（正确性不受影响，
  因为镜像字节变了 `generation` 会变；未变的旧条目也不会被误用 —— 命中判定看 `pc` 全等，
  而 RAM 地址与 Flash 地址撞槽时，正确性由 `generation` 保证）。
- **`TracePumpStop()` 超时会保留句柄且不重启**（既有缺陷）：抽数线程 2 s 内不退出，
  句柄留着，之后 `TracePumpStart()` 直接返回 ⇒ 重连后没有抽数线程。这次把解码线程的
  等待放宽到 5 s（排空 64 槽需要时间），但超时语义没改。
- **队列内存 16 MB 常驻**（`malloc` 一次，不为断开释放；`FreeLibrary` 时随进程回收）。
- **`TraceDrain()` 现在没有调用者**（worker 改走队列），保留给调试线程与外部语义，未删。
