/*
 * probe_bom_layout.cpp — 打印本驱动 BOM.H 里 struct bom 的关键字段偏移。
 *
 * 目的：UV4.exe 的 Trace Data 窗口取数门读的是 `app + 0xafc` 的 bit3，也就是
 * BOM.H 里那组标志位的第 4 位 `bTrcDisp`。只有当我们写的字节和 uVision 读的
 * 字节是同一个，`bom->bTrcDisp = 1` 才起作用。这个探针把实际偏移打出来核对。
 *
 * bTrcDisp 本身是位域，取不到地址，所以量它后面那个普通字段 VIF_Inst：
 * 那组标志位正好是一个 DWORD，所以标志位 DWORD 的偏移 = VIF_Inst - 4。
 */
#include <windows.h>
#include "BOM.H"

#include <stdio.h>

#define MEMBER_OFS(type, member) \
    ((unsigned)(size_t)((const char*)&((const type*)0)->member - (const char*)0))

int main(void)
{
    printf("sizeof(struct bom)                = 0x%X\n", (unsigned)sizeof(struct bom));
    printf("Usr1Dwrd[0]                       = 0x%X\n", MEMBER_OFS(struct bom, Usr1Dwrd));
    printf("VIF_Inst                          = 0x%X\n", MEMBER_OFS(struct bom, VIF_Inst));
    printf("  -> flags DWORD (VIF_Inst - 4)   = 0x%X   <-- UV4 reads 0xafc\n",
           MEMBER_OFS(struct bom, VIF_Inst) - 4u);
    printf("ItmLogN                           = 0x%X\n", MEMBER_OFS(struct bom, ItmLogN));
    printf("ItmLogH                           = 0x%X\n", MEMBER_OFS(struct bom, ItmLogH));
    printf("MemAccX                           = 0x%X\n", MEMBER_OFS(struct bom, MemAccX));
    printf("accSizeX                          = 0x%X\n", MEMBER_OFS(struct bom, accSizeX));
    return 0;
}
