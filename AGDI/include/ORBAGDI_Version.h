/**
 * @file ORBAGDI_Version.h
 * @brief 版本号唯一出处，C++ 与资源编译器共用。
 * 只能写整数宏：RC 预处理器解析不了 C++ 和 windows.h。
 */
#pragma once

#define ORBAGDI_VERSION_MAJOR 0
#define ORBAGDI_VERSION_MINOR 2
#define ORBAGDI_VERSION_PATCH 0
#define ORBAGDI_VERSION_STRING "0.2.0"

// VERSIONINFO wants the numbers comma separated, without the macro name.
#define ORBAGDI_VERSION_COMMA 0,2,0,0
