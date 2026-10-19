#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_leabase.py - 找 "lea r32, [reg+disp32]" (opcode 8D /r, mod=10) 的基址点。
用来定位“把某个偏移当作子对象/数组基址”的代码（块拷贝前的 lea）。
用法: python -X utf8 _re_leabase.py <exe> <disp_hex> [disp_hex ...]
"""
import struct
import sys

import pe_re


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    pe = pe_re.PE(argv[1])
    d = pe.d
    for a in argv[2:]:
        t = int(a, 0)
        res = []
        for i in range(len(d) - 6):
            if d[i] != 0x8D:
                continue
            m = d[i + 1]
            if (m & 0xC0) != 0x80:
                continue
            if (m & 7) == 4:          # SIB，跳过
                continue
            if struct.unpack('<I', d[i + 2:i + 6])[0] == t:
                res.append(i)
        print('disp=0x%X  lea_base n=%d' % (t, len(res)))
        for k in res[:40]:
            r = pe.o2r(k)
            if r is not None:
                print('    va 0x%08X' % (pe.imagebase + r))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
