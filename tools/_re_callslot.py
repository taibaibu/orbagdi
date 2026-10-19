#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_callslot.py - 扫描 "call dword ptr [reg+disp]" 间接调用点 (FF /2, mod=01|10)。

用途: 定位 C++ 虚调用 `obj->vfn()`，即 `call [reg+vtable_slot]`。
      与 _re_slotcall.py 互补: 后者抓 "mov reg,[obj+disp]; call reg"（函数指针成员），
      本工具抓 "call [reg+disp]"（虚表槽）。

用法:
    python -X utf8 _re_callslot.py <exe> <slot_hex> [slot_hex ...] [-b N]
      -b N  额外打印调用点前 N 字节的原始机器码(默认 12)，用于回看对象来源，
            例如 "8b 46 08" = mov eax,[esi+8]。

注意: disp8 只能编码 0..127，对 >0x7F 的槽位只匹配 disp32，避免截断假阳性。
"""
import struct
import sys

import pe_re


def scan(pe, slot):
    d = pe.d
    out = []
    for i in range(len(d) - 6):
        if d[i] != 0xFF:
            continue
        m = d[i + 1]
        if (m >> 3) & 7 != 2:          # reg 字段必须是 2 -> call r/m32
            continue
        mod = m >> 6
        rm = m & 7
        if rm == 4:                    # SIB
            continue
        if mod == 1:
            if not (0 <= slot <= 0x7F):
                continue
            if d[i + 2] != slot:
                continue
            out.append(i)
        elif mod == 2:
            if struct.unpack('<I', d[i + 2:i + 6])[0] != slot:
                continue
            out.append(i)
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    back = 12
    args = argv[2:]
    if '-b' in args:
        j = args.index('-b')
        back = int(args[j + 1], 0)
        del args[j:j + 2]
    pe = pe_re.PE(argv[1])
    d = pe.d
    for a in args:
        slot = int(a, 0)
        hits = scan(pe, slot)
        print('call dword ptr [reg+0x%X]   n=%d' % (slot, len(hits)))
        for k in hits[:60]:
            r = pe.o2r(k)
            if r is None:
                continue
            va = pe.imagebase + r
            ctx = d[max(0, k - back):k]
            print('    va 0x%08X   back[%d]=%s' % (va, len(ctx), ctx.hex()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
