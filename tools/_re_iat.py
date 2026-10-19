#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_iat.py - 解析 PE 导入表，定位某个导入函数（如 USER32!SetTimer）的 IAT 槽 VA，
            并扫描 .text 里 `call dword ptr [IAT]` (FF 15) / `jmp [IAT]` (FF 25) 的调用点。

复用 pe_re.PE（仅读取其 secs/ddoff/r2o/imagebase/d）。

用法:
    python -X utf8 _re_iat.py <exe> <dll子串> <函数子串>
    python -X utf8 _re_iat.py <exe> <dll子串>            # 列出该 dll 全部导入名+槽
"""
import struct
import sys

import pe_re


def imports_full(pe):
    d = pe.d
    imp_rva = struct.unpack_from('<I', d, pe.ddoff + 8)[0]
    if not imp_rva:
        return []
    io = pe.r2o(imp_rva)
    out = []
    while True:
        oft, ts, fc, name_rva, ft = struct.unpack_from('<IIIII', d, io)
        if oft == 0 and name_rva == 0 and ft == 0:
            break
        no = pe.r2o(name_rva)
        end = d.index(b'\0', no)
        dll = d[no:end].decode('latin1')
        thunk = oft if oft else ft
        to = pe.r2o(thunk)
        k = 0
        while True:
            t = struct.unpack_from('<I', d, to + k * 4)[0]
            if t == 0:
                break
            iat_va = pe.imagebase + ft + k * 4
            if t & 0x80000000:
                out.append((dll, None, t & 0xFFFF, iat_va))
            else:
                no2 = pe.r2o(t)
                e2 = d.index(b'\0', no2)
                nm = d[no2:e2].decode('latin1')
                out.append((dll, nm, None, iat_va))
            k += 1
        io += 20
    return out


def callers(pe, slot_va):
    d = pe.d
    hits = []
    for opc, kind in ((b'\xFF\x15', 'call'), (b'\xFF\x25', 'jmp')):
        pat = opc + struct.pack('<I', slot_va)
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            r = pe.o2r(k)
            if r is not None:
                hits.append((pe.imagebase + r, kind))
    return sorted(hits)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    dll_sub = argv[2].lower()
    fn_sub = argv[3].lower() if len(argv) > 3 else None
    pe = pe_re.PE(path)
    ents = imports_full(pe)
    sel = [e for e in ents if dll_sub in e[0].lower()]
    if fn_sub is not None:
        sel = [e for e in sel if (e[1] or '').lower().find(fn_sub) >= 0]
    print('imagebase=0x%08X  matched=%d' % (pe.imagebase, len(sel)))
    for dll, nm, ordv, iat in sel:
        label = nm if nm else ('#%d' % ordv)
        print('  %-16s %-30s IAT=0x%08X' % (dll, label, iat))
        for va, kind in callers(pe, iat):
            print('        %s [IAT] @0x%08X' % (kind, va))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
