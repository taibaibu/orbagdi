/**
 * @file ORBAGDI_Ui.h
 * @brief 把关键信息推进 uVision 自己的输出窗口。
 * 通道同 Keil 的 txtout()：SendMessage(主窗口, Uv2Msg, MSG_UV2_CMDLINE, 文本)；
 * 下载时换 MSG_UV2_TRNLINE（这两个 wParam 值是 uVision 私有，不在 AGDI.H）。
 * 句柄还没交过来时退回落 AG_CB_MSGSTRING。只放用户必须看见的信息，细节进日志文件。
 */
#pragma once

#include "ORBAGDI.h"    // HWND, DWORD, pCBF (via AGDI.H)

namespace orb {
namespace ui {

/// AG_Init(AG_INITITEM) 交来句柄时调用；三者可能先后到达，变化时都会再调。
void Attach(HWND mainFrame, DWORD uvMsg, pCBF cb);

/// AG_UNINIT：窗口即将消失，停止输出。
void Detach();

/// 下载期间往 build 窗口打印（同 Keil），进出下载流程时切换。
void SetFlashMode(bool on);

/// uVision 已经给了可用的输出通道。
bool Ready();

// 各条报告同时写日志文件；级别仍然受 ORBAGDI_LOG_LEVEL 控制。
void Info (const char* fmt, ...);
void Warn (const char* fmt, ...);
void Error(const char* fmt, ...);

/// 会话无法继续：同 Error() 上报，在窗口线程上再弹一个消息框。
void Fatal(const char* fmt, ...);

} // namespace ui
} // namespace orb
