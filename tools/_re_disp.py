#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_disp.py - 扫描对 `call/jmp dword ptr [reg+disp32]` (FF /2, /4 mod=10) 的引用，
              用于枚举某对象成员函数指针(如 TDI 槽 +0x11a/+0x14a)的所有调用点。

复用 pe_re.PE。

用法: python -X utf8 _re_disp.py <exe> <disp_hex> [disp_hex2] ...
"""
import struct
import sys

import pe_re

CALL = {0x90 + r: 'call' for r in range(8)}
JMP = {0xA0 + r: 'jmp' for r in range(8)}


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    pe = pe_re.PE(path)
    d = pe.d
    for a in argv[2:]:
        disp = int(a, 0)
        print('disp=0x%X' % disp)
        n = 0
        # mod=10 (disp32): ModRM = 0x80 | (reg<<3) | rm
        forms = [(0x80, struct.pack('<I', disp))]
        if 0 <= disp <= 0x7F:
            # disp8 只能表示 0..127；对 >0x7F 的偏移用 disp8 会静默截断成假阳性
            forms.append((0x40, bytes([disp])))
        for mod, tail in forms:
            for reg, kind in ((2, 'call'), (4, 'jmp')):
                for rm in range(8):
                    pat = bytes([0xFF, mod | (reg << 3) | rm]) + tail
                    j = 0
                    while True:
                        k = d.find(pat, j)
                        if k < 0:
                            break
                        j = k + 1
                        r = pe.o2r(k)
                        if r is not None:
                            n += 1
                            print('   %s [reg+0x%X] (%s) @0x%08X'
                                  % (kind, disp, 'disp32' if mod == 0x80 else 'disp8',
                                     pe.imagebase + r))
        if n == 0:
            print('   (none)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
