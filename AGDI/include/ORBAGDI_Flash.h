#pragma once

// Flash 下载（FlashOS 路径）。
// uVision 把编程任务交给驱动 DLL，由 DLL 反过来向它拉取镜像：
//   AG_INITFLASHLOAD  -> 载入 .FLM 到目标 RAM 并跑 Init()
//   AG_STARTFLASHLOAD -> EraseAll / WriteAll / VerifyAll
//   AG_CB_GETFLASHPARAM -> {start,many,image}
// 器件描述里带 flash 序列时改走 uVision 的序列解释器，由 FlashInitLoad() 选择。

namespace orb {
namespace agdi {

// 从驱动选项串里取 FLM 路径与 RAM 窗口。
// 接受 -FL"<file>.FLM"、flm=<file>、ram=<addr>,<size>。
void FlashOptions(const char* targArgs);

const char* FlashFilePath();       // "" when no algorithm is configured
bool        FlashAlgorithmReady();

bool FlashInitLoad();              // AG_INITITEM | AG_INITFLASHLOAD
bool FlashStartLoad();             // AG_INITITEM | AG_STARTFLASHLOAD
void FlashUnInit();                // AG_UNINIT / target unhook

// AG_MemAcc(AG_F_ERASE)：还没下载过就先拉起算法，然后整片擦除。
bool FlashEraseChip();

// AG_MemAcc(AG_F_RUN)：复位并放行刚烧进去的程序。
bool FlashRunApp();

const char* FlashLastError();

} // namespace agdi
} // namespace orb
