#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_uvcodes.py — 对比"可用的 AGDI 驱动"与我们的驱动用了哪些 UV_* 通知码。

做法：从 BOM.H 里解析出全部 UV_XXX = 0xNNN，再在目标 DLL 的 .text 里找
`push imm32`（68 <imm32>）形式的引用。AGDI 驱动调 pio->Notify(code, ptr)
时，code 就是这么压栈的。

用法: python _re_uvcodes.py <BOM.H> <dll> [dll2 ...]
"""
import re
import struct
import sys


def parse_codes(bom_h):
    src = open(bom_h, 'r', encoding='utf-8', errors='ignore').read()
    out = {}
    for m in re.finditer(r'\b(UV_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)', src):
        out[int(m.group(2), 16)] = m.group(1)
    return out


def scan(path, codes):
    d = open(path, 'rb').read()
    used = {}
    for c in codes:
        pat = b'\x68' + struct.pack('<I', c)
        hits = []
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            hits.append(k)
        if hits:
            used[c] = hits
    return used


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    codes = parse_codes(argv[1])
    print('BOM.H 里的 UV_* 码：%d 个\n' % len(codes))

    for path in argv[2:]:
        used = scan(path, codes)
        print('=' * 74)
        print('%s  —— 引用到 %d 个 UV_* 码' % (path, len(used)))
        print('=' * 74)
        for c in sorted(used):
            print('  0x%03X  %-26s hits=%d  %s'
                  % (c, codes[c], len(used[c]),
                     ' '.join('0x%X' % h for h in used[c][:5])))
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
