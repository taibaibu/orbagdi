/**
 * @file ORBAGDI_TraceFile.cpp
 * @brief trace 文件（OFLOW 溢出文件 + 记录 cache）。
 *
 * ① OFLOW 文件（trfile=，给外部工具）：魔数 "%%ORBFLOW1.0.0%%" + COBS/OFLOW 字节流。
 *    orbtrace 源原样写入 cache；swo= / etb=1 拿到的是裸字节流，由本层编码成帧再写。
 * ② 记录 cache（trrec=，给本驱动回看）：一块退休时把紧凑记录字节流原样追加，索引记
 *    {偏移, 该处第一条周期号}；查询按索引读回、用同一段解码展开，不必重解 ITM/ETM。
 * 文件名带时间不带版本号：ORB_trace_YYYYMMDD_HHMMSS.orb / ORB_rec_….trc。
 * 这是**内存装不下时的临时缓存**，不是日志：上一段的文件在**新段开始时全删**（见 PruneOld）。
 */
#include "pch.h"
#include "ORBAGDI_TraceFile.h"
#include "ORBAGDI_Oflow.h"     // kOflowTag* / kOflowMaxPayload / kOflowMaxEncoded
#include "ORBAGDI_Log.h"

#include <stdio.h>
#include <stdlib.h>    // realloc / free（索引按需扩容）
#include <string.h>
#include <wchar.h>
#include <algorithm>   // std::sort（删上一段缓存时按名字排序）
#include <vector>

