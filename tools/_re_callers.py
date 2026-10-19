#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_callers.py — 在 PE 里找所有 call 到指定 VA 的位置（扫描 E8 rel32）。

用法: python _re_callers.py <exe> <target_va_hex> [lo_hex] [hi_hex]
"""
import struct
import sys
import pe_re


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    target = int(argv[2], 0)
    lo = int(argv[3], 0) if len(argv) > 3 else 0
    hi = int(argv[4], 0) if len(argv) > 4 else 0xFFFFFFFF
    pe = pe_re.PE(path)
    d = pe.d
    print('callers of 0x%08X in %s' % (target, path))
    n = 0
    for i in range(len(d) - 5):
        if d[i] != 0xE8:
            continue
        rel = struct.unpack_from('<i', d, i + 1)[0]
        r = pe.o2r(i)
        if r is None:
            continue
        va = pe.imagebase + r
        if va + 5 + rel == target and lo <= va < hi:
            n += 1
            print('  0x%08X' % va)
    print('total %d' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
