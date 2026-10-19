/**
 * @file ORBAGDI_Flash.cpp
 * @brief Flash download via a Keil FLM flash algorithm (and the PDSC sequence path).
 *
 *   AG_INITFLASHLOAD  -> FlashInitLoad(): parse .FLM (ELF32), copy PT_LOAD segments into target
 *        RAM, resolve entry points from the symbol table.  Load only - Init() belongs to a pass.
 *   AG_STARTFLASHLOAD -> FlashStartLoad(): EraseAll / WriteAll / VerifyAll, image pulled with
 *        AG_CB_GETFLASHPARAM, progress via AG_CB_PROGRESS.  Each pass is Init(fnc) .. UnInit(fnc).
 *
 * The algorithm runs on the target, so it needs a real RAM window and clock: ram=<addr>,<size>,
 * clock= from the option string, else plausible defaults.
 */

#include "pch.h"
#include "ORBAGDI_Agdi.h"
#include "ORBAGDI_CodeImage.h"
#include "ORBAGDI_Config.h"
#include "ORBAGDI_DebugAccess.h"
#include "ORBAGDI_Flash.h"
#include "ORBAGDI_Pdsc.h"
#include "ORBAGDI_TracePump.h"

// uVision's own interface: UV_DOXEVENTS (the "run MessagePump n times" request)
// and the `struct bom` that carries Notify.  Needed so a long sector erase can
// keep the IDE repainting - see the poll loop in CallAlg().
#include "BOM.H"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace orb {
namespace agdi {
namespace {

// ---------------------------------------------------------------------------
// ELF32 (little endian).  A Keil .FLM is a relocatable ARM ELF image.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct Elf32Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32Phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
struct Elf32Shdr {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
};
struct Elf32Sym {
    uint32_t st_name, st_value, st_size;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
};
#pragma pack(pop)

enum : uint32_t { kPtLoad = 1, kShtSymtab = 2, kShfWrite = 0x1, kShfAlloc = 0x2 };

// struct FlashDevice from FlashOS.h.  It is *not* packed there either, so the
// natural x86 layout matches the ARM one (32-bit members aligned to 4).
struct FlashDevInfo {
    uint16_t Vers;
    char     DevName[128];
    uint16_t DevType;
    uint32_t DevAdr;
    uint32_t szDev;
    uint32_t szPage;
    uint32_t Res;
    uint8_t  valEmpty;
    uint32_t toProg;
    uint32_t toErase;
    struct Sector { uint32_t szSector; uint32_t AddrSector; } sectors[512];
};

// The loaded algorithm, expressed in target addresses.
struct AlgImage {
    bool     ok;
    uint32_t fn[8];     // see AlgFn
    uint32_t sbase;     // static base -> r9
    uint32_t sp;        // stack top
    uint32_t stack;     // stack size (from the STACK_SIZE symbol)
    uint32_t buf;       // page buffer in target RAM
    uint32_t brk;       // BKPT sentinel the algorithm "returns" to
    FlashDevInfo dev;
};

enum AlgFn {
    kFnInit = 0, kFnUnInit, kFnEraseChip, kFnEraseSector, kFnProgramPage, kFnVerify, kFnBlankCheck,
};

AlgImage s_alg;
char     s_flm[520];
uint32_t s_ramAddr = cfg::kDefaultRamAddr; // mirrors cfg::Config until
uint32_t s_ramSize = cfg::kDefaultRamSize; // FlashOptions() runs
char     s_err[256];

// Which of the two download paths this session uses.  false = classic FLM
// algorithm (s_alg), true = the device's PDSC flash sequences.  Decided once,
// in FlashInitLoad(), and only when the description really drives erase and
// program (orb::pdsc::FlashSeqReady()).
bool s_pdscSeq = false;

// Is the algorithm currently initialised for a pass?  Loading the FLM no longer runs Init(), so
// this is the only record of whether an Init(adr, clk, fnc) is still open.  Every pass is
// Init(fnc) .. UnInit(fnc), so UnInit may only be sent while this is true - an UnInit without a
// matching Init calls into an algorithm that was never configured.
bool s_algInitDone = false;

// The page currently being assembled for ProgramPage: uVision hands segments that start and end
// mid-page, while the algorithm wants whole pages from a page boundary.
//   s_pageAddr  page boundary the pending data belongs to
//   s_pageLen   how far into that page the buffer is filled (0 = nothing pending); this is the
//               length ProgramPage is called with - the bytes past it must not be claimed
// Without it a short segment is programmed at a mid-page address, which STM32 algorithms either
// reject or - worse - accept and program the wrong bytes.
std::vector<uint8_t> s_page;
uint32_t             s_pageAddr = 0;
uint32_t             s_pageLen  = 0;

// Last sector the erase pass erased.  uVision's segments can cover a sector that
// an earlier segment already erased, and a second erase of the same sector costs
// another full sector erase - seconds on a large sector.  The reference tracks
// the same thing (DbgCM/Flash.cpp:1148 ESecAddr/ESecSize).
uint32_t s_eraseAddr = 0;
uint32_t s_eraseSize = 0;

/// Records the reason a flash operation failed.  Every failure in this file goes through here and
/// is reported to uVision as well as to the log: "Flash Download failed" with nothing else is
/// exactly the report this exists to prevent.
void Fail(const char* what, ...) {
    // keep the last error printable from AG_Init's caller
    va_list ap;
    va_start(ap, what);
    vsnprintf(s_err, sizeof(s_err), what, ap);
    va_end(ap);
    ORB_LOG_ERROR("Flash: %s", s_err);
    orb::ui::Error("Flash: %s", s_err);
}

/// Points the reporting channel at the build window for exactly as long as a
/// download runs - Keil's txtout() switches on pio->FlashLoad the same way, so
/// download output lands next to the compiler output rather than in the command
/// window the debug session is using.
struct FlashModeGuard {
    FlashModeGuard()  { orb::ui::SetFlashMode(true); }
    ~FlashModeGuard() { orb::ui::SetFlashMode(false); }
    FlashModeGuard(const FlashModeGuard&) = delete;
    FlashModeGuard& operator=(const FlashModeGuard&) = delete;
};

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
uint32_t Align4(uint32_t v) { return (v + 3u) & ~3u; }

/// The rules Keil applies to a FlashDevice sector table (DbgCM/Flash.cpp:932 SectorCheck) plus
/// the sector count derived from it (:1101).  A table that breaks one of them makes every erase
/// land on the wrong sector while the download still reports success: AddrSector is an *offset*
/// from the device base, and an entry describes a run of equal sectors lasting until the next
/// entry - so an inconsistent table cannot be walked; refuse the algorithm instead of erasing
/// blindly.  A first entry of SECTOR_END means "use EraseChip": accepted with zero sectors.
bool CheckSectorTable(const FlashDevInfo& d, uint32_t& runs, uint32_t& total) {
    runs  = 0;
    total = 0;

    uint32_t prevAddr = 0;
    uint32_t prevSize = 0;

    for (uint32_t i = 0; i < 512; ++i) {
        const uint32_t adr = d.sectors[i].AddrSector;
        const uint32_t sz  = d.sectors[i].szSector;

        if (adr == 0xFFFFFFFFu && sz == 0xFFFFFFFFu) {   // SECTOR_END
            if (runs == 0) return true;                  // chip-erase only
            total += (d.szDev - prevAddr) / prevSize;    // the last run
            return true;
        }

        if (sz == 0) {
            Fail("sector %u has zero size", i);
            return false;
        }
        if (i == 0 && adr != 0) {
            Fail("sector 0 starts at offset 0x%X, not at the device base", adr);
            return false;
        }
        if (adr + sz > d.szDev) {
            Fail("sector %u (0x%X+0x%X) runs past the end of the device (0x%X)",
                 i, adr, sz, d.szDev);
            return false;
        }
        if (i != 0 && (adr < prevAddr + prevSize || (adr - prevAddr) % prevSize)) {
            Fail("sector %u starts at 0x%X, which does not follow 0x%X+0x%X",
                 i, adr, prevAddr, prevSize);
            return false;
        }
        if (i != 0) total += (adr - prevAddr) / prevSize;

        prevAddr = adr;
        prevSize = sz;
        ++runs;
    }

    Fail("the sector table has no SECTOR_END within 512 entries");
    return false;
}

bool ReadAll(const char* path, std::vector<uint8_t>& buf) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    buf.resize(static_cast<size_t>(n));
    const size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    return got == buf.size();
}

bool HasSuffixNoCase(const char* s, const char* suffix) {
    const size_t ls = strlen(s), lx = strlen(suffix);
    if (ls < lx) return false;
    return _strnicmp(s + ls - lx, suffix, lx) == 0;
}

// ---------------------------------------------------------------------------
// ELF reader
// ---------------------------------------------------------------------------
class Elf32 {
public:
    bool Load(const char* path) {
        if (!ReadAll(path, m_f) || m_f.size() < sizeof(Elf32Ehdr)) return false;
        m_eh = reinterpret_cast<const Elf32Ehdr*>(m_f.data());
        if (m_eh->e_ident[0] != 0x7F || m_eh->e_ident[1] != 'E' ||
            m_eh->e_ident[2] != 'L' || m_eh->e_ident[3] != 'F') return false;
        if (m_eh->e_ident[4] != 1) return false;            // ELFCLASS32
        if (m_eh->e_ident[5] != 1) return false;            // ELFDATA2LSB
        return m_eh->e_phnum > 0;
    }

    const Elf32Ehdr* Eh() const { return m_eh; }

    const Elf32Phdr* Ph(uint32_t i) const {
        const uint32_t off = m_eh->e_phoff + i * m_eh->e_phentsize;
        if (off + sizeof(Elf32Phdr) > m_f.size()) return nullptr;
        return reinterpret_cast<const Elf32Phdr*>(&m_f[off]);
    }

    const Elf32Shdr* Sh(uint32_t i) const {
        const uint32_t off = m_eh->e_shoff + i * m_eh->e_shentsize;
        if (off + sizeof(Elf32Shdr) > m_f.size()) return nullptr;
        return reinterpret_cast<const Elf32Shdr*>(&m_f[off]);
    }

    const uint8_t* File() const { return m_f.data(); }
    size_t         Size() const { return m_f.size(); }

