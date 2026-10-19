/**
 * @file ORBAGDI_Agdi.h
 * @brief Internal state shared between the AGDI export layer and DllMain.
 *
 * uVision loads exactly one instance of the target DLL per project, so a
 * single process-wide context is enough.
 */
#pragma once

#include "pch.h"

namespace orb {
namespace agdi {

struct Context {
    Dap         dap;
    Core        core;
    Breakpoints bps;

    // ---- uVision supplied handles (AG_INITITEM) ---------------------------
    pCBF        cb;         // AG_INITCALLBACK
    HWND        mainFrame;  // AG_INITPHANDLEP
    DWORD       uvMsg;      // AG_INITUSRMSG (a RegisterWindowMessage value)
    AG_BP**     bpHead;     // AG_INITBPHEAD (uVision hands over a pointer to the head)
    AG_BP*      bpStart;    // walking start - the reference's pBStart
    DWORD*      curPC;      // AG_INITCURPC

    bool        loaded;     // AG_Init(AG_INITFEATURES) seen
    bool        connected;  // target is open and attached

    volatile LONG running;  // 1 while AG_GoStep drives the target
    volatile LONG stopReq;  // set by AG_STOPRUN / AG_STOPREQ

    // set by AG_UNINIT, cleared by Connect()。让后台恢复立刻撒手 —— 它一轮抱着探针锁
    // 6 s，Stop Debugging 会因此退不出去。
    volatile LONG uninitReq;

    DWORD       tclk;       // debug clock hint (Hz), informational

    Context();
};

Context& Get();

// ---- 探针占用锁（官方 LinkCom(1) / CheckCom(1) 的等价物）----
// 调试线程在不可打断的区间里持有它；抽数 worker 只能非阻塞尝试，拿不到就让路。
// ComLock 非阻塞且可提前 Release()；ComLockWait 阻塞至多 timeoutMs。
struct ComLock {
    ComLock();
    ~ComLock();
    bool Held() const;
    bool TryLock();          // 再试一次（用于放弃后重试）
    void Release();          // 提前释放
private:
    bool m_held;
};

struct ComLockWait {
    explicit ComLockWait(unsigned timeoutMs);
    ~ComLockWait();
    bool Held() const;
private:
    bool m_held;
};

// Opens the RDDI session and attaches to the core (idempotent).
bool Connect();
void Disconnect();

// 运行期收掉 trace 源（设置页取消 Trace Enable 之后）。
// 三个源只看运行状态不看配置，不显式收尾它们会继续抢探针锁，把调试拖慢。
// 会话不在时什么都不做；返回 true 表示确实停掉了正在跑的源。
bool StopTraceSources(const char* why);

// Called from DllMain(DLL_PROCESS_DETACH).
void Shutdown();

// Resumes the target and blocks until it halts again, the stop request
// arrives, or timeoutMs elapses (0 = wait forever).
U32  RunAndWait(uint32_t tempBpAddr, uint32_t timeoutMs, uint32_t* stopAddr);

uint32_t ReadPC();

// uVision callback helper (no-op when uVision did not register one).
void Callback(U32 code, void* vp);
void StatusMessage(const char* text);

} // namespace agdi
} // namespace orb
