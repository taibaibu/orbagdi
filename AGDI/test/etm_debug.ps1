<#
    etm_debug.ps1 - 一条命令跑完"捕获 -> 离线解码 -> A/B 两种编码 -> 判定"的闭环。

    为什么要它（用户原话："你来自动化处理这个问题吧" / "不要再 keil 上，运行一次太慢"）：
    真机上迭代一次要开 uVision、连探针、等那个 "Cannot access target hardware" 恢复框，
    而解码出垃圾地址这件事**只需要一段字节流**。所以：

        捕获（走 ORBAGDI_AgdiProbe.exe，**不开 Keil**）
          -> etm_replay.exe 离线解两遍（--alt 1 / --alt 0）
            -> 对比 + 判定

    用法：
        .\test\etm_debug.ps1                    # 抓 5 秒，然后离线解
        .\test\etm_debug.ps1 -RunMs 10000
        .\test\etm_debug.ps1 -Cap D:\cap.orb    # 用已有的捕获，不连硬件

    退出码：0 = 两种编码都没解出越界地址（干净）；1 = 有问题；2 = 环境问题（没捕获到）。
#>
param(
    [int]$RunMs      = 5000,
    [int]$CpuKhz     = 48000,
    [int]$ClockKhz   = 1000,
    [string]$Cap     = "",
    [string]$Probe   = "",
    [string]$Replay  = ""
)

$ErrorActionPreference = 'Continue'
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Probe)  { $Probe  = Join-Path $TestDir 'bin\ORBAGDI_AgdiProbe.exe' }
if (-not $Replay) { $Replay = Join-Path $TestDir 'bin\etm_replay.exe' }
if (-not $Cap)    { $Cap    = Join-Path $env:TEMP 'etm_debug_cap.orb' }

$script:fail = 0
function Head($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok  ($m) { Write-Host "    PASS  $m" -ForegroundColor Green }
function Bad ($m) { Write-Host "    FAIL  $m" -ForegroundColor Red; $script:fail++ }
function Note($m) { Write-Host "    $m" -ForegroundColor Gray }

foreach ($p in @($Probe, $Replay)) {
    if (-not (Test-Path $p)) {
        Write-Host "[ERROR] not built: $p`n        run test\build.ps1 first" -ForegroundColor Red
        exit 2
    }
}

# ---------------------------------------------------------------------------
# 1. 捕获（除非用户直接给了文件）
# ---------------------------------------------------------------------------
if (Test-Path $Cap) {
    Note "using the existing capture: $Cap"
    $haveCapture = $true
} else {
    Head "1. capture ($RunMs ms, via the probe - no Keil)"
    Remove-Item $Cap -Force -ErrorAction SilentlyContinue
    # ⚠⚠ 这套键是照**用户真机那份能出数据的选项串**抄的，别自己精简。踩过的坑：
    #   漏了 `etmen=1` ⇒ 驱动打 "ETM 未使能（Trace 页 'ETM Trace Enable' 没勾），
    #   不配 ETM" ⇒ 整段捕获里 ETM 字节 = 0 ⇒ 离线工具报 "no ETM payload"，
    #   看着像"目标没出 trace"，其实是**我们没让驱动去配 ETM**。
    #   `tclk`/`tclkc` 也必须带：§8.5 的怀疑对象正是 trace 时钟，
    #   不带上就不是同一个现场了。
    #   pcperiod=0：PC 采样与这个问题无关（真机 A/B 已证明），少一路噪声。
    $opt = "clock=$ClockKhz iface=0 orbtrace=1 trwidth=4 trswo=0 cpu=$CpuKhz " +
           "tclk=$CpuKhz tclkc=1 trbaud=2000000 trtpui=1 " +
           "rstcon=1 stoprst=0 etmen=1 ts=1 tspresc=64 tswin=1 trunlim=1 " +
           "events=0x3F0 pcperiod=0 trfile=$Cap trkeep=0"
    # --reset：uVision 开会话时会发这一发，让目标重跑自己的启动流程 ⇒ 固件自己的
    #   trace 初始化才执行。**不发的话 ETM 配好了也只吐空闲码**：实测 369 字节全是
    #   `BC`、0 个分支；uVision 下同一个目标是 116k 分支。
    $out = & $Probe --options $opt --reset --run $RunMs 2>&1
    $out | Select-String -Pattern 'ran \d+ ms|FAIL|error' | Select-Object -Last 6 |
        ForEach-Object { Note $_.Line }

    $haveCapture = Test-Path $Cap
    if (-not $haveCapture) {
        Bad "no capture file was produced ($Cap)"
        Note "the probe could not start a run - see the log and its output above"
        exit 2
    }
    Note ("capture: {0:N0} bytes" -f (Get-Item $Cap).Length)
}

# ---------------------------------------------------------------------------
# 2. 离线解两遍（A/B 分支地址编码）
# ---------------------------------------------------------------------------
function Invoke-Replay([int]$Alt) {
    Head ("2.{0} offline decode  --alt {0}" -f $Alt)
    $out = & $Replay $Cap --alt $Alt --wild 6 2>&1
    $txt = @($out)
    $txt | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

    # 判定只用 **ASCII 锚点**，不看行首、不碰中文。
    #
    # 两个坑（2026-10-14 都踩到了，症状是"解出 484199 个干净地址却报
    # no addresses decoded at all"）：
    #   1. 重放工具输出 UTF-8，PowerShell 按本机代码页解码 ⇒ 它打出来的中文全是花的
    #      （`地址` 变成 `鍦板潃`），拿 `地址` 去 match 永远不中。
    #   2. `$out` 若被当成一整块文本，带 `^` 的行首锚点只在第一行生效。
    # 所以：拼成一段、用 `-match`（无锚点）、只认 `PASS <数字>` / `FAIL <n> / <m>`。
    $all = $txt -join "`n"
    $passN = if ($all -match 'PASS\s+(\d+)') { [int]$Matches[1] } else { -1 }
    $wild  = if ($all -match 'FAIL\s+(\d+)\s*/\s*(\d+)') { $Matches[0] } else { '' }

    if ($wild) {
        Bad ("alt={0}: {1}" -f $Alt, $wild.Trim())
    } elseif ($passN -ge 0) {
        Ok ("alt={0}: {1} addresses, none out of range" -f $Alt, $passN)
    } else {
        Bad ("alt={0}: no addresses decoded at all" -f $Alt)
    }
}

$r1 = Invoke-Replay 1     # 与驱动默认一致
$r0 = Invoke-Replay 0

# ---------------------------------------------------------------------------
# 3. 判定
# ---------------------------------------------------------------------------
Head '3. verdict'
Note 'read the two blocks above:'
Note '  * one alt clean, the other wild  -> the ENCODING is wrong (set ETM Alt accordingly)'
Note '  * BOTH wild                      -> the byte stream itself is broken'
Note '    (check the frame layer: bad sum / COBS errors; 2026-10-14 实测约 1% 的帧坏)'
Note '  * no ETM payload at all          -> the target produced no trace'
Note '    (the firmware must be running and must have programmed its own trace hardware)'

if ($script:fail -gt 0) {
    Write-Host ''
    Write-Host "FAILED  ($($script:fail) problem(s))" -ForegroundColor Red
    exit 1
}
Write-Host ''
Write-Host 'ALL PASSED' -ForegroundColor Green
exit 0