    // Value of a symbol, or 0 when it is not in the symbol table.
    uint32_t Sym(const char* want) const {
        for (uint32_t s = 0; s < m_eh->e_shnum; ++s) {
            const Elf32Shdr* sh = Sh(s);
            if (!sh || sh->sh_type != kShtSymtab) continue;
            const Elf32Shdr* str = Sh(sh->sh_link);
            if (!str || str->sh_offset + str->sh_size > m_f.size()) continue;
            const char* names = reinterpret_cast<const char*>(&m_f[str->sh_offset]);
            const uint32_t count = sh->sh_size / static_cast<uint32_t>(sizeof(Elf32Sym));
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t off = sh->sh_offset + i * static_cast<uint32_t>(sizeof(Elf32Sym));
                if (off + sizeof(Elf32Sym) > m_f.size()) break;
                const Elf32Sym* sym = reinterpret_cast<const Elf32Sym*>(&m_f[off]);
                if (sym->st_name >= str->sh_size) continue;
                if (strcmp(names + sym->st_name, want) == 0) return sym->st_value;
            }
        }
        return 0;
    }

private:
    std::vector<uint8_t> m_f;
    const Elf32Ehdr*     m_eh = nullptr;
};

// ---------------------------------------------------------------------------
// algorithm invocation on the target
// ---------------------------------------------------------------------------
const DWORD kAlgTimeoutMs = 30000;

// ---------------------------------------------------------------------------
// Algorithm session state
// The reference keeps the target register file in a global and writes it back when the session
// ends - it does NOT read/write the whole file around every call.  Doing that per call costs
// ~64 MEM-AP round trips (~38 ms), ~11 s over one download's ~300 calls.  So: save once at the
// first call of a session, restore once when it ends; FlashAlgLeave() also runs at the start of
// a new session so an aborted one cannot leak state into the next.
// ---------------------------------------------------------------------------
RgARMCM  s_savedRegs;
bool     s_regsSaved  = false;
uint32_t s_demcrSaved = 0;
uint32_t s_actlrSaved = 0;
bool     s_demcrValid = false;
bool     s_actlrValid = false;

// ---------------------------------------------------------------------------
// Progress counters
// The reference keeps Counter / TotalCount (DbgCM/Flash.cpp:64-65) and advances them while the
// erase pass walks a range, so uVision's bar moves *during* the erase.  Without it the bar sits
// still for the whole pass - on an STM32F4 that is ~7 s of frozen bar.
// ---------------------------------------------------------------------------
uint32_t s_counter = 0;
uint32_t s_total   = 0;

void Progress(int job, int pos, const char* label);   // defined further down

/// The reference's UpdatePos(): reports the byte-based percentage.
void UpdatePos(uint32_t bytes) {
    if (s_total == 0) return;
    Progress(PROGRESS_SETPOS, static_cast<int>(100ull * bytes / s_total), "Erase: ");
}

/// Saves the target register file and arms the debug controls, once per session.
bool FlashAlgEnter() {
    if (s_regsSaved) return true;
    Context& c = Get();

    if (!c.core.ReadRegs(s_savedRegs)) return false;   // retry on the next call
    s_regsSaved = true;

    // Vector catch on every fault (DEMCR[10:4]): an algorithm that faults then
    // halts on the spot with PC in the vector table and CFSR/HFSR filled in,
    // instead of vectoring into the application's handler and looking like a
    // plain timeout with nothing to go on.
    uint32_t dhcsr0 = 0;
    s_demcrValid = c.core.ReadCoreDebug(dhcsr0, s_demcrSaved);
    if (s_demcrValid) c.dap.MemWrite32(0xE000EDFCu, s_demcrSaved | 0x000007F0u);

    // Write buffer off (ACTLR.DISDEFWBUF): a buffered store to a bad address
    // otherwise surfaces later as an *imprecise* bus fault, and then BFAR is
    // invalid and there is nothing left to point at (bug.md E15).
    s_actlrValid = c.dap.MemRead32(0xE000E008u, s_actlrSaved);
    if (s_actlrValid) c.dap.MemWrite32(0xE000E008u, s_actlrSaved | 0x00000002u);

    return true;
}

/// Restores what FlashAlgEnter() saved.  Safe to call when nothing was saved.
void FlashAlgLeave() {
    if (!s_regsSaved) return;
    Context& c = Get();

    if (s_actlrValid) c.dap.MemWrite32(0xE000E008u, s_actlrSaved);
    if (s_demcrValid) c.dap.MemWrite32(0xE000EDFCu, s_demcrSaved);
    c.core.WriteRegs(s_savedRegs);

    s_regsSaved  = false;
    s_actlrValid = false;
    s_demcrValid = false;
}

// ---------------------------------------------------------------------------
// Batched algorithm register write
// The reference writes the algorithm's registers as ONE batched DAP access (SWD_SetARMRegs):
// MATCH_MASK/MATCH_RETRY make the RDDI host wait for DHCSR.S_REGRDY itself, so no polling is
// needed and every DCRSR/DCRDR pair travels in the same transfer.
// This driver used eight Core::WriteRegSel calls (DCRDR + DCRSR + poll each): 24+ round trips.
// Returns false when the host has no DAP_RegAccessBlock or the batch failed; the caller then
// falls back to the per-register path.
// ---------------------------------------------------------------------------
bool WriteAlgRegsBatched(orb::Dap& dap, const uint32_t* sel, const uint32_t* val, int n) {
    const orb::rddi::Api& api = orb::rddi::Get();
    if (!api.DAP_RegAccessBlock) return false;
    if (n <= 0 || n > 16) return false;

    int regID[4 + 4 * 16];
    int regData[4 + 4 * 16];
    int k = 0;

    regID[k] = DAP_REG_MATCH_RETRY; regData[k++] = 100;
    regID[k] = DAP_REG_MATCH_MASK;  regData[k++] = 0x00010000;   // DHCSR.S_REGRDY
    regID[k] = DAP_REG_DP_0x8;      regData[k++] = 0;            // SELECT: AP 0, bank 0
    regID[k] = DAP_REG_AP_0x0;      regData[k++] = static_cast<int>(
                                        orb::kCswBase | orb::kCswSizeU32 | orb::kCswAddrInc);

    for (int i = 0; i < n; ++i) {
        // DCRDR first (the value), then DCRSR (the selector + WnR) triggers the
        // transfer - the same order Core::WriteRegSel uses.
        regID[k] = DAP_REG_AP_0x4; regData[k++] = static_cast<int>(orb::kDcbDcrdr);
        regID[k] = DAP_REG_AP_0xC; regData[k++] = static_cast<int>(val[i]);
        regID[k] = DAP_REG_AP_0x4; regData[k++] = static_cast<int>(orb::kDcbDcrsr);
        regID[k] = DAP_REG_AP_0xC; regData[k++] = static_cast<int>(sel[i] | orb::kDcrsrRegWnR);
    }

    if (api.DAP_RegAccessBlock(dap.Handle(), 0, k, regID, regData) != 0) return false;

    // CSW was reprogrammed behind Dap's back: make it reprogram on the next
    // MEM-AP access.
    dap.InvalidateCswCache();
    return true;
}

