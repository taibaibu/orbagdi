/**
 * @file AGDIStub.cpp
 * @brief 预览构建里属于会话层、但不该被链接的 AGDI 符号的替身。
 *
 * test\build.ps1 只编设置页真正依赖的源（Settings / Config / Log / Rddi / Power），
 * 因为预览程序没有探针、没有会话。但 `src\` 会继续长：例如 Settings.cpp 里
 * "取消 Trace Enable 就当场收源"要调 `orb::agdi::StopTraceSources()`，它的实现在
 * `src\app\ORBAGDI_Agdi.cpp`，那条链会拉进 DAP / USB / 状态机。
 *
 * 规则：**src 新增"设置页会调到、但实现属于会话层"的调用时，把同签名的替身加在
 * 这里**，不要为此把会话层的 .cpp 塞进 build.ps1 的 $Sources。
 */
#include "ORBAGDI_Agdi.h"

namespace orb {
namespace agdi {

// 预览里没有会话，也就没有源在跑 —— 与真实实现里 `!c.connected` 的分支同义。
bool StopTraceSources(const char* /*why*/) { return false; }

} // namespace agdi
} // namespace orb
