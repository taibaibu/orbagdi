#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_setter.py — 找"先加载全局指针 G，随后写入它某个字节"的指令对。

用途：UV4.exe 的 Trace Data 取数门读 `[0xca2194] + 0xafc` 的 bit3。要找到是谁把
这一位设起来的，就扫描所有"写入字节"的指令，并检查其前面不远处是否刚加载过该全局量。

用法: python _re_setter.py <exe> <global_va_hex> [窗口字节数]
"""
import struct
import sys
import pe_re

# 写入字节的常见形式（modrm 的 mod=10 表示 [reg+disp32]）：
#   C6 8? <disp32> <imm8>   mov byte ptr [reg+disp32], imm8
#   88 8? <disp32>          mov byte ptr [reg+disp32], r8
#   80 8? <disp32> <imm8>   or  byte ptr [reg+disp32], imm8
#   08 8? <disp32>          or  byte ptr [reg+disp32], r8
WRITE_OPS = (0xC6, 0x88, 0x80, 0x08, 0x20, 0x28, 0x30, 0x38)


def main(argv):
    path = argv[1]
    gva = int(argv[2], 0)
    window = int(argv[3], 0) if len(argv) > 3 else 96
    pe = pe_re.PE(path)
    d = pe.d

    load = struct.pack('<I', gva)          # mov eax, [gva] = A1 <imm32>
    load_ecx = struct.pack('<I', gva)      # 8B 0D <imm32>

    print('global 0x%08X, window %d bytes' % (gva, window))
    n = 0
    for i in range(len(d) - 7):
        op = d[i]
        if op not in WRITE_OPS:
            continue
        modrm = d[i + 1]
        if (modrm & 0xC0) != 0x80:          # 只要 mod=10（[reg+disp32]），基址寄存器不限
            continue
        r = pe.o2r(i)
        if r is None:
            continue
        va = pe.imagebase + r
        disp = struct.unpack_from('<I', d, i + 2)[0]
        if disp > 0x4000:                   # 只看小位移（对象字段）
            continue

        # 往前找是否刚加载过该全局量
        back = d[max(0, i - window):i]
        near = (b'\xA1' + load in back) or (b'\x8B\x0D' + load_ecx in back)
        if not near:
            continue
        n += 1
        imm = d[i + 6] if op in (0xC6, 0x80) else None
        print('  0x%08X  op=%02X disp=0x%03X%s' %
              (va, op, disp, (' imm=%u' % imm) if imm is not None else ''))
    print('total %d' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
