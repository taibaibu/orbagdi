本文件用于AI指导AI继续推进项目开发的工具论集合；
不包括BUG,TODO
旨在于让AI结合Todo自行推进项目开发
现在请继续开发：

---

## 改完怎么验证：`test\` 下的检查与工具

一条命令跑：

```
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1           # 编译 + AGDI 探针（实机）
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -Sheet    # 另外跑设置页那三项检查
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -NoProbe  # 只编译，不跑
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -Show     # 打开设置页给人看
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -Dump     # 打印每页控件树
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -Clean    # 先清 test\obj 与 test\bin
```

**设置页那三项检查默认不跑**：`SettingsPreview.exe` 只在设置页改动时才变，
而每个用例都要开一次对话框 —— 驱动改了之后跑它证明不了新东西。设置页改完加 `-Sheet`。

### 检查（有退出码）

| 脚本 | 证明什么 |
|---|---|
| `e2e_sheet.ps1` | 用已知选项串打开预览，`EnumWindows` 找到对话框，外部 `PostMessage(WM_COMMAND, IDOK)` 按 OK，逐键比对回写的选项串；四种 trace 源各一遍，外加一个 Cancel 用例 |
| `inspect_sheet.ps1` | 遍历窗口树：页签选择器真是 `SysTabControl32` 且 5 项、**每个标题读回来是完整单词**、五页都在（按各页独有控件 id 认，不靠枚举顺序）、页在 tab 内、**没有控件越过页客户区**、"I" 页真是可滚动只读文本框 |
| `check_log_share.ps1` | 驱动持着日志时它仍可读（`COMPAT_ANALYSIS.md` §10.2）；同时断言只读共享被拒 —— 否则驱动哪天不再持有文件，检查会空过 |

### 工具（不进 `run-tests.ps1`）

| 脚本 | 用途 |
|---|---|
| `shot_sheet.ps1` | 截图设置页，`-Page Debug\|Trace\|Flash\|Pack\|Info`、`-Out x.png`、`-ExeArgs 'port=1'`。页签矩形由 `TCM_GETITEMRECT` 现算，不硬编码屏幕坐标、不移动鼠标 |
| `show_info.ps1` | 打印 "I" 分页文字，或其中一节（`-Section Target\|Where\|Messages\|Log`）。"驱动在这组选项下说了什么"用这个问，不用截图 |

`test\build.ps1` 单独编预览（`-Run` 编完直接打开）。

### 三条硬约束（都是踩过坑才立的）

