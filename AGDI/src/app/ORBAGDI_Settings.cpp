/**
 * @file ORBAGDI_Settings.cpp
 * @brief DbgCM Debug / Utilities 页打开的设置表。
 *
 * 版面照 Keil 的 ULINK Pro 驱动：412x240 容器 + 404x212 tab + 每页 400x188 子对话框，
 * 样式词汇（groupbox / STATIC / COMBOBOX / EDIT / BS_AUTOCHECKBOX）也取自它的资源。
 * 故意的取舍：不再问 RDDI 宿主 DLL 路径（自动发现，只上报，错误进 Info 页）；
 * 接口 / AP 索引改为经 RDDI 层实测（Detect + Identify + SELECT + AP IDR）后由用户挑。
 * 底部四个按钮里三个是占位，只有 Power 页是真的：它不进选项串、不落工程，且**只有 Apply
 * 碰硬件**（OK/Cancel 不碰）—— 那是目标的供电轨，误点不是外观问题。
 * 对话框在运行时由内存模板建：构建里没有 rc.exe 阶段。
 */
#include "pch.h"

#include <commdlg.h>    // OPENFILENAMEW / GetOpenFileNameW
#include <commctrl.h>   // InitCommonControlsEx / TabCtrl_*
#include <stdarg.h>     // va_list, for Report()
#include <errno.h>      // errno, reported when the log cannot be read back
#include <share.h>      // _SH_DENYNO, for reading the log while it is open
#include <algorithm>    // std::unique
#include <vector>

#include "ORBAGDI_Config.h"
// TraceOpt 的事件位（`kTraceCpi` … `kTraceExcTrc`）—— "Trace Events" 那 6 个复选框
// 要按 TraceOpt 的**位值**写进 `events=`，而不是 `1 << i`：图上的顺序（CPI 打头）与
// TraceOpt 的位序（EXCTRC 0x0010 打头）不同，见下面 kEvtBits 的说明。
#include "ORBAGDI_TraceWin.h"
// Power 页：档位表与 `OrbPowerApply` / `OrbPowerAllOff` / `OrbPowerGet` 都在那里。
// 档位表只有那一份 —— 界面从它现取，不在这里再抄一遍。
#include "ORBAGDI_Power.h"
// 运行期收 trace 源：Trace Enable 一关就必须显式停一次，见 `Accept()` 里的说明。
#include "ORBAGDI_Agdi.h"

namespace orb {
namespace cfg {

namespace {

// ---------------------------------------------------------------------------
// 控件 id，按页分组。每页第一个 id 是**钉死的**（带 ★ 的）：四个测试脚本靠
// GetDlgItem(那个 id) 判断"这页建出来了没有"，改锚点要同步改脚本。
// 其余不给初值、由编译器按声明顺序 +1 —— 手编号真正的风险是撞号：两个枚举量取同一值
// 编译不报错，而 GetDlgItem 只返回先建的那个，后建的成了永远读不到的摆设。
// ⚠ 每页余量 100 个号；一页超过 99 个控件就会撞进下一页的锚点，那时要整体重排基址。
// ---------------------------------------------------------------------------
enum : WORD {
    kIdTab         = 1000,   // the page selector

    // Debug page.
    kIdCmbIface    = 1101,   // ★ 锚点。interface, probed through the RDDI layer
    kIdChkSwj,               // "SWJ-DP" on / off
    kIdCmbClock,             // SWJ clock, a rate list
    kIdCmbPort,              // SWD / JTAG
    kIdCmbAp,                // AP index, probed through the RDDI layer
    kIdValSerial,            // read-only: adapter serial
    kIdValFw,                // read-only: adapter firmware version
    kIdValVendor,            // read-only: adapter vendor
    kIdCmbConnect,
    kIdCmbReset,
    kIdChkRstCon,
    kIdChkStopRst,
    kIdChkLogAcc,
    kIdChkCacheCd,
    kIdChkCacheMem,
    kIdChkDlFlash,
    kIdGrpAdapter,           // group boxes carry ids so they can be greyed too
    kIdGrpDebug,
    kIdGrpCache,

    // Trace page.
    kIdCmbTrcSrc   = 1201,   // ★ 锚点
    kIdEditSwo,
    kIdEditCpu,
    // "Trace Port" 现在是**一个**下拉（照 ULINK Pro 的图），写回 `trwidth` + `trswo`
    // **两个**键 —— 位宽只在并口下有义、编码只在 SWO 下有义，合起来正好 5 项。
    kIdCmbTrPort,
    kIdEditSwopresc,         // "SWO Prescaler" (swopresc)：trace 时钟 ÷ 它 = 位时钟
    kIdCmbSwomode,           // "SWO Encoding" (swomode)：UART (NRZ) / Manchester
    // `trbaud=` / `troflow=` / `trtpui=` / `events=` 的十六进制框不再有控件（键保留，
    // 仍可手改）。探针自己会报成帧方式，它的波特率跟随 SWO 值。
    kIdEditItmP,
    kIdEditItmPr,
    kIdCmbPcPre,
    kIdChkPcPer,
    kIdChkPcDrw,
    kIdChkTs,
    kIdChkTsWin,             // "Show the time column" (tswin) —— 有取数门的坑
    kIdGrpTrcSrc,
    kIdGrpItm,               // 目标侧 ITM/DWT：**三条源都用到**，只跟总开关走
    kIdGrpPort,              // "Trace Port" - the probe's own port format
    kIdGrpTs,                // "Timestamps" - ts= / tspresc= / tswin
    kIdChkTrcEn,             // "Trace Enable" - the master switch
    kIdEditTclk,             // "Trace Clock" - trace port clock (tclk)
    kIdCmbTsPre,             // "Timestamps: Prescaler" (tspresc)
    kIdChkEvt0,              // "Trace Events" - six DWT counters (events=)
    kIdChkEvt1,
    kIdChkEvt2,
    kIdChkEvt3,
    kIdChkEvt4,
    kIdChkEvt5,
    kIdGrpEvt,               // the "Trace Events" group box
    kIdChkEtmEn,             // "ETM Trace Enable" (etmen)
    kIdChkUnlim,             // "Unlimited Trace" (trunlim)
    kIdChkUseCore,           // "Use Core Clock" (tclkc)
    kIdCmbEtmAlt,            // "ETM Alt Encoding" (etmalt) —— **必须加在最后**，
                             // 枚举是位置编号的，插在中间会把后面所有 ID 挪位
                             // （`test\grey_sheet.ps1` 的表也会全部对不上）

    // Flash page.
    kIdEditFlm     = 1301,   // ★ 锚点
    kIdBtnFlmBrw,
    kIdBtnFlmAuto,
    kIdEditRamAdr,
    kIdEditRamSz,
    kIdChkProgram,
    kIdChkFdVerify,
    kIdChkRstRun,
    kIdValFlmInfo,
    kIdRadErase0,            // the three "Download Function" radios
    kIdRadErase1,
    kIdRadErase2,
    kIdGrpFlashFn,
    kIdGrpFlashRam,
    kIdGrpFlashAlg,

    // Pack page.
    kIdChkPdsc     = 1401,   // ★ 锚点
    kIdChkPdscLog,
    kIdEditPdscLog,
    kIdBtnPdscLog,
    kIdGrpPdsc,

    // Record page.  记录缓冲（P1-1 块链）与落盘（P2-1 / P2-2）。
    kIdGrpRecBuf   = 1701,   // ★ 锚点
    kIdEditTrChunk,          // trchunk= 每块 MB
    kIdEditTrMem,            // trmem=   内存上限 MB
    kIdGrpRecLog,
    kIdEditTrFile,           // trfile=  OFLOW 落盘路径
    kIdBtnTrFile,
    kIdEditTrRec,            // trrec=   记录日志路径
    kIdBtnTrRec,
    kIdEditTrKeep,           // trkeep=  同目录保留几个
    kIdValRecInfo,           // 生效值回显

    // Info page.
    kIdEditInfo    = 1500,   // ★ 锚点。the scrollable text box

    // Power 页，1801 是该页锚点（1501/1601/1701 已被 Info 文本框、底部按钮、Record 占掉）。
    // ⚠ 这一页**没有配置键**：不进选项串、不落工程，所以不在 Field 表里，Accept() 也不读它。
    kIdGrpPwrVtref = 1801,   // ★ 锚点。"Vtref" 组框
    kIdCmbPwrVtref,          // 档位下拉（固定档）
    kIdChkPwrVtrefEn,        // 使能
    kIdGrpPwrVtpwr,          // "Vtpwr" 组框
    kIdCmbPwrVtpwr,
    kIdChkPwrVtpwrEn,
    kIdBtnPwrApply,          // 下发 —— **只有这个按钮会碰探针的电源**
    kIdBtnPwrOff,            // 两路都关
    kIdValPwrStatus,         // 状态回显（只读）

