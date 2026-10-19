#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_movwr.py - 找 "mov [reg+disp], r32" (opcode 89 /r) 写入点。
_re_afc.py 只看立即数写入(mov [r+d],imm)，看不到寄存器写入，
本工具补这一块，用来定位 UV4 把 TDI 表拷进对象的写入点。

用法: python -X utf8 _re_movwr.py <exe> <disp_hex> [disp_hex ...]
"""
import struct
import sys

import pe_re


def scan(d, disp):
    pats = []
    for reg in range(8):
        for rm in range(8):
            pats.append(bytes([0x89, 0x80 | (reg << 3) | rm]) + struct.pack('<I', disp))
            if 0 <= disp <= 0x7F:
                pats.append(bytes([0x89, 0x40 | (reg << 3) | rm, disp]))
    out = set()
    for pat in pats:
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            out.add(k)
    return sorted(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    pe = pe_re.PE(argv[1])
    d = pe.d
    for a in argv[2:]:
        disp = int(a, 0)
        hs = scan(d, disp)
        print('disp=0x%X writes(candidate)=%d' % (disp, len(hs)))
        for k in hs[:80]:
            r = pe.o2r(k)
            if r is None:
                continue
            print('    @0x%08X   mov [reg+0x%X], reg32' % (pe.imagebase + r, disp))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