1. **`src\` 里不许有测试钩子。** 没有 `--selftest`、没有 `--check`、没有环境变量开关：
   对话框一律从外部驱动，uVision 加载的就是被测的东西。`SettingsPreview.exe --ui`
   只给人看。
2. **带指针的消息要么不发，要么把缓冲区开在目标进程里。** `>= WM_USER` 的消息跨进程
   **不封送**，从这边传指针会让对方去读一个在它地址空间里毫无意义的地址。
   `TCM_GETITEMW` 这类用 `VirtualAllocEx` + `WriteProcessMemory` / `ReadProcessMemory`
   在对方进程里开缓冲（`inspect_sheet.ps1` 有现成写法）。
   > 反例：早先由此得出"`TCM_GETITEMW` 不能用"，于是页签标题只靠"五项在、条带够宽"
   > 间接验证 —— 结果五个标题全是单字母（`COMPAT_ANALYSIS.md` §10.1）也没被拦住。
3. **驱动对话框之前先等就绪。** 对话框一创建就能被 `EnumWindows` 找到，而
   `WM_INITDIALOG` 还在跑（Parse → 建五页 → RDDI 探测 → 填各页控件）；此时按 OK 会
   读到半填充的控件，报出并非界面造成的失败。等"某一页变为可见"再动手 ——
   `ShowPage()` 在 `WM_INITDIALOG` 最末才执行。

### 实机排查

RDDI 层日志开关：`%TEMP%\ORBMDK_LOG_LEVEL`（内容 `0`=DEBUG，运行中热生效，
删文件恢复默认 ERROR），日志落在 `%TEMP%\ORBMDK_RDDI.log`。

---

## 实机全功能验证：怎么把 AGDI 层跑起来

上面那套只覆盖**设置页**。AGDI 的**功能**面 —— 连接、内存、寄存器、运行控制、烧录、
trace —— 平时只有 uVision 能驱动，于是"能不能用"只能靠人开 IDE 点一遍：不可重复、
不能无人值守，坏了也说不清是哪个入口。`test\ORBAGDI_AgdiProbe.cpp` 补上这一环。

### 工具

```
powershell -ExecutionPolicy Bypass -File test\build.ps1   # 同时编出两个 exe
.\test\bin\ORBAGDI_AgdiProbe.exe                          # 连目标，逐项 PASS/FAIL
```

它是**最小宿主**，替 uVision 调 AGDI 入口：

- 加载的是**构建产物** `bin\ORB_AGDI.dll`（uVision 加载的同一个文件），不链接 `src\`；
- 按 uVision 的方式填 `dbgblk` 并经 `EnumUvARM7(2)` 交出去，选项串走同一条路；
- **先注册 `AG_INITCALLBACK` 并应答 `AG_CB_MSGBOX`** —— 没有回调时驱动会退到模态
  `MessageBoxA`，在控制台宿主里那就是**卡死**，不是报错；
- 按 ABI → 连接 → 能力位 → 寄存器 → 内存 → 运行控制 → 单步 → DAP 分组检查，
  失败直接指出是哪个入口坏了。

参数：`--dll <path>`、`--options "<串>"`、`--read <hex>`、`--memwrite <hex>`、
`--device <name>`、`--keep`。

### 方法（都是踩坑换来的）

1. **先看 DHCSR，再下结论。** "PC 没动"有三个完全不同的原因：核心没停、单步还没
   生效、代码真的在自旋。`C_DEBUGEN` / `C_STEP` / `S_HALT` 三位能区分它们 ——
   宿主每步都打 DHCSR 就是为这个。
2. **区分"驱动的问题"和"下面的问题"。** 手工写一次 DHCSR（宿主里
   "Single step, done by hand" 那段）绕开 `Core::Step`：手工能单步而驱动不能，
   责任在驱动；两者都不能，责任在探针 / RDDI / 目标。
3. **调用顺序本身是测试用例。** `AG_STOPRUN` → `AG_NSTEP` 这个顺序曾让单步静默
   失效（`stopReq` 粘住），所以宿主**故意**保持这个顺序 —— 它现在就是那条 bug 的
   回归测试。
4. **`dbgblk` 有两套同名字段**：大写 `Irom`/`Iram`（iMCS166 段）与小写
   `irom`/`iram`（iMCS51 段）。`AdoptUVision` 读**大写**那对；填错不报错，只在会话
   横幅里显示 "0 kB flash at 0x00000000"。
5. **改完先关缓存复测一次。** 读缓存（`cachemem`，默认开）曾把 PPB 寄存器也缓存了，
   导致每个寄存器都返回同一个陈旧值。碰到"某个读值可疑地恒定"时，
   `--options "... cachemem=0 cachecode=0"` 是最快的分辨手段。
6. **DAP 命令失败后要重新连接再试**，不要连着试 —— 连着的失败会互相掩盖原因。

### 方案：三个阶段的验证面

| 阶段 | 覆盖 | 状态 |
|---|---|---|
| **A** 连接 / 内存 / 寄存器 / 运行控制 | ABI（`EnumUvARM7`、`DllUv3Cap` 2/100/109）、`AG_Init(AG_INITFEATURES)`、7 个能力位、`AG_AllReg`、`AG_MemAcc` 读（可选写/读回/还原）、`AG_GoStep` 停与单步、`AG_DapAcc` | **已跑通**（22 项） |
| **B** 烧录 | `AG_INITFLASHLOAD` → `AG_STARTFLASHLOAD` → 擦 / 逐页 `ProgramPage` / 校验 → `AG_CB_GETFLASHPARAM` 取镜像 → `UnInit`；回读整份镜像与文件逐字节比对 | **已跑通**（25 项，见下） |
| **C** trace / SWV | `AG_Serial(AG_ITMBOUT)` 控制台、`AG_HistFunc` + Trace Data 窗口（DWT PC 采样）、ETB、orbtrace | AGDI 侧就绪；**卡在 RDDI 宿主**（§10.13，一行修复） |

### 阶段 B 的跑法（STM32F407RET6 实测）

```
.\test\bin\ORBAGDI_AgdiProbe.exe --device STM32F407RET6 ^
    --flash "D:\WorkSpeace\F407Full\F407Ful\MDK-ARM\F407Ful\F407Ful.hex" ^
    --flm   "D:\MDK5\ARM\PACK\Keil\STM32F4xx_DFP\2.7.0\CMSIS\Flash\STM32F4xx_512.FLM" ^
    --ram   0x20000000,0x4000
