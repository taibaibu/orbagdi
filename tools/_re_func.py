#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_func.py — 单函数/VA 区间「完整反汇编 + 调用图」，结果写入文件（PE32）。

复用 pe_re（class PE 的 va2o/cstr/exports/imagebase/secs + 语法同 _re_tdi 的
capstone 直用），不重复实现 PE 解析。仅在 pe_re 之外做增量分析。

用法:
    python -X utf8 _re_func.py <module> <start_va>
    python -X utf8 _re_func.py <module> <lo_va> <hi_va>
    python -X utf8 _re_func.py <module> <start_va> --follow 2
    python -X utf8 _re_func.py <module> <start_va> --out tools\\out\\UV4.exe.45D410.asm

可选参数:
    --out FILE   输出文件（默认 tools\\out\\<模块名>.<VA>.asm）
    --follow N   沿 E8 直接调用递归展开本地函数，深度 N（默认 0=只跟当前函数）
    --max N      单函数最大反汇编字节（默认 0x2000）

标注:
    E8 rel32      -> ; -> 0xTARGETA   (直接调用边，若 TARGET 是导出会附名)
    FF 15 [slot]  -> ; call [0xSLOT]  (间接调用点，AGDI GetProcAddress 槽)
    FF 25 [slot]  -> ; jmp  [0xSLOT]