// Runs one algorithm function and returns its r0.  r0/r1/r2 carry the
// arguments, r9 is the static base and LR is pointed at a BKPT we placed in
// RAM, so the algorithm's `bx lr` halts the core instead of running wild.
bool CallAlg(uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t& result) {
    Context& c = Get();
    result = 0;
    if (!c.connected || fn == 0) return false;

    // Bracket every call in the log.  A MEM-AP transfer that never returns
    // blocks uVision's thread, and then the *last* log line is the only thing
    // that says which call did it (bug.md E14).
    const uint64_t tEnter = GetTickCount64();
    ORB_LOG_INFO("Flash: call 0x%08X(0x%08X, 0x%08X, 0x%08X)", fn, r0, r1, r2);

    // The registers below are written through DCRSR, which the hardware only
    // honours in debug state.  Resume() with the PC still somewhere in the
    // application runs the application for the whole timeout instead, and the
    // only symptom is "r0 unchanged" (bug.md E12).
    if (!c.core.IsHalted()) {
        ORB_LOG_WARN("Flash: core was not halted before 0x%08X - halting it now", fn);

        // Why the core is not where the previous call left it: a reset (clears C_HALT, re-locks debug,
        // so the halt below cannot confirm either) or a single lost MEM-AP transfer.  RCC_CSR separates
        // them, VTOR says whose vector table is in use.  This is the branch a *transient* failure takes:
        // measured, the program pass died at 0x08007C00 after 31 good pages and an identical re-run
        // passed (COMPAT_ANALYSIS.md §10.19).
        uint32_t rcc = 0, vtor = 0, cfsr = 0, hfsr = 0, dhcsrNow = 0, demcrNow = 0;
        c.dap.MemRead32(0x40023874u, rcc);       // STM32F4 RCC_CSR
        c.dap.MemRead32(0xE000ED08u, vtor);      // VTOR
        c.dap.MemRead32(0xE000ED28u, cfsr);      // CFSR
        c.dap.MemRead32(0xE000ED2Cu, hfsr);      // HFSR
        c.core.ReadCoreDebug(dhcsrNow, demcrNow);
        ORB_LOG_WARN("Flash: DHCSR=0x%08X DEMCR=0x%08X VTOR=0x%08X CFSR=0x%08X HFSR=0x%08X",
                     dhcsrNow, demcrNow, vtor, cfsr, hfsr);
        if (rcc) {
            // STM32F4 RCC_CSR reset flags.  Sticky until RMVF is written, so
            // they report the chip's whole history, not just this failure - a
            // set flag proves a reset happened at some point, a clear one only
            // rules out that source for the period since the last RMVF write.
            ORB_LOG_WARN("Flash: RCC_CSR=0x%08X%s%s%s%s%s%s%s",
                         rcc,
                         (rcc & 0x80000000u) ? " LPWRRSTF" : "",
                         (rcc & 0x40000000u) ? " WWDGRSTF" : "",
                         (rcc & 0x20000000u) ? " IWDGRSTF" : "",
                         (rcc & 0x10000000u) ? " SFTRSTF"  : "",
                         (rcc & 0x08000000u) ? " PORRSTF"  : "",
                         (rcc & 0x04000000u) ? " PINRSTF"  : "",
                         (rcc & 0x02000000u) ? " BORRSTF"  : "");
        }

        if (!c.core.Halt()) return false;
    }

    // The register file and the DEMCR/ACTLR debug settings are saved once per
    // flash session, not around every call - see FlashAlgEnter().
    if (!FlashAlgEnter()) return false;

    // The same eight registers the reference sets (xPSR/SB/SP/LR here, A1-A3 and PC by the caller),
    // written as one batched DAP access.  The per-register path stays as a fallback so a host without
    // DAP_RegAccessBlock (or a rejected batch) behaves exactly as before instead of failing.
    const uint32_t algSel[8] = {
        kSelR0 + 0, kSelR0 + 1, kSelR0 + 2, kSelR0 + 9,
        kSelR0 + 13, kSelR0 + 14, kSelR0 + 15, kSelXpsr,
    };
    const uint32_t algVal[8] = {
        r0, r1, r2, s_alg.sbase,
        s_alg.sp - s_alg.stack, s_alg.brk | 1u, fn & ~1u, 0x01000000u,
    };

    bool armed = WriteAlgRegsBatched(c.dap, algSel, algVal, 8);
    if (!armed) {
        ORB_LOG_WARN("Flash: batched register write unavailable - using the "
                     "per-register path");
        armed =
            c.core.WriteRegSel(kSelR0 + 0, r0) &&
            c.core.WriteRegSel(kSelR0 + 1, r1) &&
            c.core.WriteRegSel(kSelR0 + 2, r2) &&
            c.core.WriteRegSel(kSelR0 + 9, s_alg.sbase) &&
            c.core.WriteRegSel(kSelR0 + 13, s_alg.sp - s_alg.stack) &&
            c.core.WriteRegSel(kSelR0 + 14, s_alg.brk | 1u) &&   // thumb, as Keil does
            c.core.WriteRegSel(kSelR0 + 15, fn & ~1u) &&         // PC = the algorithm
            c.core.WriteRegSel(kSelXpsr, 0x01000000u);           // Thumb state
    }
    if (!armed) return false;

    // R15 is the write whose loss turns every call into "the application runs
    // for 30 s" (bug.md E12), so prove it landed before resuming.
    uint32_t pcNow = 0;
    if (!c.core.ReadRegSel(kSelR0 + 15, pcNow) || pcNow != (fn & ~1u)) {
        ORB_LOG_ERROR("Flash: PC write did not land: wanted 0x%08X, read back 0x%08X",
                      fn & ~1u, pcNow);
        return false;
    }

    // DEMCR (vector catch on every fault) and ACTLR (write buffer off) were
    // armed once by FlashAlgEnter() and are restored by FlashAlgLeave() when the
    // session ends.  The reference does not touch them per call either
    // (DbgCM/Flash.cpp:142-213).

    ORB_LOG_INFO("Flash: 0x%08X -> resume (LR=0x%08X SP=0x%08X)",
                 fn, s_alg.brk | 1u, s_alg.sp - s_alg.stack);
    if (!c.core.Resume()) {
        FlashAlgLeave();
        return false;
    }

    const uint64_t t0 = GetTickCount64();
    bool halted = false;
    struct bom* bom = orb::pdsc::Bom();
    for (;;) {
        // Pump uVision's message queue, exactly as the reference does (UV_DOXEVENTS).  The download runs
        // on uVision's own thread, so without this the IDE cannot repaint while an algorithm runs: a
        // sector erase takes 1-2 s and the log showed exactly that as a 2 s hole with no line in it.
        if (bom && bom->Notify) bom->Notify(UV_DOXEVENTS, (void *)1);

        if (c.core.IsHalted()) { halted = true; break; }
        // No Sleep.  The reference spins here: the DHCSR read is itself a USB round trip, so the loop is
        // throttled by the transport and UV_DOXEVENTS keeps uVision alive.  A 1 ms sleep on top is pure
        // detection latency, paid ~2000 times by one sector erase.
        if (GetTickCount64() - t0 >= kAlgTimeoutMs) {
            // Stop the core *before* reading it back: a DCRSR access while the
            // core runs is ignored, so the reads return the last value pushed
            // through DCRDR - which is why E12's dump showed 0x01000000 three
            // times over (the xPSR it had just written).
            uint32_t dhcsr = 0, demcr = 0;
            c.core.ReadCoreDebug(dhcsr, demcr);
            c.core.Halt();

            uint32_t pc = 0, lr = 0, sp = 0, xpsr = 0, r0now = 0;
            c.core.ReadRegSel(kSelR0 + 15, pc);
            c.core.ReadRegSel(kSelR0 + 14, lr);
            c.core.ReadRegSel(kSelR0 + 13, sp);
            c.core.ReadRegSel(kSelXpsr, xpsr);
            c.core.ReadRegSel(kSelR0, r0now);

            // A HardFault vectors into the application's handler and a watchdog
            // reset restarts the chip; neither is a lockup, yet both look like
            // "no S_HALT, ever".  VTOR says whose vector table was used.
            uint32_t cfsr = 0, hfsr = 0, mmfar = 0, bfar = 0, vtor = 0, rcc = 0;
            c.dap.MemRead32(0xE000ED28u, cfsr);      // CFSR
            c.dap.MemRead32(0xE000ED2Cu, hfsr);      // HFSR
            c.dap.MemRead32(0xE000ED34u, mmfar);     // MMFAR
            c.dap.MemRead32(0xE000ED38u, bfar);      // BFAR
            c.dap.MemRead32(0xE000ED08u, vtor);      // VTOR
            c.dap.MemRead32(0x40023874u, rcc);       // STM32F4 RCC_CSR

            const char* where = (pc >= s_ramAddr && pc < s_ramAddr + s_ramSize)
                                    ? "inside the RAM window (the algorithm itself)"
                                : (pc < s_ramAddr) ? "below the RAM window (application code)"
                                                   : "outside the RAM window";
            ORB_LOG_ERROR("Flash: algorithm 0x%08X timed out after %u ms - PC=0x%08X %s, "
                          "LR=0x%08X SP=0x%08X xPSR=0x%08X r0=0x%08X",
                          fn, kAlgTimeoutMs, pc, where, lr, sp, xpsr, r0now);
            ORB_LOG_ERROR("Flash: DHCSR(before halt)=0x%08X CFSR=0x%08X HFSR=0x%08X "
                          "MMFAR=0x%08X BFAR=0x%08X VTOR=0x%08X",
                          dhcsr, cfsr, hfsr, mmfar, bfar, vtor);
            if (rcc) {
                ORB_LOG_ERROR("Flash: RCC_CSR = 0x%08X%s", rcc,
                              (rcc & 0x20000000u) ? " - the IWDG reset the chip" : "");
            }

            // The memory the core was executing: the sentinel first, then the
            // image.  E13 was visible here as 0x0000 instead of 0xBE00.
            uint32_t rw[8] = {0};
            if (c.dap.MemRead(s_ramAddr, rw, sizeof(rw))) {
                ORB_LOG_ERROR("Flash: RAM 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X",
                              s_ramAddr, rw[0], rw[1], rw[2], rw[3],
                              rw[4], rw[5], rw[6], rw[7]);
            }
            break;
        }
    }

    // The only way out of the algorithm is `bx lr` into the sentinel, so a halt
    // anywhere else means it faulted (vector catch) or never got there.  Treat
    // that as a failure: r0 would otherwise pass for a return value and the
    // caller would believe the call succeeded.
    uint32_t pc = 0;
    if (halted && c.core.ReadRegSel(kSelR0 + 15, pc) &&
        pc != s_alg.brk && pc != s_alg.brk + 2u) {
        // Not the sentinel: with vector catch on this is a fault, and the core
        // halted on the handler the vector table pointed at.  Report the whole
        // picture - the fault status and the address that caused it - because
        // "did not halt" alone cannot be acted on (bug.md E15).
        uint32_t cfsr = 0, hfsr = 0, mmfar = 0, bfar = 0, vtor = 0;
        uint32_t lr = 0, sp = 0, xpsr = 0;
        c.dap.MemRead32(0xE000ED28u, cfsr);
        c.dap.MemRead32(0xE000ED2Cu, hfsr);
        c.dap.MemRead32(0xE000ED34u, mmfar);
        c.dap.MemRead32(0xE000ED38u, bfar);
        c.dap.MemRead32(0xE000ED08u, vtor);
        c.core.ReadRegSel(kSelR0 + 14, lr);
        c.core.ReadRegSel(kSelR0 + 13, sp);
        c.core.ReadRegSel(kSelXpsr, xpsr);

        const char* where = (pc >= s_ramAddr && pc < s_ramAddr + s_ramSize)
                                ? "inside the RAM window (the algorithm itself)"
                            : (pc < s_ramAddr && pc < 0x08000000u)
                                ? "in the application's vector table (a fault handler)"
                                : "outside the RAM window (application/flash)";

        ORB_LOG_ERROR("Flash: algorithm 0x%08X stopped at 0x%08X, not at the BKPT "
                      "sentinel 0x%08X", fn, pc, s_alg.brk);
        ORB_LOG_ERROR("Flash:   PC=0x%08X %s, LR=0x%08X SP=0x%08X xPSR=0x%08X VTOR=0x%08X",
                      pc, where, lr, sp, xpsr, vtor);
        ORB_LOG_ERROR("Flash:   CFSR=0x%08X HFSR=0x%08X MMFAR=0x%08X BFAR=0x%08X",
                      cfsr, hfsr, mmfar, bfar);

        if (cfsr & 0x00000001u)
            ORB_LOG_ERROR("Flash:   IACCVIOL  - the core fetched/executed from a "
                          "region the MPU forbids (MMFAR=0x%08X)", mmfar);
        if (cfsr & 0x00000002u)
            ORB_LOG_ERROR("Flash:   DACCVIOL  - a data access the MPU forbids "
                          "(MMFAR=0x%08X)", mmfar);
        if (cfsr & 0x00000100u)
            ORB_LOG_ERROR("Flash:   IBUSERR   - the core fetched from a bad bus "
                          "address (BFAR=0x%08X)", bfar);
        if (cfsr & 0x00000200u)
            ORB_LOG_ERROR("Flash:   PRECISERR - a store to BFAR=0x%08X faulted; "
                          "that address is the cause", bfar);
        if (cfsr & 0x00000400u)
            ORB_LOG_ERROR("Flash:   IMPRECISERR - a buffered store faulted (BFAR is "
                          "not valid); check what Init writes to flash/peripherals");
        if (cfsr & 0x00000800u)
            ORB_LOG_ERROR("Flash:   UNSTKERR  - fault while unstacking; the stack "
                          "pointer (SP=0x%08X) is suspect", sp);

        // The memory the core was running: the sentinel, then the image.
        uint32_t rw[8] = {0};
        if (c.dap.MemRead(s_ramAddr, rw, sizeof(rw))) {
            ORB_LOG_ERROR("Flash:   RAM 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X",
                          s_ramAddr, rw[0], rw[1], rw[2], rw[3],
                          rw[4], rw[5], rw[6], rw[7]);
        }

        // Reading the fault status is not enough to clear it; the next call
        // would see a stale flag and misattribute its own halt.
        c.dap.MemWrite32(0xE000ED28u, cfsr);
        c.dap.MemWrite32(0xE000ED2Cu, hfsr);

        halted = false;
    }

    ORB_LOG_INFO("Flash: 0x%08X -> %s after %u ms", fn,
                 halted ? "halted at the sentinel" : "did not halt",
                 static_cast<unsigned>(GetTickCount64() - tEnter));

    // The register file is deliberately NOT restored here: the session restores
    // it once, in FlashAlgLeave().  Restoring per call is what made a 300-call
    // download spend ~11 s in MEM-AP round trips (bug.md).
    c.core.ReadRegSel(kSelR0, result);
    return halted;
}