```

`--flash` 时宿主会自动补 `dlflash=1` 与 `rstcon=1`（仅在缺省时补）。这两个开关各自
制造过一次"看起来像驱动坏了"的失败，见 `COMPAT_ANALYSIS.md` §10.12：

- 没有 `dlflash=1` → 驱动**写 RAM** 却报成功，flash 一字未变；
- 没有 `rstcon=1` → 算法第一次写 FLASH 寄存器就 `PRECISERR` 总线错误，因为目标停在
  应用程序的运行态（`RCC_AHB1ENR` 只剩 OTGHS、`FLASH_ACR=0`、`FLASH_SR=0xC0`）。
  **同一地址经调试 AP 写是成功的** —— "调试器能写、CPU 不能写"是这类状态问题的特征。

选 FLM 看**器件实际 flash 容量**，不是看型号里最显眼的数字：STM32F407RET6 是
**512 KB**（`.sct` 里 `0x08000000 + 0x80000` 也对得上），所以用 `STM32F4xx_512.FLM`。

### 阶段 C：trace —— 已打通并口 1/2/4 位

SWO 与 Trace 已接线，但**不走 CMSIS-DAP 接口** —— 走 orbtrace 探针自己的 trace 端点，
即设置页的 "orbtrace probe" 源（`orbtrace=1 trwidth=… trswo=… trbaud=… troflow=…`），
解帧参考 `orbuculum`（OFLOW / COBS，见 `ORBAGDI_Oflow.h`）。

**orbtrace 的 SWO 能力来自它自己的 trace 通道，与 CMSIS-DAP 无关（专用通道，勿混）。**
orbtrace 是复合设备，三个 vendor 接口彼此独立：`0x54` = Trace / `0x50` = Power /
`0x56` = Version。SWO 与并口的采集、格式转换、解码全部由 `0x54` 这条**专用**通道承担。
探针固件在 **CMSIS-DAP** 侧没有实现 SWO 命令族（`0x19`/`0x1A`/`0x1B`）、自报
`DAP_CAPABILITIES` 也不含 SWO 位 —— **这与 trace 通道能不能做 SWO 是两回事**，
不要拿它当"SWO 不可用"的证据。排查 SWO 只看 `0x54` 通道的
`Set Input Format` / `Set Async Baudrate` 两条请求及其返回值。

**测法（关键）：按宽度各编一版固件、各烧一次。** 单片机**不会**自动切换位宽，也不该指望
驱动去改 —— 固件的 `TRACE_PARALLEL_BITS` 是唯一真源，`DBGMCU_CR` / `TPI_CPSR` /
**引脚**三处都必须跟着它走。拿 4-bit 固件去测 1/2-bit 得出的任何结论都是无效的。
详见 `COMPAT_ANALYSIS.md` §10.17。

**当前状态**（目标 48 MHz，`--run 3000`，`pcperiod=1 pcpresc=0`，各档用对应宽度的固件）：

| 配置 | 其它帧 | 校验错 | 解出 PC | 判定 |
|---|---|---|---|---|
| `trwidth=1` | 9 | 19 | 部分非法 | 通；带宽被 ETM 挤满 |
| `trwidth=2` | 1 | 3 | 全合法 | 通 |
| `trwidth=4` | 1 | 4 | 全合法 | 通 |
| `trswo=1` NRZ 2 Mbaud | — | — | — | 只滴出几十字节 |
| `trswo=2` Manchester | — | — | — | 未通 |

**判据是 `其它帧`**（tag 既非 1 也非 2 = 帧边界没对上）**和解出的 PC 是否落在
`0x0800xxxx`~`0x0802xxxx`**，不要只看总字节数 —— 溢出时字节数照样很大。

一条命令跑完整矩阵（脚本里写了完整的原理与判据）：

```
powershell -ExecutionPolicy Bypass -File test\trace_matrix.ps1
```

单档手工跑：

```
.\test\bin\ORBAGDI_AgdiProbe.exe --options "clock=1000 iface=0 orbtrace=1 cpu=48000 rstcon=1 stoprst=0 pcperiod=1 pcpresc=0 trwidth=4 trswo=0" --run 3000
```

### 阶段 C 的方法论（踩过的坑，都值钱）

**1. trace 泵只在运行循环里跑。** `ItmPump` / `OrbTracePump` 的调用点在
`RunAndWait` 的轮询循环与 `AG_WAITFORBRK` 里，所以必须把目标跑起来才采得到 ——
`--run <ms>` 起线程跑 `AG_GoStep(AG_GOFORBRK)`、主线程到时发 `AG_STOPRUN`，
**这正是 uVision 的模型**。

**2. 探针不做设备端使能。** orbtrace 的 trace 接口只有 `Set Input Format` 和
`Set Async Baudrate` 两条控制请求，目标侧的 DEMCR/TPIU/ITM/DWT/ETM 必须由我们
经 SWD 写（`ItmProgramTarget()`，在 `Connect()` 里"先目标、后探针"）。
orbuculum 用 GDB 脚本做同一件事（`orbuculum/Support/gdbtrace.init`）。

**3. "0 字节"必须分三种成因看**，只看总数永远查不下去。驱动关闭流时会打：

```
orbtrace: 读路径 调用=N 空=M 错=K
```

* `调用 == 空` → 端点全程为空，探针没在发（查探针输入/接线）
* `错 > 0` → 读本身出错，不是"没数据"
* `调用 == 0` → 泵根本没跑

**4. "偶尔收到一帧、内容是全 FF / 全 11"= 探针 FIFO 残留。** 定长常量填充、
只在"输入格式刚变更"或"刚烧录"后出现一次 —— 这个模式本身就是判据，别当成数据。

**5. 判据只能靠回读。** CoreSight 的"编程模式"有写保护，写在模式外的寄存器访问
**不报错、只是不生效**。定位方法是**对比"能写进去的"和"写不进去的"寄存器**：
本轮的 ETM 就是靠这个发现 `ETMTRACEIDR`/`ETMTECR1` 写不进去、而
`ETMTRIGGER`/`ETMTEEVR` 能写进去，从而锁定"编程模式位"。

**6. `cpu=` 必须与目标实际主频一致。** SWO 的 TPIU 分频器由驱动按 `cpu=` 算；
不匹配时目标发的波特率与探针的解码率对不上，解出来全是垃圾 —— 而且**看起来像
"有数据但内容乱"**，最容易误判成接线问题。

**7. 目标时钟不能超出数据手册范围。** 原配置 `PLLM=6/PLLN=42/PLLP=2` 在 12 MHz
晶振下 VCO 只有 84 MHz（低于 100 MHz 下限）。同步 trace 的时钟就是 HCLK，且
TPIU 在同步模式下**没有分频器**（`TPI_ACPR` 只作用于异步 SWO），所以降速只能降
系统时钟。当前 48 MHz 是实测最优（降到 24 MHz 反而全 0）。

**8. `--ram` 在 PowerShell 下必须加引号。** `--ram 0x20000000,0x4000` 里的
`0x20000000` 会被 PowerShell 当数字字面量求值，传给原生 exe 时变成十进制
`536870912`；驱动按 `%x` 解析 → 截断成 `0x36870912` → 烧录报
`cannot load algorithm image at 0x36870912`。写 `--ram '0x20000000,0x4000'`。
cmd.exe 下没这个问题（文档里的 `^` 续行范例就是 cmd）。

**9. 宿主跑完要把内核放开。** `AG_STOPRUN` 会 `c.core.Halt()`，之后没人 `Resume`。
`AgdiProbe` 现在默认在收尾前清 `DHCSR.C_HALT`（`0xA05F0001`）并回读确认，
`--halt` 保留旧的停住行为。否则板子一直停在 halt，看起来像死机，会把下一轮排查带偏。

### 阶段 C 的排查工具

```
.\test\bin\ORBAGDI_OrbTraceProbe.exe    # 逐条复刻宿主的四步 USB 扫描，两种 CreateFile 标志各试一次
.\test\bin\ORBAGDI_AgdiProbe.exe --run  # 跑目标并在结束后回读全部 trace 寄存器
powershell -File test\trace_matrix.ps1  # 全矩阵 + 判据
```

`--run` 结束后会自动回读 DEMCR / DBGMCU_CR / TPIU(CSPSR·ACPR·SPPR·FFCR) /
ITM(TCR·TER) / DWT_CTRL / ETM(CR·CCR·TRIGGER·SR·SCR·TEEVR·TECR1·TECR2·IDR·
SYNCFR·TRACEIDR·LAR) —— **判断"谁最后写赢"、以及"哪一位没写进去"只看它**。

---

## 崩溃 / 故障定位：从"界面崩了"到"改哪一行"

上面几节讲**功能**怎么验；这一节讲**崩了之后**怎么定位。以 2026-10-10
"uVision 点 Download 界面崩溃"为例走一遍完整链路 —— **每一步都只取证、不改代码**，
定位到具体一行之后才动手。

### 0. 原则

* **先定位，后修改。** 没定位到函数就不改代码，否则改的是猜测。
* **一次只改一处**，改完立刻复测。
* **崩溃类问题的第一手证据是操作系统的记录，不是源码。**

### 1. 第一步永远是"谁崩的、崩在哪"

界面崩溃先看 Windows 事件日志，它给出**故障模块 + 异常码 + 偏移**，
足以把范围从"整个工程"缩到一个函数。

```powershell
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddHours(-3)} |
  Where-Object { $_.Message -match 'UV4|ORB_AGDI|CMSIS_DAP|MSVCP|ucrtbase' } |
  Select-Object -First 6 | ForEach-Object { '=== ' + $_.TimeCreated; $_.Message }
