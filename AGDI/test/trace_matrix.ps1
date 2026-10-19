# trace 矩阵测试 —— 一条命令跑完并行 1/2/4 位与 SWO 两种编码，并给出判据。
#
# 原理
# ----
# 1) **抽数在 worker 线程上，但仍然要有东西可抽**（P0-A，2026-10-13 起）。
#    `ItmPump` / `OrbTracePump` 现在由 `ORBAGDI_TracePump.cpp` 的 worker 调，
#    调试线程的运行循环只采样 `IsHalted()`；worker 在 `Connect()` 起、`Disconnect()`
#    收。所以"目标不跑"依然意味着没数据 —— 这就是 `--run` 存在的理由：
#    目标侧要先被放开，ITM/ETM 才产包。
#
#    这一条以前写的是"泵只在运行循环里跑"，P0-A 之后已经不成立；判据没变。
#
# 2) **目标侧由驱动编程**。orbtrace 探针只做采集与格式转换，它不做设备端使能
#    （trace 接口只有 "Set Input Format" 和 "Set Async Baudrate" 两条控制请求）。
#    目标侧的 DEMCR/TPIU/ITM/DWT/ETM 由 `ItmProgramTarget()` 在 `Connect()` 里写，
#    顺序是"先目标、后探针"，否则中间的包会丢。
#
# 3) **三个计数器分开看**（驱动在关闭流时会打）：
#      ITM: target trace -> ...            目标侧编程结果（谁最后写赢看它）
#      orbtrace: trace 流已关闭 (字节/帧/ITM/ETM)   解码链路的产出
#      orbtrace: 读路径 调用=N 空=M 错=K    区分"端点没数据"和"读本身出错"
#
#    判据：
#      * 调用 == 空  -> 端点全程为空，探针没在发（往探针输入/接线查）
#      * 错 > 0      -> 读本身出错，不是"没数据"
#      * ITM > 0     -> ITM/SWO 解码链路通
#      * ETM > 0     -> 指令 trace 链路通
#
# 4) **cpu= 必须与目标实际主频一致**。SWO 的 TPIU 分频器由驱动按 cpu= 算，
#    不匹配时目标发的波特率和探针的解码率对不上，解出来全是垃圾。
#
# 5) **tag 直方图：探测"错位"的仪器。**（2026-10-13）
#
#    OFLOW 的 tag = **TPIU 通道号**（固件 `orbtrace/trace/tpiu.py` 的 Packetizer 把
#    channel 当包首字节）。合法值只有 1 = ITM、2 = DWT+ETM；其余 tag 一律是**错位
#    产物**，驱动只计数不解码（关流时打一行 tag 直方图，按字节排前 8）。
#
#    ⚠ **探针的 `trwidth` 必须与目标固件的 TPIU 宽度同档，否则测的只是错位。**
#    目标固件 `Core/Src/main.c` 里那两行是**编译期**常量，引脚复用也在同一段里：
#
#        #define TRACE_USE_SWO        0     // 0 = 并行，1 = SWO
#        #define TRACE_PARALLEL_BITS  4     // 并行位宽：1 / 2 / 4
#
#    改宽度 = **改宏 + 重编 + 重烧**（引脚：1 位 = PE2/PE3，2 位 + PE4，4 位 + PE5/PE6；
#    SWO 档还要把 PB3 配成 TRACESWO）。只改驱动 `trwidth` 而固件不动，目标仍按 4 位
#    发 ⇒ 探针按 1/2 位取 ⇒ 必然错位。**那种错位样本不能当"宽度对比"读。**
#
#    实测样本（固件恒为 4 位并行、只改驱动 trwidth，2026-10-13）—— 就是上面说的错位：
#
#        trwidth  ETM          ITM      其它（丢弃）
#        1 位     5.08 MB      0        505,120 帧（约 80% 帧）
#        2 位     33.37 MB     955 KB   154 帧
#        4 位     33.30 MB     963 KB   56 帧   <- 唯一与固件同档的一行
#
#    ⇒ 判据用 **`dropped` 帧占比**：同档时 < 1%（本板实测 0.01%）；错位时会到几十 %。
#      **全档位回归（1/2/4 位 + SWO NRZ/Manch 各烧一版固件）已置后** ——
#      要三版固件配合，见 Todo.md 的"置后"条；本脚本默认只跑**与固件同档**的那一档。
#      ⚠ `TPIU_FFCR.EnFCont` 的语义别读反：**0 = formatter 打开**、1 = bypass
#      （bypass 下只有 ITM/DWT 通过，ETM 被 TPIU 丢弃）。本板读回 0 ⇒ formatter 开着，
#      不需要动它。
#
# 用法
# ----
#     powershell -ExecutionPolicy Bypass -File test\trace_matrix.ps1
#     powershell -ExecutionPolicy Bypass -File test\trace_matrix.ps1 -RunMs 5000 -CpuKhz 48000
#
#     固件不是 4 位并行时（比如烧了 TRACE_USE_SWO=1 那一版）：
#     powershell -ExecutionPolicy Bypass -File test\trace_matrix.ps1 -FwKind swo -FwBaud 2000000
#
#     全档位扫描（**必须先按档重烧固件**，否则产出的全是错位样本）：
#     powershell -ExecutionPolicy Bypass -File test\trace_matrix.ps1 -SweepAll
#
# 前置：目标固件已烧录，且其 trace 使能正确（见 COMPAT_ANALYSIS.md §10.15）。
param(
    [int]$RunMs    = 3000,
    [int]$CpuKhz   = 48000,
    [int]$ClockKhz = 1000,

    # 目标固件当前那两行的实际取值（main.c 的 TRACE_USE_SWO / TRACE_PARALLEL_BITS）。
    # 默认就是仓库里现在这一版：并行 4 位。
    [ValidateSet('parallel','swo','manch')][string]$FwKind = 'parallel',
    [ValidateSet(1,2,4)][int]$FwBits = 4,
    [int]$FwBaud = 2000000,

    # 全档位扫描。**只有把固件按每档重烧过才开**，否则结果不可读（见 §5）。
    [switch]$SweepAll,

    [string]$Exe   = ""
)

