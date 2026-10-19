# Trace 抽 / 解吞吐 todo

> 起因：取数据和解码在同一个线程里串行，处理不过来。
> 记录日期：2026-10-19。以下是现状核对、瓶颈判据和候选方案，按性价比排序。

## 现状（已核对代码，不是推测）

抽数 worker 只有一个循环，2 ms 节拍、20 ms 预算：

```
worker (kPeriodMs = 2, kBudgetMs = 20)
  ├─ ComLock（非阻塞）→ TraceDrain(2ms)        锁内：只 memcpy 到 s_stage[256 KB]
  ├─ TraceDecodeStaged(n) → ProcessBytes       锁外：落盘 → COBS 解帧 → ITM/ETM 解码
  │                                                └─ FillGap：代码镜像扫描 + 每条分支补指令
  └─ TraceWinRead(0) / TraceWinRecoverTick     折叠状态，不碰探针
```

相关位置：

- `AGDI/src/ORBAGDI_TracePump.cpp` — `Body()` 主循环；`TraceDrain()`（锁内，只搬字节）；
  `TraceDecodeStaged()`（锁外，解码）；`kBudgetMs` / `kSliceMs` / `kStageBytes` 常量。
- `AGDI/src/ORBAGDI_OrbTrace.cpp` — `ProcessBytes()`：落盘 → 解帧 → `OnFrame` 分派；
  `OrbTracePullRaw()` / `OrbTraceDecodeRaw()`；`OrbTracePump()` 只给调试线程收尾用。
- `AGDI/src/ORBAGDI_Etm.cpp` — `EtmTraceFeed()` 持 `s.cs` 整块调 `TRACEDecoderPump`；
  `OnReport` 里的 P1-0 `FillGap()` 是热路径。

**结论：锁内/锁外已经分开了，但"抽"和"解"仍是同一个线程串行。** 解码慢直接减少下一轮
抽数的机会，探针侧环先满，表现为丢数据（`OrbTracePullRaw` 里已有 dropped 计数和
`kTErrDataOvf`）。

## 硬账：加线程补不回吞吐差

代码注释里的实测是 **~15 ms / 64 KB**，即解码吞吐约 **4.3 MB/s**。orbtrace 走 USB HS，
稳定几 MB/s 到 10 MB/s 很常见。

> 单靠换线程只能把"丢数据"从探针环推迟到自己的队列，队列满了照样丢。
> **先降单位成本，再谈线程结构。**

## 第一步：确认瓶颈在哪（用现成计数器）

`TracePumpStop` 已经会打 `s_tWait / s_tDrain / s_tDecode / s_tStatus / s_tRoundMax`。

- [ ] 跑一次真实会话，把这几项占比拿到手，判据：
  - `s_tDecode / s_tRound` > 60% 且 `s_tDrain` 小 → 解码是瓶颈，做第二步、第四步
  - `s_tDrain` 才是大头 → 传输慢，查 `OrbTracePullRaw` 块大小 / `TraceStream_Read` 参数
  - 两者都小但仍在丢 → 看 dropped delta，是采集端环回绕

## 第二步：把 `FillGap` 的成本打下来（收益最大，不动线程）

热点在每条间接分支都要"从 `lastPc` 扫到分支目标"，成本是 **O(执行指令数)**，
天然比数据率高一个量级。

- [ ] **指令边界表代替现场扫描**。Thumb-2 指令长度只取决于首半字，与执行次数无关。
      启动时（或代码镜像加载时）把整片 FLASH 扫一遍，建"起始地址 → 长度"位图，
      `FillGap` 退化成按表步进。现在每次 `FillGap` 都在重新判断指令边界，而同一段
      循环体一天要执行几百万次。
- [ ] **`(pc, len)` 结果缓存**。函数体重复执行，命中率很高；key 挂代码镜像版本号，
      镜像一变整体失效。
- [ ] **capstone 挪出热路径**。`FillGap` 只需要"下一条指令在哪"（长度扫描，
      `InstrScan` 已在做）；反汇编成助记符只为显示，可以推迟。
- [ ] **批量提交**。`TraceWinEtmInstruction(addr, true)` 是每条分支一次调用，若内部有
      锁/格式化，攒 64 条一次进环能省可观开销。

## 第三步：SPSC 环形队列 + 独立解码线程

减负之后还差一口气再动结构。

- [ ] worker **只**做 `TraceDrain` → 写 ring，做完就回去睡；它绝不解码，也不等消费者。
- [ ] 解码线程只消费 ring，**绝不碰 `ComLock`**（保持 `OrbTraceDecodeRaw` 注释里那条约束）。
- [ ] ring 用单生产者/单消费者无锁环（`Interlocked` 读写指针），容量按"峰值数据率 ×
      允许滞后时间"给，约 8–16 MB；`s_stage` 那 256 KB 退化成 ring 的一次写入缓冲。
- [ ] **ring 满必须丢最旧 + 计数**，不能等。丢一段之后 ETM 相对地址基准就断了，
      直接 `EtmTraceForceSync(false)` —— 复用 `ProcessBytes` 里 COBS 校验失败的那条路。
- [ ] `TraceWinRead` / `TraceWinRecoverTick` 留在 worker（折叠状态、不碰探针）；
      解码线程**不许**调 `bom->Notify`，否则又变成在 GUI 线程门口排队。

## 第四步：按需全指令（最省事的开关）

P1-0 全指令补全只在 Trace Data 窗口真的显示时才有意义。

- [ ] 给 P1-0 一个运行期开关（或"窗口不可见时只记分支对，可见时再补"）。
      分支对 `(from, to)` 只有 8 字节，补全一个 500 条指令的函数要写 500 条记录。

## 不要做的

- **给 ETM 解码器做分片并行**。它是有状态的流式解码（相对地址、同步点），分片等于
  每条缝都要 A-SYNC，实际拿不到加速，还会放大"地址是假的"这类问题。

## 落地顺序

| 顺序 | 动作 | 预期 | 风险 |
| --- | --- | --- | --- |
| 1 | 看 `s_tDecode/s_tRound` 占比 | 确定是不是解码 | 无 |
| 2 | 指令边界表 + `(pc,len)` 缓存 | 解码成本降一个量级 | 中（涉及 CodeImage） |
| 3 | P1-0 按需 / 延迟补全 | 窗口不开时几乎免费 | 低 |
| 4 | SPSC ring + 解码线程 | 解耦，抗突发 | 中 |
| 5 | 批量提交、ring 满丢段 + ForceSync | 兜底 | 低 |

## 待补的观测

- [ ] `TracePumpStop` 统计行里补上 `s_tDecode` 的占比和 ring 水位 —— 有了这个数，
      第 2 步和第 4 步谁先做就不用猜。
