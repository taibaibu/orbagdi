#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_tdi.py — 反汇编 ULP2CM3.dll 里 TDI（Trace Data Interface）通知的调用点，
用来回答"可用的 AGDI 驱动是怎么驱动 Trace Data 窗口的"。

用法: python _re_tdi.py <dll> <file_offset> [before] [len]
"""
import sys
import struct
import pe_re
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def show(path, off, before=0x60, length=0x90):
    pe = pe_re.PE(path)
    # 参数可能是文件偏移，也可能是 VA（>= imagebase 就按 VA 认）。
    if off >= pe.imagebase:
        va = off
    else:
        rva = pe.o2r(off)
        if rva is None:
            print('file offset 0x%X 不在任何节内' % off)
            return
        va = pe.imagebase + rva
    print('=' * 74)
    print('file 0x%X -> va 0x%08X' % (off, va))
    print('=' * 74)
    start = va - before
    o = pe.va2o(start)
    if o is None:
        print('va 0x%08X 不在任何节内' % start)
        return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for ins in md.disasm(pe.d[o:o + length], start):
        note = ''
        for op in ins.operands:
            if op.type == 2:                       # CS_OP_IMM
                s = pe.cstr(op.imm & 0xFFFFFFFF)
                if s:
                    note = '   ; "%s"' % s
                    break
        mark = '  <<<' if ins.address == va else ''
        print('0x%08X  %-26s %s%s%s'
              % (ins.address, ins.bytes.hex(),
                 ins.mnemonic + ' ' + ins.op_str, note, mark))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    off = int(argv[2], 0)
    before = int(argv[3], 0) if len(argv) > 3 else 0x60
    length = int(argv[4], 0) if len(argv) > 4 else 0x90
    show(path, off, before, length)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