$ErrorActionPreference = 'Continue'
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $TestDir
if (-not $Exe) { $Exe = Join-Path $TestDir 'bin\ORBAGDI_AgdiProbe.exe' }

if (-not (Test-Path $Exe)) {
    Write-Host "[ERROR] probe not built: $Exe" -ForegroundColor Red
    Write-Host "        run test\build.ps1 first" -ForegroundColor Red
    exit 1
}

$LogFile = Join-Path $env:TEMP 'ORB_AGDI.log'

# 每一档：并行 1/2/4 位，以及 SWO 的 NRZ / Manchester 两种编码。
# trswo=0 走并口（trwidth 有效）；trswo=1/2 走 SWO 单引脚（trwidth 无意义）。
#
# `Match` = 该档与**当前烧在目标里的固件**是否同档。只有同档的档位才是在测解码链路，
# 其余档测的是"探针按 N 位取 4 位流"的错位（见 §5）—— 所以默认不跑。
$matchOpt = switch ($FwKind) {
    'parallel' { "trswo=0 trwidth=$FwBits" }
    'swo'      { "trswo=1 trbaud=$FwBaud" }
    'manch'    { "trswo=2 trbaud=$FwBaud" }
}
$matchName = switch ($FwKind) {
    'parallel' { "parallel $FwBits-bit (fw)" }
    'swo'      { "SWO NRZ (fw)" }
    'manch'    { "SWO Manch (fw)" }
}

$cases = @( @{ Name = $matchName; Opt = $matchOpt; Match = $true } )

if ($SweepAll) {
    $cases = @(
        @{ Name = 'parallel 1-bit';   Opt = 'trswo=0 trwidth=1';      Match = ($FwKind -eq 'parallel' -and $FwBits -eq 1) },
        @{ Name = 'parallel 2-bit';   Opt = 'trswo=0 trwidth=2';      Match = ($FwKind -eq 'parallel' -and $FwBits -eq 2) },
        @{ Name = 'parallel 4-bit';   Opt = 'trswo=0 trwidth=4';      Match = ($FwKind -eq 'parallel' -and $FwBits -eq 4) },
        @{ Name = 'SWO NRZ 2Mbaud';   Opt = 'trswo=1 trbaud=2000000'; Match = ($FwKind -eq 'swo') },
        @{ Name = 'SWO Manch 2Mbaud'; Opt = 'trswo=2 trbaud=2000000'; Match = ($FwKind -eq 'manch') }
    )
    Write-Host '[warn] -SweepAll: 每档都要求固件同步重烧（main.c 的 TRACE_USE_SWO /' -ForegroundColor Yellow
    Write-Host '       TRACE_PARALLEL_BITS）；异档产出的只是错位样本，见文件头 §5。' -ForegroundColor Yellow
    Write-Host ''
}

