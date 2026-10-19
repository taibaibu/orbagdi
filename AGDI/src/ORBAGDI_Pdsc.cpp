#include "pch.h"
#include "ORBAGDI_Pdsc.h"

#include <stdio.h>
#include <string.h>

// uVision's own interfaces.  BOM.H owns struct bom (the "ioc" object uVision
// hands out for AG_CB_GETBOMPTR), the UV_NOTIFY request codes and the pio
// global; PDSCDebugEngine.h owns the PDSC_* structures those requests carry.
// BOM.H pulls in ComTyp.h, which is what the PDSC types are written against, so
// it has to come first.  Both are the exact headers the reference driver is
// built with.
#include "BOM.H"
#include "PDSCDebugEngine.h"

namespace orb {
namespace pdsc {
namespace {

// __FlashOp values.  The CMSIS flash sequences take "0 = erase, 1 = program"
// (the same order the sequences are listed in PDSCDebugEngine.h: Erase* before
// Program*).  Flagged in bug.md as the one convention that still needs a device
// that actually uses flash sequences to confirm.
const uint64_t FLASHOP_ERASE   = 0;
const uint64_t FLASHOP_PROGRAM = 1;

struct bom* s_bom = nullptr;      // 最近一次拿到的实例（AG_CB_GETBOMPTR 或 dbgblk）
struct bom* s_bomAlt = nullptr;   // 另一个来源给出的实例（两者都写状态位）
PDSC_DEBUG_PROPERTIES s_props;    // uVision's description of this target
bool s_haveProps   = false;
bool s_enabled     = false;
bool s_flashSeq    = false;
uint32_t s_seqCount = 0;          // implemented flash sequences
uint32_t s_dbgSeqCount = 0;       // implemented debug (non-flash) sequences
uint32_t s_ctx     = DBGCON_CONNECT;
char s_status[192] = { 0 };

// How the next connection / reset is described to the sequences.  Folded into
// __connection, which is what every reset sequence branches on to decide whether
// to drive the reset line or poke AIRCR.
uint32_t s_resetType    = kResetErr;
bool     s_holdReset    = false;
bool     s_preConnReset = false;

// Access variables.  PDSC_ACCESS_VAR is a packed linked list; uVision's
// interpreter resolves the names used inside a sequence against it.  The chain
// mirrors _LinkAccessVars()/_SetAccessVars() from DbgCM/PDSCDebug.cpp (which
// deliberately drops __dp/__ap because uVision owns the target port), extended
// by the flash variables and __Result: the reference template never drives the
// erase/program sequences, but the official DLL does, and those four variables
// are the VTR through which the address, length, argument and operation reach
// the script.
PDSC_ACCESS_VAR s_vars[AV_ID_COUNT];
bool s_varsBuilt = false;

// Rebuild s_status from the current state.
void RefreshStatus() {
    if (!s_bom) {
        snprintf(s_status, sizeof(s_status),
                 "PDSC: unavailable - AG_CB_GETBOMPTR returned no BOM");
    } else if (!s_bom->Notify) {
        snprintf(s_status, sizeof(s_status),
                 "PDSC: unavailable - the BOM carries no Notify entry point");
    } else if (!s_haveProps) {
        snprintf(s_status, sizeof(s_status),
                 "PDSC: uVision has no debug description for this target");
    } else if (!s_enabled) {
        snprintf(s_status, sizeof(s_status),
                 "PDSC: debug description present but not enabled by uVision");
    } else if (!s_flashSeq) {
        // Informational only.  The official gate is `enabled` on its own
        // (PDSCDebug_IsEnabled(): PDSCDebug_Supported && enabled==1,
        // DbgCM/PDSCDebug.cpp:2226); the flashSeq flag never takes part in it.
        snprintf(s_status, sizeof(s_status),
                 "PDSC: description enabled (uVision left the flashSeq flag clear)");
    } else {
        snprintf(s_status, sizeof(s_status),
                 "PDSC: %u flash + %u debug sequence(s) enabled%s%s",
                 (unsigned)s_seqCount, (unsigned)s_dbgSeqCount,
                 s_props.packid ? ", pack " : "",
                 s_props.packid ? s_props.packid : "");
    }
}

// Implemented (present and not disabled) sequence, or NULL.
const PDSC_SEQUENCE* FindSeq(uint32_t id) {
    if (!s_haveProps || id >= (uint32_t)SEQ_ID_COUNT) return nullptr;
    for (const PDSC_SEQUENCE* s = s_props.sequences; s; s = s->next) {
        if (s->id != id) continue;
        if (s->disable) return nullptr;
        return s;
    }
    return nullptr;
}

bool Implemented(uint32_t id) { return FindSeq(id) != nullptr; }

// Connect() 按实际选定的 trace 源填；SyncVars() 每次跑序列时把它推给 pack。
//
// ⚠ `SetTraceOut()` 的定义在下面的**公开段**（`} // namespace` 之后）。定义在这里会落进
// 匿名命名空间 ⇒ 内部链接 ⇒ `orb::agdi::Connect()` 报 LNK2019。
uint32_t s_traceOut = 0;

// Push the variables that depend on session state (as opposed to the per
// operation ones set by the flash helpers below).
void SyncVars() {
    uint32_t protocol = AV_PROT_TYPE_ERR;
    if (s_props.protocol == PROTOCOL_SWD)       protocol = AV_PROT_TYPE_SWD;
    else if (s_props.protocol == PROTOCOL_JTAG) protocol = AV_PROT_TYPE_JTAG;
    else if (s_props.debugConfig.defaultProtocol == PROTOCOL_SWD)
        protocol = AV_PROT_TYPE_SWD;
    else if (s_props.debugConfig.defaultProtocol == PROTOCOL_JTAG)
        protocol = AV_PROT_TYPE_JTAG;
    if (s_haveProps && s_props.debugConfig.swj) protocol |= AV_PROTOCOL_SWJ;
    s_vars[AV_ID_PROTOCOL].value = protocol;

    // uVision sets FlashLoad while it is downloading, which is what the
    // "__connection" test in the sequences looks at.  The reset bits ride in the
    // same word and are what a reset sequence switches on.
    uint32_t conn = (s_bom && s_bom->FlashLoad) ? AV_CONN_TYPE_FLASH
                                                : AV_CONN_TYPE_DEBUG;
    conn |= (s_resetType << AV_CONN_RST_P) & AV_CONN_RST;
    if (s_holdReset)    conn |= AV_CONN_HOLD_RST;
    if (s_preConnReset) conn |= AV_CONN_PRECONN_RST;
    s_vars[AV_ID_CONNECTION].value = conn;

    // `__traceout` 交给器件 pack 的 TraceStart 序列：
    //   bit0 = SWO 选中、bit1 = TPIU(并行) 选中、bits[21:16] = 并行位宽
    //
    // **这一行以前写死 0**（注释 "no trace support in this driver"）。后果是 pack 的
    // `TraceStart` 整个空转 ⇒ 引脚 / AF / `DBGMCU_CR` 只能靠固件手写顶着 ⇒ 而驱动又去
    // 硬编码配 TPIU/ITM/ETM ⇒ 三方配同一批寄存器，谁后写谁赢。见 COMPAT_ANALYSIS.md §10.28。
    s_vars[AV_ID_TRACEOUT].value    = s_traceOut;
    s_vars[AV_ID_ERRCONTROL].value  = 0;   // fail on the first sequence error
    s_vars[AV_ID_DP].value          = s_haveProps ? s_props.debugConfig.defaultDP : 0;
    s_vars[AV_ID_AP].value          = s_haveProps ? s_props.debugConfig.defaultAP : 0;
    s_vars[AV_ID_APID].value        = s_vars[AV_ID_AP].value;
}

void SetVar(uint32_t id, uint64_t value) {
    if (id < (uint32_t)AV_ID_COUNT) s_vars[id].value = value;
}

void BuildVars() {
    if (s_varsBuilt) return;
    static const char* const names[AV_ID_COUNT] = {
        "__protocol",     // AV_ID_PROTOCOL
        "__connection",   // AV_ID_CONNECTION
        "__dp",           // AV_ID_DP
        "__ap",           // AV_ID_AP
        "__traceout",     // AV_ID_TRACEOUT
        "__errorcontrol", // AV_ID_ERRCONTROL
        "__FlashAddr",    // AV_ID_FLASHADDR
        "__FlashLen",     // AV_ID_FLASHLEN
        "__FlashArg",     // AV_ID_FLASHARG
        "__FlashOp",      // AV_ID_FLASHOP
        "__Result",       // AV_ID_RESULT
        "__apid",         // AV_ID_APID
    };

    memset(s_vars, 0, sizeof(s_vars));
    PDSC_ACCESS_VAR* prev = nullptr;
    for (uint32_t i = 0; i < (uint32_t)AV_ID_COUNT; i++) {
        s_vars[i].id   = i;
        s_vars[i].next = nullptr;
        strncpy(s_vars[i].name, names[i], SIZE_ACCESSVAR_NAME - 1);

        // __dp/__ap are resolved inside uVision, never exported (see
        // _LinkAccessVars in the reference driver).
        if (i == (uint32_t)AV_ID_DP || i == (uint32_t)AV_ID_AP) continue;
        if (prev) prev->next = &s_vars[i];
        prev = &s_vars[i];
    }
    if (prev) prev->next = nullptr;
    s_varsBuilt = true;
}

/// "Log Sequences" (Pack page).  One line per successful sequence run.
///
/// ⚠ **闸门是 uVision 的那一份，不是我们自己的。** 参考：
///
///     __inline bool PDSCDebug_IsLogEnabled(void)
///     { return (PDSCDebug_Supported && (PDSCDebug_DebugProperties.log == 1)); }
///                                        -- DbgCM/PDSCDebug.cpp:2238-2241
///
/// 这里原来读的是 `cfg::Get().pdscLog`（选项键 `pdsclog`）—— 一个**自造开关**。
/// uVision 从不下发 `pdsclog`，它永远是默认值 0，于是 **Pack 页那个
/// "Log Sequences:" 勾选框在本驱动里是死的**：勾上也不出一行（实测）。而 uVision
/// 自己那份是通过 `UV_PDSCDBG_GET_PROPERTIES` 送来的，就在 `s_props` 里。
///
/// 输出走本层统一的 `ORB_LOG_*`（`ORBAGDI_Log.cpp` 的等级口径：0 = DEBUG 最详细 …
/// 9 = NONE 全关），不再自己 fopen 一个文件。
///
/// 失败那一支不在这里 —— `Execute()` 已经**无条件**用 `ORB_LOG_ERROR` 记了一行，
/// 所以"序列跑没跑起来、rc 是几"永远看得见，与这个开关无关。
void LogSequence(const char* name, uint32_t id, uint32_t rc)
{
    if (!s_haveProps || s_props.log != 1) return;
    if (rc != 1) return;                 // 失败那一行由 Execute() 记
    ORB_LOG_INFO("Pdsc: sequence \"%s\" (id %u) rc=%u", name, id, rc);
}

// Run one sequence in uVision's interpreter.  Returns false when the sequence
// is not available or uVision reported a failure.
bool Execute(uint32_t id) {
    if (!s_bom || !s_bom->Notify) return false;
    const PDSC_SEQUENCE* seq = FindSeq(id);
    if (!seq) return false;
    if (!s_varsBuilt) BuildVars();
    SyncVars();

    PDSC_SEQUENCE_CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.id           = seq->id;
    ctx.vars         = &s_vars[AV_ID_PROTOCOL];
    ctx.debugContext = s_ctx;
    ctx.timeout      = 0;   // let uVision apply its default

    const uint32_t rc = (uint32_t)s_bom->Notify(UV_PDSCDBG_EXEC_SEQUENCE, &ctx);
    LogSequence(seq->name, id, rc);
    if (rc != 1) {
        ORB_LOG_ERROR("Pdsc: sequence \"%s\" (id %u) failed (rc=%u)",
                      seq->name, (unsigned)id, (unsigned)rc);
        return false;
    }
    return true;
}

// Same, but for the sequences a device need not describe: missing means "no
// work to do", not "failed".
bool ExecuteOptional(uint32_t id) {
    if (!Implemented(id)) return true;
    return Execute(id);
}

/// The seven ids that drive a flash download, as opposed to the debug ones.
bool IsFlashSeq(uint32_t id) {
    switch (id) {
    case SEQ_FlashEraseDone:
    case SEQ_FlashProgramDone:
    case SEQ_FlashInit:
    case SEQ_FlashUninit:
    case SEQ_FlashEraseSector:
    case SEQ_FlashEraseChip:
    case SEQ_FlashProgramPage:
        return true;
    default:
        return false;
    }
}

/// Keil's own name for a sequence.  These are the strings ULP2CM3.dll carries
/// next to the ids, which is what makes a log line searchable against a pack.
const char* SeqName(uint32_t id) {
    switch (id) {
    case SEQ_DebugPortSetup:        return "DebugPortSetup";
    case SEQ_DebugPortStart:        return "DebugPortStart";
    case SEQ_DebugPortStop:         return "DebugPortStop";
    case SEQ_DebugDeviceUnlock:     return "DebugDeviceUnlock";
    case SEQ_DebugCoreStart:        return "DebugCoreStart";
    case SEQ_DebugCoreStop:         return "DebugCoreStop";
    case SEQ_DebugCodeMemRemap:     return "DebugCodeMemRemap";
    case SEQ_ResetSystem:           return "ResetSystem";
    case SEQ_ResetProcessor:        return "ResetProcessor";
    case SEQ_ResetHardware:         return "ResetHardware";
    case SEQ_ResetHardwareAssert:   return "ResetHardwareAssert";
    case SEQ_ResetHardwareDeassert: return "ResetHardwareDeassert";
    case SEQ_ResetCatchSet:         return "ResetSetCatch";
    case SEQ_ResetCatchClear:       return "ResetCatchClear";
    case SEQ_FlashEraseDone:        return "FlashEraseDone";
    case SEQ_FlashProgramDone:      return "FlashProgramDone";
    case SEQ_RecoverySupportStart:  return "RecoverySupportStart";
    case SEQ_RecoverySupportStop:   return "RecoverySupportStop";
    case SEQ_RecoveryAcknowledge:   return "RecoveryAcknowledge";
    case SEQ_TraceStart:            return "TraceStart";
    case SEQ_TraceStop:             return "TraceStop";
    case SEQ_ResetCustomized:       return "ResetCustomized";
    case SEQ_FlashInit:             return "FlashInit";
    case SEQ_FlashUninit:           return "FlashUninit";
    case SEQ_FlashEraseSector:      return "FlashEraseSector";
    case SEQ_FlashEraseChip:        return "FlashEraseChip";
    case SEQ_FlashProgramPage:      return "FlashProgramPage";
    default:                        return "?";
    }
}

/// Runs one debug (non-flash) sequence under `ctx`.
///
/// `required` decides what "the description does not have it" means.  For the
/// connect and reset groups it is not a failure - a device that describes nothing
/// has to keep working exactly as it did before these existed - but it is worth a
/// log line, because "the sequence did nothing" and "there is no sequence" look
/// identical from the outside.
bool RunDebugSeq(uint32_t id, uint32_t ctx, bool required) {
    // The Pack page's "Enable" switch turns the whole description off.  A device
    // whose description the user disabled has to behave exactly as it did before
    // any of this existed, so the gate is here rather than at each call site.
    if (!(s_haveProps && s_enabled)) return !required;

    if (!Implemented(id)) {
        if (required) {
            ORB_LOG_WARN("Pdsc: %s is not in this target's description", SeqName(id));
            return false;
        }
        ORB_LOG_INFO("Pdsc: %s absent - skipped", SeqName(id));
        return true;
    }
    s_ctx = ctx;
    return Execute(id);
}

} // namespace

// ---- session --------------------------------------------------------------

void SetBom(struct bom* bom) {
    // 主 BOM（`ioc`）：只应由 AG_CB_GETBOMPTR 的返回值来设置，见
    // ORBAGDI_Agdi.cpp 的 AG_INITCALLBACK。uVision 自己读的就是这个实例，
    // 所以它必须是 s_bom，不能被别的来源顶掉。
    if (s_bom && s_bom != bom) {
        s_bomAlt = s_bom;
        ORB_LOG_INFO("Pdsc: BOM 更换主实例 (旧=%p, 新=%p)", (void*)s_bomAlt, (void*)bom);
    }
    s_bom = bom;

    if (bom) bom->bTrcDisp = 1;          // "Sxxx supports Trace-Display-Window"
    if (s_bomAlt) s_bomAlt->bTrcDisp = 1;

    RefreshStatus();
    if (bom) ORB_LOG_INFO("Pdsc: BOM handle acquired (%s)", s_status);
    else     ORB_LOG_INFO("Pdsc: no BOM handle - PDSC path unavailable");
}

void SetTraceOut(uint32_t v) { s_traceOut = v; }

void SetBomAlt(struct bom* bom) {
    // 只登记备用实例，绝不顶掉已经拿到的 ioc。
    //
    // 顺序实测是 `EnumUvARM7(2)`（dbgblk）先跑、`AG_INITCALLBACK`（ioc）后跑，
    // 但 dbgblk 那条路可能被再次触发。如果它无条件 SetBom，就会把主指针从
    // ioc 换回 dbgblk 那个实例 —— 而 uVision 的 Trace Data 取数门读的是 ioc。
    if (!bom) return;
    if (s_bom == bom) return;
    if (!s_bom) { SetBom(bom); return; }
    if (s_bomAlt != bom) {
        s_bomAlt = bom;
        ORB_LOG_INFO("Pdsc: 备用 BOM 实例 = %p（主实例 %p 保持不变）",
                     (void*)s_bomAlt, (void*)s_bom);
    }
    if (bom) bom->bTrcDisp = 1;
}

void SetTraceDisplayFlag(bool on) {
    const DWORD v = on ? 1u : 0u;
    if (s_bom)    s_bom->bTrcDisp    = v;
    if (s_bomAlt) s_bomAlt->bTrcDisp = v;
    ORB_LOG_INFO("Pdsc: bTrcDisp=%u 已写到 %s%s", (unsigned)v,
                 s_bom ? "bom" : "(无 bom)",
                 s_bomAlt ? " + 备用实例" : "");
}

bool HaveBom() { return s_bom != nullptr && s_bom->Notify != nullptr; }

struct bom* Bom() { return s_bom; }

void SetThumbState(bool thumb) {
    // Silent no-op when the BOM was never acquired - the same trap PublishPC
    // reports for AG_INITCURPC.  It matters more here: pio->Thumb is how uVision
    // decides between ARM and Thumb decoding, so with no BOM the instruction set
    // is never published at all and the disassembly is decoded as ARM.
    static bool logged = false;
    if (!s_bom) {
        if (!logged) {
            logged = true;
            ORB_LOG_WARN("Pdsc: SetThumbState(%u) - no BOM, nothing published "
                         "(uVision keeps its own instruction-set flag)",
                         thumb ? 1u : 0u);
        }
        return;
    }
    if (!logged) {
        logged = true;
        ORB_LOG_INFO("Pdsc: SetThumbState(%u) - BOM present, published",
                     thumb ? 1u : 0u);
    }
    s_bom->Thumb = thumb ? 1 : 0;
}

// ---------------------------------------------------------------------------
// "last hit Bp" - see the declaration in ORBAGDI_Pdsc.h
//
// This is the signal uVision waits for before it refreshes the debug views.  Its
// absence is why the Registers window stayed empty while AG_AllReg answered with
// perfectly good values: the data was delivered, but nothing ever said "the
// target stopped, here and for this reason".
// ---------------------------------------------------------------------------
void SetHitBp(uint32_t addr, uint16_t acc, uint16_t bpNum, uint32_t pc) {
    // Logged once per session, exactly like PublishPC does for AG_INITCURPC.
    // Without the BOM every write below is a silent no-op, and "uVision ignored
    // the notification" is indistinguishable from "there was no notification" -
    // which is the state this window has been stuck in.
    static bool logged = false;
    if (!s_bom) {
        if (!logged) {
            logged = true;
            ORB_LOG_WARN("Pdsc: SetHitBp 0x%08X - no BOM, the stop notification is "
                         "a silent no-op (the debug views will not refresh)", addr);
        }
        return;
    }
    if (!logged) {
        logged = true;
        ORB_LOG_INFO("Pdsc: SetHitBp 0x%08X acc=0x%04X - BOM present, stop published",
                     addr, acc);
    }
    s_bom->hitbp.nAdr      = addr;
    s_bom->hitbp.nAcc      = acc;
    s_bom->hitbp.nBpNum    = bpNum;
    s_bom->hitbp.nPC       = pc;
    // Fifth field, which this driver used to leave untouched: the reference
    // writes it on every hit (DbgCM/AGDI.CPP:7280)
    //     pio->hitbp.nTickMark = Bp ? ((BKS *)Bp)->nTickMark : 0;
    // It is the per-breakpoint tick mark uVision keeps; we have no
    // breakpoint-number mapping, so the reference's own "no Bp" value applies.
    s_bom->hitbp.nTickMark = 0;
}

void ClearHitBp() {
    if (!s_bom) return;
    s_bom->hitbp.nAcc   = 0x0000;   // HIT_NOREASON (BOM.H:3051)
    s_bom->hitbp.nBpNum = 0xFFFF;   // undefined
}

bool Refresh() {
    s_haveProps   = false;
    s_enabled     = false;
    s_flashSeq    = false;
    s_seqCount    = 0;
    s_dbgSeqCount = 0;

    if (!HaveBom()) {
        RefreshStatus();
        return false;
    }

    PDSC_DEBUG_PROPERTIES props;
    memset(&props, 0, sizeof(props));
    const uint32_t rc =
        (uint32_t)s_bom->Notify(UV_PDSCDBG_GET_PROPERTIES, &props);
    if (rc != 1) {
        // Normal for every target that does not come from a pack with a debug
        // description; the classic driver keeps working unchanged.
        RefreshStatus();
        ORB_LOG_INFO("Pdsc: %s (rc=%u)", s_status, (unsigned)rc);
        return false;
    }

    s_props     = props;
    s_haveProps = true;
    s_enabled   = (props.enabled != 0);

    for (const PDSC_SEQUENCE* s = props.sequences; s; s = s->next) {
        if (s->disable) continue;
        if (s->id >= (uint32_t)SEQ_ID_COUNT) continue;
        if (IsFlashSeq(s->id)) s_seqCount++;
        else                   s_dbgSeqCount++;
    }
    s_flashSeq = s_enabled && (props.flashSeq != 0);

    RefreshStatus();
    ORB_LOG_INFO("Pdsc: %s (log=%u, sdf=%u)", s_status,
                 (unsigned)(props.log != 0), (unsigned)(props.sdf != 0));
    // Which of the debug sequences are actually there decides whether a device
    // needs the new connect / reset path at all, so name them.
    for (const PDSC_SEQUENCE* s = props.sequences; s; s = s->next) {
        if (s->disable || IsFlashSeq(s->id) || s->id >= (uint32_t)SEQ_ID_COUNT)
            continue;
        ORB_LOG_INFO("Pdsc:   debug sequence \"%s\" (id %u)",
                     SeqName(s->id), (unsigned)s->id);
    }
    for (const PDSC_FLASH_INFO* fi = props.flashInfos; fi; fi = fi->next) {
        ORB_LOG_INFO("Pdsc: flash \"%s\" @0x%08X (%u KB, page %u B)",
                     fi->name, (unsigned)fi->addr,
                     (unsigned)(fi->size / 1024), (unsigned)fi->pageSize);
    }
    return s_haveProps;
}

void Shutdown() {
    s_haveProps   = false;
    s_enabled     = false;
    s_flashSeq    = false;
    s_seqCount    = 0;
    s_dbgSeqCount = 0;
    s_resetType    = kResetErr;
    s_holdReset    = false;
    s_preConnReset = false;
    memset(&s_props, 0, sizeof(s_props));
    s_ctx = DBGCON_CONNECT;
    RefreshStatus();
}

bool Available() { return s_haveProps && s_enabled; }

bool FlashSeqReady() {
    // Gate: the description is enabled, exactly as the official driver decides
    // it (PDSCDebug_IsEnabled() = supported && enabled==1).  Requiring
    // props.flashSeq on top was our own invention and it is what sent
    // STM32F407VETx back to the FLM: uVision reports `enabled` yet leaves
    // flashSeq clear (bug.md E16).  Absent sequences are what the next test
    // covers, not the flag.
    if (!s_enabled) return false;
    // Programming plus some way of erasing is the minimum that makes the
    // sequence path usable; anything less falls back to the FLM.
    return Implemented(SEQ_FlashProgramPage) &&
           (Implemented(SEQ_FlashEraseSector) || Implemented(SEQ_FlashEraseChip));
}

const char* Status() { return s_status[0] ? s_status : nullptr; }

// ---- flash description ----------------------------------------------------

bool FlashInfoFor(uint32_t adr, FlashInfo& out) {
    if (!s_haveProps) return false;
    for (const PDSC_FLASH_INFO* fi = s_props.flashInfos; fi; fi = fi->next) {
        if (adr < (uint32_t)fi->addr) continue;
        if (adr >= (uint32_t)(fi->addr + fi->size)) continue;

        out.addr         = (uint32_t)fi->addr;
        out.size         = fi->size;
        out.pageSize     = fi->pageSize;
        out.sectorSize   = 0;
        out.progTimeout  = fi->progTimeout;
        out.eraseTimeout = fi->eraseTimeout;
        out.haveArg      = false;
        out.arg          = 0;

        for (const PDSC_FLASH_BLOCKS* b = fi->blocks; b; b = b->next) {
            const uint64_t end = b->addr + (uint64_t)b->count * (uint64_t)b->blockSize;
            if (adr < (uint32_t)b->addr || (uint64_t)adr >= end) continue;
            out.haveArg    = true;
            out.arg        = b->arg;
            out.sectorSize = b->blockSize;
            break;
        }
        return true;
    }
    return false;
}

uint32_t PageSizeFor(uint32_t adr) {
    FlashInfo fi;
    return FlashInfoFor(adr, fi) ? fi.pageSize : 0;
}

// ---- debug sequences -------------------------------------------------------

void SetResetMode(uint32_t resetType, bool holdReset, bool preConnReset) {
    s_resetType    = resetType;
    s_holdReset    = holdReset;
    s_preConnReset = preConnReset;
}

bool DebugSeqReady() { return (s_haveProps && s_enabled) && s_dbgSeqCount != 0; }

uint32_t DebugSeqCount() { return s_dbgSeqCount; }

bool DebugPortSetupAvailable() { return Implemented(SEQ_DebugPortSetup); }

bool ConnectDebugPort() {
    // "Prepares Debug Port for connection."  Runs before the DP is touched, so a
    // device that has to drive a pin or gate a clock does it here - and one that
    // does not is unaffected.
    return RunDebugSeq(SEQ_DebugPortSetup, DBGCON_CONNECT, false);
}

bool StartDebugPort() {
    // "Connect to Debug Port and power up."
    return RunDebugSeq(SEQ_DebugPortStart, DBGCON_CONNECT, false);
}

bool UnlockDevice() {
    // "Unlock device after connection to Debug Port."
    //
    // This is the one that matters for the many parts that keep their debug port
    // locked until a magic value lands somewhere: without it they identify
    // perfectly and then refuse every access, which is indistinguishable from a
    // wiring fault.
    return RunDebugSeq(SEQ_DebugDeviceUnlock, DBGCON_CONNECT, false);
}

bool StartCore() {
    // "Configure device specific logic for debug/trace."
    return RunDebugSeq(SEQ_DebugCoreStart, DBGCON_CONNECT, false);
}

void StopCore() {
    // "Clear configuration of device specific logic."  Best effort: the session
    // is ending, and a device that cannot clean up must not stop it ending.
    RunDebugSeq(SEQ_DebugCoreStop, DBGCON_DISCONNECT, false);
}

void StopDebugPort() {
    // "Power down Debug Port and disconnect from it."
    RunDebugSeq(SEQ_DebugPortStop, DBGCON_DISCONNECT, false);
}

bool Reset(uint32_t resetType) {
    if (!(s_haveProps && s_enabled)) return false;

    SetResetMode(resetType, s_holdReset, s_preConnReset);

    // An explicit mode on the Debug page picks one sequence and only that one.
    // "Autodetect" walks the fallback order the reference driver uses, ending at
    // the device's own ResetCustomized.
    static const uint32_t kAuto[] = { SEQ_ResetHardware, SEQ_ResetSystem,
                                      SEQ_ResetProcessor, SEQ_ResetCustomized };
    static const uint32_t kHw[]   = { SEQ_ResetHardware };
    static const uint32_t kSys[]  = { SEQ_ResetSystem };
    static const uint32_t kVec[]  = { SEQ_ResetProcessor };

    const uint32_t* order = kAuto;
    size_t n = sizeof(kAuto) / sizeof(kAuto[0]);
    switch (resetType) {
    case kResetHw:  order = kHw;  n = 1; break;
    case kResetSys: order = kSys; n = 1; break;
    case kResetVec: order = kVec; n = 1; break;
    default: break;                          // kResetErr == Autodetect
    }

    for (size_t i = 0; i < n; ++i) {
        if (!Implemented(order[i])) continue;
        s_ctx = DBGCON_RESET;
        ORB_LOG_INFO("Pdsc: reset through \"%s\"", SeqName(order[i]));
        return Execute(order[i]);
    }
    ORB_LOG_INFO("Pdsc: the description has no reset sequence for this mode");
    return false;
}

bool ResetCatchSet() {
    // "Configure processor where to stop after reset."
    //
    // Returns false both when the description has no such sequence and when it
    // failed - the caller's question is "did the description take over the
    // stop-after-reset?", and in both cases the answer is no, so it has to
    // arrange the halt itself.
    if (!(s_haveProps && s_enabled)) return false;
    if (!Implemented(SEQ_ResetCatchSet)) return false;
    s_ctx = DBGCON_RESET;
    return Execute(SEQ_ResetCatchSet);
}

bool ResetCatchClear() {
    // "Clear target configuration applied in \"ResetSetCatch\" sequence."
    return RunDebugSeq(SEQ_ResetCatchClear, DBGCON_RESET, false);
}

bool ResetHardwareAssert() {
    // "Assert system-wide reset via debugger reset line."
    //
    // 描述没有这条序列时**不编代替方案**：参考的
    // `PDSCDebug_ResetHardwareAssert()`（`PDSCDebug.cpp:4057-4082`）在 EU26
    // （sequence not implemented）之后走的"默认实现"只有一句
    //     DEVELOP_MSG("Todo: \nAssert HW Chip Reset");
    // 也就是什么都不做。这里同样只跑序列，没有就答 false —— 调用方据此知道复位线
    // 并没有被拉住，而不是以为拉住了。
    //
    // ⚠ `required` 必须是 **true**。这两个函数问的是"线到底拉住了没有"，而
    //   `RunDebugSeq(..., false)` 的契约是"序列不存在 = 不算失败"（给
    //   TraceStart/RecoveryStart 那类可选钩子用的）。写成 false 的后果实测过：
    //   序列不存在时这里返回 true ⇒ 调用方的兜底分支永远不进 ⇒ 复位线一次都没被
    //   驱动，日志里只有一行 `ResetHardwareAssert absent - skipped`，看着像成功。
    return RunDebugSeq(SEQ_ResetHardwareAssert, DBGCON_RECOVERY, true);
}

bool ResetHardwareDeassert() {
    // "Deassert system-wide reset via debugger reset line."
    // 参考 `PDSCDebug.cpp:4085-...` 的默认实现同样是 TODO。`required` 同理为 true：
    // 拉住了却没放开，目标就一直是死的。
    return RunDebugSeq(SEQ_ResetHardwareDeassert, DBGCON_RECOVERY, true);
}

bool RecoveryStart() {
    return RunDebugSeq(SEQ_RecoverySupportStart, DBGCON_CONNECT, false);
}

bool RecoveryStop() {
    return RunDebugSeq(SEQ_RecoverySupportStop, DBGCON_DISCONNECT, false);
}

bool RecoveryAcknowledge() {
    // 参考 `PDSCDebug_RecoveryAcknowledge()`（`PDSCDebug.cpp:4260-4273`）：
    // "No default implementation"，EU26/EU37 都被吞掉 ⇒ 缺失不算失败。
    // 器件为"连接断了之后还能找回来"专门留的硬件/状态，靠这条序列收尾。
    return RunDebugSeq(SEQ_RecoveryAcknowledge, DBGCON_RECOVERY, false);
}

bool TraceStart() {
    // "Configure device specific logic for trace capture."
    return RunDebugSeq(SEQ_TraceStart, DBGCON_TARGET_ACCESS, false);
}

bool TraceSeqAvailable() { return Implemented(SEQ_TraceStart); }

bool TraceStop() {
    // "Clear configuration of device specific logic for trace capture."
    return RunDebugSeq(SEQ_TraceStop, DBGCON_DISCONNECT, false);
}

// ---- flash sequences ------------------------------------------------------

bool Init() {
    SetDebugContext(DBGCON_CONNECT);
    return ExecuteOptional(SEQ_FlashInit);
}

bool Uninit() {
    return ExecuteOptional(SEQ_FlashUninit);
}

bool EraseDone() {
    SetDebugContext(DBGCON_FLASH_ERASE);
    return ExecuteOptional(SEQ_FlashEraseDone);
}

bool ProgramDone() {
    SetDebugContext(DBGCON_FLASH_PROGRAM);
    return ExecuteOptional(SEQ_FlashProgramDone);
}

bool EraseSector(uint32_t adr) {
    FlashInfo fi;
    const uint64_t len =
        (FlashInfoFor(adr, fi) && fi.sectorSize) ? fi.sectorSize : 0;
    SetDebugContext(DBGCON_FLASH_ERASE);
    SetVar(AV_ID_FLASHADDR, adr);
    SetVar(AV_ID_FLASHLEN,  len);
    SetVar(AV_ID_FLASHARG,  (FlashInfoFor(adr, fi) && fi.haveArg) ? fi.arg : 0);
    SetVar(AV_ID_FLASHOP,   FLASHOP_ERASE);
    return Execute(SEQ_FlashEraseSector);
}

bool EraseChip(uint32_t adr, uint32_t size) {
    SetDebugContext(DBGCON_FLASH_ERASE);
    SetVar(AV_ID_FLASHADDR, adr);
    SetVar(AV_ID_FLASHLEN,  size);
    SetVar(AV_ID_FLASHARG,  0);
    SetVar(AV_ID_FLASHOP,   FLASHOP_ERASE);
    return Execute(SEQ_FlashEraseChip);
}

bool ProgramPage(uint32_t adr, uint32_t size, uint32_t dataAdr) {
    // __FlashArg is the "argument to the flash operation" of the covering
    // block; when the description does not define one, hand the sequence the
    // address the caller staged the page data at - which is the only useful
    // argument a program operation can take.
    FlashInfo fi;
    const uint64_t arg = (FlashInfoFor(adr, fi) && fi.haveArg) ? fi.arg : dataAdr;
    SetDebugContext(DBGCON_FLASH_PROGRAM);
    SetVar(AV_ID_FLASHADDR, adr);
    SetVar(AV_ID_FLASHLEN,  size);
    SetVar(AV_ID_FLASHARG,  arg);
    SetVar(AV_ID_FLASHOP,   FLASHOP_PROGRAM);
    return Execute(SEQ_FlashProgramPage);
}

void     SetDebugContext(uint32_t ctx) { s_ctx = ctx; }
uint32_t DebugContext()                { return s_ctx; }

}}