"""

import os
import struct
import sys

import pe_re
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_REG

X86 = (CS_ARCH_X86, CS_MODE_32)


def _is_pad(d, o):
    """函数尾：其后是 CC/90 填充 或 新序言 push ebp;mov ebp,esp。"""
    if d[o:o + 2] in (b'\xCC\xCC', b'\x90\x90'):
        return True
    if d[o:o + 3] == b'\x55\x8B\xEC':
        return True
    return False


def disas_func(pe, md, start_va, maxlen):
    """线性反汇编单函数并自动定界，返回 (insns, end_va)。"""
    o = pe.va2o(start_va)
    if o is None:
        return [], start_va
    insns = []
    for ins in md.disasm(pe.d[o:o + maxlen], start_va):
        insns.append(ins)
        m = ins.mnemonic
        no = pe.va2o(ins.address + ins.size)
        if m.startswith('ret') and (no is None or _is_pad(pe.d, no)):
            break
        if m == 'int3':
            break
        if m == 'jmp' and no is not None and _is_pad(pe.d, no):
            break
    end_va = insns[-1].address + insns[-1].size if insns else start_va
    return insns, end_va


def annotate(pe, ins):
    """返回 (note, edge)；edge = ('call'|'icall'|'ijmp', target_va)。"""
    note = ''
    for op in ins.operands:
        if op.type == CS_OP_IMM:
            s = pe.cstr(op.imm & 0xFFFFFFFF)
            if s:
                note = '   ; "%s"' % s
                break
    b = ins.bytes
    if ins.mnemonic == 'call' and b[:1] == b'\xe8' and len(b) == 5:
        t = (ins.address + 5 + struct.unpack('<i', b[1:5])[0]) & 0xFFFFFFFF
        return '   ; -> 0x%08X' % t + note, ('call', t)
    if ins.mnemonic == 'call' and b[:2] == b'\xff\x15' and len(b) == 6:
        slot = struct.unpack('<I', b[2:6])[0]
        return '   ; call [0x%08X]' % slot + note, ('icall', slot)
    if ins.mnemonic == 'jmp' and b[:2] == b'\xff\x25' and len(b) == 6:
        slot = struct.unpack('<I', b[2:6])[0]
        return '   ; jmp  [0x%08X]' % slot + note, ('ijmp', slot)
    return note, None


def emit_block(pe, md, va, maxlen, fh, exp_by_va, edges, seen):
    if va in seen:
        return
    seen.add(va)
    insns, end = disas_func(pe, md, va, maxlen)
    nm = exp_by_va.get(va)
    fh.write('\n; ===== func 0x%08X%s  (%d insn, 0x%08X-0x%08X) =====\n'
             % (va, (' [' + nm + ']') if nm else '', len(insns), va, end))
    if not insns:
        fh.write('; <无法反汇编：VA 不在任何节内>\n')
        return end
    for ins in insns:
        note, edge = annotate(pe, ins)
        if edge:
            if edge[0] == 'call' and edge[1] in exp_by_va:
                note = note.replace('; ->', '; -> [' + exp_by_va[edge[1]] + ']', 1)
            edges.append((ins.address, edge[0], edge[1]))
        fh.write('0x%08X  %-24s %s%s\n'
                 % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str, note))
    return end


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    rest = argv[2:]
    follow, maxlen, out = 0, 0x2000, None
    pos = []
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == '--follow':
            follow = int(rest[i + 1], 0); i += 2
        elif a == '--max':
            maxlen = int(rest[i + 1], 0); i += 2
        elif a == '--out':
            out = rest[i + 1]; i += 2
        else:
            pos.append(a); i += 1
    if not pos:
        print('需要 <VA> 或 <lo> <hi>')
        return 1
    lo = int(pos[0], 0)
    hi = int(pos[1], 0) if len(pos) > 1 else None

    pe = pe_re.PE(path)
    md = Cs(*X86)
    md.detail = True
    exp_by_va = {pe.imagebase + rva: n for n, _o, rva in pe.exports()}

    if out is None:
        outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')
        modname = os.path.basename(path)
        tag = '%08X' % (hi if hi else lo)
        out = os.path.join(outdir, '%s.%s.asm' % (modname, tag))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    seen, edges = set(), []
    with open(out, 'w', encoding='utf-8') as fh:
        fh.write('; module   = %s\n' % path)
        fh.write('; imagebase= 0x%08X\n' % pe.imagebase)
        if hi:
            fh.write('; range    = 0x%08X - 0x%08X (线性)\n' % (lo, hi))
        else:
            fh.write('; start    = 0x%08X  (auto boundary, follow=%d)\n' % (lo, follow))

        if hi:
            exp_by_va.get(lo)
            insns = []
            o = pe.va2o(lo)
            if o is not None:
                fh.write('\n; ===== range 0x%08X-0x%08X =====\n' % (lo, hi))
                for ins in md.disasm(pe.d[o:o + (hi - lo)], lo):
                    note, edge = annotate(pe, ins)
                    if edge:
                        edges.append((ins.address, edge[0], edge[1]))
                    fh.write('0x%08X  %-24s %s%s\n'
                             % (ins.address, ins.bytes.hex(),
                                ins.mnemonic + ' ' + ins.op_str, note))
                    insns.append(ins)
        else:
            emit_block(pe, md, lo, maxlen, fh, exp_by_va, edges, seen)
            if follow > 0:
                frontier = [e[1] for e in edges if e[1] == 'call']
                for _ in range(follow):
                    nxt = [t for _a, k, t in edges
                           if k == 'call' and t not in seen]
                    if not nxt:
                        break
                    for t in nxt:
                        if pe.va2o(t) is not None:
                            emit_block(pe, md, t, maxlen, fh, exp_by_va, edges, seen)

        fh.write('\n; ===== call graph (edges=%d) =====\n' % len(edges))
        calls = sorted(set(t for _a, k, t in edges if k == 'call'))
        slots = sorted(set(t for _a, k, t in edges if k in ('icall', 'ijmp')))
        for t in calls:
            fh.write(';  call -> 0x%08X%s\n'
                     % (t, (' [' + exp_by_va[t] + ']') if t in exp_by_va else ''))
        for s in slots:
            fh.write(';  indirect [0x%08X]\n' % s)

    print('wrote %s  (funcs=%d, edges=%d, direct=%d, indirect=%d)'
          % (out, len(seen), len(edges), len(calls), len(slots)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
