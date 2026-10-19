#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_fstart.py - 从给定 VA 向前回溯，找最近的函数序言(55 8B EC / 55 8B EC 81 EC ...)
                以及最近的 CC/90 对齐填充；用于确定包含某 VA 的函数起始。

复用 pe_re.PE。

用法: python -X utf8 _re_fstart.py <exe> <va_hex> [maxback]
"""
import sys

import pe_re

PROLOGUES = [b'\x55\x8B\xEC', b'\x55\x89\xE5']


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    va = int(argv[2], 0)
    maxback = int(argv[3], 0) if len(argv) > 3 else 0x6000
    pe = pe_re.PE(path)
    d = pe.d
    o = pe.va2o(va)
    if o is None:
        print('VA 不在任何节内')
        return 1
    lo = max(0, o - maxback)
    win = d[lo:o]
    # 最近的 CC CC 对齐填充
    pad = win.rfind(b'\xCC\xCC')
    # 最近的序言
    best = -1
    for p in PROLOGUES:
        k = win.rfind(p)
        if k > best:
            best = k
    print('VA 0x%08X (file 0x%X)' % (va, o))
    if pad >= 0:
        print('  最近 CC 填充   @0x%08X' % (pe.imagebase + pe.o2r(lo + pad)))
    if best >= 0:
        print('  最近函数序言   @0x%08X  (bytes %s)'
              % (pe.imagebase + pe.o2r(lo + best), win[best:best + 3].hex()))
    else:
        print('  回溯 %d 字节内未找到序言' % maxback)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