Write-Host "trace matrix  (run=${RunMs}ms, cpu=${CpuKhz}kHz, clock=${ClockKhz}kHz)"
Write-Host ("probe: {0}" -f $Exe)
Write-Host ''
Write-Host ("{0,-20} {1,12} {2,10} {3,11} {4,12} {5,9} {6,7}  {7}" -f `
            'case','bytes','frames','ITM','ETM','otherFr','drop%','verdict')
Write-Host ('-' * 96)

$fail = 0
foreach ($c in $cases) {
    Remove-Item $LogFile -Force -ErrorAction SilentlyContinue

    # rstcon=1 stoprst=0：每档先复位目标并让它跑起来。
    #
    # 这一条是**判据的一部分**，不是可选项：没有它，每档的起点都不一样，
    # 而"目标没在跑"时 trace 为 0 是**结果**不是原因 —— 很容易被误读成
    # "某种宽度/编码不通"。实测加上它之后，三档并口的结果才互相自洽。
    $opts = "clock=$ClockKhz iface=0 orbtrace=1 cpu=$CpuKhz rstcon=1 stoprst=0 " +
            "pcperiod=1 pcpresc=0 " + $c.Opt
    & $Exe --options $opts --run $RunMs 2>&1 | Out-Null

    # 锚点用 ASCII（'orbtrace: trace'）—— 日志正文是中文，写成英文模式匹配不上。
    $text = Get-Content $LogFile -Encoding UTF8 -ErrorAction SilentlyContinue
    $line = ($text | Select-String -Pattern 'orbtrace: trace' | Select-Object -Last 1).Line

    # 关闭那一行同时带出七个计数；中文在部分控制台上会乱码，所以按数字位置解析。
    #   1 字节  2 帧  3 校验错  4 COBS错  5 ITM  6 ETM  7 其它(帧)
    $bytes = 0; $frames = 0; $itm = 0; $etm = 0; $otherFr = 0
    if ($line -match '\(.*?=(\d+).*?=(\d+).*?=(\d+).*?=(\d+).*?=(\d+).*?=(\d+).*?=(\d+)') {
        $bytes   = [int64]$Matches[1]
        $frames  = [int64]$Matches[2]
        $itm     = [int64]$Matches[5]
        $etm     = [int64]$Matches[6]
        $otherFr = [int64]$Matches[7]
    }

    # tag 直方图（只打前 8 条，所以这里的字节数是**下界**；判据用帧比例，那是精确的）。
    $otherB = 0
    $topTag = ''
    $hist = ($text | Select-String -Pattern 'tag \d+: \d+ frames / \d+ B' -AllMatches)
    if ($hist) {
        $ms = [regex]::Matches(($hist | ForEach-Object { $_.Line }) -join ' ',
                               'tag (\d+): (\d+) frames / (\d+) B')
        foreach ($m in $ms) { $otherB += [int64]$m.Groups[3].Value }
        if ($ms.Count -gt 0) {
            $topTag = ("top tag {0} ({1} B)" -f $ms[0].Groups[1].Value, $ms[0].Groups[3].Value)
        }
    }

    $dropPct = 0
    if ($frames -gt 0) { $dropPct = [math]::Round(100.0 * $otherFr / $frames, 1) }

    # 判据（见文件头 §5）：不认识 tag 的比例就是"错位"的量度。
    # 异档（探针宽度/编码与固件不一致）**必然**错位或没数据，单独标注、不算失败 ——
    # 它不是被测对象，只是给 -SweepAll 留个"这一档没配固件"的可见标记。
    $offFw = (-not $c.Match)
    $verdict = 'no data at all'
    $colour  = 'Red'
    if ($frames -eq 0) {
        if ($offFw) { $verdict = 'off-firmware: no data (expected)'; $colour = 'DarkGray' }
        else        { $verdict = 'no data at all';                   $colour = 'Red'; $fail++ }
    } elseif ($dropPct -gt 10) {
        if ($offFw) { $verdict = 'off-firmware: scattered (expected)'; $colour = 'DarkGray' }
        else        { $verdict = 'scattered tags - framing broken';    $colour = 'Red'; $fail++ }
    } elseif ($itm -gt 0 -and $etm -gt 0) { $verdict = 'ITM + ETM ok'; $colour = 'Green' }
    elseif ($itm -gt 0)                   { $verdict = 'ITM ok';       $colour = 'Green' }
    elseif ($etm -gt 0)                   { $verdict = 'ETM only';     $colour = 'Yellow' }
    else                                  { $verdict = 'frames but no ITM/ETM'; $colour = 'Yellow' }

    Write-Host ("{0,-20} {1,12} {2,10} {3,11} {4,12} {5,9} {6,6}%  {7}" -f `
                $c.Name, $bytes, $frames, $itm, $etm, $otherFr, $dropPct, $verdict) `
               -ForegroundColor $colour
    if ($topTag) { Write-Host ("{0,-20} {1}" -f '', $topTag) -ForegroundColor DarkGray }
}

Write-Host ''
Write-Host 'pinned (2026-10-13): tag = TPIU channel; legal values are 1 (ITM) and 2 (DWT+ETM).'
Write-Host '  Any other tag is a framing artefact.  The only cause seen so far is a probe'
Write-Host '  width/encoding that does NOT match the target firmware:'
Write-Host '      main.c  TRACE_USE_SWO=0 + TRACE_PARALLEL_BITS=4   <->   trswo=0 trwidth=4'
Write-Host '  On mismatch ~80% of frames land on bogus tags (or nothing arrives at all),'
Write-Host '  so it measures the mismatch, not the link.  A real width/encoding sweep needs'
Write-Host '  a firmware rebuild per slot -- deferred (see Todo.md, "置后").'
Write-Host '  Keep the probe in the SAME slot as the firmware.  TPIU_FFCR.EnFCont: 0 = formatter ON.'
Write-Host ''
if ($fail -eq 0) {
    Write-Host 'all cases produced trace data' -ForegroundColor Green
    exit 0
}
Write-Host ("{0} case(s) produced nothing - see COMPAT_ANALYSIS.md 10.15 for the" -f $fail) -ForegroundColor Yellow
Write-Host 'known-open items (SWO NRZ trickles, Manchester silent).'
exit 1
