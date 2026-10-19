/*
 * MSVC 的桩头：orbuculum 的 `Inc/cobs.h` 里有一句 `#include <sys/time.h>`，
 * 那是 POSIX 头，Windows SDK 没有。
 *
 * `cobs.c` 本身一个符号都不用（只有 `oflow.c` 用了 POSIX 的 `clock_gettime`），
 * 而本目录的读器 `ofl_read.c` 只链 `cobs.c` —— 所以空桩就够。
 *
 * ⚠ 只对**测试工具**生效：`/I<test>\stub` 排在 `/I<orbuculum>\Inc` 之前，
 * 驱动的构建完全看不到这个目录。
 */
#ifndef ORBAGDI_TEST_STUB_SYS_TIME_H
#define ORBAGDI_TEST_STUB_SYS_TIME_H
#endif
