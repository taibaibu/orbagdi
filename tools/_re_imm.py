#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_imm.py - 找某个立即数被"真正作为 imm 操作数"使用的指令（PE32）。

与盲扫字节不同：每个候选命中都用 capstone 在附近重新对齐反汇编，要求某条指令的
起点正好落在候选字节处、且其 imm 操作数 == 目标值，才计入；否则判为假阳性
（disp 位移 / ModRM 字段等），避免把 `mov [eax+0x258],edi` 的 disp 当成
`mov eax,0x258`（例: 0x5BF423 旧口径假阳性）。

复用 pe_re.PE。
用法: python -X utf8 _re_imm.py <exe> <imm_hex>
"""
import struct
import sys

import pe_re
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM

X86 = (CS_ARCH_X86, CS_MODE_32)
MOVR = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']


def _true_form(pe, md, off, imm):
    """off 处确实是指令起点且带 imm==目标时返回指令文本，否则 None。"""
    r = pe.o2r(off)
    if r is None:
        return None
    va = pe.imagebase + r
    for back in range(1, 16):
        sv = va - back
        o = pe.va2o(sv)
        if o is None:
            continue
        for ins in md.disasm(pe.d[o:o + back + 16], sv):
            if ins.address < va:
                continue
            if ins.address > va:
                break
            for op in ins.operands:
                if op.type == CS_OP_IMM and (op.imm & 0xFFFFFFFF) == imm:
                    return ins.mnemonic + ' ' + ins.op_str
            return None
    return None


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    imm = int(argv[2], 0)
    pe = pe_re.PE(path)
    md = Cs(*X86)
    md.detail = True
    d = pe.d
    tail = struct.pack('<I', imm)
    print('imm=0x%X (%d)  [仅计数 capstone 确认的真正立即数形式]' % (imm, imm))

    cands = [(b'\x68' + tail, 'push imm32'),
             (b'\x3D' + tail, 'cmp eax,imm32')]
    for op in range(0xB8, 0xC0):
        cands.append((bytes([op]) + tail, 'mov %s,imm32' % MOVR[op - 0xB8]))
    for rm in range(8):
        cands.append((bytes([0x81, 0xF8 | rm]) + tail, 'cmp r%d,imm32' % rm))
    for rm in range(8):
        cands.append((bytes([0xC7, 0xC0 | rm]) + tail, 'mov r%d,imm32' % rm))

    n = 0
    for pat, label in cands:
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            form = _true_form(pe, md, k, imm)
            if form is None:
                continue
            n += 1
            print('  @0x%08X  %-16s -> %s' % (pe.imagebase + pe.o2r(k), label, form))
    print('total %d (true imm forms)' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