```

只看三行：

| 字段 | 例 | 用途 |
|---|---|---|
| 错误模块名称 | `ORB_AGDI.dll` | 责任方：本层 / 宿主 / 系统 DLL |
| 异常代码 | `0xc0000005` | 访问违例 = 野指针；`0xc0000374` = 堆损坏；`0xc0000409` = 栈溢出 |
| 错误偏移量 | `0x0000328a` | **模块内 RVA**，下一步拿它反汇编定位 |

> 同一条 WER 记录里还有 `memory.hdmp` 全内存转储路径。本机**没装** `cdb.exe`
> （Windows SDK 的 Debuggers 组件），所以走反汇编路线；装了的话
> `cdb -z <dump> -c "!analyze -v; kb; q"` 直接给调用栈，更省事。

### 2. 第二步：日志给出"执行到哪一行"

驱动日志是**有顺序的**，最后一行就是它临死前做完的事。

```powershell
Get-ChildItem "$env:TEMP" -Filter 'ORB*.log' | Sort-Object LastWriteTime -Descending |
  Select-Object -First 6 |
  ForEach-Object { '{0,-24} {1,10:N0} B   {2}' -f $_.Name, $_.Length, $_.LastWriteTime }
```

两份日志分工不同，**都要看**：

| 日志 | 谁写的 | 回答什么 |
|---|---|---|
| `%TEMP%\ORB_AGDI.log` | 本层（AGDI 驱动） | 驱动自己的进度：选项串、FLM、RAM 窗口、连接、每页烧录 |
| `%TEMP%\ORBMDK_RDDI.log` | RDDI 宿主 | 底层每一次 DAP 访问（`DAP_ReadReg/WriteReg/Transfer`） |

**用两份日志的最后时间戳判断崩在哪一层**：本层日志停在中途、而 RDDI 日志最后一条
还是上一次会话的 `RDDI_Close` → **崩在本层，连底层都还没碰到**。
（反向：RDDI 日志停在某条 DAP 访问上 → 崩在底层或总线。）

> 顺带一看文件大小：`ORBMDK_RDDI.log` 涨到 2.1 GB 就是这么发现的。

### 3. 分析日志的思路

1. **先找"最后一行"** —— 崩溃点之前最后一个已知位置。
2. **再找"内容不对劲的行"，而不是"报错的行"。** 本例 `no Keil pack root found`
   只是 WRN，但它解释了后面 `flash algorithm ... is not installed` 从哪来 ——
   **同一批证据要串起来读**，单看某一行会读错方向。
3. **时间戳对齐两份日志**，确定"崩之前底层动没动过"。
4. **把日志原句当锚点** —— 它是下一步在源码里搜索的 key（见 §5）。

### 4. 第三步：把 RVA 落到函数（反编译）

有了 `0x328a`，用 `dumpbin` 两步走（路径在 VS 的
`VC\Tools\MSVC\<ver>\bin\Hostx86\x86\dumpbin.exe`）。

**（a）先拿导出表，把 RVA 夹进区间** —— 不用符号也能做：

```powershell
& '<VS>\...\Hostx86\x86\dumpbin.exe' /nologo /exports '<Keil>\ARM\BIN\ORB_AGDI.dll'
```

导出按 RVA 排序，取 `0x328a` 前后两个：`AG_Init`=`0x2D00`、`AG_MemAcc`=`0x34E0`
→ **崩溃点落在 `AG_Init` 里**。

**（b）再反汇编该地址附近**：

```powershell
& '<VS>\...\Hostx86\x86\dumpbin.exe' /nologo /disasm:nobytes '<Keil>\ARM\BIN\ORB_AGDI.dll' |
  Select-String -Pattern '100032[2-9A-B][0-9A-F]:' | Select-Object -First 90 |
  ForEach-Object { $_.Line }
