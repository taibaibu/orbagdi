#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_rtti.py — 还原 MSVC C++ RTTI：类名 → 类型描述符 → COL → vtable（PE32）。

复用 pe_re（class PE 的 va2o/o2r/imagebase/secs/.d），不重复 PE 解析。

原理（MSVC x86 布局）:
    TypeDescriptor:  +0 pVFTable, +4 spare, +8 "name"（.?AV<Class>@@）
    所以  descriptor_va = (".?AV...@@" 字符串 VA) - 8
    CompleteObjectLocator(COL): +0 sig(0/1), +4 offset, +8 cdOffset,
        +12 pTypeDescriptor(=descriptor_va), +16 pClassDescriptor
        → 在文件里找到 dword==descriptor_va 处 k，则 COL = k-12
    vtable: vtable[-1] = COL_va（绝对 dword）→ 找到 dword==col_va 处 j，vtable = j+4

用法:
    python -X utf8 _re_rtti.py <module>                 # 列出所有类 + vtable
    python -X utf8 _re_rtti.py <module> <类名子串>       # 只看匹配的类
    python -X utf8 _re_rtti.py <module> CTrace --limit 20
"""

import re
import struct
import sys

import pe_re


def scan(pe, pat=None, limit=0, show_vt=True):
    d = pe.d
    rx = re.compile(rb'\.\?AV[A-Za-z0-9_@?$<>]{1,80}@@')
    found = {}
    for m in rx.finditer(d):
        name = m.group(0)[4:-2].decode('latin1')      # 去掉 .?AV 与 @@
        if pat and pat not in name:
            continue
        o = pe.o2r(m.start())
        if o is None:
            continue
        found.setdefault(name, pe.imagebase + o)
    rows = []
    for name, str_va in sorted(found.items()):
        desc_va = str_va - 8
        col_va = _find_col(pe, desc_va)
        vt_va = _find_vtable(pe, col_va) if col_va else None
        rows.append((name, desc_va, col_va, vt_va))
        if limit and len(rows) >= limit:
            break
    return rows


def _find_col(pe, desc_va):
    d = pe.d
    pat = struct.pack('<I', desc_va)
    j = 0
    while True:
        k = d.find(pat, j)
        if k < 0:
            return None
        j = k + 1
        if k < 12:
            continue
        col_va = pe.imagebase + (pe.o2r(k - 12) or -1)
        if pe.o2r(k - 12) is None:
            continue
        sig = struct.unpack_from('<I', d, k - 12)[0]
        off = struct.unpack_from('<I', d, k - 8)[0]
        if sig in (0, 1) and off == 0:
            return col_va
    return None


def _find_vtable(pe, col_va):
    d = pe.d
    pat = struct.pack('<I', col_va)
    j = 0
    while True:
        k = d.find(pat, j)
        if k < 0:
            return None
        j = k + 1
        if pe.o2r(k + 4) is not None:
            return pe.imagebase + pe.o2r(k + 4)
    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    path = argv[1]
    pat = None
    limit = 0
    i = 2
    while i < len(argv):
        if argv[i] == '--limit':
            limit = int(argv[i + 1], 0); i += 2
        else:
            pat = argv[i]; i += 1
    pe = pe_re.PE(path)
    rows = scan(pe, pat, limit)
    print('%s  RTTI classes=%d  (filter=%s)' % (path, len(rows), pat))
    print('%-34s %-10s %-10s %-10s' % ('class', 'typeinfo', 'COL', 'vtable'))
    nvt = 0
    for name, desc, col, vt in rows:
        nvt += 1 if vt else 0
        print('%-34s 0x%08X %s %s'
              % (name, desc,
                 ('0x%08X' % col) if col else '   ?    ',
                 ('0x%08X' % vt) if vt else '   ?    '))
    print('vtable resolved: %d/%d' % (nvt, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