// ---------------------------------------------------------------------------
// FLASHPARM / progress plumbing
// vp of AG_CB_GETFLASHPARAM is "(FLASHPARM *) or NULL" (AGDI.H:1290); the reference chains the
// blocks: pass the previous block back on every call, NULL only at the start of a pass.
// Passing NULL again asks for the *first* block again, so a pass that does it never advances and
// spins (bug.md E14).
// ---------------------------------------------------------------------------
FLASHPARM* NextFlashParam(FLASHPARM* prev) {
    Context& c = Get();
    if (!c.cb) {
        ORB_LOG_WARN("Flash: uVision registered no callback - no FLASHPARM available");
        return nullptr;
    }

    // Bracket the callback (bug.md E14): a hang inside uVision leaves the "..."
    // line without its answer, which names the call that never came back.
    ORB_LOG_INFO("Flash: cb(AG_CB_GETFLASHPARAM) prev=%p ...", static_cast<void*>(prev));
    FLASHPARM* p = reinterpret_cast<FLASHPARM*>(c.cb(AG_CB_GETFLASHPARAM, prev));
    if (!p) {
        ORB_LOG_INFO("Flash: cb(AG_CB_GETFLASHPARAM) prev=%p -> NULL (list exhausted)",
                     static_cast<void*>(prev));
        return nullptr;
    }
    ORB_LOG_INFO("Flash: param %p start=0x%08lX many=%lu actSize=%lu stop=%u image=%p",
                 static_cast<const void*>(p), p->start, p->many, p->ActSize,
                 static_cast<unsigned>(p->Stop), static_cast<const void*>(p->image));

    // P1-0：把这一块镜像**留下来**。uVision 的 `p->image` 下载一结束就没人保证它还在，
    // 所以 `CodeImageAdd()` 会立刻拷走。固件 ETMCR bit8 = 0 ⇒ 直接分支要靠它推
    // （见 ORBAGDI_CodeImage.h）。擦除 pass 里 image 为空 / many 为 0，那里自动跳过。
    CodeImageAdd(p->start, p->image, p->many);

    return p;
}

void Progress(int job, int pos, const char* label) {
    Context& c = Get();
    if (!c.cb) return;
    PgRess p;
    memset(&p, 0, sizeof(p));
    p.Job   = job;
    p.pos   = pos;
    p.low   = 0;
    p.hig   = 100;
    p.label = reinterpret_cast<SC8*>(const_cast<char*>(label));

    // Same bracketing, but only INIT/KILL: SETPOS fires once per segment and
    // would drown the log for no extra information.
    const bool trace = (job != PROGRESS_SETPOS);
    if (trace) {
        ORB_LOG_INFO("Flash: cb(AG_CB_PROGRESS) job=%d pos=%d '%s' ...", job, pos,
                     label ? label : "");
    }
    c.cb(AG_CB_PROGRESS, &p);
    if (trace) ORB_LOG_INFO("Flash: cb(AG_CB_PROGRESS) job=%d returned", job);
}

// ---------------------------------------------------------------------------
// erase / program / verify
// ---------------------------------------------------------------------------
bool EraseRange(uint32_t adr, uint32_t size) {
    const FlashDevInfo& d = s_alg.dev;
    if (size == 0) return true;

    ORB_LOG_INFO("Flash: erase 0x%08X..0x%08X (%u B)", adr, adr + size, size);

    // uVision hands over every loadable segment, including ones not in flash at all (a RAM function,
    // a config block, an EEPROM window).  The reference skips those and says so ("Partial Programming
    // Done..."), because failing the whole download over them is not useful - and erasing there is
    // worse: the algorithm would compute a sector from an address outside its own table.
    if (adr < d.DevAdr || adr >= d.DevAdr + d.szDev) {
        ORB_LOG_WARN("Flash: 0x%08X..0x%08X is outside the algorithm's device "
                     "(0x%08X..0x%08X) - segment skipped",
                     adr, adr + size, d.DevAdr, d.DevAdr + d.szDev);
        return true;
    }

    // A single 0xFFFFFFFF entry means "use EraseChip for everything".
    if (d.sectors[0].szSector == 0xFFFFFFFFu) {
        uint32_t res = 0;
        return CallAlg(s_alg.fn[kFnEraseChip], d.DevAdr, 0, 0, res) && res == 0;
    }

    // How the table really reads (bug.md E17).  Two easy mistakes, each costing a silent no-op erase:
    //   1. AddrSector is an *offset* from the device base, not an address (STM32F4xx_512.FLM starts
    //      at 0x00000000 while DevAdr is 0x08000000); comparing it absolutely rejects every entry.
    //   2. An entry is the *start of a run* of equally sized sectors, lasting until the next entry
    //      and the last one until szDev.  The F4 table compresses eight sectors into three lines, so
    //      walking entry by entry erases only the first sector of each run.
    const uint32_t devBase = d.DevAdr;

    uint32_t count = 0;
    while (count < 512 && d.sectors[count].szSector != 0 &&
           d.sectors[count].szSector != 0xFFFFFFFFu) {
        ++count;
    }
    if (count == 0 || d.szDev == 0) {
        Fail("the algorithm describes no usable sectors");
        return false;
    }

    // -----------------------------------------------------------------------
    // The reference's EraseFlash loop, step for step: it erases ONE sector per iteration and lets
    // the top-of-loop "already erased" test advance past it (ESecAddr/ESecSize then describe it).
    // That same test is what skips a sector an earlier segment erased, and why there is no "nothing
    // to do" failure path - an already-erased range returns success.  Counting erase *operations*
    // and failing at zero turned exactly that case into "no sector of the algorithm covers ...".
    // -----------------------------------------------------------------------
    uint32_t nAdr   = adr;
    uint32_t nMany  = size;
    uint32_t erased = 0;

    while (nMany) {
        // Check for Last Erased Sector (:1148-1160)
        if (nAdr >= s_eraseAddr && nAdr < s_eraseAddr + s_eraseSize) {
            const uint32_t n = (s_eraseAddr + s_eraseSize) - nAdr;
            if (nMany > n) {
                nAdr  += n;
                nMany -= n;
                // Advance the progress bar while the erase pass walks the range,
                // exactly as the reference does (DbgCM/Flash.cpp:1153-1157).
                s_counter += n;
                UpdatePos(s_counter);
            } else {
                s_counter += nMany;
                UpdatePos(s_counter);
                ORB_LOG_INFO("Flash: 0x%08X..0x%08X already erased", adr, adr + size);
                return true;                    // Previously Erased
            }
        }

        // Search for the sector containing nAdr (:1162-1183).  An entry is the
        // *start of a run* of equally sized sectors, so a run that does not
        // begin at nAdr is extrapolated with its own size (:1168-1177) - that is
        // what lets a range in the middle of a run be erased at all.
        uint32_t oldadr = 0, oldsz = 0;
        uint32_t a = 0, sz = 0;
        for (uint32_t i = 0; i < count; ++i) {
            a  = devBase + d.sectors[i].AddrSector;
            sz = d.sectors[i].szSector;
            if (nAdr < a) {
                if (i != 0) {
                    const uint32_t n = oldadr + oldsz;
                    sz = oldsz;
                    a  = n + sz * ((nAdr - n) / sz);
                    break;
                }
            } else if (nAdr >= (a + sz)) {
                if ((i + 1) == count) {
                    const uint32_t n = a + sz;
                    a = n + sz * ((nAdr - n) / sz);
                    break;
                }
            } else {
                break;
            }
            oldadr = a;
            oldsz  = sz;
        }

        // A table whose first run starts above the requested address makes the extrapolation wrap to a
        // huge address that never advances (the reference loops forever).  Fail with a diagnostic
        // instead of hanging - but only after checking that a sector really does contain the address.
        if (!(nAdr >= a && nAdr < a + sz)) {
            Fail("sector table does not describe 0x%08X (found 0x%08X + %u)",
                 nAdr, a, sz);
            return false;
        }

        // Erase the sector, but only if it is not already blank (:1185-1190).
        // BlankCheck answers non-zero when the block does *not* match the empty
        // pattern, i.e. when it needs erasing.  Without a BlankCheck entry point
        // the sector is erased unconditionally, as before.
        uint32_t res = 1;
        if (s_alg.fn[kFnBlankCheck]) {
            if (!CallAlg(s_alg.fn[kFnBlankCheck], a, sz, d.valEmpty, res)) {
                Fail("blank check of 0x%08X did not stop at the sentinel", a);
                return false;
            }
        }

        if (res != 0) {
            if (!CallAlg(s_alg.fn[kFnEraseSector], a, 0, 0, res) || res != 0) {
                Fail("erase sector 0x%08X failed (%u)", a, res);
                return false;
            }
            ++erased;
            ORB_LOG_INFO("Flash: sector 0x%08X (%u B) erased", a, sz);
        } else {
            ORB_LOG_INFO("Flash: sector 0x%08X (%u B) already blank - not erased", a, sz);
        }

        // Recorded whether or not the erase ran - the reference does the same
        // (:1191-1192), and it is what advances the loop.
        s_eraseAddr = a;
        s_eraseSize = sz;
    }

    ORB_LOG_INFO("Flash: erase 0x%08X..0x%08X done (%u sector(s) erased)",
                 adr, adr + size, erased);
    return true;
}

/// Programs the pending page, if there is one.  Called when the next segment
/// leaves the page and once at the end of the program pass.
bool FlushPage() {
    if (s_pageLen == 0) return true;

    Context& c = Get();
    const uint32_t at   = s_pageAddr;
    const uint32_t len  = s_pageLen;
    s_pageLen = 0;                       // a failure must not leave it pending

    if (!c.dap.MemWrite(s_alg.buf, s_page.data(), len)) {
        Fail("cannot stage the pending page (%u bytes) at 0x%08X", len, s_alg.buf);
        return false;
    }
    uint32_t res = 0;
    if (!CallAlg(s_alg.fn[kFnProgramPage], at, len, s_alg.buf, res) || res != 0) {
        Fail("program page 0x%08X (%u bytes) failed (%u)", at, len, res);
        return false;
    }
    ORB_LOG_INFO("Flash: page 0x%08X programmed (%u bytes)", at, len);
    return true;
}

