#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_dref.py - 在整文件里查找指向某个 VA 的 32 位小端立即数/指针（vtable 槽、数据引用）。

用途：找 vtable 槽（如某析构函数地址出现在 .rdata 里），从而定位类的 vftable。
复用 pe_re.PE。

用法: python -X utf8 _re_dref.py <exe> <va_hex> [va2] [va3] ...
"""
import struct
import sys

import pe_re


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    pe = pe_re.PE(path)
    d = pe.d
    for a in argv[2:]:
        va = int(a, 0)
        pat = struct.pack('<I', va)
        hits = []
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            if pe.o2r(k) is not None:
                hits.append(pe.imagebase + pe.o2r(k))
        print('0x%08X  refs=%d' % (va, len(hits)))
        for h in hits:
            print('    @0x%08X' % h)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