namespace orb {
namespace agdi {
namespace {

// orbuculum `Src/orbuculum.c:70`：OFLOW 文件内容的签名。**不是文件名的一部分。**
const char kOflowSig[] = "%%ORBFLOW1.0.0%%";

// 索引规模：一条 16 B。初始 4096 条（64 KB），满时翻倍，封顶 65536 条（1 MB）。
// ⚠ 不能是**固定** 4096：File cap 默认不限后，按 4 MB/块算 4096 条只覆盖 16 GB，
// 之后的块会撞上"索引已满"而**完全不进索引** —— Locate 只能定位到最后一条 mark，
// 再往后全靠顺序解码 GB 级数据。1 MB 覆盖 256 GB，一次会话远远够用。
const unsigned kMarkInit     = 4096;
const unsigned kMarkMaxTotal = 65536;

struct Mark {
    uint64_t ofs;      // 文件偏移
    uint64_t cycle;    // 周期号
};

// 一份 cache 文件的全部状态。
struct FileState {
    FILE*              f;
    wchar_t            pathW[MAX_PATH];
    char               pathU8[MAX_PATH * 2];
    unsigned long long bytes;      // 已写字节（含签名）
    unsigned long long raw;        // OFLOW：其中"原样写入 cache"的
    unsigned long long frames;     // OFLOW：其中"本层编码"的帧数
    bool               failed;
    // 文件大小上限（字节），0 = 不限；到顶就停止追加并告警一次。
    // 选"停"不选"环形覆盖"：文件装的是旧历史，停在顶部正好等于"只能回溯到这么远"；
    // 环形覆盖要把索引里的偏移全部重基（Mark.ofs），代价大且容易出错。
    unsigned long long capBytes;
    bool               capWarned;
    bool               capped;      // 到过上限：不再追加，也不再算"可用"
    bool               capTaken;    // 到顶提示已经取走过（每次开会话重置）
        // 动态索引：满一条翻倍（64 KB → 1 MB）。`markCap` = 已分配条数（0 = 还没分配）。
        // 数组在进程内复用：CloseFile 不释放，CloseAndDeleteFile 才还给系统。
    Mark*              mark;
    unsigned           markCount;
    unsigned           markCap;
    bool               markWarned;
};

FileState        s_ofl;   // OFLOW 文件（trfile=）
FileState        s_rec;   // 记录 cache（trrec=）
CRITICAL_SECTION s_cs;
bool             s_csInit = false;

void Lock()
{
    if (!s_csInit) { InitializeCriticalSection(&s_cs); s_csInit = true; }
    EnterCriticalSection(&s_cs);
}
void Unlock() { LeaveCriticalSection(&s_cs); }

// ANSI（选项串）→ 宽字符。uVision 的选项串是本地代码页。
std::wstring Widen(const char* s)
{
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

// 宽字符 → UTF-8（日志文件是 UTF-8，中文路径不能变成问号）。
std::string NarrowUtf8(const wchar_t* s)
{
    if (!s || !*s) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string u(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &u[0], n, nullptr, nullptr);
    return u;
}

// 日志所在目录（日志还没开时退回 %TEMP%）。cache 文件默认放这里 —— 和日志在一起，
// 好找，也好一起删。
std::wstring LogDir()
{
    const wchar_t* lp = orb::log::FilePath();
    if (lp && *lp) {
        std::wstring d(lp);
        const size_t p = d.find_last_of(L"\\/");
        if (p != std::wstring::npos && p > 0) { d.resize(p); return d; }
    }
    wchar_t tmp[MAX_PATH] = {0};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n && n < MAX_PATH) {
        std::wstring d(tmp);
        while (!d.empty() && (d[d.size() - 1] == L'\\' || d[d.size() - 1] == L'/')) {
            d.resize(d.size() - 1);
        }
        if (!d.empty()) return d;
    }
    return L".";
}

bool IsDir(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && ((a & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

// 自动命名：`<prefix>_YYYYMMDD_HHMMSS.<ext>`（带时间，不带版本号）。
std::wstring AutoName(const wchar_t* prefix, const wchar_t* ext)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t nm[96] = {0};
    _snwprintf_s(nm, _TRUNCATE, L"%s_%04u%02u%02u_%02u%02u%02u.%s",
                 prefix,
                 (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                 (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                 ext);
    return std::wstring(nm);
}

// 新采集段开始时，把同目录里**更早的** `<prefix>_*.<ext>` 全部删掉。
// 写文件在本项目里不是功能，只是"内存装不下"时的临时缓存：上一段的数据留在盘上
// 没有任何用处，只会越攒越多 —— 新段开始就是该删的时候，没有"保留几个"这个旋钮。
// 名字里就是时间戳 ⇒ **按名字排序即按时间排序**，不用去读每个文件的时间。
unsigned PruneOld(const std::wstring& dir, const wchar_t* prefix, const wchar_t* ext)
{
    const std::wstring pat = dir + L"\\" + prefix + L"_*." + ext;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (names.empty()) return 0;
    std::sort(names.begin(), names.end());

    // 本段的新文件在删完之后才建，所以这里有旧的就能全删。
    unsigned removed = 0;
    for (const std::wstring& n : names) {
        const std::wstring victim = dir + L"\\" + n;
        if (DeleteFileW(victim.c_str())) {
            ++removed;
        } else {
            // 删不掉（被别的进程打开？）不该影响本次写文件。
            ORB_LOG_WARN("TraceFile: 旧缓存删不掉（%s）", NarrowUtf8(victim.c_str()).c_str());
        }
    }
    return removed;
}

// ---------------------------------------------------------------------------
// 打开 / 关闭 / 写（两份文件共用）
// ---------------------------------------------------------------------------
bool OpenFile(FileState& st, const char* path, unsigned capMB,
              const wchar_t* prefix, const wchar_t* ext, const char* sig)
{
    Lock();
    if (st.f) { Unlock(); return true; }
    Unlock();

    if (!path || !*path) return false;

    std::wstring dir;
    std::wstring name;

    if (!strcmp(path, "1")) {
        dir  = LogDir();
        name = AutoName(prefix, ext);
    } else {
        const std::wstring w = Widen(path);
        const bool dirLike = (!w.empty() && (w[w.size() - 1] == L'\\' || w[w.size() - 1] == L'/'));
        if (dirLike || IsDir(w)) {
            dir  = w;
            name = AutoName(prefix, ext);
        } else {
            // 给了具体文件名：按原样用（不再加时间戳），目录缺省放日志旁边。
            const size_t p = w.find_last_of(L"\\/");
            if (p == std::wstring::npos) { dir = LogDir(); name = w; }
            else                         { dir = w.substr(0, p); name = w.substr(p + 1); }
        }
    }
    while (!dir.empty() && (dir[dir.size() - 1] == L'\\' || dir[dir.size() - 1] == L'/')) {
        dir.resize(dir.size() - 1);
    }
    if (dir.empty() || name.empty()) {
        ORB_LOG_WARN("TraceFile: cache 路径不合法（dir=\"%s\"），本次不写 cache", path);
        return false;
    }

    // **先删上一段的，再建本段的**：开新段就是"该删除时"，旧缓存留着没有意义。
    const unsigned pruned = PruneOld(dir, prefix, ext);
    if (pruned) {
        ORB_LOG_INFO("TraceFile: 新段开始，删掉上一段遗留的 %u 个缓存文件", pruned);
    }

    const std::wstring full = dir + L"\\" + name;

    Lock();
    // ⚠ **`w+b` 而不是 `wb`**：记录 cache 要被**读回来**（P2-2 的查询回放），
    // 只写流上 `fread` 一律返回 0 —— 那正是"回放一条也读不到"的成因。
    st.f = _wfopen(full.c_str(), L"w+b");
    if (!st.f) {
        Unlock();
        ORB_LOG_WARN("TraceFile: 建不出 cache 文件（%s）—— 本次不写 cache",
                     NarrowUtf8(full.c_str()).c_str());
        return false;
    }

    wcsncpy_s(st.pathW, MAX_PATH, full.c_str(), _TRUNCATE);
    const std::string u8 = NarrowUtf8(full.c_str());
    strncpy_s(st.pathU8, sizeof(st.pathU8), u8.c_str(), _TRUNCATE);

    st.bytes      = 0;
    st.raw        = 0;
    st.frames     = 0;
    st.failed     = false;
    st.capBytes   = static_cast<unsigned long long>(capMB) << 20;
    st.capWarned  = false;
    st.capped     = false;
    st.capTaken   = false;
    st.markCount  = 0;
    st.markWarned = false;

    // OFLOW 文件要写签名（orbuculum `Src/orbuculum.c:1175`）；记录 cache 没有签名。
    if (sig) {
        const size_t n = strlen(sig);
        if (fwrite(sig, 1, n, st.f) != n) {
            fclose(st.f);
            st.f = nullptr;
            Unlock();
            ORB_LOG_WARN("TraceFile: 签名写不进去（%s）—— 本次不写 cache", st.pathU8);
            return false;
        }
        st.bytes = n;
    }
    Unlock();

    ORB_LOG_INFO("TraceFile: 缓存已开 %s（%s，上限 %u MB%s）"
                 "—— 开新段会自动删掉上一段",
                 st.pathU8, sig ? sig : "记录 cache", capMB,
                 capMB ? "" : "（不限；disconnect / reset 时自动删除）");
    return true;
}

// 把索引还给系统。**只在文件已经关掉之后调用**：`Locate` / `TraceFileNextMark` /
// `TraceFileRecordsRange` 都以 `st.f != nullptr` 为前提，文件一关索引就不可查了，
// 留着只是常驻 64 KB ~ 1 MB。pthread/临界区已由调用方持有。
void DropMarks(FileState& st)
{
    free(st.mark);
    st.mark      = nullptr;
    st.markCap   = 0;
    st.markCount = 0;
    st.markWarned = false;
}

void CloseFile(FileState& st, const char* what, const char* why)
{
    Lock();
    if (!st.f) { Unlock(); return; }

    fflush(st.f);
    fclose(st.f);
    st.f = nullptr;

    const bool failed = st.failed;
    ORB_LOG_INFO("TraceFile: %s 已关闭(%s) %s —— %llu 字节%s，索引 %u 条%s"
                 "（disconnect / reset 时自动删除）",
                 what, why ? why : "?", st.pathU8, st.bytes,
                 (st.raw || st.frames) ? "" : "",
                 st.markCount,
                 st.markWarned ? "（索引到上限，之后的数据没有索引）" : "");
    if (st.raw || st.frames) {
        ORB_LOG_INFO("TraceFile: %s 明细 —— 原样 %llu 字节 + 编码 %llu 帧",
                     what, st.raw, st.frames);
    }
    if (failed) {
        ORB_LOG_WARN("TraceFile: %s 中途写失败过 —— 文件可能不完整", what);
    }
        // 日志里要的 `markCount` / `markWarned` 已经用完，索引可以丢了（disconnect
        // 之后文件马上被 PruneLeftovers 删掉，索引留着没有任何用途）。
    DropMarks(st);
    Unlock();
}

// 关掉并删除（返回是否真删了一个文件）。与 CloseFile 只差两处：① 关完 DeleteFileW；
// ② 状态彻底归零（含 pathW/pathU8），下次 OpenFile 是全新一份，不会带旧路径。
bool CloseAndDeleteFile(FileState& st, const char* what, const char* why)
{
    Lock();
    if (st.f) {
        fflush(st.f);
        fclose(st.f);
        st.f = nullptr;
    }

    bool removed = false;
    if (st.pathW[0]) {
        removed = (DeleteFileW(st.pathW) != 0);
        if (removed) {
            ORB_LOG_INFO("TraceFile: %s 已删除(%s) %s（%llu 字节）—— reset 语义："
                         "这一轮从头开始，上一轮的缓存作废",
                         what, why ? why : "?", st.pathU8, st.bytes);
        } else {
            ORB_LOG_WARN("TraceFile: %s 删不掉(%s) %s —— 缓存文件会留在盘上"
                         "（被别的进程打开？）",
                         what, why ? why : "?", st.pathU8);
        }
    }

    // 归零。`markCount` 就是记录 cache 的查询索引（P2-2），一并丢 —— 与
    // `TraceFileClearRecords()` 等价，但这里连文件都不在了，必须一起清。
    st.pathW[0]  = 0;
    st.pathU8[0] = 0;
    st.bytes     = 0;
    st.raw       = 0;
    st.frames    = 0;
    st.failed    = false;
    st.capWarned = false;
    st.capped    = false;
    st.capTaken  = false;
        // 索引数组一起还给系统：下一次 OpenFile 从 64 KB 重新长，不把上一段的
        // 1 MB 常驻在进程里（这段文件已经不在了，索引留着也没有意义）。
    DropMarks(st);
    Unlock();
    return removed;
}

void WriteBytes(FileState& st, const uint8_t* p, unsigned n, bool raw)
{
    if (!p || !n) return;
    Lock();
    if (st.f && !st.failed) {
        // 到大小上限就**停**（不是失败）：`failed` 会打"文件可能不完整"，
        // 而这里是有意停的，另有一条说明。
        if (st.capBytes && (st.bytes + n) > st.capBytes) {
            if (!st.capWarned) {
                st.capWarned = true;
                ORB_LOG_WARN("TraceFile: %s 到达大小上限（%llu MB）—— **停止追加**。"
                             "文件里是这段历史**最早**的那一段，最近的仍在内存窗口里"
                             "（要更长就调大上限，或分段采集）",
                             st.pathU8, (unsigned long long)(st.capBytes >> 20));
            }
            // ★ 到顶就不"可用"了：调用方（TraceWin）据 `RecordsActive()` 决定
            //   继续送块还是转走"丢历史"那条路（ERROR + Data Overflow）。
            //   只置 `capped` 不置 `failed`：这是有意停的，不是写坏。
            st.capped = true;
            Unlock();
            return;
        }
        if (fwrite(p, 1, n, st.f) != n) {
            st.failed = true;
            ORB_LOG_WARN("TraceFile: 写失败（%s），cache 停止", st.pathU8);
        } else {
            st.bytes += n;
            if (raw) st.raw += n;
        }
    }
    Unlock();
}

// 索引扩容：满时翻倍（64 KB → 1 MB）。false = 已到上限或分配失败。
// 只在 Mark() 里调用，调用方已持 Lock。
bool GrowMarks(FileState& st)
{
    if (st.markCap >= kMarkMaxTotal) return false;
    unsigned want = st.markCap ? st.markCap * 2u : kMarkInit;
    if (want > kMarkMaxTotal) want = kMarkMaxTotal;

    Mark* p = static_cast<Mark*>(realloc(st.mark, sizeof(Mark) * want));
    if (!p) return false;          // 内存不足：停在当前容量，退回"只 warn 一次"
    st.mark    = p;
    st.markCap = want;
    return true;
}

void Mark(FileState& st, uint64_t cycle)
{
    Lock();
    // 到顶后 `st.bytes` 不再增长，继续记索引只会让一批周期号指向同一个偏移
    // （回放定位全乱）。索引也一起停在顶部。
    if (st.f && !st.failed && !st.capped) {
            // 满了先扩容（64 KB → 1 MB），扩不动才算真满。
        if (st.markCount == st.markCap) GrowMarks(st);

        if (st.markCount < st.markCap) {
            st.mark[st.markCount].ofs   = st.bytes;
            st.mark[st.markCount].cycle = cycle;
            ++st.markCount;
        } else if (!st.markWarned) {
            st.markWarned = true;
            ORB_LOG_WARN("TraceFile: 索引已到上限（%u 条 / 已写 %llu MB）—— 文件仍然完整，"
                         "但再往后的区域没有索引可查（回放会变慢）",
                         st.markCap, st.bytes >> 20);
        }
    }
    Unlock();
}

// 按周期号定位：取"周期号 <= cycle 的最后一条索引"（索引按周期升序）。
bool Locate(const FileState& st, uint64_t cycle, uint64_t& ofs, uint64_t& cycleAt)
{
    if (!st.f || st.markCount == 0) return false;
    if (cycle < st.mark[0].cycle) return false;         // 比文件里最早的一条还早

    unsigned a = 0, b = st.markCount;
    while (a < b) {
        const unsigned mid = a + ((b - a) >> 1);
        if (st.mark[mid].cycle <= cycle) a = mid + 1; else b = mid;
    }
    const unsigned idx = (a > 0) ? (a - 1) : 0;
    ofs     = st.mark[idx].ofs;
    cycleAt = st.mark[idx].cycle;
    return true;
}

// ---------------------------------------------------------------------------
// OFLOW 帧编码（写侧）
// COBS 编码逐行对照 orbuculum Src/cobs.c:49：输入 front || msg || back，输出以 0x00 结尾。
// ---------------------------------------------------------------------------
unsigned CobsEncode(const uint8_t* front, unsigned lfront,
                    const uint8_t* back,  unsigned lback,
                    const uint8_t* msg,   unsigned lmsg,
                    uint8_t* out)
{
    uint8_t* wp = out;
    unsigned len = lfront + lmsg + lback;
    if (len == 0) return 0;

    uint8_t* cp = wp++;
    unsigned seglen = 1;

    for (unsigned i = 0; len > 0; ++i, --len) {
        const uint8_t* rp = (i < lfront)                ? &front[i]
                          : (i < lfront + lmsg)         ? &msg[i - lfront]
                          :                               &back[i - (lfront + lmsg)];

        if (*rp != 0) { *wp++ = *rp; ++seglen; }

        if (*rp == 0 || seglen == 0xFF) {
            *cp = static_cast<uint8_t>(seglen);
            seglen = 1;
            cp = wp;
            // 原代码里这个判断用的是**已经递减**的 len（`for (...; len--; ...)`），
            // 所以这里等价于"后面还有字节"。
            if (*rp == 0 || len > 1) ++wp;
        }
    }
    *cp = static_cast<uint8_t>(seglen);
    *wp++ = kOflowSyncChar;
    return static_cast<unsigned>(wp - out);
}

// 一个 OFLOW 帧：`tag(1) + payload(len) + sum(1)`，COBS 编码 + 0x00 定界。
// 逐行对照 orbuculum `Src/oflow.c:52` OFLOWEncode。
unsigned OflowEncodeFrame(uint8_t tag, const uint8_t* payload, unsigned len, uint8_t* out)
{
    uint8_t sum = tag;
    for (unsigned i = 0; i < len; ++i) sum = static_cast<uint8_t>(sum + payload[i]);
    const uint8_t back = static_cast<uint8_t>(0u - sum);   // = 256 - sum
    return CobsEncode(&tag, 1, &back, 1, payload, len, out);
}

} // namespace

// ===========================================================================
// 打开 / 关闭
// ===========================================================================

bool TraceFileOpen(const char* path, unsigned capMB)
{
    return OpenFile(s_ofl, path, capMB, L"ORB_trace", L"orb", kOflowSig);
}

bool TraceFileOpenRecords(const char* path, unsigned capMB)
{
    return OpenFile(s_rec, path, capMB, L"ORB_rec", L"trc", nullptr);
}

void TraceFileClose(const char* why)
{
    CloseFile(s_ofl, "OFLOW 文件", why);
    CloseFile(s_rec, "记录 cache", why);
}

unsigned TraceFileCloseAndDelete(const char* why)
{
    unsigned n = 0;
    if (CloseAndDeleteFile(s_ofl, "OFLOW 文件", why)) ++n;
    if (CloseAndDeleteFile(s_rec, "记录 cache", why)) ++n;
    if (n) {
        ORB_LOG_INFO("TraceFile: reset 清理 —— 删掉 %u 个缓存文件（%s）", n,
                     why ? why : "?");
    }
    return n;
}

bool TraceFileActive()
{
    Lock();
    const bool a = (s_ofl.f != nullptr) || (s_rec.f != nullptr);
    Unlock();
    return a;
}

bool TraceFileRecordsActive()
{
    Lock();
    // `failed` / `capped` 都要算"不成立"：写失败或到顶之后 `WriteBytes` 直接跳过，
    // 再往里送块等于把历史丢掉却不吭声 —— 调用方（TraceWin 的内存满分支）要靠
    // 这个返回值去置 `kTErrDataOvf`、记 `丢历史`。
    const bool a = (s_rec.f != nullptr) && !s_rec.failed && !s_rec.capped;
    Unlock();
    return a;
}

bool TraceFileTakeCapNotice()
{
    Lock();
    const bool take = (s_rec.f != nullptr) && s_rec.capped && !s_rec.capTaken;
    if (take) s_rec.capTaken = true;
    Unlock();
    return take;
}

const char* TraceFilePath()
{
    Lock();
    const char* p = s_ofl.f ? s_ofl.pathU8 : (s_rec.f ? s_rec.pathU8 : "");
    Unlock();
    return p;
}

// ===========================================================================
// 写：OFLOW 文件
// ===========================================================================

void TraceFileWriteRaw(const uint8_t* p, unsigned n)
{
    WriteBytes(s_ofl, p, n, /*raw*/ true);
}

void TraceFileWriteStream(uint8_t tag, const uint8_t* p, unsigned n)
{
    if (!p || !n) return;

    uint8_t enc[kOflowMaxEncoded];
    Lock();
    if (s_ofl.f && !s_ofl.failed) {
        while (n) {
            const unsigned chunk = (n > kOflowMaxPayload) ? kOflowMaxPayload : n;
            const unsigned e = OflowEncodeFrame(tag, p, chunk, enc);
            if (s_ofl.capBytes && (s_ofl.bytes + e) > s_ofl.capBytes) {
                if (!s_ofl.capWarned) {
                    s_ofl.capWarned = true;
                    ORB_LOG_WARN("TraceFile: %s 到达大小上限（%llu MB）—— **停止追加**"
                                 "（要更长就调大上限，或分段采集）",
                                 s_ofl.pathU8,
                                 (unsigned long long)(s_ofl.capBytes >> 20));
                }
                s_ofl.capped = true;
                break;
            }
            if (fwrite(enc, 1, e, s_ofl.f) != e) {
                s_ofl.failed = true;
                ORB_LOG_WARN("TraceFile: 写失败（%s），cache 停止", s_ofl.pathU8);
                break;
            }
            s_ofl.bytes += e;
            ++s_ofl.frames;
            p += chunk;
            n -= chunk;
        }
    }
    Unlock();
}

void TraceFileMark(uint64_t cycle)
{
    Mark(s_ofl, cycle);
}

void TraceFileLog(const char* where)
{
    Lock();
    const bool    on    = (s_ofl.f != nullptr);
    const char*   path  = on ? s_ofl.pathU8 : "";
    const unsigned long long bytes  = s_ofl.bytes;
    const unsigned long long raw    = s_ofl.raw;
    const unsigned long long frames = s_ofl.frames;
    const unsigned nMark = s_ofl.markCount;
    const uint64_t first = (nMark ? s_ofl.mark[0].cycle : 0);
    const uint64_t last  = (nMark ? s_ofl.mark[nMark - 1].cycle : 0);
    const uint64_t lastOfs = (nMark ? s_ofl.mark[nMark - 1].ofs : 0);
    Unlock();

    if (!on) {
        ORB_LOG_INFO("TraceFile(%s): 未写 cache（选项 trfile= 没开）", where ? where : "?");
        return;
    }
    ORB_LOG_INFO("TraceFile(%s): %s —— %llu 字节（原样 %llu + 编码 %llu 帧），"
                 "索引 %u 条 [周期 %llu..%llu, 偏移到 %llu]",
                 where ? where : "?", path, bytes, raw, frames,
                 nMark, first, last, lastOfs);
}

// ===========================================================================
// 记录 cache（P2-2）
// ===========================================================================

void TraceFileWriteRecords(const uint8_t* p, unsigned n)
{
    WriteBytes(s_rec, p, n, /*raw*/ false);
}

void TraceFileMarkRecords(uint64_t firstCycle)
{
    Mark(s_rec, firstCycle);
}

bool TraceFileLocateRecords(uint64_t cycle, uint64_t& ofs, uint64_t& cycleAtOfs)
{
    Lock();
    const bool ok = Locate(s_rec, cycle, ofs, cycleAtOfs);
    Unlock();
    return ok;
}

unsigned TraceFileReadRecords(uint64_t ofs, uint8_t* dst, unsigned max)
{
    if (!dst || !max) return 0;

    Lock();
    FILE* f = s_rec.f;
    if (!f) { Unlock(); return 0; }

    // 读文件不破坏写位置（写侧用的是同一个 FILE*，位置由写累积）。读之前显式定位，
    // 读完**把写位置恢复回去**，否则下一次追加会覆盖刚读过的数据。
    const long long wpos = _ftelli64(f);
    if (_fseeki64(f, static_cast<long long>(ofs), SEEK_SET) != 0) {
        Unlock();
        return 0;
    }
    const size_t n = fread(dst, 1, max, f);
    if (wpos >= 0) _fseeki64(f, wpos, SEEK_SET);
    Unlock();
    return static_cast<unsigned>(n);
}

bool TraceFileNextMark(uint64_t afterOfs, uint64_t& ofs, uint64_t& cycleAtOfs)
{
    Lock();
    bool ok = false;
    for (unsigned i = 0; i < s_rec.markCount; ++i) {
        if (s_rec.mark[i].ofs > afterOfs) {
            ofs     = s_rec.mark[i].ofs;
            cycleAtOfs = s_rec.mark[i].cycle;
            ok = true;
            break;
        }
    }
    Unlock();
    return ok;
}

bool TraceFileRecordsRange(uint64_t& firstCycle, uint64_t& lastCycle)
{
    Lock();
    const bool ok = (s_rec.f != nullptr) && (s_rec.markCount > 0);
    if (ok) {
        firstCycle = s_rec.mark[0].cycle;
        lastCycle  = s_rec.mark[s_rec.markCount - 1].cycle;
    }
    Unlock();
    return ok;
}

void TraceFileClearRecords()
{
    Lock();
    const unsigned had = s_rec.markCount;
    s_rec.markCount  = 0;
    s_rec.markWarned = false;
    Unlock();

    if (had) {
        // 文件是 append-only 的，**旧字节不回收**（也没法回收：文件正在被追加）。
        // 丢掉的只是"可达性"：索引一清，那些周期号就再也定位不到了。
        // 下一次追加从当前文件位置继续记索引，所以新旧两段不会串起来。
        ORB_LOG_INFO("TraceFile: 记录 cache 的可查历史已清空（丢下 %u 条索引；"
                     "文件字节不回收，下次追加从当前位置继续）", had);
    }
}

void TraceFileLogRecords(const char* where)
{
    Lock();
    const bool    on   = (s_rec.f != nullptr);
    const char*   path = on ? s_rec.pathU8 : "";
    const unsigned long long bytes = s_rec.bytes;
    const unsigned nMark = s_rec.markCount;
    const uint64_t first = (nMark ? s_rec.mark[0].cycle : 0);
    const uint64_t last  = (nMark ? s_rec.mark[nMark - 1].cycle : 0);
    Unlock();

    if (!on) {
        ORB_LOG_INFO("TraceFile(%s): 无记录 cache（选项 trrec= 没开）", where ? where : "?");
        return;
    }
    ORB_LOG_INFO("TraceFile(%s): 记录 cache %s —— %llu 字节，索引 %u 条 [周期 %llu..%llu]",
                 where ? where : "?", path, bytes, nMark, first, last);
}

unsigned TraceFilePruneLeftovers(const char* why)
{
    // 删的是**日志目录**里的残留（和 ORB_AGDI.log 同一处，默认 %TEMP%）。
    // 会话结束时索引已经清空、文件不可查，留着只是占盘 —— 所以 disconnect
    // 之后与下一次 Connect 之前都无条件清一遍，不依赖"下次还开 cache"。
    const std::wstring dir = LogDir();
    unsigned n = PruneOld(dir, L"ORB_rec",   L"trc");
    n += PruneOld(dir, L"ORB_trace", L"orb");
    if (n) {
        ORB_LOG_INFO("TraceFile: 清理残留 —— 删掉 %u 个上一次会话的 cache 文件（%s）",
                     n, why ? why : "?");
    }
    return n;
}

} // namespace agdi
} // namespace orb
