#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_settimer.py - 对每个 `call dword ptr [SetTimer IAT]` 站点，
                  粗糙解码其前 48 字节内的 push imm / mov reg,imm，列出最后 4 个。

cdecl: SetTimer(hWnd, nIDEvent, uElapse, lpfnTimer)
  执行序(地址升序): push lpfnTimer; push uElapse; push nIDEvent; push hWnd; call
  或 MFC CWnd::SetTimer(nID, uElapse, lpfn): push lpfn; push uElapse; push nID; call
  => 4 个 push 时 uElapse = 倒数第 3；3 个 push 时 uElapse = 倒数第 2。

用法: python -X utf8 _re_settimer.py <exe> <SetTimer_IAT_hex>
"""
import re
import struct
import sys

import pe_re


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    pe = pe_re.PE(argv[1])
    d = pe.d
    iat = int(argv[2], 0)
    pat = re.compile(b'\xff\x15' + struct.pack('<I', iat))
    n = 0
    for m in pat.finditer(d):
        k = m.start()
        win = d[max(0, k - 48):k]
        items = []
        for pm in re.finditer(rb'\x6a(.)', win, re.S):
            items.append((pm.end(), 'push8', pm.group(1)[0]))
        for pm in re.finditer(rb'\x68(....)', win, re.S):
            items.append((pm.end(), 'push32', struct.unpack('<I', pm.group(1))[0]))
        for pm in re.finditer(rb'\xb8(....)', win, re.S):
            items.append((pm.end(), 'eax', struct.unpack('<I', pm.group(1))[0]))
        items = [it for it in items if it[0] >= len(win) - 40]
        items.sort()
        r = pe.o2r(k)
        if r is None:
            continue
        n += 1
        print('@0x%08X : %s' % (pe.imagebase + r,
              ', '.join('%s=0x%X' % (t[1], t[2]) for t in items[-4:])))
    print('total %d' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
