#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_immcall.py - 对每个 `push imm32`(68 <imm32>) / `mov r32,imm32` 引用点，向后扫最多 32 字节
                 找 `call dword ptr [IAT]`(FF 15) 并给出被调用的导入函数名。

用来回答"600 / 500 这些节拍常量被谁消费(SetTimer/Sleep/timeSetEvent/WaitFor)"。
复用 pe_re.PE + _re_iat.imports_full。

用法: python -X utf8 _re_immcall.py <exe> <imm_hex> [forward]
"""
import struct
import sys

import pe_re
import _re_iat


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    imm = int(argv[2], 0)
    fwd = int(argv[3], 0) if len(argv) > 3 else 32
    pe = pe_re.PE(path)
    d = pe.d
    by_iat = {}
    for dll, nm, ordv, iat in _re_iat.imports_full(pe):
        label = nm if nm else ('#%d' % ordv if ordv is not None else '?')
        by_iat[iat] = '%s!%s' % (dll, label)
    tail = struct.pack('<I', imm)

    sites = []
    for op in [0x68] + list(range(0xB8, 0xC0)) + [0x3D]:
        pat = bytes([op]) + tail
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            if pe.o2r(k) is not None:
                sites.append(k)
    print('imm=0x%X sites=%d' % (imm, len(sites)))
    for k in sorted(sites):
        va = pe.imagebase + pe.o2r(k)
        call = None
        for x in range(k + 5, min(len(d) - 6, k + 5 + fwd)):
            if d[x] == 0xFF and d[x + 1] == 0x15:
                iat = struct.unpack_from('<I', d, x + 2)[0]
                call = (pe.imagebase + pe.o2r(x), by_iat.get(iat, 'IAT 0x%08X' % iat))
                break
        print('  @0x%08X  -> %s' % (va, call if call else '(无 FF15 调用)'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