/// Programs [adr, adr+size) through the algorithm's ProgramPage, assembling whole pages first.
///
/// A partial page is held here and flushed when the next segment leaves it (DbgCM/Flash.cpp:1360),
/// filling the bytes before a mid-page segment with FlashDevice.valEmpty - they belong to a just
/// erased sector, so that is what they hold - and programming with the *filled* length.
/// No read-back: the reference leaves checking to the algorithm's Verify (per-page read-back cost
/// as much as the programming itself and was a large part of a crawling download, bug.md E20).
bool ProgramRange(uint32_t adr, uint32_t size, const uint8_t* data) {
    const FlashDevInfo& d = s_alg.dev;
    if (size == 0) return true;

    ORB_LOG_INFO("Flash: program 0x%08X..0x%08X (%u B)", adr, adr + size, size);

    if (adr < d.DevAdr || adr >= d.DevAdr + d.szDev) {
        ORB_LOG_WARN("Flash: 0x%08X..0x%08X is outside the algorithm's device "
                     "(0x%08X..0x%08X) - segment skipped",
                     adr, adr + size, d.DevAdr, d.DevAdr + d.szDev);
        return true;
    }

    const uint32_t page  = d.szPage ? d.szPage : 0x100u;
    const uint8_t  empty = d.valEmpty;
    if (s_page.size() != page) s_page.assign(page, empty);

    uint32_t done = 0;
    while (done < size) {
        const uint32_t a  = adr + done;
        const uint32_t pa = d.DevAdr + ((a - d.DevAdr) / page) * page;
        const uint32_t n  = a - pa;                  // offset inside the page
        uint32_t m = (size - done) + n;              // end offset inside the page
        if (m > page) m = page;

        // A different page, or a gap inside this one: finish what is pending.
        if (s_pageLen != 0 && (pa != s_pageAddr || n != s_pageLen)) {
            if (!FlushPage()) return false;
        }
        if (s_pageLen == 0) {
            s_pageAddr = pa;
            for (uint32_t i = 0; i < n; ++i) s_page[i] = empty;
        }

        memcpy(&s_page[n], data + done, m - n);
        for (uint32_t i = m; i < page; ++i) s_page[i] = empty;

        if (m == page) {
            s_pageLen = page;
            if (!FlushPage()) return false;
        } else {
            s_pageLen = m;
        }
        done += m - n;
    }
    return true;
}