```

地址 = `imagebase + RVA`；本 DLL 的 imagebase 是 `0x10000000`。

**反汇编的读法**（不看符号也能读出很多）：

* 崩在 `mov eax,dword ptr [edi]` / `mov ecx,[eax+..]` → **寄存器里是坏指针**；
* **往上看是谁给这个寄存器赋值的** → 就知道参数从哪来；
* 看 `call` 的**参数个数与顺序**（`push` 几项 + `add esp,N`）→ 能认出被调函数；
* 看 `jmp dword ptr [eax*4+表地址]` → **switch 跳转表**；
  紧邻的 `add eax,0FFFFFFF6h` / `cmp eax,0Ah` 就把**分派公式**读出来了
  （本例 `(nCode & 0xFF) - 10`，即 nCode `0x0A..0x14`）。

### 5. 第四步：反汇编 ? 源码对齐（怎么找文件名与函数位置）

反汇编只能给"像什么"；**要落到具体一行，靠特征去源码里搜**：

| 反汇编 / 日志里的线索 | 在源码里搜什么 | 得到 |
|---|---|---|
| 日志原句（`Flash: RAM window =`） | **直接搜这句** | **文件:行**（最省事，日志串几乎唯一） |
| `push [esi+2C8h]`、`mov [esi+2CCh],eax` | 结构体成员名（`mainFrame` / `uvMsg`） | 哪个类 / 命名空间 |
| `call 1001D750`（3 个参数） | 被调函数名（`ui::Attach`） | 哪个文件 |
| 跳转表公式 `(nCode&0xFF)-10` | `switch (nCode & 0x00FF)` | 哪个 switch 的哪个 case |

> **最省事的一条：拿日志原文搜源码。** 日志字符串几乎都唯一，一搜就得到 `文件:行`；
> 反汇编只用来回答"崩的是哪一句、参数从哪来"。

**然后必须对照官方参考实现**（`DbgCM/` 是 Keil 原版）：

* 同一个 `case`，官方怎么写、我们怎么写，**逐行比**；
* 本例就是靠这一比出来的：官方 `hMfrm = (HWND)vp;`，
  我们 `*static_cast<HWND*>(vp);` —— **多解引用了一次**；
* ?? **`AGDI.H` 的注释可能是误导的**（本例注释写 "pointer to parent handle"，
  实际官方是**按值**传句柄）。**以官方代码为准，不以注释为准。**

### 6. 第五步：解释"为什么测试没抓到"

定位完还要回答一句：**为什么离线全绿？**

* 看 `test\` 的宿主**调了哪些入口**：`ORBAGDI_AgdiProbe.exe` 只调
  `AG_INITITEM|AG_INITCALLBACK`，**没调** `AG_INITPHANDLEP` / `AG_INITUSRMSG`；
* 于是"探针全过"与"真 uVision 一秒就崩"并不矛盾 —— **测试覆盖有洞**；
* 结论要落成**回归用例**：给探针补上这条调用，否则下次还会以同样方式漏掉。

### 7. 收尾：验证与备份

* 改完 → `build.ps1`（要 0 warning）→ `build.ps1 -DeployOnly` → `test\run-tests.ps1`；
* 部署**前后各核一次官方文件的时间戳**，确认没被覆盖；
* 目标目录已有同名产物时 `-Force` 会**静默覆盖**，先手工备份；
* 动 `<Keil>\TOOLS.INI` 这类**工作区之外**的文件前，先复制一份 `.bak`；
* 用哈希核对"被部署的那份"与"被测的那份"是不是同一个文件（`Get-FileHash`）。
