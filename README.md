---
# ORBAGDI

An open CMSIS-DAP AGDI target driver for Keil uVision / µVision (MDK).

uVision loads **AGDI** ("ARM Generic Debug Interface") DLLs as target drivers.
This project builds `ORB_AGDI.dll`, an AGDI driver for CMSIS-DAP probes that
talks to the probe through the **RDDI** host interface (`CMSIS_DAP.dll`, the
same interface elaphureLink and Keil's own driver use).

* Build product: `bin\ORB_AGDI.dll` (32-bit, `/MT` static CRT)
* Deployed as: `<Keil>\ARM\BIN\ORB_AGDI.dll`
* Target: Cortex-M (ARMv6-M / ARMv7-M / ARMv8-M mainline)
* Release: **0.2.0** — version lives in `include\ORBAGDI_Version.h`, is compiled
  into the DLL's `VERSIONINFO` (`ORBAGDI.rc`) and printed in the session banner
  (`ORBAGDI 0.2.0 - …`). History in §7.

---

## 1. Build

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

The script locates the toolchain on its own (`vswhere`, then the standard
install folders, then the registry for the Windows SDK), so no "Developer
Command Prompt" is required. It mirrors the ORBMDK build script.

| Flag | Effect |
| --- | --- |
| *(none)* | build `bin\ORB_AGDI.dll` |
| `-Clean` | wipe `obj\` and `bin\` first |
| `-Deploy` | build, then copy to `<Keil>\ARM\BIN\ORB_AGDI.dll` |
| `-DeployOnly` | copy an already built DLL into `ARM\BIN` |

Compile flags: `x86`, `/O2`, `/MT`, `/std:c++17`, `/utf-8`, `/W3`, `/EHsc`.
`/MT` matters: uVision ships an old `MSVCP140.dll`, so linking the static CRT
avoids a DLL-hell fight with the IDE.

All sources under `src\*.cpp` are compiled; `DbgCM\` and `TraceView\` are Keil's
reference implementations kept for study and are **not** part of the build.

## 2. Install into Keil

Copy `bin\ORB_AGDI.dll` to

```
<Keil>\ARM\BIN\ORB_AGDI.dll
```

`build.ps1 -Deploy` does this. Note the driver is deliberately **not** installed
as `CMSIS_AGDI.dll`: Keil's own driver is left in place and ORB_AGDI sits next
to it, so the two can be switched between in uVision without a reinstall.

### 2.1 Register in TOOLS.INI

uVision only offers a debugger that is registered in `<Keil>\TOOLS.INI`. Add the
driver under the next free index in the `[ARM]` section. On the reference machine
`<Keil>` is `D:\MDK5` and `TDRV0`…`TDRV20` are all taken, so the first free index
is `TDRV21`:

```ini
[ARM]
PATH="D:\MDK5\ARM\"                            # BIN\ below is relative to this
TDRV21=BIN\ORB_AGDI.dll("ORBTRACE Debugger")
```

`TDRV` paths are relative to the section's `PATH=`, i.e. the `[ARM]` folder — not
the Keil root — so `BIN\ORB_AGDI.dll` resolves to `<Keil>\ARM\BIN\ORB_AGDI.dll`,
which is exactly where `-Deploy` puts it.

Note the `TDRV` list in parentheses on the `CPUDLL` lines:
`CPUDLLn=<deviceDB>.DLL(TDRV…)` binds a set of debug drivers to the device
families covered by that device database DLL, and that set is what filters the
Debug dropdown. A `TDRV` that is registered but missing from the matching
`CPUDLL` line may therefore not appear for the target device. For Cortex-M that
is `CPUDLL1`:

```ini
CPUDLL1=SARMCM3.DLL(TDRV1,…,TDRV16,TDRV21)     # Cortex-M devices
```

`CPUDLL3=SARMV8M.DLL(…)` is the ARMv8-M list and is deliberately left alone —
per §D2 the driver does not handle ARMv8-M / TrustZone yet. This mirrors how Keil
wires its own pair: `CMSIS_AGDI.dll` (`TDRV12`) sits in `CPUDLL1`,
`CMSIS_AGDI_V8M.DLL` (`TDRV19`) in `CPUDLL3`.

Keil ships the `[ARM]` entries a second time in a near-identical `[ARMADS]`
section, `TDRV0`…`TDRV20` in both. Which of the two uVision 5.43 actually reads
was not determined here, so keep them in sync — and use the **same free index in
both**. Reusing an occupied index (e.g. `TDRV15`, the ULINKplus entry) silently
replaces that official debugger instead of adding one.

Finally pick the driver for the target: **Options for Target → Debug**, choose
the ORBTRACE entry, and point the RDDI host at `CMSIS_DAP.dll` (see §5 for the
override variable). uVision hands the driver its own file name in
`dbgblk::TargDll`; `EnumUvARM7(2, pdbg)` returning `7` is what registers it.

## 3. Layer overview

```
uVision  ──AGDI──▶  ORBAGDI_Agdi.cpp   export surface (AG_Init, AG_GoStep, …)
                          │
                          ├── ORBAGDI_Core.cpp     Cortex-M core: attach, reset,
                          │                        run/halt/step, DCRSR/DCRDR regs
                          ├── ORBAGDI_Break.cpp    FPB hardware BPs + BKPT software BPs
                          │                        (DWT data watchpoints)
                          ├── ORBAGDI_Flash.cpp    FlashOS: FLM loader, page assembly,
                          │                        on-target algorithm scheduling,
                          │                        Flash / Erase and Flash / Run
                          ├── ORBAGDI_Ui.cpp       key information into uVision's
                          │                        command / build window (not the log)
                          ├── ORBAGDI_TraceWin.cpp uVision Trace Data window (TDI):
                          │                        TRACE_RECORD_64 ring, the
                          │                        UV_TRACE_IF_INIT / _WIN_CONFIG setup
                          └── ORBAGDI_Dap.cpp      DP/AP registers, MEM-AP memory access
                                      │
                                      └── ORBAGDI_Rddi.cpp   dynamic RDDI host binding
                                                  │
                                                  └── RDDI_DAP.dll  (Keil / probe vendor)
```

Trace has **three mutually exclusive sources** (driver options pick one) and they
all converge on the same uVision reporting path:

```
  source                acquisition                          decode              report
  ─────────────────────────────────────────────────────────────────────────────────────
  SWO pin      swo=<Hz>  RDDI CMSIS_DAP_SWO_*        ─┐
  on-chip ETB  etb=1     MEM-AP RAM read-back        ─┼─▶ ITM decoder      ─┐
  orbtrace     orbtrace=1 RDDI TraceStream_Read     ─┘   (ORBAGDI_Itm)     │
                          └ ORBAGDI_Oflow (COBS/OFLOW)                      ├─▶ AG_Serial
                                                                            │   (AG_CB_ITMDATA)
  ETB formatter / orbtrace tag 2 ─▶ ORBAGDI_Etm (shared sink) ──────────────┤
                                                                            └─▶ ORBAGDI_TraceWin
                                                                                (TDI records)
```

| File | Role |
| --- | --- |
| `include\ORBAGDI.h` | Public header. Defines `_IN_TARG_` so `AGDI.H` declares every entry point as `__declspec(dllexport)`, then includes `AGDI.H`. |
| `include\ORBAGDI_Rddi.h` | RDDI function table (incl. the optional `CMSIS_DAP_ConfigureDebugger` / `CMSIS_DAP_SWO_*`), DAP register numbers, `rddi::Load/Unload/Get`. |
| `include\ORBAGDI_Dap.h` | DP/AP register access, MEM-AP block memory access (auto-incrementing TAR). |
| `include\ORBAGDI_Core.h` | Attach/detach, reset, halt/resume/step, `RgARMCM` register block. |
| `include\ORBAGDI_Break.h` | Breakpoint table: FPB comparators first, BKPT patching after, DWT watchpoints alongside. |
| `include\ORBAGDI_Flash.h` | Flash-download entry points called from `AG_Init(AG_INITFLASHLOAD/STARTFLASHLOAD)`. |
| `include\ORBAGDI_Itm.h` | SWO capture, ITM decode and the `AG_HistFunc` PC-sample history. |
| `include\ORBAGDI_Etb.h` | ETB/TMC capture: ROM-table discovery, setup/activate, RAM read-back. |
| `include\ORBAGDI_Etm.h` | ETM v3.5/v4 branch decoder (skeleton; ported from `orbrddi-main`) + the shared ETM sink both ETB and orbtrace feed. |
| `include\ORBAGDI_OrbTrace.h` | orbtrace probe trace source (RDDI `OrbTrace_*` / `TraceStream_*`). |
| `include\ORBAGDI_Oflow.h` | OFLOW/COBS deframer — the wire format orbtrace sends (orbuculum). |
| `include\ORBAGDI_TraceWin.h` | uVision Trace Data window upload path (Keil TDI). |
| `include\ORBAGDI_Log.h` | `ORB_LOG_*` macros writing to `%TEMP%\ORB_AGDI.log`. |
| `include\ORBAGDI_Ui.h` | `ui::Info/Warn/Error/Fatal`: key information into uVision's command / build window (the `SendMessage(mainFrame, Uv2Msg, 0x0C|0x06, text)` channel). |
| `include\ORBAGDI_Config.h` | Driver option string (`rddi=`, `iface=`, `clock=`, `port=`, `ap=`, `swo=`, `cpu=`, `orbtrace=`, …) and `cfg::ShowDialog`. |
| `src\ORBAGDI_Agdi.cpp` | Every `AG_*` export plus `EnumUvARM7` / `DllUv3Cap`. |
| `src\ORBAGDI_Flash.cpp` | FlashOS: ELF32 FLM loader, sector-table validation, `CallAlg` register marshalling, page assembly, erase/program/verify passes, Flash / Erase, Flash / Run. |
| `src\ORBAGDI_Itm.cpp` | Raw SWO drain, ITM packet decoder, `AG_Serial(AG_ITMBOUT)` forwarding. |
| `src\ORBAGDI_Etb.cpp` | CoreSight ROM scan, ETB formatter unpack, `RD_DATA` non-incrementing read. |
| `src\ORBAGDI_Etm.cpp` | ETM decode state machines + the shared ETM sink (`EtmTraceFeed`). |
| `src\ORBAGDI_OrbTrace.cpp` | orbtrace probe: detect, configure, drain `TraceStream_Read`, dispatch by tag. |
| `src\ORBAGDI_Oflow.cpp` | Incremental COBS + OFLOW deframer (byte-for-byte port of orbuculum). |
| `src\ORBAGDI_TraceWin.cpp` | TDI callback table + `TRACE_RECORD_64` ring + window description. |
| `src\ORBAGDI_Ui.cpp` | The reporting channel: `SendMessage` into uVision's command / build window, with `AG_CB_MSGSTRING` as the fallback. |
| `src\ORBAGDI_Config.cpp` | Parses and formats the option string uVision stores per project. |
| `src\ORBAGDI_Settings.cpp` | `DllUv3Cap(1)` / `(110)`: the modal Settings sheet — Debug / Trace / Flash / Pack / Info — built from a run-time `DLGTEMPLATE`, including the RDDI bench probe and the layout self-check. |
| `src\ORBAGDI_Dll.cpp` | `DllMain`; tears the RDDI session down on unload. |

## 4. Implemented AGDI surface

Exported (checked against `dumpbin /exports`):

```
AG_Init  AG_MemAtt  AG_BpInfo  AG_BreakFunc  AG_GoStep  AG_Serial
AG_MemAcc  AG_RegAcc  AG_AllReg  AG_MemAccX  AG_DapAcc
AG_BreakResAlloc  AG_HistFunc  EnumUvARM7  DllUv3Cap
```

Notable behaviour:

* `AG_Init(AG_INITFEATURES)` opens the RDDI session and attaches to the core.
  A non-zero return makes uVision drop the driver, which is the correct outcome
  when no probe is present.
* `AG_GoStep(AG_GOFORBRK/AG_GOTILADR)` resumes the target and blocks until it
  halts, so breakpoints installed through `AG_BpInfo` actually stop it.
  `AG_STOPRUN` / `AG_Init(AG_STOPREQ)` unblock that wait from uVision's thread.
* `AG_BpInfo` mirrors uVision's per-address attribute word; `ATRX_BREAK` means
  "breakpoint installed here"; `ATRX_WATCH` routes data watchpoints to DWT
  comparators (`AG_F_EXTBRKSUPP` is 1).
* `AG_Init(AG_INITFLASHLOAD/AG_STARTFLASHLOAD)` drives flash download: the FLM
  image uVision hands over is loaded into target RAM, its sector table is
  validated, and its `Init/EraseSector/ProgramPage/Verify` are invoked through
  `DCRSR`.  Image segments are assembled into whole pages before `ProgramPage`,
  because a segment routinely starts and ends mid-page.  `AG_MemAcc(AG_F_ERASE)`
  and `AG_MemAcc(AG_F_RUN)` answer uVision's Flash / Erase and Flash / Run.
* `AG_Serial(AG_ITMBOUT, …)` forwards decoded ITM stimulus to uVision's ITM
  viewers via `AG_CB_ITMDATA`; `AG_HistFunc` returns the DWT PC-sample history.
  Both need a trace source: `swo=<Hz>` (SWO pin), `etb=1` (on-chip ETB) or
  `orbtrace=1` (the probe's own trace endpoint); off by default.
* The **Trace Data window** is fed through the Trace Data Interface (TDI).
  `ORBAGDI_TraceWin.cpp` registers the callback table with uVision through the
  BOM and answers its record queries; ITM stimulus, DWT PC samples and decoded
  ETM branches all land in one `TRACE_RECORD_64` ring.  It is registered
  automatically once the BOM is available — when it is not, the ITM / printf
  viewers still work and only this window stays dark.
* `etb=1` captures into the on-chip ETB instead of the SWO pin. The ETB is
  located by scanning the CoreSight ROM table, read back over the MEM-AP
  (`ORBAGDI_Etb.cpp`) and its formatter output is fed to the same ITM decoder,
  so the ITM viewers and the PC-sample history work unchanged. Capture is armed
  around `AG_GoStep` and read out once the target halts.
* `AGDIMsgBox` is provided (declared by `AGDI.H`, not exported).

## 5. Logging

A log file is written to `%TEMP%`:

```
%TEMP%\ORB_AGDI.log
```

The folder comes from `GetTempPathW`, i.e. `TMP`, then `TEMP`, then
`USERPROFILE`. It is deliberately *not* the DLL's own folder: that is
`<Keil>\ARM\BIN`, a tree belonging to uVision which may well be read-only, and a
log that silently fails to open is worse than no log at all — the log is the
only place `AG_Init` spells out why it refused to start.

Set the environment variable `ORBAGDI_LOG_LEVEL` to `0` (silent) … `4` (debug)
to change verbosity. The default is `3` (info). This is the first thing to check
when uVision refuses to start a debug session.

The Settings sheet's **Info** page shows the last 12 kB of the same file, so the
reason a session misbehaved is readable without leaving uVision.  `ORBAGDI_LOG_LEVEL=0`
also silences the log tail, because then there is no file to show.

### Reporting key information to uVision

A log file is not where a user looks.  `ORBAGDI_Ui.cpp` is the other half: the
dozen facts that answer "is it looking at the right chip, through the right probe,
with the right clock", pushed into a window uVision already has open.

The channel is the one Keil's own drivers use.  `AG_Init(AG_INITITEM)` hands over
`AG_INITPHANDLEP` (the main frame) and `AG_INITUSRMSG` (a message registered with
`RegisterWindowMessage`), and a line of text is then

```c
SendMessage(mainFrame, uvMsg, 0x0C /* or 0x06 while flashing */, text);
```

Those two wParam values are **not** in AGDI.H.  They were read out of Keil's own
driver — `ULP2CM3.dll`'s `txtout()` at VA `0x1008B1B0` — with
`tools/agdi_out_channels.py`; `ORBAGDI_Ui.h` carries the disassembly.  The call is
in-process (this DLL runs inside uVision), so the text pointer needs no
marshalling.  Before the handles arrive, or after `AG_UNINIT`, the report falls
back to `AG_CB_MSGSTRING`, the documented channel into the same message pane.

| Reported | Where |
| --- | --- |
| driver version, device, flash window, RDDI host, port / interface / clock / AP, trace source, flash algorithm, PDSC status | the session banner, once, after connect |
| every flash failure | `Flash::Fail()`, one place, covers all of them |
| which algorithm was loaded, for which device, page size | after `AG_INITFLASHLOAD` |
| `download finished OK`, chip erase result | end of each download |
| "cannot connect — check the probe / interface / SWJ clock" | before uVision gives up on the driver |

Everything else — per-access traces, retry loops, packet dumps — stays in the log
file, which is why `ORB_LOG_*` is still the right call for it.  Each `ui::` report
also goes to the log, so a line that scrolled past on screen is still recoverable.

## 6. Known limitations

See `bug.md` for the tracked list, `Todo.md` for what is still open, and
`COMPAT_ANALYSIS.md` §9.11 for the per-dimension completeness figures.

**Verified on real hardware** (ORBTrace probe + STM32F1 / F407, SWD): the Settings
sheet — all seven pages, layout and write-back; the RDDI session bring-up; the
orbtrace **parallel** trace port at 1 / 2 / 4 bits; the orbtrace **SWO NRZ**
source at 1 Mbaud; and the flash programming chain (FLM load, page assembly,
erase / program / verify), exercised by `test\ORBAGDI_AgdiProbe.exe --flash`,
which byte-compares the whole image back out of the target.

**Implemented but not yet exercised on real hardware**: the ITM console path into
uVision's own printf window, the Trace Data window (TDI), the execution history,
watchpoint hits, the PDSC debug-sequence path, and uVision's own Download flow.

**Not implemented**: instruction-level trace — the history is statistical DWT PC
sampling; the ETM decoder is a skeleton fixed at v3.5 while Cortex-M7 needs v4;
Exception Trace / Event Counters and real timestamps in the Trace window; SWO
Manchester; CoreSight ROM-table recursion, multi-device JTAG and SWD multi-drop;
Debug Recovery policy; and Flash multi-algorithm support.

The flash path covers Keil's FlashOS programming chain but still takes a single
algorithm per project (no `FlashConf.Dev[NFlash]` list), has no built-in
`BlankCheck_ARM` / `CRC32_ARM` images, and no CRC-based verify. There is no
memory-access-while-running (`AG_F_MEMACCR` is advertised as 0 for that reason).

The option string carries far more keys than the driver acts on — `bug.md` §I lists
them key by key.  The Settings sheet only offers the ones that do something, and its
**Info** page repeats the two lists, so the answer is visible without the docs.

### The Settings sheet

`DllUv3Cap(1)` / `DllUv3Cap(110)` open a five-page sheet whose layout, sizes, font
and control styles are taken from Keil's own ULINK Pro driver: a 412x240
container, a 404x212 `SysTabControl32` (a 400x188 display area), `Microsoft Sans
Serif` 8pt, group boxes with a right-aligned "Key:" column, and a dropdown for
every enumerated option.

| Tab | Contents |
| --- | --- |
| **Debug** | the **probed** interface list with the adapter's serial / firmware / vendor beside it, **Port** (SWD first), the `SWJ-DP` switch (greyed when Port is JTAG), the **Clock** rate list, the **probed** AP list (`0: AHB-AP (0x24770011)`); Connect / Reset modes; cache and download switches |
| **Trace** | the **source selector** (Off / SWO pin / on-chip ETB / orbtrace probe) plus the ITM/DWT half and the orbtrace half.  Whichever half the source does not use is greyed out |
| **Flash** | erase mode, program / verify / reset-and-run, RAM for the algorithm, the `.FLM` and **Detect from Keil** |
| **Pack** | the debug description: enable, sequence log |
| **Info** | the scrollable text box: the device uVision reported, pack roots, where the RDDI host was found, every message the sheet raised, and the tail of the driver log |

Three things the sheet deliberately does *not* do:

* **It never asks for the RDDI host DLL.**  `CMSIS_DAP.dll` is auto-discovered
  (`$ORBAGDI_RDDI_DLL`, then next to this DLL, then `<Keil>\ARM\BIN`) and merely
  reported; when none is found that is an error on the **Info** page.
* **It never hard-codes the interface or AP lists.**  Both are probed through the
  RDDI layer — `CMSIS_DAP_Detect` + `CMSIS_DAP_Identify` for the interfaces,
  `DP SELECT` + AP IDR reads for the APs — so the choices are what is really on
  the bench.  The AP probe deliberately touches neither `nRESET` nor the SWJ
  clock, so opening the sheet cannot reset a target or re-time a live session.
* **It never reports a problem anywhere but Info.**  Every message goes through
  one `Report()` path (log + Info page + scroll to the bottom), and a rejected
  value switches the sheet to Info rather than raising a modal box.

The four buttons along the bottom (`Defaults` / `Test` / `Log` / `Power`) are
placeholders for future modules — orbtrace's Vtref / Vpwer power outputs, for
instance — and currently change nothing.

Every control the sheet *does* offer is acted on by the driver (`bug.md` §J), and
the only greyed controls are the halves a choice cannot use: the Trace half the
chosen source does not drive (`bug.md` §K9), and `SWJ-DP` while **Port** is JTAG,
because SWJ-DP is the combined SWD/JTAG port and a JTAG session uses a plain
JTAG-DP.

The sheet writes the project's option string, so it is exactly the same thing as
editing that string by hand.  Pairs are separated by **spaces**, which is what
`Format()` writes and what Keil's own option strings look like; `;` is accepted as
well.  `ports` / `priv` / `events` are bit masks and are read as **hexadecimal**
(with or without a `0x`).

### Testing the sheet

`test/` compiles the real `ORBAGDI_Settings.cpp` together with a small `main()`, so
what is exercised is the actual dialog rather than a mock-up.  One command runs
everything:

```
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -Show   # + look at it
powershell -ExecutionPolicy Bypass -File test\run-tests.ps1 -Dump   # control tree
```

The scripts it runs, the two that are tools rather than checks, and the constraints
that shape them, are listed in `auto.mk`.

There is deliberately **no test code in `src\`**.  The driver has no `--selftest`,
no `--check`, and no environment-variable hooks: the dialog is driven from the
outside, exactly the way a user drives it, so what uVision loads is what is tested.
`SettingsPreview.exe --ui` exists only so a human can look at the sheet.

`test\build.ps1` still builds (and with `-Run` shows) the preview on its own if you
only want the dialog on screen.

### Trace options

Trace is off by default and has **three mutually exclusive sources**.  Pick one.

SWO pin — the ITM console, drained through the RDDI host's SWO engine:

```
rddi=CMSIS_DAP.DLL;iface=-1;clock=1000;swo=115200;cpu=72000
```

`swo` is the SWO baud rate in Hz (`0`, the default, keeps the console off) and
`cpu` is the core clock in kHz, used for the TPIU prescaler.

On-chip ETB — captured into RAM and read back over the MEM-AP:

```
rddi=CMSIS_DAP.DLL;iface=-1;clock=1000;etb=1
```

orbtrace probe — the probe's own trace endpoint (parallel port or SWO), which
carries the raw target trace port rather than the probe's SWO engine:

```
rddi=CMSIS_DAP.DLL;iface=-1;clock=1000;orbtrace=1;trwidth=4;trswo=0;troflow=1
```

| Key | Meaning |
| --- | --- |
| `orbtrace=1` | use the probe trace source; **takes priority over `etb=` / `swo=`** |
| `trwidth` | parallel trace port width in **bits**: `1`, `2` or `4` |
| `trswo` | `0` = parallel port, `1` = SWO UART/NRZ, `2` = SWO Manchester |
| `trbaud` | SWO baud for the probe in Hz; `0` = follow `swo=` |
| `troflow` | the probe frames the stream with OFLOW/COBS (default `1`) |

The probe's own report wins: if it announces protocol `0x10` the stream is
OFLOW-framed regardless of `troflow=`, and protocol `0x01` (TPIU raw) turns the
deframer off.

### Trace layering

* **All three sources belong to this layer.** The RDDI host only moves bytes:
  it has no ETM/ETB API and no trace semantics (`COMPAT_ANALYSIS.md` §8).
* **SWO** and **orbtrace** are the same ITM stream arriving on different wires,
  so both end up in the ITM decoder in `ORBAGDI_Itm.cpp`.  orbtrace's stream is
  OFLOW/COBS framed and is deframed first (`ORBAGDI_Oflow.cpp`).
* **ETB** is read back over the MEM-AP and its formatter output is split into the
  ITM and ETM halves; the ETM half goes to the shared ETM sink
  (`ORBAGDI_Etm.cpp: EtmTraceFeed`), which the orbtrace `tag=2` stream feeds too.
* **Reporting** goes through uVision's Trace Data Interface:
  `ORBAGDI_TraceWin.cpp` registers `TRACE_INTERFACE_INIT_64` with uVision via the
  BOM (`Notify(UV_TRACE_IF_INIT = 0x900)`) and describes the window with
  `UV_TRACE_WIN_CONFIG (0x902)`.  uVision then pulls records by cycle range
  (`GetTraceRecords64` / `GetFiltTraceRecords64`).  ITM stimulus, DWT PC samples
  and ETM branches all land in one `TRACE_RECORD_64` ring.
  Cycle numbers are **synthetic** (one per record) and timestamps are hidden —
  Keil calls these "fake timestamps": the time column stays empty, but range
  queries work and no timestamp decoding is needed.
  Reference: Keil's `DbgCM/TraceWinConnect.cpp` has the contract but every TDI
  callback in it is a `DEVELOP_MSG("Todo: …")` stub (`DBGCM_FEATURE_TRCDATA_WIN`
  is 0), so the implementation is ours.
* **The ETM decoder is still a skeleton** (`FuncReturn` / `ExceptionReturn` /
  v4 prefix unhandled) and its version is fixed at v3.5 — Cortex-M7 uses ETMv4.
  Completing it is a decode problem, not a plumbing one; orbuculum's
  `traceDecoder_etm35/etm4` is the reference.

## 7. Version history

The number is written down once, in `include\ORBAGDI_Version.h`. Bumping it and
rebuilding updates the DLL's file properties (via `ORBAGDI.rc`) and the
`ORBAGDI <version> - <device>, …` line the driver prints at the start of every
session.

**0.2.0** (2026-10-19)

* Flash — whole-chip erase: `FlashInitLoad()` no longer runs `Init(1)`; the
  load only loads, and each pass opens its own `Init(fnc)` (`EraseAll` 1,
  `WriteAll` 2, `VerifyAll` 3) and closes it with the matching `UnInit(fnc)`.
  The double `Init(1)` was what made Flash → Erase fail with
  `did not stop at the sentinel (Init fnc=1)`.
* Flash — `UnInit` is only sent to an algorithm that really ran `Init`, and the
  "Download to Flash off" RAM path no longer calls the algorithm's `UnInit`
  at all (`FlashUnInitRam()`).
* Run control — `DHCSR.C_MASKINTS` is cleared after every step and before every
  resume (`Core::ClearStepMaskInts()`). It cannot be cleared by `Resume()`'s own
  write on ARMv7-M, so a run that stepped off a breakpoint used to continue with
  interrupts masked.
* Release plumbing — `ORBAGDI.rc` adds a `VERSIONINFO` resource; `build.ps1`
  compiles it with `rc.exe` from the same Windows SDK it already locates.

**0.1.0** — first numbered build: DAP/SWD connection, run control, breakpoints,
flash download through a Keil FLM, ITM/SWO + ETB + orbtrace trace, Trace Data
window bridge, Settings sheet.

## 8. Credits

* `AGDI.H`, `AGSI.h`, `COMTYP.H`, `PDSCDebugEngine.h` and the `DbgCM\` /
  `TraceView\` trees are Keil / ARM artefacts shipped with MDK.
* The build script layout follows the sibling ORBMDK project.