bool VerifyRange(uint32_t adr, uint32_t size, const uint8_t* data) {
    Context& c = Get();
    ORB_LOG_INFO("Flash: verify 0x%08X..0x%08X (%u B)", adr, adr + size, size);

    // Verify on the target: the image is staged into the algorithm's page buffer and the FLM's own
    // Verify(adr, sz, buf) compares it (DbgCM/Flash.cpp:1585).  Only an FLM without a Verify entry
    // falls back to reading flash back, and that reads 0x1000 bytes at a time, never one long run.
    if (s_alg.fn[kFnVerify]) {
        const uint32_t page = s_alg.dev.szPage ? s_alg.dev.szPage : 0x100u;
        for (uint32_t done = 0; done < size; ) {
            uint32_t n = size - done;
            if (n > page) n = page;
            if (!c.dap.MemWrite(s_alg.buf, data + done, n)) {
                Fail("cannot stage %u bytes at 0x%08X for verify", n, s_alg.buf);
                return false;
            }
            uint32_t res = 0;
            if (!CallAlg(s_alg.fn[kFnVerify], adr + done, n, s_alg.buf, res)) {
                Fail("verify of 0x%08X did not stop at the sentinel", adr + done);
                return false;
            }
            // Verify returns the *end* address when the block matches and the
            // failing address otherwise (FlashOS.h).  Testing for zero here
            // would call every good block a mismatch.
            if (res != adr + done + n) {
                Fail("contents mismatch at 0x%08X (algorithm reported 0x%08X, "
                     "wanted 0x%08X)", adr + done, res, adr + done + n);
                return false;
            }
            done += n;
        }
        return true;
    }

    std::vector<uint8_t> rd;
    for (uint32_t done = 0; done < size; ) {
        uint32_t n = size - done;
        if (n > 0x1000u) n = 0x1000u;
        rd.assign(n, 0);
        if (!c.dap.MemRead(adr + done, rd.data(), n)) {
            Fail("cannot read 0x%08X back for verify", adr + done);
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (rd[i] != data[done + i]) {
                Fail("contents mismatch at 0x%08X: target 0x%02X, image 0x%02X",
                     adr + done + i, rd[i], data[done + i]);
                return false;
            }
        }
        done += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PDSC flash sequences
// A device whose CMSIS-Pack debug description drives erase and program itself is flashed through
// uVision's sequence interpreter instead of an FLM: every EraseSector / ProgramPage call below
// returns into uVision, which runs the pack script (its target accesses come back through the
// ordinary AG_MemAccX / AG_DapAcc).  Which path runs is decided once in FlashInitLoad(), by
// orb::pdsc::FlashSeqReady().
// ---------------------------------------------------------------------------

// Programming granularity covering 'adr', with the usual 256 byte fallback.
uint32_t PdscPageFor(uint32_t adr) {
    const uint32_t p = orb::pdsc::PageSizeFor(adr);
    return p ? p : 0x100u;
}

// Erase [adr, adr+size) through FlashEraseSector, one sector at a time using
// the block geometry the description carries.  A description without block
// information falls back to a single chip erase of the range.
bool PdscEraseRange(uint32_t adr, uint32_t size) {
    ORB_LOG_INFO("Flash: PDSC erase 0x%08X..0x%08X (%u B)", adr, adr + size, size);

    const uint32_t end = adr + size;
    for (uint32_t a = adr; a < end; ) {
        orb::pdsc::FlashInfo fi;
        if (!orb::pdsc::FlashInfoFor(a, fi) || fi.sectorSize == 0) {
            ORB_LOG_INFO("Flash: PDSC has no sector geometry - chip erase "
                         "0x%08X..0x%08X", adr, end);
            if (!orb::pdsc::EraseChip(adr, size)) {
                Fail("PDSC: chip erase of 0x%08X..0x%08X failed", adr, end);
                return false;
            }
            return true;
        }
        if (!orb::pdsc::EraseSector(a)) {
            Fail("PDSC: erase sector 0x%08X failed", a);
            return false;
        }
        a += fi.sectorSize;
    }
    return true;
}

// Program [adr, adr+size) from 'data' through FlashProgramPage.  The page is
// staged in the RAM window first: the sequence runs on the target side of
// uVision, so it cannot be handed a host pointer, and __FlashArg carries the
// staging address.
bool PdscProgramRange(uint32_t adr, uint32_t size, const uint8_t* data) {
    Context& c = Get();
    ORB_LOG_INFO("Flash: PDSC program 0x%08X..0x%08X (%u B)", adr, adr + size, size);

    const uint32_t page = PdscPageFor(adr);
    const uint32_t stage = s_ramAddr;
    for (uint32_t done = 0; done < size; ) {
        uint32_t n = size - done;
        if (n > page) n = page;
        if (stage + n > s_ramAddr + s_ramSize) {
            Fail("RAM window too small to stage a %u byte page (need ram=<addr>,<size>)",
                 page);
            return false;
        }
        if (!c.dap.MemWrite(stage, data + done, n)) {
            Fail("cannot stage %u bytes at 0x%08X", n, stage);
            return false;
        }
        if (!orb::pdsc::ProgramPage(adr + done, n, stage)) {
            Fail("PDSC: program page 0x%08X failed", adr + done);
            return false;
        }
        done += n;
    }
    return true;
}

// One pass over the image uVision hands us.  A pass is demanded three times (erase / program /
// verify) - uVision replays the sequence each time.  Inside a pass the blocks must be chained
// (see NextFlashParam); a pass ends when uVision answers with a zero-byte block (or NULL).
template <class Op>
bool ForEachSegment(const char* label, bool needImage, Op op) {
    ORB_LOG_INFO("Flash: %s pass - asking uVision for the image list", label);
    Progress(PROGRESS_INIT, 0, label);

    bool ok = true;
    uint32_t seen = 0, total = 0, blocks = 0;
    FLASHPARM* prev = nullptr;   // NULL: first block of this pass
    for (;;) {
        FLASHPARM* p = NextFlashParam(prev);
        if (!p) break;
        prev = p;

        if (p->Stop) { Fail("%s cancelled by uVision", label); ok = false; break; }
        if (p->many == 0) break;
        // The erase pass never looks at the image, so do not require it there.
        if (needImage && !p->image) break;

        if (total == 0) total = p->ActSize ? p->ActSize : p->many;
        ok = op(p->start, p->many, reinterpret_cast<const uint8_t*>(p->image));
        if (!ok) break;

        seen += p->many;
        Progress(PROGRESS_SETPOS, total ? static_cast<int>(seen * 100u / total) : 0, label);

        // Safety net: an image list that never terminates must not spin forever
        // and take uVision down with it (bug.md E14).
        if (++blocks > 4096) {
            Fail("%s: uVision keeps handing out image blocks (%u so far) - giving up",
                 label, blocks);
            ok = false;
            break;
        }
    }

    Progress(PROGRESS_KILL, 0, label);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// option / path handling
// ---------------------------------------------------------------------------
void FlashOptions(const char* targArgs) {
    // flm= / ram= were already digested by cfg::Parse(), called from the same two entry points that
    // call this function.  Reuse that result: two independent parsers with two sets of defaults is
    // how the RAM window could silently disagree with what the dialog showed.
    {
        // Effective() = the project's options with everything uVision told us
        // about the target filled in.  A project that never named an algorithm
        // still gets the one its Utility page (or its device) implies.
        const cfg::Config c = cfg::Effective();
        strncpy_s(s_flm, sizeof(s_flm), c.flm, _TRUNCATE);
        s_ramAddr = c.ramAddr;
        s_ramSize = c.ramSize;
    }

    // Keil's ULINK-style command line spells the algorithm as a bare
    // "-FL<path>.FLM" token, which has no '=' and is therefore not something
    // cfg::Parse() can see.  Keep accepting that legacy form here.
    if (targArgs) {
        char buf[cfg::kArgsMax];
        strncpy_s(buf, sizeof(buf), targArgs, _TRUNCATE);

        char* p = buf;
        while (*p) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;

            char* tok = p;
            if (*p == '"') {                   // "..." keeps its content
                tok = ++p;
                while (*p && *p != '"') ++p;
                if (*p) *p++ = '\0';
            } else {
                while (*p && *p != ' ' && *p != '\t') ++p;
                if (*p) *p++ = '\0';
            }

            if (_strnicmp(tok, "flm=", 4) == 0 && tok[4]) {
                // The value may be quoted: that is how Format() writes a path, and how one with spaces survives.
                // The tokenizer only strips a quote when the token *starts* with one, and this starts with
                // "flm=", so the quotes used to reach the ELF loader ("cannot parse '"D:\...\x.FLM"' as an
                // ELF32 image") - any project with an algorithm chosen in the Settings sheet could not download.
                char* v = tok + 4;
                if (*v == '"') {
                    ++v;
                    char* close = strrchr(v, '"');
                    if (close) *close = '\0';
                }
                strncpy_s(s_flm, sizeof(s_flm), v, _TRUNCATE);
            } else if (HasSuffixNoCase(tok, ".FLM")) {
                if (_strnicmp(tok, "-FL", 3) == 0) tok += 3;
                while (*tok == '"') ++tok;
                strncpy_s(s_flm, sizeof(s_flm), tok, _TRUNCATE);
            }
        }
    }

    if (s_flm[0]) ORB_LOG_INFO("Flash: FLM = %s", s_flm);
    ORB_LOG_INFO("Flash: RAM window = 0x%08X..0x%08X", s_ramAddr, s_ramAddr + s_ramSize);
}

const char* FlashFilePath() { return s_flm; }
bool        FlashAlgorithmReady() { return s_alg.ok; }
const char* FlashLastError() { return s_err; }

// ---------------------------------------------------------------------------
// AG_INITFLASHLOAD
// ---------------------------------------------------------------------------
bool FlashInitLoad() {
    Context& c = Get();
    s_alg.ok = false;
    s_pdscSeq = false;
    s_algInitDone = false;
    s_err[0] = '\0';
    s_pageLen = 0;                      // no page pending from a previous session
    s_eraseAddr = s_eraseSize = 0;

    // A previous session that ended on an error path may still hold the saved
    // register file and the armed debug controls.  Restore them before this
    // session saves its own, so what is written back is the application's
    // register values and not an earlier session's leftovers.
    FlashAlgLeave();

    if (!c.connected) {
        Fail("target not connected");
        return false;
    }

    // The PDSC path stages page data in target RAM, so the RAM window has to
    // be valid even when the project names no FLM.  FlashOptions() normally
    // filled it; this covers a session where no entry point ran (bug.md E6).
    if (s_ramSize == 0) {
        const cfg::Config e = cfg::Effective();
        s_ramAddr = e.ramAddr;
        s_ramSize = e.ramSize;
    }

    // Path 1: the device's debug description drives erase and program itself.  Nothing of the FLM
    // machinery below is used - no algorithm, no BKPT sentinel, no FLASHPARM data pages.  The choice
    // is made here, once, so FlashStartLoad()/FlashUnInit() only have to look at s_pdscSeq.
    if (orb::pdsc::FlashSeqReady()) {
        if (!orb::pdsc::Init()) {
            Fail("PDSC: flash initialisation sequence failed");
            return false;
        }
        s_pdscSeq = true;
        s_alg.ok   = true;
        ORB_LOG_INFO("Flash: PDSC flash sequences active - %s", orb::pdsc::Status());
        orb::ui::Info("Flash: using the device's debug description - %s",
                      orb::pdsc::Status());
        return true;
    }
    if (orb::pdsc::Status()) ORB_LOG_INFO("Flash: %s", orb::pdsc::Status());
    ORB_LOG_INFO("Flash: no PDSC flash sequences for this target - using the FLM path");

    // FlashOptions() is driven by the debug block and normally ran long before
    // this point - but no entry point is guaranteed to have been called
    // (bug.md E6), and an algorithm is free to look at twice.  Read the
    // effective options again rather than fail on a technicality.
    if (!s_flm[0]) {
        const cfg::Config e = cfg::Effective();
        if (e.flm[0]) {
            strncpy_s(s_flm, sizeof(s_flm), e.flm, _TRUNCATE);
            s_ramAddr = e.ramAddr;
            s_ramSize = e.ramSize;
            ORB_LOG_INFO("Flash: FLM = %s", s_flm);
            ORB_LOG_INFO("Flash: RAM window = 0x%08X..0x%08X",
                         s_ramAddr, s_ramAddr + s_ramSize);
        }
    }
    if (!s_flm[0]) {
        Fail("no flash algorithm: the project names none and none could be "
             "detected - pick one under Options for Target / Utilities / "
             "Settings (Flash Download)");
        return false;
    }

    Elf32 elf;
    if (!elf.Load(s_flm)) {
        Fail("cannot parse '%s' as an ELF32 image", s_flm);
        return false;
    }

    // The FlashDevice structure describes the device; it lives in its own
    // PT_LOAD segment which is *not* copied into RAM.
    const uint32_t devAdr = elf.Sym("FlashDevice");
    if (devAdr == 0) {
        Fail("'%s' has no FlashDevice symbol", s_flm);
        return false;
    }

    bool haveDev = false;
    uint32_t base = 0;
    bool haveBase = false;
    uint32_t imageEnd = 0;

    for (uint32_t i = 0; i < elf.Eh()->e_phnum; ++i) {
        const Elf32Phdr* ph = elf.Ph(i);
        if (!ph || ph->p_type != kPtLoad || ph->p_filesz == 0) continue;
        if (ph->p_offset + ph->p_filesz > elf.Size()) continue;

        if (devAdr >= ph->p_vaddr && devAdr < ph->p_vaddr + ph->p_filesz &&
            ph->p_filesz >= sizeof(FlashDevInfo)) {
            memcpy(&s_alg.dev, elf.File() + ph->p_offset, sizeof(FlashDevInfo));
            haveDev = true;
            continue;
        }
        if (!haveBase || ph->p_vaddr < base) { base = ph->p_vaddr; haveBase = true; }

        // p_memsz, not p_filesz: .bss follows .data inside the last segment and
        // the page buffer must not land in it.
        const uint32_t end = ph->p_vaddr + (ph->p_memsz ? ph->p_memsz : ph->p_filesz);
        if (end > imageEnd) imageEnd = end;
    }
    if (!haveDev || !haveBase) {
        Fail("malformed FLM (no device record / no loadable segment)");
        return false;
    }
    if (s_alg.dev.szDev == 0) {
        Fail("'%s' describes a zero sized device", s_flm);
        return false;
    }

    // Clamp the page size the way the reference does (DbgCM/Flash.cpp:1093): a
    // page larger than the algorithm's own PAGE_MAX cannot be staged, and a zero
    // page size would divide by zero in every page computation below.
    if (s_alg.dev.szPage > 65536u) s_alg.dev.szPage = 65536u;
    if (s_alg.dev.szPage == 0)     s_alg.dev.szPage = 1u;
    if (s_alg.dev.toProg  == 0)    s_alg.dev.toProg  = 1u;
    if (s_alg.dev.toErase == 0)    s_alg.dev.toErase = 1u;

    // The sector table drives every erase, so it is validated before a single
    // byte is written (DbgCM/Flash.cpp:932 SectorCheck, :1101).
    {
        uint32_t runs = 0, sectors = 0;
        if (!CheckSectorTable(s_alg.dev, runs, sectors)) return false;
        ORB_LOG_INFO("Flash: '%s' - %u sector run(s) / %u sector(s), page=%u, "
                     "empty=0x%02X", s_alg.dev.DevName, runs, sectors,
                     s_alg.dev.szPage, s_alg.dev.valEmpty);
    }

    // Copy the loadable segments into the RAM window keeping their linked layout (one delta for all,
    // as Keil's "Relocate Programming Algorithms" does).  Four bytes at the window start are reserved
    // for the BKPT sentinel ("BreakPoint_ARM" = 00 BE) - behind the image it would sit in .data/.bss.
    const uint32_t ofs = 4;
    const uint32_t delta = s_ramAddr + ofs - base;

    for (uint32_t i = 0; i < elf.Eh()->e_phnum; ++i) {
        const Elf32Phdr* ph = elf.Ph(i);
        if (!ph || ph->p_type != kPtLoad || ph->p_filesz == 0) continue;
        if (ph->p_offset + ph->p_filesz > elf.Size()) continue;
        if (devAdr >= ph->p_vaddr && devAdr < ph->p_vaddr + ph->p_filesz) continue;

        const uint32_t dst = ph->p_vaddr + delta;
        if (!c.dap.MemWrite(dst, elf.File() + ph->p_offset, ph->p_filesz)) {
            Fail("cannot load algorithm image at 0x%08X", dst);
            return false;
        }

        // Read it back.  A write that reports success but lands nowhere leaves
        // the core fetching whatever the RAM happened to hold - "MOVS r0,r0"
        // forever when it is erased, with no register changed and no fault.
        uint8_t back[64];
        for (uint32_t o = 0; o < ph->p_filesz; o += sizeof(back)) {
            const uint32_t n = (ph->p_filesz - o < sizeof(back))
                                   ? (ph->p_filesz - o)
                                   : static_cast<uint32_t>(sizeof(back));
            if (!c.dap.MemRead(dst + o, back, n) ||
                memcmp(back, elf.File() + ph->p_offset + o, n) != 0) {
                Fail("algorithm image does not read back at 0x%08X", dst + o);
                return false;
            }
        }
    }
    auto fn = [&](const char* name) -> uint32_t {
        const uint32_t v = elf.Sym(name);
        return v ? (v + delta) : 0;
    };

    s_alg.fn[kFnInit]        = fn("Init");
    s_alg.fn[kFnUnInit]      = fn("UnInit");
    s_alg.fn[kFnEraseChip]   = fn("EraseChip");
    s_alg.fn[kFnEraseSector] = fn("EraseSector");
    s_alg.fn[kFnProgramPage] = fn("ProgramPage");
    s_alg.fn[kFnVerify]      = fn("Verify");
    s_alg.fn[kFnBlankCheck]  = fn("BlankCheck");

    if (!s_alg.fn[kFnInit] || !s_alg.fn[kFnProgramPage] || !s_alg.fn[kFnEraseSector]) {
        Fail("FLM is missing Init/EraseSector/ProgramPage");
        return false;
    }

    // Static base (r9): the algorithm's first writable section, which is what
    // the linker used for its SB relative addresses.  The RAM window start is
    // only right when the image happens to begin with .data (Keil: FlashAlg.rSB).
    s_alg.sbase = 0;
    for (uint32_t i = 0; i < elf.Eh()->e_shnum; ++i) {
        const Elf32Shdr* sh = elf.Sh(i);
        if (!sh || sh->sh_addr == 0 || sh->sh_size == 0) continue;
        if ((sh->sh_flags & (kShfAlloc | kShfWrite)) != (kShfAlloc | kShfWrite)) continue;
        const uint32_t a = sh->sh_addr + delta;
        if (!s_alg.sbase || a < s_alg.sbase) s_alg.sbase = a;
    }
    if (!s_alg.sbase) s_alg.sbase = s_ramAddr + ofs;

    // No BreakPoint symbol needed: Keil's own sentinel, planted above.
    s_alg.brk   = s_ramAddr;
    s_alg.stack = elf.Sym("STACK_SIZE");
    if (s_alg.stack == 0 || s_alg.stack > 0x4000u) s_alg.stack = 0x200u;
    s_alg.sp    = s_ramAddr + s_ramSize;
    s_alg.buf   = Align4(imageEnd + delta);

    if (s_alg.buf + s_alg.dev.szPage + s_alg.stack > s_alg.sp) {
        Fail("RAM window too small for the algorithm (need ram=<addr>,<size>)");
        return false;
    }

    // Plant it with a word write and read it back: the old 2 byte write went
    // through the MEM-AP's byte lanes and silently stored 0x0000, so `bx lr`
    // fell through the sentinel into the image and the core spun inside
    // GetSecNum until the timeout (bug.md E13).
    const uint32_t sentinel = 0x0000BE00u;    // BKPT 0xBE00, filler halfword after
    uint32_t chk = 0;
    if (!c.dap.MemWrite32(s_alg.brk, sentinel)) {
        Fail("cannot place the BKPT sentinel at 0x%08X", s_alg.brk);
        return false;
    }
    if (!c.dap.MemRead32(s_alg.brk, chk) || chk != sentinel) {
        Fail("BKPT sentinel at 0x%08X reads back 0x%08X, not 0x%08X",
             s_alg.brk, chk, sentinel);
        return false;
    }

    ORB_LOG_INFO("Flash: '%s' dev='%s' szDev=0x%X page=%u loaded at 0x%08X..0x%08X, "
                 "Init=0x%08X sb=0x%08X brk=0x%08X buf=0x%08X",
                 s_flm, s_alg.dev.DevName, s_alg.dev.szDev, s_alg.dev.szPage,
                 s_ramAddr, s_ramAddr + ofs + (imageEnd - base), s_alg.fn[kFnInit],
                 s_alg.sbase, s_alg.brk, s_alg.buf);

    // The sector table drives every erase, and misreading it is silent: the
    // erase pass just stops calling the algorithm and still reports success.
    // Print the runs so the log shows what erase is about to walk (bug.md E17).
    {
        uint32_t runs = 0;
        while (runs < 512 && s_alg.dev.sectors[runs].szSector != 0 &&
               s_alg.dev.sectors[runs].szSector != 0xFFFFFFFFu) {
            ++runs;
        }
        for (uint32_t i = 0; i < runs; ++i) {
            ORB_LOG_INFO("Flash: sector run %u: DevAdr(0x%08X)+0x%08X sz=0x%X",
                         i, s_alg.dev.DevAdr, s_alg.dev.sectors[i].AddrSector,
                         s_alg.dev.sectors[i].szSector);
        }
    }

    // No Init() here - loading and initialising are two different steps in the reference: every pass
    // opens with its own Init(DevAdr, clk, fnc) and closes with the matching UnInit(fnc).
    // Running Init(1) here cost two things: the first erase ran Init(1) again on a target already in
    // erase mode and the algorithm never reached the sentinel, and with the erase pass skipped (or
    // "Download to Flash" off) that Init was never closed.
    ORB_LOG_INFO("Flash: algorithm loaded - the passes run their own Init/UnInit");

    // Path 2: the device still uses the FLM for erase/program, but its debug description may add
    // state changes around it (FlashInit / FlashEraseDone / FlashProgramDone / FlashUninit).  The
    // reference keeps the FLM and layers the PDSC hooks on top; absent sequences are no-ops.
    orb::pdsc::Init();

    s_alg.ok = true;
    // The one line a user needs to see before pressing Download: which algorithm
    // was loaded, for which device, and how big a page it writes.  "Wrong .FLM
    // silently selected" is otherwise only visible in a log.
    orb::ui::Info("Flash: algorithm ready - %s, %u kB, page %u B, "
                  "sectors from 0x%08X",
                  s_alg.dev.DevName, s_alg.dev.szDev / 1024u, s_alg.dev.szPage,
                  s_alg.dev.DevAdr);
    return true;
}

// ---------------------------------------------------------------------------
// pass bracketing
// Every pass is bracketed with Init(adr, clk, fnc) .. UnInit(fnc): EraseAll fnc = 1, WriteAll
// fnc = 2, VerifyAll fnc = 3.  Init is where a Keil algorithm unlocks the flash controller,
// picks wait states and decides which registers it may touch, so an algorithm only ever
// initialised for "erase" can return 0 from EraseSector/ProgramPage without the flash changing.
// ---------------------------------------------------------------------------
static bool FlashInitFnc(uint32_t fnc) {
    Context& c = Get();
    if (!s_alg.fn[kFnInit]) return true;
    const uint32_t autoHz = cfg::Effective().clockHz;
    const uint32_t clk = c.tclk ? c.tclk : (autoHz ? autoHz : 1000000u);
    uint32_t res = 0;
    if (!CallAlg(s_alg.fn[kFnInit], s_alg.dev.DevAdr, clk, fnc, res)) {
        Fail("flash algorithm did not stop at the sentinel (Init fnc=%u)", fnc);
        return false;
    }
    if (res != 0) {
        Fail("algorithm Init(fnc=%u) returned %u", fnc, res);
        return false;
    }
    s_algInitDone = true;                // this pass owes an UnInit(fnc)
    return true;
}

static void FlashUnInitFnc(uint32_t fnc) {
    // Straight from the reference: UnInit carries the fnc of the Init it closes, one of each per pass.
    // Sending UnInit when no Init is open talks to an algorithm never configured for this mode - what
    // the old code did for every erase pass, because the Init it believed in happened at load time.
    if (!s_algInitDone) return;
    s_algInitDone = false;
    if (!s_alg.fn[kFnUnInit]) return;
    uint32_t res = 0;
    CallAlg(s_alg.fn[kFnUnInit], fnc, 0, 0, res);
}

/// Session teardown for the "Download to Flash off" path.  It writes the image over the MEM-AP and
/// never runs a flash pass, so no Init() was issued and no UnInit() may be sent either.  What has
/// to be closed is the session state: the PDSC FlashInit hook (paired with FlashUninit when the
/// device describes one) and s_alg.ok, so the next Flash / Erase loads a fresh image.
static void FlashUnInitRam() {
    orb::pdsc::Uninit();
    s_pdscSeq     = false;
    s_algInitDone = false;
    s_alg.ok      = false;
}

// ---------------------------------------------------------------------------
// AG_STARTFLASHLOAD
// ---------------------------------------------------------------------------
/// "Reset and Run" (Flash page): reset the target once the image is in and let it go.
void ResetAndRunIfAsked() {
    if (!cfg::Get().resetAndRun) return;
    Context& c = Get();
    ORB_LOG_INFO("Flash: 'Reset and Run' - resetting the target");
    if (c.core.Reset(false)) c.core.Resume();
}

// ---------------------------------------------------------------------------
// PDSC 序列之后的访问恢复 —— 参考的固定形状（FlashEraseDone/ProgramDone 之后调 DebugAccessEnsure）
// 为什么必须在这里：跑序列时目标在 pack 脚本里执行擦/写，是长操作，期间链路可能已经掉级，
// 等下一次用户访问才修就太晚。
// 与参考的有意差别：失败只告警、不中止下载 —— 链路暂时不健康就丢掉整次结果更糟；
// 恢复由后台监视器（TracePump 里的 DSMonitor 等价物）继续推进。
// ---------------------------------------------------------------------------
void DebugAccessEnsureAfterSeq()
{
    if (!orb::pdsc::Available()) return;      // 参考的 PDSCDebug_IsEnabled()
    Context& c = Get();
    if (DebugAccessEnsure(c.dap, c.core, &c.bps) != 0) {
        ORB_LOG_WARN("Flash: 序列结束后调试访问未恢复到 kAccNormal - 后台监视器会继续修");
    }
}

// ---------------------------------------------------------------------------
// 抽数挂起（官方 DSM_SuspendMonitor；参考在擦/写/校验之前都先挂起监控线程）
// 抽数移到 worker 线程之后，"worker 拿不到锁就让开"对 Flash 不够：它会插进两个时间片之间，
// 而擦/写/校验是长操作。所以显式挂起，再等它退出当前排空段。
// ---------------------------------------------------------------------------
namespace {
struct PumpQuiesce {
    PumpQuiesce() {
        TracePumpSuspend();                 // worker 不会再开始新的一片

        // 拿一次锁等它退出正在跑的那一片，**拿到就放开**：持有整段下载会让
        // uVision 的寄存器轮询/断点安装干等（官方 CheckCom 是无限等待）。
        // 顺序成立靠 worker 拿到锁后的二次挂起检查（见 ORBAGDI_TracePump.cpp）。
        {
            ComLockWait lock(1000);
            if (!lock.Held()) {
                ORB_LOG_WARN("Flash: 1 s 内没等到抽数 worker 让出探针，"
                             "继续但可能并发访问探针");
            }
        }
    }
    ~PumpQuiesce() { TracePumpResume(); }
};
} // namespace

bool FlashStartLoad() {
    if (!s_alg.ok) {
        Fail("flash algorithm is not loaded");
        return false;
    }

    // 抽数挂起：整个下载期间探针归 Flash。
    PumpQuiesce pumpOff;


    // Everything below is download output, so it goes to the build window.
    FlashModeGuard flashMode;

    // The Flash page's switches.  "Do not Erase" skips the erase pass, and the
    // program / verify passes are gated the same way.  Verify is additionally
    // gated on Program: verifying a block that was never programmed says nothing
    // about the image that was supposed to be there.
    const cfg::Config& opt = cfg::Get();
    const bool doErase  = (opt.eraseMode != cfg::kEraseNone);
    const bool doProg   = opt.program;
    const bool doVerify = doProg && opt.verify;
    ORB_LOG_INFO("Flash: passes - erase=%d program=%d verify=%d",
                 doErase ? 1 : 0, doProg ? 1 : 0, doVerify ? 1 : 0);

    // "Download to Flash" off: the image goes straight to its addresses over the
    // MEM-AP instead of through the flash algorithm - a plain RAM download.
    if (!opt.downloadToFlash) {
        ORB_LOG_INFO("Flash: 'Download to Flash' is off - writing the image to RAM");
        // Remember which block failed.  "RAM download failed" alone names only the symptom; the address
        // and DAP status separate "image not meant for RAM" from "wrong RAM window" from "link dropped".
        // This is the path taken whenever "Download to Flash" is unchecked, so it may be all the user sees.
        uint32_t badAdr = 0, badLen = 0;
        int      badErr = 0;
        const bool ok = ForEachSegment("Write", true,
                                       [&](uint32_t adr, uint32_t many, const uint8_t* img) {
                                           Context& c = Get();
                                           if (c.dap.MemWrite(adr, img, many)) return true;
                                           badAdr = adr;
                                           badLen = many;
                                           badErr = c.dap.LastError();
                                           return false;
                                       });
        if (ok) {
            orb::ui::Info("Flash: RAM download finished OK");
        } else if (badAdr) {
            orb::ui::Warn("Flash: RAM download failed - the write of %u bytes to "
                          "0x%08X was refused (DAP error %d). The image goes to RAM "
                          "because 'Download to Flash' is off; check that the address "
                          "is writable RAM and that the target is halted.",
                          badLen, badAdr, badErr);
        } else {
            // Stopped before a single block was written: uVision cancelled the
            // pass, or the image list ended early.  Not guessed at here - the
            // log carries the reason.
            orb::ui::Warn("Flash: RAM download failed - the pass stopped before any "
                          "block was written (cancelled, or the image list ended "
                          "early); see the log");
        }
        // AG_INITFLASHLOAD only loads now, so nothing ran the algorithm's Init
        // on this path - and an UnInit without an Init is a call into an
        // algorithm that was never configured.  Only the session state is
        // closed (see FlashUnInitRam).
        FlashUnInitRam();

        ResetAndRunIfAsked();
        return ok;
    }

    // uVision replays its image list for every callback round, so each pass
    // starts from the beginning with a NULL - but within a pass the blocks are
    // chained through the callback (bug.md E14).
    if (s_pdscSeq) {
        // PDSC path: the erase and program work happens inside uVision's
        // sequence interpreter; this loop only feeds it the addresses, the
        // page data and the completion hooks, in the same order as the
        // reference driver (DbgCM/Flash.cpp FlashLoad).
        if (doErase) {
            if (!ForEachSegment("Erase", false,
                                [](uint32_t adr, uint32_t many, const uint8_t*) {
                                    return PdscEraseRange(adr, many);
                                })) {
                return false;
            }
            orb::pdsc::EraseDone();
            DebugAccessEnsureAfterSeq();     // 参考 Flash.cpp:1864
        }

        if (doProg) {
            if (!ForEachSegment("Program", true,
                                [](uint32_t adr, uint32_t many, const uint8_t* img) {
                                    return PdscProgramRange(adr, many, img);
                                })) {
                return false;
            }
            orb::pdsc::ProgramDone();
            DebugAccessEnsureAfterSeq();     // 参考 Flash.cpp:1909
        }

        if (doVerify) {
            if (!ForEachSegment("Verify", true,
                                [](uint32_t adr, uint32_t many, const uint8_t* img) {
                                    return VerifyRange(adr, many, img);
                                })) {
                return false;
            }
        }

        // Same "put the target back" step as the FLM path below.  For a description-driven device that
        // step is SEQ_FlashUninit, and it must not wait for AG_UNINIT either - a session that keeps
        // debugging after a download would otherwise leave the target in the programming state.
        FlashUnInit();

        orb::ui::Info("Flash: download finished OK");
        ResetAndRunIfAsked();
        return true;
    }

    if (doErase) {
        // The pass's own Init - mode 1, exactly as EraseAll does it
        // (DbgCM/Flash.cpp:1249).  Loading the FLM no longer runs one, so this
        // is the only Init(1) of the pass and the UnInit(1) at its end now has
        // something to close.
        if (!FlashInitFnc(1)) return false;

        // A fresh pass: nothing is known to be erased yet.
        s_eraseAddr = s_eraseSize = 0;

        // Byte-based progress, as the reference sets it up before erasing
        // (DbgCM/Flash.cpp:1220 `TotalCount = pF->ActSize;`).  The segments are
        // summed first so EraseRange() can report a real percentage while it
        // walks a range - this pass touches no target memory.
        s_counter = 0;
        s_total   = 0;
        ForEachSegment("Erase", false,
                       [](uint32_t, uint32_t many, const uint8_t*) {
                           s_total += many;
                           return true;
                       });

        if (!ForEachSegment("Erase", false,
                            [](uint32_t adr, uint32_t many, const uint8_t*) {
                                return EraseRange(adr, many);
                            })) {
            return false;
        }
        // The "done" hooks are the one piece of the flash sequences the
        // reference driver always runs, FLM or not; absent sequences are no-ops.
        orb::pdsc::EraseDone();
        DebugAccessEnsureAfterSeq();     // 参考 Flash.cpp:1864
        FlashUnInitFnc(1);
    }

    if (doProg) {
        // Re-init for programming, exactly as WriteAll does (DbgCM/Flash.cpp:1459).
        if (!FlashInitFnc(2)) return false;
        s_pageLen = 0;                   // nothing pending from an earlier pass
        if (!ForEachSegment("Program", true,
                            [](uint32_t adr, uint32_t many, const uint8_t* img) {
                                return ProgramRange(adr, many, img);
                            })) {
            return false;
        }
        // The last segment of the pass usually stops short of a page boundary,
        // so the page it left behind is still pending.  The reference flushes it
        // with a WriteFlash(start, 0, NULL) call for exactly this reason
        // (DbgCM/Flash.cpp:1476).
        if (!FlushPage()) return false;
        orb::pdsc::ProgramDone();
        DebugAccessEnsureAfterSeq();     // 参考 Flash.cpp:1909
        FlashUnInitFnc(2);
    }

    if (doVerify) {
        // Re-init for verification, exactly as VerifyAll does: FlashOS.h gives it
        // a distinct Init(3) entry (DbgCM/Flash.cpp:1523), and an algorithm that
        // is in its program state cannot be assumed to verify correctly.
        if (!FlashInitFnc(3)) return false;
        if (!ForEachSegment("Verify", true,
                            [](uint32_t adr, uint32_t many, const uint8_t* img) {
                                return VerifyRange(adr, many, img);
                            })) {
            return false;
        }
        FlashUnInitFnc(3);
    }

    orb::ui::Info("Flash: download finished OK");

    // "Put the target back" - the algorithm's UnInit(fnc) and, if described, its FlashUninit - is done
    // by the passes above, each closing its own Init.  That is the point: an algorithm configures the
    // flash controller while programming (STM32F4: ACR latency / prefetch / data cache) and restores
    // it in UnInit, so leaving it until session end sends every flash read in between through a
    // controller still set up for programming ("same address, different data").
    // What is left here is the session itself: close an Init no pass closed, run the PDSC FlashUninit
    // hook, clear s_alg.ok.  Must stay before FlashAlgLeave() and ResetAndRunIfAsked().
    FlashUnInit();

    // Session over: write the saved register file back and disarm DEMCR/ACTLR.
    FlashAlgLeave();
    ResetAndRunIfAsked();
    return true;
}

// ---------------------------------------------------------------------------
// AG_UNINIT
// ---------------------------------------------------------------------------
void FlashUnInit() {
    if (!s_alg.ok) return;
    Context& c = Get();

    if (s_pdscSeq) {
        if (c.connected) orb::pdsc::Uninit();
        s_pdscSeq     = false;
        s_algInitDone = false;
        s_alg.ok      = false;
        return;
    }

    // Only when a pass left its Init open - a download that failed between Init(fnc) and its
    // UnInit(fnc), or was cancelled.  A completed download closed every pass and the reference sends
    // nothing more; an unconditional UnInit(0) here was a second UnInit for the same algorithm.
    if (s_algInitDone) {
        s_algInitDone = false;
        if (c.connected && s_alg.fn[kFnUnInit]) {
            uint32_t res = 0;
            CallAlg(s_alg.fn[kFnUnInit], 0, 0, 0, res);
        }
    }
    orb::pdsc::Uninit();
    s_alg.ok = false;
}

// ---------------------------------------------------------------------------
// AG_MemAcc(AG_F_ERASE) / (AG_F_RUN) - uVision's Flash menu
// ---------------------------------------------------------------------------
/// Flash / Erase.  The reference answers with EraseFull() plus a dead loop at the algorithm's
/// entry plus ExitDebug; the dead loop is what the BKPT sentinel already is, and the session is
/// kept alive - the user asked to erase the chip, not to end the debug session.
bool FlashEraseChip() {
    Context& c = Get();
    if (!c.connected) { Fail("target not connected"); return false; }

    // 整片擦除同样是长目标操作，探针归 Flash。
    PumpQuiesce pumpOff;

    // Flash / Erase can be pressed without a download having run first, so load
    // the algorithm here - exactly what AG_INITFLASHLOAD does.  Loading is all
    // it does: the pass's own Init(1) is issued below, once.
    if (!s_alg.ok) {
        ORB_LOG_INFO("Flash: AG_F_ERASE with no algorithm loaded - loading it now");
        if (!FlashInitLoad()) return false;
    }

    const uint32_t base = s_alg.dev.DevAdr;
    const uint32_t size = s_alg.dev.szDev;
    FlashModeGuard flashMode;
    orb::ui::Info("Flash: erasing the whole device - %u kB from 0x%08X",
                  size / 1024u, base);

    if (s_pdscSeq) {
        if (!PdscEraseRange(base, size)) return false;
        orb::pdsc::EraseDone();
        orb::ui::Info("Flash: chip erase finished OK");
        return true;
    }

    if (!FlashInitFnc(1)) return false;
    s_eraseAddr = s_eraseSize = 0;
    if (!EraseRange(base, size)) return false;
    orb::pdsc::EraseDone();
    DebugAccessEnsureAfterSeq();     // 参考 Flash.cpp:1981
    FlashUnInitFnc(1);
    orb::ui::Info("Flash: chip erase finished OK");
    return true;
}

/// Flash / Run: reset the target and let the application go.  The reference does
/// a hardware reset and prints "Application running ..." (DbgCM/Flash.cpp:1731
/// RunFlashAppl).
bool FlashRunApp() {
    Context& c = Get();
    if (!c.connected) { Fail("target not connected"); return false; }
    ORB_LOG_INFO("Flash: AG_F_RUN - resetting and running the application");
    if (!c.core.Reset(false)) return false;
    return c.core.Resume();
}

// ---------------------------------------------------------------------------
// initialisation of the module state
// ---------------------------------------------------------------------------
struct FlashBoot {
    FlashBoot() {
        memset(&s_alg, 0, sizeof(s_alg));
        s_flm[0] = '\0';
        s_err[0] = '\0';
        s_pdscSeq   = false;
        s_pageAddr  = 0;
        s_pageLen   = 0;
        s_eraseAddr = 0;
        s_eraseSize = 0;
    }
} g_flashBoot;

} // namespace agdi
} // namespace orb