    // Container buttons.  Placeholders: see the file comment.
    kIdBtnDefault  = 1601,
    kIdBtnTest,
    kIdBtnLog,
    kIdBtnPower
};

// ---------------------------------------------------------------------------
// The pages.  Titles stay short so all six fit the tab strip without eliding.
// ---------------------------------------------------------------------------
enum {
    kPageDebug  = 0,
    kPageTrace  = 1,
    kPageFlash  = 2,
    kPagePack   = 3,
    kPageRecord = 4,
    kPagePower  = 5,
    kPageInfo   = 6,
    kPageCount  = 7
};

const wchar_t* const kPageNames[kPageCount] = {
    L"Debug", L"Trace", L"Flash", L"Pack", L"Record", L"Power", L"Info"
};

// Standard control class atoms.  A tab control has no atom - it is created from
// the class *name* "SysTabControl32", which is why AddItemCls() exists.  (The
// previous revision passed 0x0084 here, which is the SCROLLBAR atom: the "tab"
// was a scrollbar and the pages were never really switched by it.)
enum : WORD {
    kAtomButton = 0x0080,
    kAtomEdit   = 0x0081,
    kAtomStatic = 0x0082,
    kAtomCombo  = 0x0085
};

// ---------------------------------------------------------------------------
// Styles - Keil's, verbatim where it matters.
// ---------------------------------------------------------------------------
const DWORD kStyleGroup  = WS_CHILD | WS_VISIBLE | BS_GROUPBOX;
const DWORD kStyleLabel  = WS_CHILD | WS_VISIBLE | SS_LEFT;
const DWORD kStyleKey    = WS_CHILD | WS_VISIBLE | WS_GROUP | SS_RIGHT;
const DWORD kStyleValue  = WS_CHILD | WS_VISIBLE | SS_LEFT;
const DWORD kStyleEdit   = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
const DWORD kStyleCombo  = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST;
const DWORD kStyleChk    = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
const DWORD kStyleBtn    = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
const DWORD kStyleOk     = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
const DWORD kStyleTab    = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS;
const DWORD kStyleRadio  = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;

// The Info page's text box: read-only, multi-line, wrapped, with a scrollbar of
// its own.  ES_AUTOHSCROLL must stay off or the lines would run off sideways.
const DWORD kStyleText   = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                           WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                           ES_AUTOVSCROLL | ES_LEFT;

// A dropdown is 12 units tall when closed; the template height is the height of
// the *open* list, so every combo gets a generous one.
const int kDropH = 90;

// ---------------------------------------------------------------------------
// Small control helpers
// ---------------------------------------------------------------------------
void FillCombo(HWND page, WORD id, const wchar_t* const* items, int count, int sel) {
    HWND h = GetDlgItem(page, id);
    if (!h) return;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) {
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
    }
    if (sel < 0 || sel >= count) sel = 0;
    SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

int ComboSel(HWND page, WORD id, int fallback) {
    HWND h = GetDlgItem(page, id);
    if (!h) return fallback;
    const LRESULT r = SendMessageW(h, CB_GETCURSEL, 0, 0);
    return (r == CB_ERR) ? fallback : static_cast<int>(r);
}

void SetCheck(HWND page, WORD id, bool on) {
    CheckDlgButton(page, id, on ? BST_CHECKED : BST_UNCHECKED);
}

bool IsCheck(HWND page, WORD id) {
    return IsDlgButtonChecked(page, id) == BST_CHECKED;
}

void SetDec(HWND page, WORD id, int v) {
    SetDlgItemInt(page, id, static_cast<UINT>(v), TRUE);
}

int GetDec(HWND page, WORD id, int fallback) {
    BOOL ok = FALSE;
    const int v = static_cast<int>(GetDlgItemInt(page, id, &ok, TRUE));
    return ok ? v : fallback;
}

void SetHex(HWND page, WORD id, unsigned v) {
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%08X", v);
    SetDlgItemTextW(page, id, buf);
}

/// Parsed hexadecimal; `ok` reports whether the text was well-formed.
unsigned GetHex(HWND page, WORD id, unsigned fallback, bool* ok) {
    wchar_t buf[32] = L"";
    GetDlgItemTextW(page, id, buf, 32);
    if (!buf[0]) { if (ok) *ok = true; return fallback; }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(buf, &end, 16);
    if (ok) *ok = (end && *end == L'\0');
    return static_cast<unsigned>(v);
}

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------
std::wstring Widen(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

/// The log file is written from /utf-8 sources, so its bytes are UTF-8, not
/// CP_ACP.  Invalid sequences come back as U+FFFD rather than blanking the box.
std::wstring WidenUtf8(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

void Narrow(const wchar_t* w, char* out, size_t cch) {
    if (!out || cch == 0) return;
    out[0] = '\0';
    if (!w || !*w) return;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, static_cast<int>(cch), nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Discovery：设置页不再问机器已经知道的事 —— RDDI 宿主在本 DLL 旁（或 Keil 的 ARM\BIN），
// flash 算法在 Keil 的 pack 目录里。
// ---------------------------------------------------------------------------
std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring Join(const std::wstring& dir, const wchar_t* leaf) {
    if (dir.empty()) return std::wstring(leaf);
    std::wstring s = dir;
    if (s.back() != L'\\' && s.back() != L'/') s += L'\\';
    s += leaf;
    return s;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

/// Folder this DLL lives in - the first place to look for its RDDI host.
std::wstring SelfDir() {
    wchar_t path[MAX_PATH] = L"";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfDir), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    return DirOf(path);
}

/// A Keil install root, e.g. C:\Keil_v5.
std::wstring KeilRoot() {
    for (const wchar_t* v : {L"KEIL_ROOT", L"UV4", L"KEIL_ARM_ROOT"}) {
        std::wstring r = Env(v);
        if (!r.empty() && DirExists(r)) return r;
    }
    for (const wchar_t* root : {L"C:\\", L"D:\\", L"E:\\"}) {
        if (!DirExists(root)) continue;
        for (const wchar_t* k : {L"Keil_v5", L"Keil", L"Keil_v4", L"MDK"}) {
            std::wstring c = Join(root, k);
            if (DirExists(Join(c, L"ARM\\BIN"))) return c;
        }
    }
    return std::wstring();
}

std::vector<std::wstring> RddiCandidates() {
    std::vector<std::wstring> v;
    const std::wstring env = Env(L"ORBAGDI_RDDI_DLL");
    if (!env.empty()) v.push_back(env);
    v.push_back(Join(SelfDir(), L"CMSIS_DAP.dll"));
    const std::wstring keil = KeilRoot();
    if (!keil.empty()) {
        v.push_back(Join(keil, L"ARM\\BIN\\CMSIS_DAP.dll"));
        v.push_back(Join(keil, L"ARM\\BIN\\ORBMDK_RDDI.dll"));
    }
    return v;
}

/// First candidate that exists on disk, or an empty string when none does.
std::wstring AutoDetectRddi() {
    for (const std::wstring& c : RddiCandidates()) {
        if (FileExists(c)) return c;
    }
    return std::wstring();
}

/// Folders a Keil install keeps .FLM files in.  Depth is bounded so a big pack
/// tree cannot stall the UI.
void ScanFlm(const std::wstring& dir, int depth, std::vector<std::wstring>& out) {
    if (depth > 6 || !DirExists(dir) || out.size() >= 400) return;

    WIN32_FIND_DATAW fd;
    const std::wstring mask = Join(dir, L"*");
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring full = Join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanFlm(full, depth + 1, out);
            if (out.size() >= 400) break;
        } else {
            const size_t n = wcslen(fd.cFileName);
            if (n > 4 && _wcsicmp(fd.cFileName + n - 4, L".FLM") == 0) out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<std::wstring> FlmFolders() {
    std::vector<std::wstring> v;
    for (int i = 0, n = PackRootCount(); i < n; ++i) {
        char buf[MAX_PATH];
        if (PackRoot(i, buf, sizeof(buf))) v.push_back(Widen(buf));
    }
    const std::wstring keil = KeilRoot();
    if (!keil.empty()) {
        v.push_back(Join(keil, L"ARM\\Flash"));
        v.push_back(Join(keil, L"ARM"));
    }
    const std::wstring home = Env(L"LOCALAPPDATA");
    if (!home.empty()) v.push_back(Join(home, L"Arm\\Packs"));
    return v;
}

std::vector<std::wstring> AllFlms() {
    std::vector<std::wstring> found;
    for (const std::wstring& d : FlmFolders()) ScanFlm(d, 0, found);
    for (size_t i = 1; i < found.size(); ++i) {          // insertion sort
        std::wstring key = found[i];
        size_t j = i;
        while (j > 0 && _wcsicmp(found[j - 1].c_str(), key.c_str()) > 0) {
            found[j] = found[j - 1];
            --j;
        }
        found[j] = key;
    }
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

// ---------------------------------------------------------------------------
// 页 / 表几何
// 页就是 tab 显示区：404x212 的 tab 只报 400x188，更高的模板会把越过下边缘的控件**静默裁掉**
// （"Debug 页最后一个复选框不见了"就是这个）。容器比 Keil 的高 20 个单位。
// 这四个数也是 DlgTpl 的越界上限：建模板时就查，不再只靠 inspect_sheet.ps1 事后发现。
// ---------------------------------------------------------------------------
const int kPageCx  = 400;        // page size, dialog units
const int kPageCy  = 188;
const int kSheetCx = 412;        // container size (Keil's width, +20 height)
const int kSheetCy = 240;

// ---------------------------------------------------------------------------
// DLGTEMPLATE builder.  Every DLGITEMTEMPLATE must start on a DWORD boundary,
// which Align() takes care of; strings are UTF-16 and NUL terminated.
// ---------------------------------------------------------------------------
struct DlgTpl {
    std::vector<BYTE> b;
    WORD count = 0;
    // 已经用掉的控件 id，用来查重号（见 CheckUniqueId）。
    std::vector<WORD> ids;
    // 这份模板允许的客户区。默认是页大小；容器模板（BuildSheetTpl）传自己的。
    int  maxCx = kPageCx;
    int  maxCy = kPageCy;

    DlgTpl() = default;
    DlgTpl(int cx, int cy) : maxCx(cx), maxCy(cy) {}

    void Align() { while (b.size() & 3) b.push_back(0); }
    void W(WORD v) {
        b.push_back(static_cast<BYTE>(v & 0xFF));
        b.push_back(static_cast<BYTE>(v >> 8));
    }
    void DW(DWORD v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<BYTE>((v >> (8 * i)) & 0xFF));
    }
    void Str(const wchar_t* s) {
        while (*s) W(static_cast<WORD>(*s++));
        W(0);
    }
};

/// 越出页客户区就记一条日志（带控件 id —— 它直接对回上面的 enum）。
/// 这个 bug 类在界面上只表现为"少了一个控件"，没有任何别的提示。
void CheckFits(const DlgTpl& t, WORD id, int x, int y, int cx, int cy) {
    // 下拉框的模板 cy 是**下拉列表**的高度（kDropH），收起时窗口只有 12 个单位
    // —— 列表是弹出窗口，不归页边界管。不折算的话每个下拉都会误报。
    // （与 test\inspect_sheet.ps1 一致：那边量的是 GetWindowRect，实际就是收起的
    //   那个高度。）
    if (cy == kDropH) cy = 12;
    if (x >= 0 && y >= 0 && x + cx <= t.maxCx && y + cy <= t.maxCy) return;
    ORB_LOG_WARN("Layout: control id %u at (%d,%d) %dx%d reaches past the %dx%d "
                 "client area - it will be clipped",
                 static_cast<unsigned>(id), x, y, cx, cy, t.maxCx, t.maxCy);
}

/// 重号是**静默**的：C++ 允许两个枚举量取同一个值，编译不报错；而同一个对话框里
/// 两个控件撞号时，`GetDlgItem` 只会返回先建的那个 —— 后建的那个就成了永远读不到
/// 的摆设。`0xFFFF` 是"这个静态文本没有 id"（见 Lbl / Key），不算重号。
void CheckUniqueId(DlgTpl& t, WORD id) {
    if (id == 0xFFFF) return;
    for (WORD used : t.ids) {
        if (used != id) continue;
        ORB_LOG_WARN("Layout: control id %u is used twice - GetDlgItem will only "
                     "ever return the first one", static_cast<unsigned>(id));
        return;
    }
    t.ids.push_back(id);
}

void AddItemCommon(DlgTpl& t, DWORD style, WORD id, const wchar_t* text,
                   WORD x, WORD y, WORD cx, WORD cy) {
    (void)text;                     // the callers append it after the class
    CheckFits(t, id, x, y, cx, cy);
    CheckUniqueId(t, id);
    t.Align();
    t.DW(style);
    t.DW(0);                        // dwExtendedStyle
    t.W(x); t.W(y); t.W(cx); t.W(cy);
    t.W(id);
}

void AddItemAtom(DlgTpl& t, DWORD style, WORD id, WORD atom, const wchar_t* text,
                 int x, int y, int cx, int cy) {
    AddItemCommon(t, style, id, text, static_cast<WORD>(x), static_cast<WORD>(y),
                  static_cast<WORD>(cx), static_cast<WORD>(cy));
    t.W(0xFFFF);                    // class given as an ordinal
    t.W(atom);
    t.Str(text);
    t.W(0);                         // no creation data
    ++t.count;
}

/// For controls that have no class atom - the tab control ("SysTabControl32").
void AddItemCls(DlgTpl& t, DWORD style, WORD id, const wchar_t* cls,
                const wchar_t* text, int x, int y, int cx, int cy) {
    AddItemCommon(t, style, id, text, static_cast<WORD>(x), static_cast<WORD>(y),
                  static_cast<WORD>(cx), static_cast<WORD>(cy));
    t.Str(cls);
    t.Str(text);
    t.W(0);
    ++t.count;
}

// Shorthand for the common cases.
void Lbl(DlgTpl& t, const wchar_t* text, int x, int y, int cx) {
    AddItemAtom(t, kStyleLabel, 0xFFFF, kAtomStatic, text, x, y, cx, 8);
}
void Key(DlgTpl& t, const wchar_t* text, int x, int y, int cx) {
    AddItemAtom(t, kStyleKey, 0xFFFF, kAtomStatic, text, x, y, cx, 8);
}
void Val(DlgTpl& t, WORD id, int x, int y, int cx, int cy = 8) {
    AddItemAtom(t, kStyleValue, id, kAtomStatic, L"", x, y, cx, cy);
}
void Grp(DlgTpl& t, WORD id, const wchar_t* text, int x, int y, int cx, int cy) {
    AddItemAtom(t, kStyleGroup, id, kAtomButton, text, x, y, cx, cy);
}
void Grp(DlgTpl& t, const wchar_t* text, int x, int y, int cx, int cy) {
    Grp(t, 0xFFFF, text, x, y, cx, cy);
}
void Text(DlgTpl& t, WORD id, int x, int y, int cx, int cy) {
    AddItemAtom(t, kStyleText, id, kAtomEdit, L"", x, y, cx, cy);
}
void Edit(DlgTpl& t, WORD id, int x, int y, int cx, DWORD extra = 0) {
    AddItemAtom(t, kStyleEdit | extra, id, kAtomEdit, L"", x, y, cx, 12);
}
void Cmb(DlgTpl& t, WORD id, int x, int y, int cx, DWORD extra = 0) {
    AddItemAtom(t, kStyleCombo | extra, id, kAtomCombo, L"", x, y, cx, kDropH);
}
void Chk(DlgTpl& t, WORD id, const wchar_t* text, int x, int y, int cx,
         DWORD extra = 0) {
    AddItemAtom(t, kStyleChk | extra, id, kAtomButton, text, x, y, cx, 10);
}
void Btn(DlgTpl& t, WORD id, const wchar_t* text, int x, int y, int cx,
         bool isDefault = false) {
    AddItemAtom(t, isDefault ? kStyleOk : kStyleBtn, id, kAtomButton, text,
                x, y, cx, 14);
}
void Radio(DlgTpl& t, WORD id, const wchar_t* text, int x, int y, int cx,
           DWORD extra = 0) {
    AddItemAtom(t, kStyleRadio | extra, id, kAtomButton, text, x, y, cx, 10);
}

// ---------------------------------------------------------------------------
// 行距与对齐偏移（从原来手写坐标里量出来的，游标重排后才与手写观感一致）
// 编辑框/下拉/按钮高 12/12/14，键与标签只有 8 高 ⇒ 下移 2 个单位；复选框 10 高 ⇒ 下移 1。
// 行距 = 行高 + 4。
// ---------------------------------------------------------------------------
const int kRowGap  = 4;   // 行与行之间
const int kKeyGap  = 4;   // 键列与它的控件之间
const int kCtlGap  = 4;   // 控件与下一个控件之间
// 键 / 标签 / 只读回显相对本行的下移。
// 它们是普通静态文本（SS_LEFT/SS_RIGHT，无 SS_CENTERIMAGE），文字画在矩形顶部；同行的
// 下拉/编辑框 12 高、文字垂直居中（8 高字顶边落在 +2）。不下移 2 个，只读回显会明显偏高。
const int kKeyDrop = 2;
const int kChkDrop = 1;   // 复选框相对本行的下移

// ===========================================================================
// Layout cursor
// 坐标原来逐个数着写，加一行要把下面整片重排。游标把"下一个控件放哪儿"变成状态：
//   Lay L(t, 12, 15, 374); L.AddKey(L"Block size:"); L.AddEdit(kId, 40); L.Row();
// 一页可开多个游标（左右半各一个）。越界检查不在这里 —— 所有控件最终都过 AddItemCommon()。
// ===========================================================================
class Lay {
public:
    Lay(DlgTpl& t, int left, int top, int width, int rowH = 12)
        : t_(t), left_(left), x_(left), y_(top), width_(width), rowH_(rowH) {}

    /// 换到下一行。`h > 0` 时同时改这一行的行高。
    Lay& Row(int h = 0) {
        if (h > 0) rowH_ = h;
        y_ += rowH_ + kRowGap;
        x_ = left_;
        return *this;
    }

    /// 跳到绝对 y 开一行（分组之间留白用）。
    Lay& At(int y, int h = 0) {
        if (h > 0) rowH_ = h;
        y_ = y;
        x_ = left_;
        return *this;
    }

    Lay& Gap(int dx) { x_ += dx; return *this; }

    Lay& AddKey(const wchar_t* text, int w = 62) {
        Key(t_, text, x_, y_ + kKeyDrop, w);
        x_ += w + kKeyGap;
        return *this;
    }
    /// `w <= 0` = 一直铺到这一列的右边缘。
    Lay& AddLabel(const wchar_t* text, int w = 0) {
        if (w <= 0) w = (left_ + width_) - x_;
        Lbl(t_, text, x_, y_ + kKeyDrop, w);
        x_ += w + kCtlGap;
        return *this;
    }
    Lay& AddEdit(WORD id, int w) {
        CheckColumn(id, x_, w);
        Edit(t_, id, x_, y_, w);
        x_ += w + kCtlGap;
        return *this;
    }
    Lay& AddCombo(WORD id, int w) {
        CheckColumn(id, x_, w);
        Cmb(t_, id, x_, y_, w);
        x_ += w + kCtlGap;
        return *this;
    }
    Lay& AddCheck(WORD id, const wchar_t* text, int w) {
        CheckColumn(id, x_, w);
        Chk(t_, id, text, x_, y_ + kChkDrop, w);
        x_ += w + kCtlGap;
        return *this;
    }
    /// 单选钮。与复选框一样高，所以用同一个下移量。
    Lay& AddRadio(WORD id, const wchar_t* text, int w, DWORD extra = 0) {
        CheckColumn(id, x_, w);
        Radio(t_, id, text, x_, y_ + kChkDrop, w, extra);
        x_ += w + kCtlGap;
        return *this;
    }
    Lay& AddButton(WORD id, const wchar_t* text, int w) {
        CheckColumn(id, x_, w);
        Btn(t_, id, text, x_, y_, w);
        x_ += w + kCtlGap;
        return *this;
    }
    Lay& AddValue(WORD id, int w, int h) {
        CheckColumn(id, x_, w);
        // 和 `AddKey` / `AddLabel` 同一个下移量：静态文本是**顶部对齐**的，
        // 不加这 2 个就会比同行的标签和下拉框高出一截（实测过 —— 就是它）。
        Val(t_, id, x_, y_ + kKeyDrop, w, h);
        x_ += w;
        return *this;
    }

    int Top()   const { return y_; }
    int Right() const { return x_; }

private:
    /// 越过这一列的右边缘就记一条日志。**页边界检查抓不到这一种** —— 组框离页边
    /// 还有距离，压出边框的控件仍在页内，看起来只是"盖住了边框"。
    void CheckColumn(WORD id, int x, int w) const {
        if (x + w <= left_ + width_) return;
        ORB_LOG_WARN("Layout: control id %u at x=%d w=%d reaches past this "
                     "column's right edge (%d) - it will spill out of its group",
                     static_cast<unsigned>(id), x, w, left_ + width_);
    }

    DlgTpl& t_;
    int left_, x_, y_, width_, rowH_;
};

/// Wraps an item list in a dialog template header.
DlgTpl WrapTpl(DWORD style, int cx, int cy, const wchar_t* title,
               const DlgTpl& items) {
    DlgTpl t;
    t.DW(style);
    t.DW(0);                        // dwExtendedStyle
    t.W(items.count);
    t.W(0); t.W(0); t.W(static_cast<WORD>(cx)); t.W(static_cast<WORD>(cy));
    t.W(0);                         // no menu
    t.W(0);                         // default dialog class
    t.Str(title);
    t.W(8);                         // point size
    t.Str(L"Microsoft Sans Serif"); // Keil's dialog face
    t.Align();                      // the first item must start DWORD aligned
    t.b.insert(t.b.end(), items.b.begin(), items.b.end());
    return t;
}

// ---------------------------------------------------------------------------
// Choice lists.  The index is the value the driver stores, so the order here is
// the order of the option encoding, not alphabetical.
// ---------------------------------------------------------------------------
const wchar_t* const kPortNames[]  = { L"SWD", L"JTAG" };   // SWD first: default
const wchar_t* const kConnectNames[] = {
    L"Normal", L"with Pre-reset", L"under Reset", L"without Stop"
};
const wchar_t* const kResetNames[] = {
    L"Autodetect", L"HW RESET", L"SYSRESETREQ", L"VECTRESET",
    L"Core and Peripherals"
};
const wchar_t* const kTraceSrcNames[] = {
    L"SWO pin (ITM console)", L"On-chip ETB", L"orbtrace probe"
};
// "Trace Port" 合成**一个**下拉（照 ULINK Pro 的图）：位宽只在并口下有义、编码只在 SWO 下
// 有义，拆成两个必然有一个是死的。前 3 项写 trwidth + trswo=0；后 2 项写 trswo 且**不动**
// trwidth（切回并口要用回用户原来那档，见 ReadTrace）。名字沿用 Keil 措辞，便于对照截图。
const wchar_t* const kTrPortNames[] = {
    L"Sync Trace Port with 1bit Data",
    L"Sync Trace Port with 2bit Data",
    L"Sync Trace Port with 4bit Data",
    L"SWO UART (NRZ)",
    L"SWO Manchester",
};
const int kTrPortCount     = ARRAYSIZE(kTrPortNames);
const int kTrPortParaCount = 3;                    // 前 3 项是并口
const int kTrPortWidths[]  = { 1, 2, 4, 4, 4 };    // SWO 档那 4 只是占位
const int kTrPortModes[]   = { 0, 0, 0, 1, 2 };    // -> trSwoMode
// "SWO Encoding"（`swomode`）—— **只对"SWO 引脚"那条源有义**（`ItmStart` 拿它选
// UART/Manchester）；orbtrace 那条用的是 Trace Port 下拉里的编码。
const wchar_t* const kSwoModeNames[] = { L"UART (NRZ)", L"Manchester" };
const int kSwoModeCount = ARRAYSIZE(kSwoModeNames);
// "ETM Alt Encoding"（etmalt）—— ETM 的分支地址编码，两档 Backup (ALT) / Standard (STD)，
// **由用户选**（寄存器自动判定已去掉：固件启动后才配 ETM，驱动早读会拿到旧值 ⇒ 安静选错）。
// 顺序即下拉顺序，默认档 1 = ALT 放第一项，初始选择就是默认值。
const wchar_t* const kEtmAltNames[] = { L"Backup (ALT)", L"Standard (STD)" };
const int            kEtmAltVals[]  = {  1,                0                  };
const int kEtmAltCount = ARRAYSIZE(kEtmAltNames);
const wchar_t* const kPcTapNames[] = {
    L"64 cycles", L"128 cycles", L"256 cycles", L"512 cycles", L"1024 cycles"
};
// ITM_TCR.TSPRESC 能表达的档位（ORBAGDI_Itm.cpp 的 TsPrescCode）。
const wchar_t* const kTsPrescNames[] = { L"1", L"4", L"16", L"64", L"256" };
const int            kTsPrescVals[]  = {  1,   4,   16,   64,   256  };
// "Trace Events" —— 6 个复选框，与 events= 的 TraceConf.Opt 位（TRACE_EXCTRC 0x0010 …
// TRACE_FOLD 0x0200）同序；消费端在 ORBAGDI_Itm.cpp 的 ProgramItmDwt() 逐位映射到 DWT_CTRL
// 的 bit16..bit22。TRACE_CYCCNT 没有复选框，但手改 events= 同样有效。
// ⚠ 用短名而非图上长名：这一组只有 194 单位宽，长名会被截断（LSU: Load Store Unit C…）。
// 全名：CPI=Cycles per Instruction、EXC=Exception overhead、SLEEP=Sleep Cycles、
// LSU=Load Store Unit Cycles、FOLD=Folded Instructions、EXCTRC=Exception Tracing。
const wchar_t* const kEvtNames[] = {
    L"CPI", L"EXC", L"SLEEP", L"LSU", L"FOLD", L"EXCTRC"
};
// 复选框（按图上的顺序）-> `events=` 里的位值。
// ⚠ 两个顺序**不一样**，所以必须查表：图上是 CPI 打头，TraceOpt 的位是
//   `TRACE_EXCTRC` 0x0010 打头。`1 << i` 会写出一套谁也不认的掩码。
const unsigned kEvtBits[] = {
    orb::agdi::kTraceCpi,   orb::agdi::kTraceExc,    orb::agdi::kTraceSleep,
    orb::agdi::kTraceLsu,   orb::agdi::kTraceFold,   orb::agdi::kTraceExcTrc
};
// 6 个复选框覆盖的那几位。`events=` 里还可能带着没有复选框的 `TRACE_CYCCNT`
// （0x0400），读回时不能被界面清掉。
const unsigned kEvtMask = 0x03F0u;
const wchar_t* const kEraseNames[] = {
    L"Erase Full Chip", L"Erase Sectors", L"Do not Erase"
};

const int kTraceSrcCount = 3;    // 0=swo 1=etb 2=orbtrace（"Off" 归 Trace Enable）
const int kPcTapCount    = 5;    // must match cfg::kPcPrescalerCount
const int kTsPrescCount  = 5;
const int kEvtCount      = 6;    // must match kIdChkEvt0..kIdChkEvt5
const int kEraseCount    = 3;
const int kPortCount     = 2;
const int kConnectCount  = 4;
const int kResetCount    = 5;

/// SWJ clock rates.  Keil's ULINK driver offers the same ladder; the first
/// entry is "let the host decide" so an untouched project stays at clock=0.
struct ClockRate { const wchar_t* name; int khz; };
const ClockRate kClockRates[] = {
    { L"25 MHz",     25000 },
    { L"10 MHz",     10000 },
    { L"5 MHz",       5000 },
    { L"2 MHz",       2000 },
    { L"1 MHz",       1000 },
    { L"500 kHz",      500 },
    { L"200 kHz",      200 },
    { L"100 kHz",      100 },
    { L"50 kHz",        50 },
    { L"20 kHz",        20 },
    { L"10 kHz",        10 },
    { L"5 kHz",          5 },
};
const int kClockRateCount = static_cast<int>(sizeof(kClockRates) / sizeof(kClockRates[0]));

// ---------------------------------------------------------------------------
// What the RDDI layer reported back
// ---------------------------------------------------------------------------
struct IfaceInfo {
    int          index = 0;
    std::wstring vendor;    // CMSIS_DAP_Identify idNo 1
    std::wstring name;      // CMSIS_DAP_Identify idNo 2 - the name Keil shows
    std::wstring serial;    // CMSIS_DAP_Identify idNo 3
    std::wstring fw;        // CMSIS_DAP_Identify idNo 4
};

struct ApInfo {
    int          index = 0;
    uint32_t     idr   = 0; // 0 = not probed, the index is offered as-is
    std::wstring name;
};

struct SheetCtx {
    char   args[kArgsMax] = {0};
    char*  out            = nullptr;
    size_t outCch         = 0;
    int    page0          = 0;
    bool   ok             = false;
    HWND   hTab           = nullptr;
    HWND   hPage[kPageCount] = {nullptr};

    std::vector<IfaceInfo> ifaces;      // from ProbeInterfaces()
    std::vector<ApInfo>    aps;         // from ProbeAps()
    std::vector<int>       clockVals;   // parallel to the Clock combo
    std::wstring           report;      // every message raised while the sheet is up
    // Power 页的能力探测结果。**只探一次**（它要枚举 USB），建页时填，
    // 之后灰显状态与状态回显都用它 —— 设置页开着的时候能力不会变。
    orb::agdi::PowerCaps   powerCaps;
};

void RefreshInfo(SheetCtx* ctx);
// 下面的字段表在越界时要报消息并把页切到 Info；这两个定义在本文件靠后的位置。
void ReportW(SheetCtx* ctx, const wchar_t* fmt, ...);
void SwitchPage(SheetCtx* ctx, int sel);

// ===========================================================================
// Field binding —— 一个选项键一行
// 每键原来要写三处（建控件 / 回填 / 读回），漏一处就是一个"改了不生效"的开关。这张表把
// 回填与读回压成一行；建控件仍在各页 Add*Items（布局本来就是逐页的），坐标由 Lay 给。
// 数值范围也写在表里：越界就报一条消息、焦点放回那个框、留在原页。
// ⚠ 只覆盖"1 控件 <-> 1 字段"；探测出来的下拉（接口/AP/时钟档位）与单选组仍要自己写。
// ===========================================================================
enum class FKind : uint8_t {
    Dec,    // 十进制编辑框 <-> int
    Hex,    // 十六进制编辑框 <-> unsigned（GetHex / SetHex）
    Bool,   // 复选框     <-> bool
    Text,   // 路径编辑框 <-> char[size]
};

struct Field {
    WORD           id;
    FKind          kind;
    size_t         offset;            // offsetof(Config, ...)
    size_t         size  = 0;         // Text：目标缓冲的字节数
    long long      min   = 0;         // 下限（含）
    long long      max   = 0;         // 上限（含）；`max <= min` = 不校验
    // 值不能用时给用户看的那句话。**可以带一个 `%s`** —— 会被填成控件里的原文，
    // 因为十六进制框要能说清"你写的这一串不是十六进制"。Dec 的消息里没有 `%s`，
    // 多传的那个参数被 printf 忽略掉。
    const wchar_t* msg   = nullptr;
};

void FieldFill(HWND p, const Config& c, const Field* f, int n) {
    const char* base = reinterpret_cast<const char*>(&c);
    for (int i = 0; i < n; ++i) {
        const Field& k = f[i];
        switch (k.kind) {
        case FKind::Dec:
            SetDec(p, k.id, *reinterpret_cast<const int*>(base + k.offset));
            break;
        case FKind::Hex:
            SetHex(p, k.id, *reinterpret_cast<const unsigned*>(base + k.offset));
            break;
        case FKind::Bool:
            SetCheck(p, k.id, *reinterpret_cast<const bool*>(base + k.offset));
            break;
        case FKind::Text:
            SetDlgItemTextW(p, k.id, Widen(base + k.offset).c_str());
            break;
        }
    }
}

/// 读回。返回 false = 有个值越界：原因已经写到 Info 页，调用方直接 `return false`
/// 把设置页留在原地（焦点在那个框上）。
bool FieldRead(SheetCtx* ctx, HWND p, Config& c, const Field* f, int n) {
    char* base = reinterpret_cast<char*>(&c);
    // 取一个数值字段的值；`bad` 报告它能不能用。
    auto value = [&](const Field& k, bool& bad) -> long long {
        bad = false;
        if (k.kind == FKind::Dec) return static_cast<long long>(GetDec(p, k.id, 0));
        // 十六进制：**格式错**和**超范围**是两件事，但对用户都是"这个框里的东西
        // 不能用"，所以共用一句话（消息带 %s 时他能看见自己写了什么）。
        bool wellFormed = true;
        const long long v = static_cast<long long>(GetHex(p, k.id, 0, &wellFormed));
        bad = !wellFormed;
        return v;
    };

    // 第一遍：**只校验**，一个都不写。原来是"先查完再赋值"；一遍写下去的话，
    // 中途被拒会把实时配置改掉一半 —— 用户接着按 Cancel 也回不去（设置页的
    // Cancel 不还原内存里的配置，只还原写出去的选项串）。
    for (int i = 0; i < n; ++i) {
        const Field& k = f[i];
        if (k.kind != FKind::Dec && k.kind != FKind::Hex) continue;

        bool bad = false;
        const long long v = value(k, bad);
        if (!bad && k.max > k.min && (v < k.min || v > k.max)) bad = true;
        if (!bad) continue;

        wchar_t raw[64] = L"";
        GetDlgItemTextW(p, k.id, raw, 64);
        ReportW(ctx, k.msg ? k.msg : L"\"%s\" is not a valid value.", raw);
        SwitchPage(ctx, kPageInfo);
        SetFocus(GetDlgItem(p, k.id));
        return false;
    }

    // 第二遍：写。到这里已经不可能失败了。
    for (int i = 0; i < n; ++i) {
        const Field& k = f[i];
        switch (k.kind) {
        case FKind::Dec: {
            bool ignored = false;
            *reinterpret_cast<int*>(base + k.offset) =
                static_cast<int>(value(k, ignored));
            break;
        }
        case FKind::Hex: {
            bool ignored = false;
            *reinterpret_cast<unsigned*>(base + k.offset) =
                static_cast<unsigned>(value(k, ignored));
            break;
        }
        case FKind::Bool:
            *reinterpret_cast<bool*>(base + k.offset) = IsCheck(p, k.id);
            break;
        case FKind::Text: {
            std::vector<wchar_t> w(k.size ? k.size : 1);
            GetDlgItemTextW(p, k.id, w.data(), static_cast<int>(w.size()));
            Narrow(w.data(), base + k.offset, k.size);
            break;
        }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 经 RDDI 层探测台面上的东西
// ---------------------------------------------------------------------------
/// 一次探测会话 —— 设置页唯一允许"碰探针"的方式，析构函数负责拆干净。
/// 为什么要有：RDDI_Close 只还句柄、不拆链路（ConfigureInterface 锁了传输层、Connect 建了
/// SWD/JTAG），链路不拆探针就一直 connected、USB 端一直被占。文档的拆法是 DAP_Disconnect
/// 再 RDDI_Close。收进析构后，每条提前 return false 的路都会拆干净，新加的探针也不会漏。
class ProbeSession {
public:
    ProbeSession() = default;
    ~ProbeSession() { Close(); }
    ProbeSession(const ProbeSession&) = delete;
    ProbeSession& operator=(const ProbeSession&) = delete;

    /// Loads the host when nobody else has it loaded, then opens a session.
    bool Open() {
        m_loadedByUs = !rddi::IsLoaded();
        if (m_loadedByUs && !rddi::Load()) return false;
        const rddi::Api& api = rddi::Get();
        if (!api.Open || api.Open(&m_h, nullptr) != 0 || m_h == 0) {
            Close();
            return false;
        }
        return true;
    }

    /// 配置传输层并建立链路。定义在 `ConfigureSession()` 之后 —— 就是它调用的。
    bool Link(int iface, int port);

    RDDIHandle handle() const { return m_h; }

    void Close() {
        if (m_closed) return;
        m_closed = true;

        if (m_h) {
            const rddi::Api& api = rddi::Get();
            // 两件都做，顺序照文档。`m_touched` 是 `Link()` 在动手**之前**置的：
            // `CMSIS_DAP_ConfigureInterface` 自己就会锁传输层，所以 Connect 失败
            // 也得拆（同 `Dap::Disconnect()` 的注释）。
            if (m_touched && api.DAP_Disconnect) api.DAP_Disconnect(m_h);
            if (api.Close) api.Close(m_h);
            m_h = 0;
        }
        m_touched = false;
        if (m_loadedByUs) rddi::Unload();
        m_loadedByUs = false;
    }

private:
    RDDIHandle m_h          = 0;
    bool       m_loadedByUs = false;   // 宿主是这次探测加载的 -> 结束时要卸载
    bool       m_touched    = false;   // 已经动过传输层 -> 必须先 DAP_Disconnect
    bool       m_closed     = false;
};

/// Enumerates the CMSIS-DAP interfaces the RDDI host can see, then identifies
/// each one.  The RDDI module is loaded only when it is not loaded already (a
/// live session keeps its own), and the temporary session is torn down by
/// `ProbeSession`, so nothing is left behind for the session to trip over.
bool ProbeInterfaces(std::vector<IfaceInfo>& out) {
    out.clear();

    ProbeSession s;
    if (!s.Open()) return false;

    const rddi::Api& api = rddi::Get();
    int numIf = 0;
    if (!api.CMSIS_DAP_Detect || api.CMSIS_DAP_Detect(s.handle(), &numIf) != 0 ||
        numIf <= 0) {
        return false;
    }

    const RDDIHandle h = s.handle();
    for (int i = 0; i < numIf; ++i) {
        IfaceInfo f;
        f.index = i;
        // idNo: 1 = vendor, 2 = product (the name Keil shows),
        //       3 = serial, 4 = firmware version.
        for (int idNo = 1; api.CMSIS_DAP_Identify && idNo <= 4; ++idNo) {
            char buf[256];
            memset(buf, 0, sizeof(buf));
            if (api.CMSIS_DAP_Identify(h, i, idNo, buf, sizeof(buf)) != 0) continue;
            switch (idNo) {
            case 1: f.vendor = Widen(buf); break;
            case 2: f.name   = Widen(buf); break;
            case 3: f.serial = Widen(buf); break;
            default: f.fw    = Widen(buf); break;
            }
        }
        out.push_back(f);
    }
    return !out.empty();
}

/// 把会话带到 DP 有应答为止，顺序照 ORBMDK Usage.md §2（也是 Keil AGDI 走的）：
///   RDDI_Open → Detect/Identify → ConfigureInterface → DAP_Configure("SWD"/"JTAG")
///   → GetNumberOfDAPs + GetDAPIDList → CMSIS_DAP_Connect
/// CMSIS_DAP_Connect 才是做 SWJ 切换与首次 DPIDR 读的那步（DAP_Connect 只填个字符串）。
/// 本探测曾停在它之前，于是在目标完全正常的台面上报"AP 探测无人应答"。
/// SWJ 时钟仍故意不配：文档顺序把它放在 Connect 之后，且本探测承诺不给运行中的会话重定时。
/// iface/port 是调用方想探的、**不是**已保存配置（设置页从下拉处理函数里调，那时还是旧值）。
bool ConfigureSession(RDDIHandle h, int iface, int port) {
    const rddi::Api& api = rddi::Get();
    if (!api.CMSIS_DAP_Detect || !api.CMSIS_DAP_ConfigureInterface) return false;

    int numIf = 0;
    if (api.CMSIS_DAP_Detect(h, &numIf) != 0 || numIf <= 0) return false;

    const int useIf = (iface >= 0 && iface < numIf) ? iface : 0;

    char cfgStr[64];
    char* cfgArg = rddi::InterfaceConfig(cfgStr, sizeof(cfgStr));
    if (api.CMSIS_DAP_ConfigureInterface(h, useIf, cfgArg) != 0) {
        // The layer locks the transport here and refuses to fall back to the other
        // one, so this is the end of the road - name the interface that was asked
        // for, because "failed" on its own says nothing about which.
        ORB_LOG_WARN("Cfg: CMSIS_DAP_ConfigureInterface failed for interface %d%s",
                     useIf, cfgArg ? " (the host rejected the config string)" : "");
        return false;
    }

    if (api.DAP_Configure) {
        api.DAP_Configure(h, (port == kPortJtag) ? "JTAG" : "SWD");
    }

    // ARM's rddi_dap.h order: both DAP list calls come before Connect.
    int numDaps = 0;
    if (api.DAP_GetNumberOfDAPs) api.DAP_GetNumberOfDAPs(h, &numDaps);
    if (api.DAP_GetDAPIDList && numDaps > 0) {
        int ids[16] = {0};
        const size_t n = (numDaps < 16) ? static_cast<size_t>(numDaps) : 16;
        api.DAP_GetDAPIDList(h, ids, n);
    }

    if (!api.CMSIS_DAP_Connect) {
        ORB_LOG_WARN("Cfg: the RDDI host has no CMSIS_DAP_Connect - the target "
                     "cannot be linked and the AP list stays unprobed");
        return false;
    }
    int linked = 0;
    const int rc = api.CMSIS_DAP_Connect(h, &linked);
    if (rc != 0) {
        ORB_LOG_WARN("Cfg: CMSIS_DAP_Connect failed (rc=%d) - no target on the "
                     "port, or the target is not powered", rc);
        return false;
    }
    ORB_LOG_INFO("Cfg: target linked over %s", (linked == 2) ? "JTAG" : "SWD");
    return true;
}

/// 一次"配置 + 建链"。`m_touched` 在动手**之前**置起：`ConfigureInterface` 自己
/// 就会锁住传输层，所以即使 Connect 失败，析构时也必须 `DAP_Disconnect`。
bool ProbeSession::Link(int iface, int port) {
    if (!m_h) return false;
    m_touched = true;
    return ConfigureSession(m_h, iface, port);
}

/// 按 IDR 给 AP 命名，字段布局取自 Keil 的 CoreSight 表（DbgCM/Debug.h）：
///   [31:28] REVISION [23:17] DESIGNER [16:13] CLASS [7:4] VARIANT [3:0] TYPE
///   CLASS 0x0=JTAG-AP 0x8=MEM-AP；TYPE 0x0=JTAG 0x1=AHB 0x2=APB 0x4=AXI
/// CLASS 是**四位**：原来只取三位（& 7）表示不了 0x8 ⇒ 所有 MEM-AP 都被叫成 JTAG-AP
/// （实测 STM32F1：AP0 IDR=0x24770011，界面印 JTAG-AP，而驱动拿它做全部内存访问）。
const wchar_t* ApClassName(uint32_t idr) {
    if (((idr & 0x0001E000u) >> 13) != 0x8u) return L"JTAG-AP";
    switch (idr & 0x0000000Fu) {
    case 0x1: return L"AHB-AP";
    case 0x2: return L"APB-AP";
    case 0x4: return L"AXI-AP";
    default:  return L"MEM-AP";
    }
}

/// 读 DP 应答的每个 AP 索引的 IDR，让下拉能显示 "<index>: AHB-AP (0x24770011)"。
/// 故意没有"从配置读"的默认值（设置页也从 Interface 下拉处理函数里调，配置里还是上一个
/// adapter）。也故意不碰 nRESET 与 SWJ 时钟 —— 打开设置页绝不能复位目标或重定时；
/// 没有目标应答就安静放弃，页面退回普通 0..3 列表。
bool ProbeAps(std::vector<ApInfo>& out, int iface, int port) {
    out.clear();

    ProbeSession s;
    if (!s.Open()) return false;

    const rddi::Api& api = rddi::Get();
    if (!api.DAP_ReadReg || !api.DAP_WriteReg) return false;

    const RDDIHandle h = s.handle();
    bool ok = false;
    {
        s.Link(iface, port);

        int idcode = 0;
        const bool dpAlive =
            api.DAP_ReadReg(h, 0, DAP_DP_REG_IDCODE, &idcode) == 0 &&
            idcode != 0 && static_cast<uint32_t>(idcode) != 0xFFFFFFFFu;
        if (dpAlive) {
            api.DAP_WriteReg(h, 0, DAP_DP_REG_ABORT, 0x1E);       // clear sticky
            api.DAP_WriteReg(h, 0, DAP_DP_REG_CTRL_STAT, 0x50000000);
            for (int i = 0; i < 200; ++i) {
                int v = 0;
                if (api.DAP_ReadReg(h, 0, DAP_DP_REG_CTRL_STAT, &v) != 0) break;
                if ((static_cast<uint32_t>(v) & 0xA0000000u) == 0xA0000000u) break;
                Sleep(1);
            }
            for (int ap = 0; ap < 8; ++ap) {
                const int sel = (ap << 24) | (0xF << 4);          // bank 0xF
                if (api.DAP_WriteReg(h, 0, DAP_DP_REG_SELECT, sel) != 0) continue;
                int v = 0;
                if (api.DAP_ReadReg(h, 0, DAP_AP_REG_APIDR, &v) != 0) continue;
                const uint32_t idr = static_cast<uint32_t>(v);
                if (idr == 0 || idr == 0xFFFFFFFFu) continue;
                ApInfo a;
                a.index = ap;
                a.idr   = idr;
                a.name  = ApClassName(idr);
                out.push_back(a);
            }
            ok = !out.empty();
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Info 页：一个可滚动文本框装下所有值得一提的东西（含驱动日志尾部）。设置页报出的每条
// 消息也落这里 —— 错误属于 "I" 页，不属于惹出它的那一页。
// ---------------------------------------------------------------------------
std::wstring LogTail() {
    const wchar_t* path = orb::log::FilePath();
    if (!path || !*path) {
        return L"(the log has not been started yet - it is created when a debug "
               L"session opens)";
    }

    // 说清楚到底是哪一种："打不开日志文件"原来盖住了全部情形，包括"还没跑过会话"这种正常情况。
    // 要区分的是"还没有文件"（没出问题）与"有文件但读不了"（出了问题）。
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        wchar_t msg[600];
        _snwprintf_s(msg, _TRUNCATE,
                     L"(no log file yet - %s is created when a debug session "
                     L"opens, or on the first message the driver writes)", path);
        return msg;
    }

    // _wfsopen + _SH_DENYNO，不是 _wfopen_s：驱动整个会话都持有该文件的写句柄，而 _wfopen_s
    // 的默认共享模式不含 FILE_SHARE_WRITE ⇒ 读自家日志报 ERROR_SHARING_VIOLATION（errno 13）。
    errno = 0;
    SetLastError(ERROR_SUCCESS);
    FILE* f = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!f) {
        wchar_t msg[600];
        _snwprintf_s(msg, _TRUNCATE,
                     L"(the log file exists but could not be read: %s - errno=%d "
                     L"win32=%lu)", path, static_cast<int>(errno),
                     static_cast<unsigned long>(GetLastError()));
        return msg;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return L"(the log file could not be seeked)";
    }
    const long size = ftell(f);
    if (size < 0) { fclose(f); return L"(the log file size could not be read)"; }
    if (size == 0) {
        fclose(f);
        return L"(the log file is empty - no message has been written yet)";
    }

    const long kMax = 12000;                 // enough for a session's worth
    const long start = (size > kMax) ? size - kMax : 0;
    fseek(f, start, SEEK_SET);
    std::string buf(static_cast<size_t>(size - start), '\0');
    const size_t got = buf.empty() ? 0 : fread(&buf[0], 1, buf.size(), f);
    fclose(f);
    buf.resize(got);
    return WidenUtf8(buf.c_str());
}

std::wstring BuildInfoText(SheetCtx* ctx) {
    const Config& c = Get();
    std::wstring s;
    wchar_t line[768];

    _snwprintf_s(line, _TRUNCATE,
                 L"== Target (as reported by uVision) ==\r\n"
                 L"    device        : %s\r\n"
                 L"    flash window  : %08X .. %08X   (%u kB)\r\n"
                 L"    core clock    : %u Hz\r\n",
                 c.device[0] ? Widen(c.device).c_str() : L"(unknown)",
                 c.iromStart, c.iromStart + c.iromSize, c.iromSize / 1024u,
                 c.clockHz);
    s += line;

    _snwprintf_s(line, _TRUNCATE,
                 L"\r\n== Where this driver looks ==\r\n"
                 L"    log file      : %s\r\n",
                 (orb::log::FilePath() && *orb::log::FilePath())
                     ? orb::log::FilePath() : L"(none yet)");
    s += line;

    s += L"    RDDI host     : ";
    const std::wstring found = AutoDetectRddi();
    s += found.empty() ? L"NOT FOUND - see the messages below\r\n"
                       : found + L"\r\n";
    s += L"    pack roots    :\r\n";
    {
        int listed = 0;
        for (int i = 0, n = PackRootCount(); i < n && listed < 4; ++i, ++listed) {
            char buf[MAX_PATH];
            if (!PackRoot(i, buf, sizeof(buf))) continue;
            _snwprintf_s(line, _TRUNCATE, L"        %s\r\n", Widen(buf).c_str());
            s += line;
        }
        if (!listed) s += L"        (none found)\r\n";
    }

    s += L"\r\n== Messages ==\r\n";
    s += ctx->report.empty() ? L"(none)\r\n" : ctx->report + L"\r\n";

    s += L"\r\n== Log tail (%TEMP%\\ORB_AGDI.log) ==\r\n";
    s += LogTail();
    s += L"\r\n";
    return s;
}

void RefreshInfo(SheetCtx* ctx) {
    HWND page = ctx->hPage[kPageInfo];
    if (!page) return;
    HWND box = GetDlgItem(page, kIdEditInfo);
    if (!box) return;
    const std::wstring text = BuildInfoText(ctx);
    SetWindowTextW(box, text.c_str());
    // Keep the newest line in view: the caret goes to the end and the box
    // scrolls after it.
    SendMessageW(box, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(box, EM_SCROLLCARET, 0, 0);
}

/// Appends one line to the Info page (and to the log).  Every error and every
/// status note goes through here, so "the reason" is never on a page the user
/// has already left.
void Report(SheetCtx* ctx, const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    ORB_LOG_INFO("Cfg: %s", body);
    if (!ctx->report.empty()) ctx->report += L"\r\n";
    ctx->report += Widen(body);
    RefreshInfo(ctx);
}

// 页/容器的尺寸常量已上移到 DlgTpl 那边（它们同时是越界检查的上限）。
// 这一页（Debug）按 400x188 排：控件一个都不许越过它。

// ===========================================================================
// Page 0 - "Debug"
// RDDI 宿主 DLL 没有自己的控件：自动发现、只在 Info 页上报。本页给的是 RDDI 层对台面的
// 回答（接口列表、adapter 身份、AP 列表）加上端口、SWJ 开关与时钟。
// ===========================================================================
void AddDebugItems(DlgTpl& t) {
    Grp(t, kIdGrpAdapter, L"RDDI Adapter", 6, 4, 386, 96);
    {
        Lay L(t, 12, 15, 374);
        L.AddKey(L"Interface:", 52).AddCombo(kIdCmbIface, 318);
        L.Row();
        L.AddKey(L"Port:", 52).AddCombo(kIdCmbPort, 80)
                              .AddCheck(kIdChkSwj, L"SWJ-DP", 60);
        L.Gap(24);
        L.AddKey(L"Clock:", 40).AddCombo(kIdCmbClock, 102);
        L.Row();
        L.AddKey(L"AP Index:", 52).AddCombo(kIdCmbAp, 318);
        L.Row();
        // 这一行的后两个是只读回显（探针报回来的身份），不是输入。
        L.AddKey(L"Serial:", 52).AddValue(kIdValSerial, 140, 8);
        L.AddKey(L"Firmware:", 44).AddValue(kIdValFw, 122, 8);
        L.Row();
        L.AddKey(L"Vendor:", 52).AddValue(kIdValVendor, 318, 8);
    }

    Grp(t, kIdGrpDebug, L"Debug", 6, 106, 188, 76);
    {
        Lay L(t, 12, 117, 174);
        L.AddKey(L"Connect:", 42).AddCombo(kIdCmbConnect, 128);
        L.Row();
        L.AddKey(L"Reset:", 42).AddCombo(kIdCmbReset, 128);
        L.At(147, 8);                        // 三个开关：行高 8 + 4 = 每行 12
        L.AddCheck(kIdChkRstCon,  L"Reset after Connect", 172); L.Row();
        L.AddCheck(kIdChkStopRst, L"Stop after Reset",    172); L.Row();
        L.AddCheck(kIdChkLogAcc,  L"Log Debug Accesses",  172);
    }

    Grp(t, kIdGrpCache, L"Cache && Download", 198, 106, 194, 62);
    {
        Lay L(t, 206, 118, 178, 8);
        L.AddCheck(kIdChkCacheCd,  L"Cache Code",        178); L.Row();
        L.AddCheck(kIdChkCacheMem, L"Cache Memory",      178); L.Row();
        L.AddCheck(kIdChkDlFlash,  L"Download to Flash", 178);
    }
}

// ===========================================================================
// Page 1 - "Trace"（照 ULINK Pro 的 Trace 页）
//   顶部："Trace Enable" 总开关 + 两个时钟框；关掉时整页灰显。
//   左 "ITM / DWT"：SWO 波特率、两个时钟、SWO 分频、SWO 编码、ITM 端口/特权、PC 采样。
//   右上 "Trace Port"：探针侧一个下拉（Nbit 并口 / SWO UART / SWO Manchester）。
//   右中 "Trace Events"：6 个复选框，与 events= 的位一一对应。
//   右下 "Timestamps"：ts= / tspresc= / tswin。
// 比 ULINK 多一个 "Source" 下拉：ULINK 只有一条 trace 通路，本驱动有三条（SWO 引脚 /
// 片上 ETB / orbtrace 探针）。⚠ 400x188 已排满，加控件前先想清楚从哪儿腾地方。
// ===========================================================================
void AddTraceItems(DlgTpl& t) {
    // ---- 顶部：总开关那一排 ----
    // 图上这排是 Core/Trace Clock + Trace Enable / Unlimited Trace / ETM Trace Enable /
    // Use Core Clock；两个时钟框我们放在下面的 "ITM / DWT" 组里，这里只放四个复选框。
    Grp(t, kIdGrpTrcSrc, L"Trace Source", 6, 4, 386, 42);
    {
        Lay L(t, 12, 13, 380);              // 到组框右内边 392
        L.AddKey(L"Source:", 50).AddCombo(kIdCmbTrcSrc, 150);
        L.Gap(2);
        L.AddCheck(kIdChkTrcEn, L"Trace Enable",     80);
        L.AddCheck(kIdChkEtmEn, L"ETM Trace Enable", 86);
        L.Row();
        L.AddCheck(kIdChkUnlim,   L"Unlimited Trace", 100);
        L.AddCheck(kIdChkUseCore, L"Use Core Clock",   90);
        // ETM 分支地址编码（etmalt）必须与目标 ETM 硬件一致，判据是 ETMBASE+0x1E4 的 bit20
        // （orbuculum 的 describeETM 用它判；实测这颗 STM32F407 报 bit20=1 ⇒ ALT）。
        // 用户指令：复选框改成两档下拉、默认 ALT；寄存器自动判定已去掉（早读会拿到旧值）。
        L.AddKey(L"ETM Alt:", 46).AddCombo(kIdCmbEtmAlt, 80);
    }

    // ---- 左：目标侧 ITM / DWT --------------------------------------------
    Grp(t, kIdGrpItm, L"ITM / DWT", 6, 50, 188, 138);
    {
        Lay L(t, 12, 61, 180);              // 到组框右内边 192
        L.AddKey(L"SWO Baud:").AddEdit(kIdEditSwo, 50).AddLabel(L"Hz", 20);
        L.Row();
        L.AddKey(L"Core Clock:").AddEdit(kIdEditCpu, 50).AddLabel(L"kHz", 24);
        L.Row();
        // "Trace Clock"（tclk）：0 = 跟随核心时钟；勾了 "Use Core Clock" 就灰掉（那时只读）。
        // 同一行第二个框是 SWO 分频（swopresc）：位时钟 = trace 时钟 ÷ 它（swo= 给了显式值时那个
        // 优先），所以放在 "÷" 后面，读起来就是"1500 kHz ÷ 1"。
        L.AddKey(L"Trace Clock:").AddEdit(kIdEditTclk, 46).AddLabel(L"kHz", 20);
        L.AddLabel(L"÷", 10).AddEdit(kIdEditSwopresc, 26);
        L.Row();
        // SWO 编码（`swomode`）—— **只对"SWO 引脚"这条源有义**（`ItmStart` 拿它选
        // UART/Manchester）。orbtrace 那条用的是 Trace Port 下拉里的编码，所以这条
        // 在别的源下会灰掉。
        L.AddKey(L"SWO Encoding:", 76).AddCombo(kIdCmbSwomode, 100);
        L.Row();
        L.AddKey(L"ITM Ports:").AddEdit(kIdEditItmP, 110);
        L.Row();
        L.AddKey(L"Privilege:").AddEdit(kIdEditItmPr, 110);
        L.Row();
        L.AddKey(L"PC Sample Tap:", 82).AddCombo(kIdCmbPcPre, 94);
        L.Row();
        L.AddCheck(kIdChkPcPer, L"Periodic",    82);
        L.AddCheck(kIdChkPcDrw, L"on Data R/W", 82);
    }

    // ---- 右上：探针自己的 trace 端口格式 ---------------------------------
    //
    // 照 ULINK Pro 的图并成**一个**下拉（原来是 `Width, bits:` + `Mode:` 两个）。
    // 那两个里永远有一个是死的：位宽只在并口下有义，编码只在 SWO 下有义。
    Grp(t, kIdGrpPort, L"Trace Port", 198, 50, 194, 26);
    {
        Lay L(t, 202, 61, 186);             // 到组框右内边 388
        L.AddKey(L"Trace Port:", 62).AddCombo(kIdCmbTrPort, 120);
    }

    // ---- 右中：Trace Events（图上的 6 个复选框）--------------------------
    //
    // 排成**两列**（3+3）：单列要 6 行，右下这块放不下，而下面还要留给 Timestamps。
    // 按列读下来的顺序（CPI/EXC/SLEEP/LSU/FOLD/EXCTRC）与 `events=` 的位序一致。
    Grp(t, kIdGrpEvt, L"Trace Events", 198, 80, 194, 54);
    {
        Lay A(t, 206, 92, 88, 9);           // 行高 9 + 4 = 每行 13
        Lay B(t, 298, 92, 86, 9);
        for (int i = 0; i < kEvtCount; ++i) {
            const bool first = (i < kEvtCount / 2);
            Lay& L = first ? A : B;
            L.AddCheck(static_cast<WORD>(kIdChkEvt0 + i), kEvtNames[i],
                       first ? 88 : 86);
            L.Row();
        }
    }

    // ---- 右下：Timestamps（ts= / tspresc= / tswin）----
    // ⚠ tswin（"Show the time column"）单独放：bHideTimestamps 是 uVision 取数门的一环，
    // 打开可能让 Trace Data 窗口一行都不取。所以它带一行说明，勾上时还往 Info 页写一条提示。
    Grp(t, kIdGrpTs, L"Timestamps", 198, 138, 194, 50);
    {
        Lay L(t, 206, 150, 178, 8);         // 行高 8 + 4 = 每行 12
        L.AddCheck(kIdChkTs, L"Enable", 70);
        L.AddKey(L"Prescaler:", 54).AddCombo(kIdCmbTsPre, 46);
        L.Row();
        L.AddCheck(kIdChkTsWin, L"Show the time column", 178);
        L.Row();
        L.AddLabel(L"(may stop the Trace Data window fetching)");
    }
}

// ===========================================================================
// Page 2 - "Flash"
// ===========================================================================
void AddFlashItems(DlgTpl& t) {
    // 两组各两列：擦除方式（单选）+ 三个开关（复选）。行高 8 + kRowGap 4 = 12。
    Grp(t, kIdGrpFlashFn, L"Download Function", 6, 4, 188, 50);
    {
        Lay L(t, 14, 15, 92, 8);
        L.AddRadio(kIdRadErase0, kEraseNames[0], 92, WS_GROUP);
        L.Row(); L.AddRadio(kIdRadErase1, kEraseNames[1], 92);
        L.Row(); L.AddRadio(kIdRadErase2, kEraseNames[2], 92);

        Lay R(t, 110, 15, 78, 8);
        R.AddCheck(kIdChkProgram,  L"Program",       78);
        R.Row(); R.AddCheck(kIdChkFdVerify, L"Verify",         78);
        R.Row(); R.AddCheck(kIdChkRstRun,   L"Reset and Run",  78);
    }

    Grp(t, kIdGrpFlashRam, L"RAM for Algorithm", 198, 4, 194, 50);
    {
        Lay L(t, 206, 16, 182);
        L.AddKey(L"Start:", 34).AddEdit(kIdEditRamAdr, 58);
        L.Gap(6);
        L.AddKey(L"Size:", 30).AddEdit(kIdEditRamSz, 42);
    }

    Grp(t, kIdGrpFlashAlg, L"Programming Algorithm", 6, 60, 386, 118);
    {
        Lay L(t, 12, 72, 380);
        L.AddKey(L"Algorithm:", 56).AddEdit(kIdEditFlm, 268)
                                   .AddButton(kIdBtnFlmBrw, L"...", 42);
        // "Detect from Keil" 贴右边缘（原来就是 238..386），不是跟在上一行后面。
        L.At(92); L.Gap(226);
        L.AddButton(kIdBtnFlmAuto, L"Detect from Keil", 148);
        L.At(112);
        L.AddValue(kIdValFlmInfo, 374, 60);
    }
}

// ===========================================================================
// Page 3 - "Pack"
// ===========================================================================
void AddPackItems(DlgTpl& t) {
    Grp(t, kIdGrpPdsc, L"Debug Description", 6, 4, 386, 90);
    {
        Lay L(t, 14, 17, 372);
        L.AddCheck(kIdChkPdsc, L"Enable", 120);
        L.At(36);
        L.AddCheck(kIdChkPdscLog, L"Log Sequences:", 110);
        L.AddEdit(kIdEditPdscLog, 226);
        L.AddButton(kIdBtnPdscLog, L"...", 28);
    }

    // 六行说明，中间留一段空白。行高 8 + kRowGap 4 = 每行 12 —— 比别的页密，
    // 所以行高显式给 8，不然六行铺不下（188 的下边缘）。
    Lay M(t, 12, 100, 380, 8);
    M.AddLabel(L"uVision owns the debug description and runs the sequences;").Row();
    M.AddLabel(L"this driver only asks for them.  With \"Enable\" off the").Row();
    M.AddLabel(L"download falls back to the .FLM on the Flash page.");
    M.At(144);
    M.AddLabel(L"Sequences come from the device's CMSIS-Pack (PDSC) debug").Row();
    M.AddLabel(L"description: flash algorithms, reset behaviour, and the").Row();
    M.AddLabel(L"initialisation the device needs before its first instruction.");
}

// ===========================================================================
// Page 4 - "Record"：记录缓冲（块链）与落盘。这 5 个键原来只能手改选项串，而实机验收
// 必须手填（trchunk=1 trmem=2 trrec=1 trfile=1 trkeep=2）—— 默认 1 GB 上限到不了。
// ⚠ 本页文字必须与 kMemLimitMax（ORBAGDI_TraceWin.cpp）一致：两边一起改，否则界面写一个
// 数、实际另一个，属于最难查的偏差。
// ⚠ trrecmb=（落盘文件上限，默认 2 GB）还没上这一页，只能手改选项串。
// ===========================================================================
void AddRecordItems(DlgTpl& t) {
    Grp(t, kIdGrpRecBuf, L"Record Buffer", 6, 4, 386, 58);
    {
        Lay L(t, 12, 15, 374);
        L.AddKey(L"Block size:").AddEdit(kIdEditTrChunk, 40).AddLabel(L"MB", 24);
        L.Gap(24);
        L.AddKey(L"Memory cap:", 66).AddEdit(kIdEditTrMem, 40).AddLabel(L"MB", 24);
        L.Row();
        L.AddLabel(L"0 = default: 4 MB per block, cap = physical RAM / 4 (at most 1 GB).", 374);
        L.Row();
        L.AddLabel(L"Applied at the first Connect - changing them needs a new session.", 374);
    }

    Grp(t, kIdGrpRecLog, L"Logging", 6, 66, 386, 88);
    {
        Lay L(t, 12, 77, 374);
        L.AddKey(L"Trace file:").AddEdit(kIdEditTrFile, 258).AddButton(kIdBtnTrFile, L"...", 42);
        L.Row();
        L.AddKey(L"Record log:").AddEdit(kIdEditTrRec, 258).AddButton(kIdBtnTrRec, L"...", 42);
        L.Row();
        L.AddKey(L"Keep files:").AddEdit(kIdEditTrKeep, 40)
                                .AddLabel(L"0 = never delete old files", 140);
        L.Row();
        L.AddLabel(L"Empty = off.  1 = the log directory.  <dir> = that directory.  <name>.orb = that name.", 374);
        L.Row();
        L.AddLabel(L"Files: ORB_trace_<time>.orb (OFLOW, for external tools) / ORB_rec_<time>.trc (compact records).", 374);
    }

    Val(t, kIdValRecInfo, 6, 160, 386, 24);
}

// ===========================================================================
// Page 5 - "Power"：Vtref / Vtpwr，探针直接给目标供电的那两路。
// **不进选项串、不落工程**（暂不做持久化）：没有 Field 表，Accept() 不读它，关页不碰电源，
// 重开时显示驱动进程里记着的"最后一次下发成功的东西"。
// **只有 Apply 会碰探针电源**，OK/Cancel 一律不碰（外部测试按的就是 OK）。刻意如此：
// 探针一上电就是给目标供电，误触发会烧东西。
// 档位是固定档（Vtref 1.8/3.3 V、Vtpwr 3.3/5.0 V），所以是下拉 —— 让用户填任意毫伏，
// 探针会落到最近档而界面还显示他填的数。
// ===========================================================================
void AddPowerItems(DlgTpl& t) {
    Grp(t, kIdGrpPwrVtref, L"Vtref", 6, 4, 188, 62);
    {
        Lay L(t, 12, 18, 176);
        L.AddKey(L"Voltage:", 52).AddCombo(kIdCmbPwrVtref, 118);
        L.At(40);
        L.AddCheck(kIdChkPwrVtrefEn, L"Enable output", 170);
    }
    Grp(t, kIdGrpPwrVtpwr, L"Vtpwr", 198, 4, 194, 62);
    {
        Lay L(t, 204, 18, 182);
        L.AddKey(L"Voltage:", 52).AddCombo(kIdCmbPwrVtpwr, 124);
        L.At(40);
        L.AddCheck(kIdChkPwrVtpwrEn, L"Enable output", 176);
    }

    // 五行说明。行高 8 + kRowGap 4 = 每行 12 —— 比别的页密，五行走得下。
    Lay M(t, 6, 70, 386, 8);
    M.AddLabel(L"These outputs feed the target straight from the probe.").Row();
    M.AddLabel(L"Nothing is sent until you press Apply; OK and Cancel").Row();
    M.AddLabel(L"never touch the probe's power.  Nothing on this page").Row();
    M.AddLabel(L"is saved in the project, and the probe only has the").Row();
    M.AddLabel(L"fixed levels listed above.");

    Lay B(t, 6, 132, 386);
    B.AddButton(kIdBtnPwrApply, L"Apply",   76);
    B.AddButton(kIdBtnPwrOff,   L"All Off", 76);

    Val(t, kIdValPwrStatus, 6, 150, 386, 34);
}

// ===========================================================================
// Page 6 - "Info".  Read only, and the only place the sheet reports anything.
// ===========================================================================
void AddInfoItems(DlgTpl& t) {
    Lbl(t, L"Everything this driver was told, what it detected, and the tail of "
           L"%TEMP%\\ORB_AGDI.log:", 6, 4, 386);
    Text(t, kIdEditInfo, 6, 16, 386, 170);
}

DlgTpl BuildPage(int page) {
    DlgTpl items;
    switch (page) {
    case kPageDebug:  AddDebugItems(items);  break;
    case kPageTrace:  AddTraceItems(items);  break;
    case kPageFlash:  AddFlashItems(items);  break;
    case kPagePack:   AddPackItems(items);   break;
    case kPageRecord: AddRecordItems(items); break;
    case kPagePower:  AddPowerItems(items);  break;
    default:          AddInfoItems(items);   break;
    }
    return WrapTpl(WS_CHILD | DS_CONTROL | DS_SETFONT, kPageCx, kPageCy, L"", items);
}

void AddCommonItems(DlgTpl& t) {
    // The tab control is the one control with no class atom.
    AddItemCls(t, kStyleTab, kIdTab, L"SysTabControl32", L"", 4, 4, 404, 212);

    // Placeholders.  They used to be Defaults / Auto DLL / Test / View log;
    // the DLL is auto-detected now and the log is on the Info page, so the row
    // is kept as a home for the modules that will need it (orbtrace's Vtref and
    // Vpwer power outputs, for instance).  None of them changes anything.
    Btn(t, kIdBtnDefault, L"Defaults",   7, 222, 52);
    Btn(t, kIdBtnTest,    L"Test",      63, 222, 52);
    Btn(t, kIdBtnLog,     L"Log",      119, 222, 52);
    Btn(t, kIdBtnPower,   L"Power",    175, 222, 52);
    Btn(t, IDOK,          L"OK",       290, 222, 50, true);
    Btn(t, IDCANCEL,      L"Cancel",   344, 222, 50);
}

DlgTpl BuildSheetTpl() {
    DlgTpl items(kSheetCx, kSheetCy);   // 容器模板用容器的上限，不是页的
    AddCommonItems(items);
    return WrapTpl(DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                   kSheetCx, kSheetCy, L"ORBTRACE Debugger Settings", items);
}

// ===========================================================================
// Filling the pages from the options in force
// ===========================================================================

/// The interface list is what the RDDI layer answered, plus "auto" at the top.
void FillIfaceCombo(SheetCtx* ctx, HWND p, int iface) {
    HWND h = GetDlgItem(p, kIdCmbIface);
    if (!h) return;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    SendMessageW(h, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Auto (first available)"));
    for (const IfaceInfo& f : ctx->ifaces) {
        wchar_t item[300];
        _snwprintf_s(item, _TRUNCATE, L"%d: %s", f.index,
                     f.name.empty() ? L"(unnamed)" : f.name.c_str());
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    int sel = 0;
    for (size_t i = 0; i < ctx->ifaces.size(); ++i) {
        if (ctx->ifaces[i].index == iface) { sel = static_cast<int>(i) + 1; break; }
    }
    SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

int IfaceValue(SheetCtx* ctx, HWND p) {
    const int sel = ComboSel(p, kIdCmbIface, 0);
    if (sel <= 0) return -1;                       // "auto"
    const size_t i = static_cast<size_t>(sel) - 1;
    return (i < ctx->ifaces.size()) ? ctx->ifaces[i].index : -1;
}

/// One entry per AP the DP answered on, IDCODE and class included.
void FillApCombo(SheetCtx* ctx, HWND p, int ap) {
    HWND h = GetDlgItem(p, kIdCmbAp);
    if (!h) return;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (const ApInfo& a : ctx->aps) {
        wchar_t item[160];
        if (a.idr) {
            _snwprintf_s(item, _TRUNCATE, L"%d: %s (0x%08X)",
                         a.index, a.name.c_str(), a.idr);
        } else {
            _snwprintf_s(item, _TRUNCATE, L"%d: (not probed)", a.index);
        }
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    int sel = 0;
    for (size_t i = 0; i < ctx->aps.size(); ++i) {
        if (ctx->aps[i].index == ap) { sel = static_cast<int>(i); break; }
    }
    SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

int ApValue(SheetCtx* ctx, HWND p) {
    const int sel = ComboSel(p, kIdCmbAp, 0);
    if (sel < 0 || sel >= static_cast<int>(ctx->aps.size())) return 0;
    return ctx->aps[static_cast<size_t>(sel)].index;
}

/// The classic 0..3 list, used when no target answered the AP probe.
void ApFallback(std::vector<ApInfo>& out) {
    out.clear();
    for (int i = 0; i < 4; ++i) {
        ApInfo a;
        a.index = i;
        out.push_back(a);
    }
}

void FillClockCombo(SheetCtx* ctx, HWND p, int khz) {
    HWND h = GetDlgItem(p, kIdCmbClock);
    if (!h) return;
    ctx->clockVals.clear();
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kClockRateCount; ++i) {
        ctx->clockVals.push_back(kClockRates[i].khz);
        SendMessageW(h, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kClockRates[i].name));
    }
    int sel = 0;
    bool found = false;
    for (int i = 0; i < kClockRateCount; ++i) {
        if (kClockRates[i].khz == khz) { sel = i; found = true; break; }
    }
    if (!found) {
        // A hand-written rate stays reachable instead of being silently
        // replaced by the default when the sheet is accepted.
        wchar_t item[64];
        _snwprintf_s(item, _TRUNCATE, L"%d kHz (from the project)", khz);
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        ctx->clockVals.push_back(khz);
        sel = static_cast<int>(ctx->clockVals.size()) - 1;
    }
    SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

int ClockValue(SheetCtx* ctx, HWND p) {
    const int sel = ComboSel(p, kIdCmbClock, 0);
    if (sel < 0 || sel >= static_cast<int>(ctx->clockVals.size())) return 0;
    return ctx->clockVals[static_cast<size_t>(sel)];
}

/// The adapter's identity, for whichever interface the combo points at.
void ShowIfaceInfo(SheetCtx* ctx, HWND p) {
    const int sel = ComboSel(p, kIdCmbIface, 0);
    const IfaceInfo* f = nullptr;
    if (sel >= 1 && sel <= static_cast<int>(ctx->ifaces.size())) {
        f = &ctx->ifaces[static_cast<size_t>(sel) - 1];
    }
    SetDlgItemTextW(p, kIdValSerial, (f && !f->serial.empty()) ? f->serial.c_str() : L"-");
    SetDlgItemTextW(p, kIdValFw,     (f && !f->fw.empty())     ? f->fw.c_str()     : L"-");
    SetDlgItemTextW(p, kIdValVendor, (f && !f->vendor.empty()) ? f->vendor.c_str() : L"-");
}

/// 灰掉所选端口用不到的东西。SWJ-DP 是 SWD/JTAG 合一的调试口，只在 SWD 下有意义：JTAG 下
/// 访问口就是普通 JTAG-DP，该选项无从生效（Keil 自己的页面也在这里灰掉）。
/// 留着可点会让工程存下 swj=1 却在 JTAG 会话里被解析后悄悄忽略 —— 又死一个开关。
void UpdatePortEnable(HWND p, int portSel) {
    const bool swd = (portSel != 1);        // 0 = SWD (default), 1 = JTAG
    EnableWindow(GetDlgItem(p, kIdChkSwj), swd);
}

/// 按下拉当前指向的接口重探 AP 列表：AP 列表是"这个 adapter + 这个 port"的属性，不是全局
/// 的。新接口必须**传进去**（不能从已保存配置读 iface，那时还是旧值，重探出来的仍是旧
/// adapter 的 AP，看着就像没刷新）。
/// 探测是同步的（枚举 USB、Connect、读 8 个 APIDR，慢时几百毫秒），期间把这组灰掉并写一行
/// "(probing...)"，否则用户以为卡死会再点几下。
void ReprobeForIface(SheetCtx* ctx) {
    HWND p = ctx->hPage[kPageDebug];
    if (!p) return;

    ShowIfaceInfo(ctx, p);

    const int iface  = IfaceValue(ctx, p);
    const int port   = (ComboSel(p, kIdCmbPort, 0) == 1) ? kPortJtag : kPortSwd;
    const int keepAp = ApValue(ctx, p);

    const WORD busy[] = { kIdCmbIface, kIdCmbAp, kIdCmbPort, kIdChkSwj };
    for (const WORD id : busy) EnableWindow(GetDlgItem(p, id), FALSE);
    SetDlgItemTextW(p, kIdValSerial, L"(probing...)");
    UpdateWindow(p);

    std::vector<ApInfo> aps;
    if (ProbeAps(aps, iface, port)) {
        ctx->aps.swap(aps);
        Report(ctx, "RDDI: the selected interface answered the AP probe (%u AP(s)).",
               static_cast<unsigned>(ctx->aps.size()));
    } else {
        // 目标没回答就退回 0..3，并且**保留原来那个索引** —— 与建页时同一套
        // 做法，否则改一次 adapter 就把 AP 选择悄悄丢了。
        ApFallback(ctx->aps);
        bool have = false;
        for (const ApInfo& a : ctx->aps) {
            if (a.index == keepAp) { have = true; break; }
        }
        if (!have) { ApInfo a; a.index = keepAp; ctx->aps.push_back(a); }
        Report(ctx, "RDDI: the selected interface did not answer the AP probe - "
                    "AP 0..3 are offered unprobed.");
    }

    FillApCombo(ctx, p, keepAp);
    ShowIfaceInfo(ctx, p);

    for (const WORD id : busy) EnableWindow(GetDlgItem(p, id), TRUE);
    // `SWJ-DP` 的可用性由 Port 决定，不是无条件恢复。
    UpdatePortEnable(p, ComboSel(p, kIdCmbPort, 0));
}

/// 灰掉所选源用不到的东西；总开关关掉时其余全灰（Source 下拉本身也灰）。
///   0 SWO 引脚 / 1 ETB -> 目标侧 ITM/DWT live，Trace Port 灰
///   2 orbtrace        -> 目标侧也 live，另加 Trace Port live
/// ⚠ 目标侧那一半三条源都用到（ItmProgramTarget→ProgramItmDwt / ItmArmForEtb / orbSource），
/// 它们读 itmPorts/itmPriv/pc*/ts*/traceEvents，所以在 orbtrace 下改得动东西 —— 原来写成
/// enabled && (src==0||src==1)，选 orbtrace 就整块灰掉，等于"看着不可用、其实在用"。
void UpdateTraceEnable(HWND p, int src, bool enabled) {
    const bool orb    = enabled && (src == 2);   // 探针自己的端口格式
    const bool swoPin = enabled && (src == 0);   // 只有 SWO 引脚用得上

    const WORD itmCtl[] = {
        kIdEditSwo, kIdEditItmP, kIdEditItmPr,
        kIdCmbPcPre, kIdChkPcPer, kIdChkPcDrw,
        kIdGrpEvt, kIdChkEvt0, kIdChkEvt1, kIdChkEvt2,
        kIdChkEvt3, kIdChkEvt4, kIdChkEvt5,
        // Timestamps 组（`ts=` / `tspresc=` / `tswin`）也属于 ITM/DWT 那一半。
        kIdGrpTs, kIdChkTs, kIdCmbTsPre, kIdChkTsWin,
        // 这三个也跟着总开关：trace 关着时它们什么都不影响
        // （`etmen` 只影响 ProgramEtm、`trunlim` 只报进 TraceOpt、
        //   `tclkc` 只参与时钟推导）—— 留活着就是"整页其余全灰"那句话的反例。
        kIdChkEtmEn, kIdChkUnlim, kIdChkUseCore,
        // `etmalt` 只喂给解码器的构造（`ORBAGDI_Etm.cpp` 的 CreateDecoder）——
        // 总开关关着时它同样什么都不影响，所以跟上面三个一样归总开关管。
        kIdCmbEtmAlt
    };
    for (WORD id : itmCtl) EnableWindow(GetDlgItem(p, id), enabled);
    EnableWindow(GetDlgItem(p, kIdGrpItm), enabled);

    // 总开关关掉时 Source 下拉也没意义（ULINK Pro 的 Trace Enable 就是这个作用）。
    EnableWindow(GetDlgItem(p, kIdCmbTrcSrc), enabled ? TRUE : FALSE);

    // 两个时钟框只要任一条源开着就保持可用：它们不只属于 ITM 半边 —— orbtrace/ETB 的 SWO 路
    // 从它们推目标 TPIU 预分频（acprBaseKhz → ItmProgramTarget），跟着 ITM 灰掉等于藏起依赖。
    // "Use Core Clock" 让 Trace Clock 只读（驱动照样取核心时钟），所以灰掉而不是看着可编辑。
    const bool coreClk = IsCheck(p, kIdChkUseCore) != FALSE;
    EnableWindow(GetDlgItem(p, kIdEditCpu),  enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(p, kIdEditTclk),
                 (enabled && !coreClk) ? TRUE : FALSE);
    // SWO 分频（`swopresc`）和这两个时钟是**同一条推导链**（`SwoBaud()`：位时钟 =
    // trace 时钟 ÷ 它），而且 orbtrace 那条路也拿它当波特率的兜底 —— 所以跟时钟
    // 一样只跟总开关，不跟某一条源。
    EnableWindow(GetDlgItem(p, kIdEditSwopresc), enabled ? TRUE : FALSE);

    // SWO 编码（`swomode`）只对"SWO 引脚"有义：orbtrace 的编码在 Trace Port 下拉
    // 里，ETB 根本不走 SWO 引脚。所以它跟 `swoPin`，不跟 ITM 那半边。
    EnableWindow(GetDlgItem(p, kIdCmbSwomode), swoPin ? TRUE : FALSE);

    // 探针自己的端口格式 —— 只有 orbtrace 那条源用得上。
    EnableWindow(GetDlgItem(p, kIdCmbTrPort), orb ? TRUE : FALSE);
    EnableWindow(GetDlgItem(p, kIdGrpPort),   orb ? TRUE : FALSE);
}

// Debug 页的字段表：7 个开关。接口 / AP / 时钟三个下拉是**探测出来的**列表，
// Port / Connect / Reset 是"下标 -> 枚举值"的映射，都不在表里（见文件上方说明）。
const Field kDebugFields[] = {
    { kIdChkSwj,      FKind::Bool, offsetof(Config, swj) },
    { kIdChkRstCon,   FKind::Bool, offsetof(Config, resetAfterConnect) },
    { kIdChkStopRst,  FKind::Bool, offsetof(Config, stopAfterReset) },
    { kIdChkLogAcc,   FKind::Bool, offsetof(Config, logAccesses) },
    { kIdChkCacheCd,  FKind::Bool, offsetof(Config, cacheCode) },
    { kIdChkCacheMem, FKind::Bool, offsetof(Config, cacheMem) },
    { kIdChkDlFlash,  FKind::Bool, offsetof(Config, downloadToFlash) },
};
const int kDebugFieldCount = sizeof(kDebugFields) / sizeof(kDebugFields[0]);

void FillDebug(SheetCtx* ctx, HWND p, const Config& c) {
    FillIfaceCombo(ctx, p, c.iface);
    ShowIfaceInfo(ctx, p);
    FillCombo(p, kIdCmbPort, kPortNames, kPortCount, (c.port == kPortJtag) ? 1 : 0);
    UpdatePortEnable(p, (c.port == kPortJtag) ? 1 : 0);
    FillClockCombo(ctx, p, c.clockKhz);
    FillApCombo(ctx, p, c.ap);

    FillCombo(p, kIdCmbConnect, kConnectNames, kConnectCount, c.connect);
    FillCombo(p, kIdCmbReset, kResetNames, kResetCount, c.reset);
    FieldFill(p, c, kDebugFields, kDebugFieldCount);
}

// Trace 页的字段表 —— 只放"读一个框写一个字段"。**不在表里**：
//   Source 下拉（同时决定 orbtrace/etb/swoBaud，还要与 traceEnable 一起算）、
//   四个下拉（PC 档位/时间戳分频/端口宽度/端口模式）与 6 个事件复选（写查表值或位掩码）、
//   swoBaud（被 Source 门控）、itmPorts（**空框 = 用默认 0xFFFFFFFF**，不是 0）。
const Field kTraceFields[] = {
    { kIdChkTrcEn,   FKind::Bool, offsetof(Config, traceEnable) },
    { kIdChkEtmEn,   FKind::Bool, offsetof(Config, etmEnable) },
    { kIdChkUnlim,   FKind::Bool, offsetof(Config, unlimitedTrace) },
    { kIdChkUseCore, FKind::Bool, offsetof(Config, useCoreClock) },
    // ⚠ `etmalt`（kIdCmbEtmAlt）**不在本表里** —— 它 2026-10-14 从复选框改成了
    //   下拉框，与 `swomode` 同理，在 FillTrace()/ReadTrace() 里手工读写。
    //   理由：`FKind` 只有 Bool/Dec/Hex 三档，没有"枚举下拉"；给下拉新加一档要同时
    //   改这张表的读、写、校验三处，为一个控件不值。
    { kIdEditCpu,    FKind::Dec,  offsetof(Config, cpuKhz), 0, 1, 1000000,
      L"Core clock must be 1..1000000 kHz." },
    { kIdEditTclk,   FKind::Dec,  offsetof(Config, traceClockKhz), 0, 0, 1000000,
      L"Trace clock must be 0 (follow the core clock) or 1..1000000 kHz." },
    { kIdEditSwopresc, FKind::Dec, offsetof(Config, swoPrescaler), 0, 1, 65536,
      L"SWO prescaler must be 1 or more (it divides the trace clock)." },
    { kIdEditItmPr,  FKind::Hex,  offsetof(Config, itmPriv) },
    { kIdChkPcPer,   FKind::Bool, offsetof(Config, pcPeriodic) },
    { kIdChkPcDrw,   FKind::Bool, offsetof(Config, pcDataRw) },
    { kIdChkTs,      FKind::Bool, offsetof(Config, tsEnable) },
    { kIdChkTsWin,   FKind::Bool, offsetof(Config, tsWin) },
};
const int kTraceFieldCount = sizeof(kTraceFields) / sizeof(kTraceFields[0]);

void FillTrace(HWND p, const Config& c) {
    // "Off" 不再是下拉里的一项 —— 它现在是顶上那个 "Trace Enable" 复选框。
    int src = 0;
    if (c.orbtrace)         src = 2;
    else if (c.etb)         src = 1;
    else if (c.swoBaud > 0) src = 0;
    FillCombo(p, kIdCmbTrcSrc, kTraceSrcNames, kTraceSrcCount, src);

    // 表里的先填（含四个开关与两个时钟）—— `UpdateTraceEnable` 要读
    // "Use Core Clock" 的当前状态，所以必须在它之前。
    FieldFill(p, c, kTraceFields, kTraceFieldCount);
    UpdateTraceEnable(p, src, c.traceEnable);

    SetDec(p, kIdEditSwo, c.swoBaud);
    SetHex(p, kIdEditItmP, c.itmPorts);

    int tap = c.pcPrescaler;
    if (tap < 0 || tap >= kPcTapCount) tap = 0;
    FillCombo(p, kIdCmbPcPre, kPcTapNames, kPcTapCount, tap);

    int tp = 0;
    for (int i = 0; i < kTsPrescCount; ++i) {
        if (kTsPrescVals[i] == c.tsPrescaler) { tp = i; break; }
    }
    FillCombo(p, kIdCmbTsPre, kTsPrescNames, kTsPrescCount, tp);

    for (int i = 0; i < kEvtCount; ++i) {
        SetCheck(p, static_cast<WORD>(kIdChkEvt0 + i),
                 (static_cast<unsigned>(c.traceEvents) & kEvtBits[i]) != 0);
    }

    // Trace Port：SWO 档看 `trswo`，并口档看 `trwidth`。
    int portSel = 2;                             // 默认落在 4 位并口
    if (c.trSwoMode > 0) {
        portSel = (c.trSwoMode == 2) ? 4 : 3;
    } else {
        for (int i = 0; i < kTrPortParaCount; ++i) {
            if (kTrPortWidths[i] == c.trWidth) { portSel = i; break; }
        }
    }
    FillCombo(p, kIdCmbTrPort, kTrPortNames, kTrPortCount, portSel);
    FillCombo(p, kIdCmbSwomode, kSwoModeNames, kSwoModeCount,
              (c.swoMode == 1) ? 1 : 0);
    // etmalt（分支地址编码）：取值 -> 下拉下标（找不到就落第一项 = 默认 ALT）。
    int altSel = 0;
    for (int i = 0; i < kEtmAltCount; ++i) {
        if (kEtmAltVals[i] == (c.etmAltAddr ? 1 : 0)) { altSel = i; break; }
    }
    FillCombo(p, kIdCmbEtmAlt, kEtmAltNames, kEtmAltCount, altSel);
}

/// Restates what the Algorithm box holds, and whether it is on disk.
void FlashInfo(HWND p, const wchar_t* prefix) {
    wchar_t path[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditFlm, path, kPathMax);

    wchar_t text[900];
    if (!path[0]) {
        _snwprintf_s(text, _TRUNCATE,
                     L"%sNo algorithm selected.\r\n"
                     L"\"Detect from Keil\" uses the one the device's pack declares; "
                     L"\"...\" picks a file by hand.",
                     prefix ? prefix : L"");
    } else if (FileExists(path)) {
        _snwprintf_s(text, _TRUNCATE, L"%sOn disk:\r\n%s", prefix ? prefix : L"", path);
    } else {
        _snwprintf_s(text, _TRUNCATE, L"%sNot on disk:\r\n%s", prefix ? prefix : L"", path);
    }
    SetDlgItemTextW(p, kIdValFlmInfo, text);
}

// Flash 页的字段表。两条十六进制带格式校验（`msg` 里的 %s 会被填成框里的原文）；
// 擦除方式是一组单选，不是"一个控件对一个字段"，所以不在表里。
const Field kFlashFields[] = {
    { kIdEditFlm,     FKind::Text, offsetof(Config, flm), sizeof(Config::flm) },
    { kIdEditRamAdr,  FKind::Hex,  offsetof(Config, ramAddr), 0, 0, 0xFFFFFFFFll,
      L"RAM start \"%s\" is not hexadecimal, e.g. 20000000." },
    { kIdEditRamSz,   FKind::Hex,  offsetof(Config, ramSize), 0, 1, 0xFFFFFFFFll,
      L"RAM size \"%s\" must be a non-zero hexadecimal size, e.g. 1000." },
    { kIdChkProgram,  FKind::Bool, offsetof(Config, program) },
    { kIdChkFdVerify, FKind::Bool, offsetof(Config, verify) },
    { kIdChkRstRun,   FKind::Bool, offsetof(Config, resetAndRun) },
};
const int kFlashFieldCount = sizeof(kFlashFields) / sizeof(kFlashFields[0]);

void FillFlash(HWND p, const Config& c) {
    FieldFill(p, c, kFlashFields, kFlashFieldCount);
    CheckRadioButton(p, kIdRadErase0, kIdRadErase2,
                     kIdRadErase0 + ((c.eraseMode >= 0 && c.eraseMode < kEraseCount)
                                     ? c.eraseMode : 1));
    FlashInfo(p, L"");
}

// Pack 页的字段表：两个开关 + 一条日志路径。三个都没有范围可校验。
const Field kPackFields[] = {
    { kIdChkPdsc,     FKind::Bool, offsetof(Config, pdscEnable) },
    { kIdChkPdscLog,  FKind::Bool, offsetof(Config, pdscLog) },
    { kIdEditPdscLog, FKind::Text, offsetof(Config, pdscLogFile),
      sizeof(Config::pdscLogFile) },
};
const int kPackFieldCount = sizeof(kPackFields) / sizeof(kPackFields[0]);

void FillPack(HWND p, const Config& c) {
    FieldFill(p, c, kPackFields, kPackFieldCount);
}

/// Restates what the Record page will actually do.  "0" is a *default*, not
/// "off", and the derived block count is the number that says whether the
/// block chain (P1-1) will ever switch blocks - so it is worth spelling out.
void RecInfo(HWND p) {
    const int chunk = GetDec(p, kIdEditTrChunk, 0);
    const int mem   = GetDec(p, kIdEditTrMem, 0);
    const int keep  = GetDec(p, kIdEditTrKeep, 3);

    wchar_t wf[kPathMax] = L"";
    wchar_t wr[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditTrFile, wf, kPathMax);
    GetDlgItemTextW(p, kIdEditTrRec, wr, kPathMax);

    const int chunkMb = (chunk > 0) ? chunk : 4;

    wchar_t text[640];
    if (mem > 0) {
        _snwprintf_s(text, _TRUNCATE,
                     L"Effective: %d MB per block, cap %d MB -> %d block(s)\r\n"
                     L"Trace file : %s\r\n"
                     L"Record log : %s      keep %d file(s)",
                     chunkMb, mem, (chunkMb > 0) ? (mem / chunkMb) : 0,
                     wf[0] ? wf : L"(off)", wr[0] ? wr : L"(off)", keep);
    } else {
        _snwprintf_s(text, _TRUNCATE,
                     L"Effective: %d MB per block, cap = physical RAM / 4 "
                     L"(at most 1 GB)\r\n"
                     L"Trace file : %s\r\n"
                     L"Record log : %s      keep %d file(s)",
                     chunkMb, wf[0] ? wf : L"(off)", wr[0] ? wr : L"(off)", keep);
    }
    SetDlgItemTextW(p, kIdValRecInfo, text);
}

// Record 页的字段表：3 个普通数值。两条路径带 `...` 按钮，单独处理（见下面）。
const Field kRecFields[] = {
    // 上下界**与驱动侧的夹取保持一致**（`kChunkMBMin/Max` = 1/64、`kMemMBMax` = 4096，
    // 见 ORBAGDI_TraceWin.cpp）。原来这里是 4096 / 65536 —— 用户填 4096 会被驱动
    // 悄悄钳到 64，界面却显示填进去了。
    { kIdEditTrChunk, FKind::Dec, offsetof(Config, trChunkMB), 0, 0, 64,
      L"Record block size must be 0 (default) or 1..64 MB." },
    { kIdEditTrMem,   FKind::Dec, offsetof(Config, trMemMB),   0, 0, 4096,
      L"Record memory cap must be 0 (default) or 1..4096 MB." },
    { kIdEditTrKeep,  FKind::Dec, offsetof(Config, trKeep),    0, 0, 999,
      L"\"Keep files\" must be 0 (never delete) or 1..999." },
};
const int kRecFieldCount = sizeof(kRecFields) / sizeof(kRecFields[0]);

void FillRecord(HWND p, const Config& c) {
    FieldFill(p, c, kRecFields, kRecFieldCount);
    SetDlgItemTextW(p, kIdEditTrFile, Widen(c.trFile).c_str());
    SetDlgItemTextW(p, kIdEditTrRec, Widen(c.trRec).c_str());
    RecInfo(p);
}

// ===========================================================================
// Power page.  **没有配置键**（见 AddPowerItems 的说明）—— 所以不走 `Field` 表，
// `Accept()` 也不读它。这里读写的是 `ORBAGDI_OrbTrace.h` 里那份"最后下发成功的东西"。
// ===========================================================================

/// 把档位表填进下拉。档位表**只有一份**（`OrbPowerLevels`，在驱动侧），界面从它
/// 现取 —— 在这里再抄一遍就成了两处真相，改一处漏一处。
void FillPowerCombo(HWND p, WORD id, int ch, int selMv) {
    int n = 0;
    const orb::agdi::PowerLevel* lv = orb::agdi::OrbPowerLevels(ch, n);
    std::vector<const wchar_t*> names;
    names.reserve(static_cast<size_t>(n));
    int sel = 0;
    for (int i = 0; i < n; ++i) {
        names.push_back(lv[i].name);
        if (lv[i].mv == selMv) sel = i;
    }
    FillCombo(p, id, names.data(), static_cast<int>(names.size()), sel);
}

/// 下拉当前那一项的毫伏值。0 = 表里没有（不该发生）。
int PowerComboMv(HWND p, WORD id, int ch) {
    int n = 0;
    const orb::agdi::PowerLevel* lv = orb::agdi::OrbPowerLevels(ch, n);
    const int sel = ComboSel(p, id, 0);
    return (lv && sel >= 0 && sel < n) ? lv[sel].mv : 0;
}

/// 能力不足时把这一页的控件灰掉。状态框**留着** —— 它就是说明原因的地方。
void SetPowerEnabled(HWND p, bool on) {
    const WORD ids[] = { kIdCmbPwrVtref, kIdChkPwrVtrefEn,
                         kIdCmbPwrVtpwr, kIdChkPwrVtpwrEn,
                         kIdBtnPwrApply, kIdBtnPwrOff };
    for (WORD id : ids) EnableWindow(GetDlgItem(p, id), on ? TRUE : FALSE);
}

/// 状态回显。**探针没有回读接口**，所以能说的只有三件事：能力够不够、我们下发过
/// 什么、还没下发过时页面打算发什么。绝不假装知道探针现在的状态。
void RefreshPowerStatus(HWND p, const orb::agdi::PowerCaps& caps) {
    if (!p) return;
    const orb::agdi::PowerState st = orb::agdi::OrbPowerGet();
    wchar_t text[512];
    if (!caps.ready) {
        _snwprintf_s(text, _TRUNCATE,
                     L"Power control is unavailable, so this page is greyed out:\r\n"
                     L"  %s",
                     caps.why ? caps.why : L"(unknown reason)");
    } else if (!st.known) {
        _snwprintf_s(text, _TRUNCATE,
                     L"Nothing has been sent yet - the probe's outputs are whatever "
                     L"they were before this dialog opened.\r\n"
                     L"Apply would send:    Vtref %s    Vtpwr %s",
                     IsCheck(p, kIdChkPwrVtrefEn) ? L"ON" : L"off",
                     IsCheck(p, kIdChkPwrVtpwrEn) ? L"ON" : L"off");
    } else {
        _snwprintf_s(text, _TRUNCATE,
                     L"Last applied:    Vtref %d mV (%s)    Vtpwr %d mV (%s)\r\n"
                     L"The probe has no read-back, so this is what this driver sent.",
                     st.mv[orb::agdi::kPowerChVtref],
                     st.on[orb::agdi::kPowerChVtref] ? L"ON" : L"off",
                     st.mv[orb::agdi::kPowerChVtpwr],
                     st.on[orb::agdi::kPowerChVtpwr] ? L"ON" : L"off");
    }
    SetDlgItemTextW(p, kIdValPwrStatus, text);
}

void FillPower(SheetCtx* ctx) {
    HWND p = ctx->hPage[kPagePower];
    if (!p) return;
    const orb::agdi::PowerState st = orb::agdi::OrbPowerGet();
    FillPowerCombo(p, kIdCmbPwrVtref, orb::agdi::kPowerChVtref,
                   st.mv[orb::agdi::kPowerChVtref]);
    FillPowerCombo(p, kIdCmbPwrVtpwr, orb::agdi::kPowerChVtpwr,
                   st.mv[orb::agdi::kPowerChVtpwr]);
    SetCheck(p, kIdChkPwrVtrefEn, st.on[orb::agdi::kPowerChVtref]);
    SetCheck(p, kIdChkPwrVtpwrEn, st.on[orb::agdi::kPowerChVtpwr]);

    // 能力不足 → **整页灰掉**。在建页的时候就查，不是等用户点了才报错：
    //   ① 宿主不是 orbtrace 的（没有 OrbTrace_SetVoltage 导出）；
    //   ② 没找到探针；③ 探针没有 Power 接口（能力位 0x04 为 0）。
    // 结果留在 `ctx->powerCaps` —— 下面几个处理器还要用它，而它每次都要枚举 USB。
    SetPowerEnabled(p, ctx->powerCaps.ready);
    RefreshPowerStatus(p, ctx->powerCaps);
}

// ===========================================================================
// 读回一页。返回 false 保持设置页打开；被拒的值报到 Info 页并把页面切过去，
// 这样原因在屏幕上，而不是弹在一个没人看第二遍的模态框里。
// ===========================================================================
void SwitchPage(SheetCtx* ctx, int sel);          // defined with the sheet below

void ReportW(SheetCtx* ctx, const wchar_t* fmt, ...) {
    wchar_t body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, sizeof(body) / sizeof(body[0]), _TRUNCATE, fmt, ap);
    va_end(ap);

    char narrow[2048];
    Narrow(body, narrow, sizeof(narrow));
    ORB_LOG_WARN("Cfg: %s", narrow);

    if (!ctx->report.empty()) ctx->report += L"\r\n";
    ctx->report += body;
    RefreshInfo(ctx);
}

bool ReadDebug(SheetCtx* ctx, HWND p, Config& c) {
    const int clock = ClockValue(ctx, p);
    if (clock < 0 || clock > 100000) {
        ReportW(ctx, L"SWJ clock %d kHz is out of range (0 = host default, "
                    L"otherwise 1..100000); the host default is used.", clock);
        c.clockKhz = 0;
    } else {
        c.clockKhz = clock;
    }

    // rddiDll is deliberately not touched: the host DLL is auto-discovered and
    // the sheet has no control for it.
    c.port    = (ComboSel(p, kIdCmbPort, 0) == 1) ? kPortJtag : kPortSwd;
    c.iface   = IfaceValue(ctx, p);
    c.ap      = ApValue(ctx, p);
    c.connect = ComboSel(p, kIdCmbConnect, 0);
    c.reset   = ComboSel(p, kIdCmbReset, 0);
    return FieldRead(ctx, p, c, kDebugFields, kDebugFieldCount);
}

bool ReadTrace(SheetCtx* ctx, HWND p, Config& c) {
    wchar_t witm[32] = L"";
    GetDlgItemTextW(p, kIdEditItmP, witm, 32);
    if (witm[0]) {
        wchar_t* end = nullptr;
        wcstoul(witm, &end, 16);
        if (end && *end) {
            ReportW(ctx, L"ITM stimulus ports must be hexadecimal, e.g. FFFFFFFF.");
            SwitchPage(ctx, kPageInfo);
            SetFocus(GetDlgItem(p, kIdEditItmP));
            return false;
        }
    }

    const int src  = ComboSel(p, kIdCmbTrcSrc, 0);
    const int swo  = GetDec(p, kIdEditSwo, 0);
    if (swo < 0 || swo > 100000000) {
        ReportW(ctx, L"SWO baud must be 0 (console off) or 1..100000000 Hz.");
        SwitchPage(ctx, kPageInfo);
        SetFocus(GetDlgItem(p, kIdEditSwo));
        return false;
    }
    // 表里的字段（含 `traceEnable` 与两个时钟）：校验全过之后才写。
    if (!FieldRead(ctx, p, c, kTraceFields, kTraceFieldCount)) return false;

    // "Trace Enable" 是总开关，Source 只决定走哪条路。关掉时清空三个源标志，工程就不可能带着
    // "disabled + orbtrace=1"。
    // 图上那三个开关（ETM / Unlimited / Use Core Clock）与总开关**平级**、不是子项，照原样存：
    // 关掉 trace 时它们只是暂时不起作用，值要留住。
    c.orbtrace = c.traceEnable && (src == 2);
    c.etb      = c.traceEnable && (src == 1);
    c.swoBaud  = (c.traceEnable && src == 0) ? swo : 0;

    bool ok = true;
    c.itmPorts    = GetHex(p, kIdEditItmP, 0xFFFFFFFFu, &ok);
    c.pcPrescaler = ComboSel(p, kIdCmbPcPre, 0);

    const int tsi = ComboSel(p, kIdCmbTsPre, 0);
    c.tsPrescaler = kTsPrescVals[(tsi >= 0 && tsi < kTsPrescCount) ? tsi : 0];

    unsigned evt = 0;
    for (int i = 0; i < kEvtCount; ++i) {
        if (IsCheck(p, static_cast<WORD>(kIdChkEvt0 + i))) evt |= kEvtBits[i];
    }
    // 只覆盖 6 个复选框那几位 —— 手改 `events=` 里带的 `TRACE_CYCCNT`（0x0400，
    // 没有复选框）要留住，不能被界面清掉。
    c.traceEvents = (static_cast<unsigned>(c.traceEvents) & ~kEvtMask) | evt;

    // Trace Port：**一个**下拉写回**两个**键。SWO 档**不动 `trwidth`** —— 切回并口时
    // 要用回用户原来那一档，而不是被 SWO 顺手改成 4。
    const int tpi = ComboSel(p, kIdCmbTrPort, 2);
    const int tpx = (tpi >= 0 && tpi < kTrPortCount) ? tpi : 2;
    c.trSwoMode = kTrPortModes[tpx];
    if (tpx < kTrPortParaCount) c.trWidth = kTrPortWidths[tpx];
    c.swoMode   = (ComboSel(p, kIdCmbSwomode, 0) == 1) ? 1 : 0;
    // etmalt：下拉下标 -> 取值（越界一律回到默认档 = ALT）。
    const int altSel = ComboSel(p, kIdCmbEtmAlt, 0);
    c.etmAltAddr = (altSel >= 0 && altSel < kEtmAltCount) ? (kEtmAltVals[altSel] != 0)
                                                          : true;
    // trbaud= / troflow= / trtpui= have no control any more: the probe reports
    // its own framing, and its SWO baud follows the SWO value.  Their parsed
    // values are left alone so a hand-edited project still round-trips.
    return true;
}

bool ReadFlash(SheetCtx* ctx, HWND p, Config& c) {
    if (!FieldRead(ctx, p, c, kFlashFields, kFlashFieldCount)) return false;

    for (int i = 0; i < kEraseCount; ++i) {
        if (IsDlgButtonChecked(p, kIdRadErase0 + i) == BST_CHECKED) c.eraseMode = i;
    }

    // A warning, not a blocker: the file may live on a share that is not mounted
    // yet, and the download itself reports the failure in the log.
    const std::wstring wflm = Widen(c.flm);
    if (!wflm.empty() && !FileExists(wflm.c_str())) {
        ReportW(ctx, L"Warning: the algorithm \"%s\" is not on disk.", wflm.c_str());
    }
    return true;
}

bool ReadPack(SheetCtx* ctx, HWND p, Config& c) {
    return FieldRead(ctx, p, c, kPackFields, kPackFieldCount);
}

bool ReadRecord(SheetCtx* ctx, HWND p, Config& c) {
    if (!FieldRead(ctx, p, c, kRecFields, kRecFieldCount)) return false;

    // 上限 < 一块：不是错误（`TraceWinSetBuffer` 会把它钳成单块），但代价是整段
    // 历史都没了，所以要说一声。
    if (c.trMemMB > 0 && c.trChunkMB > 0 && c.trMemMB < c.trChunkMB) {
        ReportW(ctx, L"Memory cap %d MB is smaller than one block (%d MB): the "
                    L"buffer is clamped to a single block and no earlier history "
                    L"is kept.", c.trMemMB, c.trChunkMB);
    }

    wchar_t wf[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditTrFile, wf, kPathMax);
    Narrow(wf, c.trFile, sizeof(c.trFile));
    wchar_t wr[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditTrRec, wr, kPathMax);
    Narrow(wr, c.trRec, sizeof(c.trRec));
    return true;
}

// ---------------------------------------------------------------------------
// The page buttons
// ---------------------------------------------------------------------------
void FlmBrowse(HWND p) {
    wchar_t path[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditFlm, path, kPathMax);

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = p;
    ofn.lpstrFilter = L"Keil flash algorithm (*.FLM)\0*.FLM\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = kPathMax;
    ofn.lpstrTitle  = L"Select the flash algorithm";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        SetDlgItemTextW(p, kIdEditFlm, path);
        FlashInfo(p, L"");
    }
}

/// "Detect from Keil": the algorithm the device's pack (or the project's
/// Utilities page) declares comes first; the pack scan is only the fallback, so
/// pressing the button on a configured project picks the *right* FLM instead of
/// whichever file happens to sort first.
void FlmDetect(SheetCtx* ctx, HWND p) {
    const Config e = Effective();
    if (e.flm[0] && FileExists(Widen(e.flm))) {
        SetDlgItemTextW(p, kIdEditFlm, Widen(e.flm).c_str());
        FlashInfo(p, L"From the device's pack / the Keil project:\r\n");
        Report(ctx, "Flash: algorithm taken from Keil: %s", e.flm);
        return;
    }

    const std::vector<std::wstring> flms = AllFlms();
    if (flms.empty()) {
        FlashInfo(p, L"");
        Report(ctx, "Flash: Keil named no algorithm and no .FLM was found on this "
                    "machine - install the device pack, or use \"...\" to pick one.");
        return;
    }
    SetDlgItemTextW(p, kIdEditFlm, flms[0].c_str());
    wchar_t prefix[200];
    _snwprintf_s(prefix, _TRUNCATE,
                 L"Keil named no algorithm; %u found on disk, first selected:\r\n",
                 static_cast<unsigned>(flms.size()));
    FlashInfo(p, prefix);
    Report(ctx, "Flash: %u .FLM file(s) on disk; selected %ls",
           static_cast<unsigned>(flms.size()), flms[0].c_str());
}

/// Picks the file the PDSC sequence log is written to.
void PdscLogBrowse(HWND p) {
    wchar_t path[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditPdscLog, path, kPathMax);

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = p;
    ofn.lpstrFilter = L"Log file (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = kPathMax;
    ofn.lpstrTitle  = L"Where to log the debug-description sequences";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) SetDlgItemTextW(p, kIdEditPdscLog, path);
}

/// Where the raw trace stream is dumped (`trfile=`, P2-1, OFLOW format).
void TrFileBrowse(HWND p) {
    wchar_t path[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditTrFile, path, kPathMax);

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = p;
    ofn.lpstrFilter = L"Trace stream (*.orb)\0*.orb\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = kPathMax;
    ofn.lpstrTitle  = L"Where to dump the raw trace stream (trfile=)";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) {
        SetDlgItemTextW(p, kIdEditTrFile, path);
        RecInfo(p);
    }
}

/// Where the compact record log goes (`trrec=`, P2-2).
void TrRecBrowse(HWND p) {
    wchar_t path[kPathMax] = L"";
    GetDlgItemTextW(p, kIdEditTrRec, path, kPathMax);

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = p;
    ofn.lpstrFilter = L"Record log (*.trc)\0*.trc\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = kPathMax;
    ofn.lpstrTitle  = L"Where to keep the record log (trrec=)";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) {
        SetDlgItemTextW(p, kIdEditTrRec, path);
        RecInfo(p);
    }
}

/// Power 页的 Apply —— **整个设置页里只有这里**会把电压/使能发给探针（OK/Cancel 不走这条路）。
/// 两趟：先关掉没勾的、再开勾上的，于是 Apply 之后页面状态就是探针状态，也不会"先开 A 又关 A"。
void PowerApply(SheetCtx* ctx) {
    HWND p = ctx->hPage[kPagePower];
    if (!p) return;

    // 灰显是第一道闸，这是第二道：外部脚本能绕过灰显直接发命令，那时也要给出
    // **说得清的原因**，而不是一个笼统的失败。
    const orb::agdi::PowerCaps caps = orb::agdi::OrbPowerCaps();
    if (!caps.ready) {
        ReportW(ctx, L"Power: %s", caps.why ? caps.why : L"unavailable.");
        SetPowerEnabled(p, false);
        RefreshPowerStatus(p, caps);
        return;
    }

    struct Ch { int ch; WORD cmb; WORD chk; };
    const Ch chans[] = {
        { orb::agdi::kPowerChVtref, kIdCmbPwrVtref, kIdChkPwrVtrefEn },
        { orb::agdi::kPowerChVtpwr, kIdCmbPwrVtpwr, kIdChkPwrVtpwrEn },
    };

    bool ok = true;
    for (int pass = 0; pass < 2; ++pass) {          // pass 0 = 关，pass 1 = 开
        for (const Ch& c : chans) {
            const bool on = IsCheck(p, c.chk);
            if (on != (pass == 1)) continue;
            if (!orb::agdi::OrbPowerApply(c.ch, PowerComboMv(p, c.cmb, c.ch), on)) {
                ok = false;
            }
        }
    }

    Report(ctx, ok ? "Power: applied."
                   : "Power: at least one output did not take it - see the log.");
    RefreshPowerStatus(p, caps);
}

void PowerAllOff(SheetCtx* ctx) {
    HWND p = ctx->hPage[kPagePower];
    const orb::agdi::PowerCaps caps = orb::agdi::OrbPowerCaps();
    if (!caps.ready) {
        ReportW(ctx, L"Power: %s", caps.why ? caps.why : L"unavailable.");
        if (p) SetPowerEnabled(p, false);
        RefreshPowerStatus(p, caps);
        return;
    }

    const bool ok = orb::agdi::OrbPowerAllOff();
    if (p) {
        SetCheck(p, kIdChkPwrVtrefEn, false);
        SetCheck(p, kIdChkPwrVtpwrEn, false);
        RefreshPowerStatus(p, caps);
    }
    Report(ctx, ok ? "Power: both outputs off."
                   : "Power: All Off did not take - see the log.");
}

/// The bottom row.  Nothing here changes an option any more - the buttons are
/// kept as the home for the modules that will need them.
void Placeholder(SheetCtx* ctx, const wchar_t* which) {
    ReportW(ctx, L"\"%s\" is a placeholder: it is reserved for a future module "
                 L"(orbtrace's Vtref / Vpwer power outputs, for instance) and "
                 L"currently changes nothing.", which);
    SwitchPage(ctx, kPageInfo);
}

// ---------------------------------------------------------------------------
// OK
// ---------------------------------------------------------------------------
/// 把每一页读回在线选项；返回 false 表示某页拒绝了自己的输入、要保持设置页打开。
bool Accept(SheetCtx* ctx) {
    Config& c = Get();
    if (!ReadDebug(ctx, ctx->hPage[kPageDebug], c)) return false;
    if (!ReadTrace(ctx, ctx->hPage[kPageTrace], c)) return false;
    if (!ReadFlash(ctx, ctx->hPage[kPageFlash], c)) return false;
    if (!ReadPack (ctx, ctx->hPage[kPagePack], c)) return false;
    if (!ReadRecord(ctx, ctx->hPage[kPageRecord], c)) return false;

    // Trace Enable 一关就**当场**收源，不能等下一次 Connect：ReadTrace() 只写全局配置，而运行中
    // 的状态机不读配置 —— OrbTrace/ITM 还在跑、TraceDrainWanted() 仍为真，worker 继续每 2 ms
    // 抢探针锁抽数，调试线程的非阻塞 ComLock 拿不到就报 probe busy（"取消 trace 后更卡"）。
    if (!c.traceEnable) orb::agdi::StopTraceSources("settings: trace disabled");

    Format(ctx->out, ctx->outCch);
    ctx->ok = true;
    ORB_LOG_INFO("Cfg: options set to \"%s\"", ctx->out);
    return true;
}

// ---------------------------------------------------------------------------
// Driving the sheet
// ---------------------------------------------------------------------------
/// 页只是占位用的裸子对话框，只有控件所有权；控件发的命令一律转发给表，由表统一处理。
INT_PTR CALLBACK PageProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) {
        HWND parent = GetParent(hDlg);
        if (parent) return static_cast<INT_PTR>(SendMessageW(parent, msg, wp, lp));
        return FALSE;
    }
    if (msg == WM_CTLCOLORSTATIC) {
        // A read-only EDIT is painted through WM_CTLCOLORSTATIC, whose default
        // brush is the dialog face - which would make the Info box look
        // disabled.  Give it the window colours it should have.
        HWND ctl = reinterpret_cast<HWND>(lp);
        if (ctl && GetDlgCtrlID(ctl) == kIdEditInfo) {
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            SetTextColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
        }
    }
    return FALSE;
}

/// Restacks every page over the tab's display area and shows only `sel`.
void ShowPage(SheetCtx* ctx, int sel) {
    RECT rc;
    GetClientRect(ctx->hTab, &rc);
    TabCtrl_AdjustRect(ctx->hTab, FALSE, &rc);
    MapWindowPoints(ctx->hTab, GetParent(ctx->hTab),
                    reinterpret_cast<POINT*>(&rc), 2);

    for (int i = 0; i < kPageCount; ++i) {
        HWND page = ctx->hPage[i];
        if (!page) continue;
        SetWindowPos(page, nullptr, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(page, (i == sel) ? SW_SHOW : SW_HIDE);
    }
}

int SelectedPage(HWND hTab) {
    const int sel = TabCtrl_GetCurSel(hTab);
    return (sel < 0 || sel >= kPageCount) ? 0 : sel;
}

void SwitchPage(SheetCtx* ctx, int sel) {
    if (!ctx->hTab) return;
    if (sel < 0 || sel >= kPageCount) sel = 0;
    TabCtrl_SetCurSel(ctx->hTab, sel);
    ShowPage(ctx, sel);
    if (ctx->hPage[sel]) SetFocus(ctx->hPage[sel]);
}

INT_PTR CALLBACK SheetProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    SheetCtx* ctx = reinterpret_cast<SheetCtx*>(GetWindowLongPtrW(hDlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<SheetCtx*>(lp);
        SetWindowLongPtrW(hDlg, DWLP_USER, lp);
        Parse(ctx->args);                   // show what is in effect right now

        ctx->hTab = GetDlgItem(hDlg, kIdTab);

        // TCM_INSERTITEMW 写全，**不用** TabCtrl_InsertItem 宏：宏按构建字符集选 A/W，而这是 ANSI
        // 构建（build.ps1 没定义 UNICODE）⇒ 展开成 ...A，而模板是用 DialogBoxIndirectParamW 建的、
        // 传的是 wchar_t。控件把 L"Debug" 当 ANSI 读，只画出 'D' —— 每个页标题都成了首字母。
        // CB_ADDSTRING 不需要同样处理：组合框/列表框由系统按目标窗口转换。
        for (int i = 0; i < kPageCount; ++i) {
            TCITEMW item;
            memset(&item, 0, sizeof(item));
            item.mask    = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(kPageNames[i]);
            SendMessageW(ctx->hTab, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&item));
        }

        HINSTANCE hInst = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&SheetProc), &hInst);
        for (int i = 0; i < kPageCount; ++i) {
            const DlgTpl tpl = BuildPage(i);
            ctx->hPage[i] = CreateDialogIndirectParamW(
                hInst, reinterpret_cast<LPCDLGTEMPLATEW>(tpl.b.data()),
                hDlg, PageProc, 0);
        }

        Config& c = Get();

        // ---- the RDDI host DLL: found, never asked for ---------------------
        if (!c.rddiDll[0]) {
            const std::wstring found = AutoDetectRddi();
            if (!found.empty()) {
                Narrow(found.c_str(), c.rddiDll, sizeof(c.rddiDll));
                Report(ctx, "RDDI host: %s", c.rddiDll);
            } else {
                Report(ctx, "ERROR: no RDDI host DLL found. Looked for "
                            "CMSIS_DAP.dll next to ORB_AGDI.dll, in <Keil>\\ARM\\BIN, "
                            "and at $ORBAGDI_RDDI_DLL. Debugging cannot start until "
                            "one of those is in place.");
            }
        } else {
            Report(ctx, "RDDI host: %s (from the project)", c.rddiDll);
        }

        // ---- what the RDDI layer answers about the bench -------------------
        if (ProbeInterfaces(ctx->ifaces)) {
            Report(ctx, "RDDI: %u CMSIS-DAP interface(s) detected.",
                   static_cast<unsigned>(ctx->ifaces.size()));
        } else {
            Report(ctx, "RDDI: no CMSIS-DAP interface answered - no probe attached, "
                        "or the host DLL is not an RDDI build.");
        }
        if (c.iface >= 0) {                 // keep a project's index selectable
            bool have = false;
            for (const IfaceInfo& f : ctx->ifaces) {
                if (f.index == c.iface) { have = true; break; }
            }
            if (!have) { IfaceInfo f; f.index = c.iface; ctx->ifaces.push_back(f); }
        }

        if (ProbeAps(ctx->aps, c.iface, c.port)) {
            Report(ctx, "RDDI: %u AP(s) answered the IDR probe.",
                   static_cast<unsigned>(ctx->aps.size()));
        } else {
            ApFallback(ctx->aps);
            Report(ctx, "RDDI: no target answered the AP probe - AP 0..3 are "
                        "offered unprobed.");
        }
        {
            bool have = false;
            for (const ApInfo& a : ctx->aps) {
                if (a.index == c.ap) { have = true; break; }
            }
            if (!have) { ApInfo a; a.index = c.ap; ctx->aps.push_back(a); }
        }

        FillDebug(ctx, ctx->hPage[kPageDebug], c);
        FillTrace(ctx->hPage[kPageTrace], c);
        FillFlash(ctx->hPage[kPageFlash], c);
        FillPack(ctx->hPage[kPagePack], c);
        FillRecord(ctx->hPage[kPageRecord], c);

        // Power 页：能力探测**只做一次**（它要枚举 USB），建页前拿到，
        // `FillPower` 与后面几个命令处理器共用 —— 否则点一下复选框就枚举一次 USB。
        ctx->powerCaps = orb::agdi::OrbPowerCaps();
        FillPower (ctx);

        // Offer the algorithm the device implies when the project names none
        // (bug.md E6 / E9): without this a Download ends in "no flash algorithm
        // configured" and nothing says why.
        if (!c.flm[0]) {
            const Config e = Effective();
            if (e.flm[0]) {
                strncpy_s(c.flm, e.flm, _TRUNCATE);
                FillFlash(ctx->hPage[kPageFlash], c);
                FlashInfo(ctx->hPage[kPageFlash], L"Detected for this device:\r\n");
                Report(ctx, "Flash: algorithm detected for this device: %s", c.flm);
            }
        }

        RefreshInfo(ctx);

        const int page0 = (ctx->page0 >= 0 && ctx->page0 < kPageCount) ? ctx->page0 : 0;
        TabCtrl_SetCurSel(ctx->hTab, page0);
        ShowPage(ctx, page0);
        return FALSE;                       // focus is already where we want it
    }
    case WM_NOTIFY: {
        const NMHDR* nm = reinterpret_cast<const NMHDR*>(lp);
        if (ctx && nm && nm->hwndFrom == ctx->hTab && nm->code == TCN_SELCHANGE) {
            SwitchPage(ctx, SelectedPage(ctx->hTab));
            return TRUE;
        }
        return FALSE;
    }
    case WM_ERASEBKGND: {
        // Fill with the system colour to keep the pages from flickering black.
        HBRUSH hbr = reinterpret_cast<HBRUSH>(
            SendMessageW(hDlg, WM_CTLCOLORDLG, wp, reinterpret_cast<LPARAM>(hDlg)));
        if (!hbr) hbr = GetSysColorBrush(COLOR_BTNFACE);
        RECT rc;
        GetClientRect(hDlg, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, hbr);
        return TRUE;
    }
    case WM_COMMAND:
        if (!ctx) return FALSE;
        switch (LOWORD(wp)) {
        case IDOK:
            // Every page is read, so one OK writes the whole option string.
            if (Accept(ctx)) EndDialog(hDlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;

        // The master switch and the source combo together decide which half of
        // the Trace page is live.
        case kIdCmbTrcSrc:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                UpdateTraceEnable(ctx->hPage[kPageTrace],
                                  ComboSel(ctx->hPage[kPageTrace], kIdCmbTrcSrc, 0),
                                  IsCheck(ctx->hPage[kPageTrace], kIdChkTrcEn));
            }
            return TRUE;

        case kIdChkTrcEn:
            UpdateTraceEnable(ctx->hPage[kPageTrace],
                              ComboSel(ctx->hPage[kPageTrace], kIdCmbTrcSrc, 0),
                              IsCheck(ctx->hPage[kPageTrace], kIdChkTrcEn));
            return TRUE;

        // "Use Core Clock" makes the Trace Clock box read-only (the driver takes
        // the core clock whatever the box says), so the greying has to be redone
        // when it is toggled.
        case kIdChkUseCore:
            UpdateTraceEnable(ctx->hPage[kPageTrace],
                              ComboSel(ctx->hPage[kPageTrace], kIdCmbTrcSrc, 0),
                              IsCheck(ctx->hPage[kPageTrace], kIdChkTrcEn));
            return TRUE;

        // `tswin`（"Show the time column"）**单独提示**，不跟别的复选框一起沉默：
        // `bHideTimestamps` 同时是 uVision 取数门的一环（COMPAT_ANALYSIS.md
        // §10.20.1），打开可能让 Trace Data 窗口**一行都不取**。勾上时把这句话写到
        // Info 页 —— 窗口空了就知道该关回哪里，而不是去猜驱动。
        case kIdChkTsWin:
            if (IsCheck(ctx->hPage[kPageTrace], kIdChkTsWin)) {
                Report(ctx, "Trace: \"Show the time column\" (tswin) is ON.  That bit "
                            "is part of uVision's fetch gate, so the Trace Data window "
                            "may stop fetching records - turn it back off if the window "
                            "goes blank.  The real time axis does not need it.");
            }
            return TRUE;

        // 选 adapter 不只是换一行身份回显：AP 列表是**这个 adapter** 的属性，
        // 必须按新接口重探一次（见 ReprobeForIface）。
        case kIdCmbIface:
            if (HIWORD(wp) == CBN_SELCHANGE) ReprobeForIface(ctx);
            return TRUE;

        // ... and the port combo decides whether SWJ-DP applies at all.
        case kIdCmbPort:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                UpdatePortEnable(ctx->hPage[kPageDebug],
                                 ComboSel(ctx->hPage[kPageDebug], kIdCmbPort, 0));
            }
            return TRUE;

        case kIdBtnFlmBrw:   FlmBrowse(ctx->hPage[kPageFlash]);           return TRUE;
        case kIdBtnFlmAuto:  FlmDetect(ctx, ctx->hPage[kPageFlash]);      return TRUE;
        case kIdBtnPdscLog:  PdscLogBrowse(ctx->hPage[kPagePack]);        return TRUE;

        case kIdBtnTrFile:   TrFileBrowse(ctx->hPage[kPageRecord]);       return TRUE;
        case kIdBtnTrRec:    TrRecBrowse (ctx->hPage[kPageRecord]);       return TRUE;

        // Power 页。**只有 Apply 会碰探针的电源**（见 PowerApply 的说明）。
        // 两个使能框只刷新状态回显（"Apply 会发什么"），一个字节都不发。
        case kIdBtnPwrApply: PowerApply(ctx);  return TRUE;
        case kIdBtnPwrOff:   PowerAllOff(ctx); return TRUE;
        case kIdChkPwrVtrefEn:
        case kIdChkPwrVtpwrEn:
            // 只刷新"Apply 会发什么"，一个字节都不发。用建页时探到的能力，
            // 免得点一次复选框就枚举一次 USB。
            RefreshPowerStatus(ctx->hPage[kPagePower], ctx->powerCaps);
            return TRUE;

        case kIdBtnDefault:  Placeholder(ctx, L"Defaults");  return TRUE;
        case kIdBtnTest:     Placeholder(ctx, L"Test");      return TRUE;
        case kIdBtnLog:      Placeholder(ctx, L"Log");       return TRUE;
        // `Power` 现在有真页面了，所以这个按钮改成切过去，不再是占位。
        case kIdBtnPower:    SwitchPage(ctx, kPagePower);    return TRUE;
        default:
            break;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/// The one entry point behind both Settings buttons.  `page0` picks the tab the
/// sheet opens on; `prjName` only goes to the log.
bool ShowSheet(HWND parent, const char* inArgs, char* outArgs, size_t outCch,
               int page0, const char* prjName) {
    if (!outArgs || outCch == 0) return false;

    SheetCtx ctx;
    strncpy_s(ctx.args, inArgs ? inArgs : "", _TRUNCATE);
    ctx.out    = outArgs;
    ctx.outCch = outCch;
    ctx.page0  = page0;
    if (prjName && prjName[0]) ORB_LOG_INFO("Cfg: sheet for project \"%s\"", prjName);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    const DlgTpl t = BuildSheetTpl();

    HINSTANCE hInst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ShowSheet), &hInst);

    const INT_PTR r = DialogBoxIndirectParamW(
        hInst, reinterpret_cast<LPCDLGTEMPLATEW>(t.b.data()), parent, SheetProc,
        reinterpret_cast<LPARAM>(&ctx));

    if (r == -1) {
        // A malformed template is the only realistic cause, and a Settings
        // button that silently does nothing is the exact bug we are fixing, so
        // report it instead of returning quietly.
        const DWORD err = GetLastError();
        ORB_LOG_ERROR("Cfg: settings sheet could not be created (err %lu)", err);
        wchar_t text[160];
        _snwprintf_s(text, _TRUNCATE,
                     L"The settings sheet could not be created.\n\nError %lu.", err);
        MessageBoxW(parent, text, L"ORBTRACE Debugger Settings",
                    MB_ICONERROR | MB_OK);
        return false;
    }

    // Cancel closes over a sheet that may have adopted an auto-detected DLL or
    // algorithm into the live options, so put the old string back.
    if (!ctx.ok) Parse(ctx.args);
    return ctx.ok;
}

} // namespace

// ---------------------------------------------------------------------------
// The two entry points uVision calls.  Both open the same sheet; only the tab
// it starts on differs, which is how Keil's own driver behaves.
// ---------------------------------------------------------------------------
bool ShowDialog(HWND parent, const char* inArgs, char* outArgs, size_t outCch) {
    // The Debug sheet's "Settings" button (DllUv3Cap(1)) lands on Debug.
    return ShowSheet(parent, inArgs, outArgs, outCch, kPageDebug, nullptr);
}

bool ShowFlashDialog(HWND parent, const char* inArgs, char* outArgs,
                     size_t outCch, const char* prjName) {
    // The Utilities sheet's Settings button (DllUv3Cap(110), Keil's
    // DoFDlgSetup()) lands on the flash page.
    return ShowSheet(parent, inArgs, outArgs, outCch, kPageFlash, prjName);
}

} // namespace cfg
} // namespace orb
