#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_timer.py - 枚举 SetTimer/KillTimer 调用点并解码其栈参数（找"0.6s"节拍来源）。

复用 pe_re.PE；反汇编 call 点前一段窗口，按「执行序（地址升序）」收集全部 push
指令（含 68/6A 立即数与 push [mem]），再按 __stdcall 右->左解释:

    SetTimer(hWnd, nIDEvent, uElapse, lpTimerFunc)
    压栈序 = lpTimerFunc, uElapse, nIDEvent, hWnd
    => uElapse = 执行序第 2 个 push（args[1]），**不是**最靠近 call 的那个非零 imm。

每个调用点同时打印两种口径（push#2 与旧口径"最近非零 imm"）与完整 push 序列，
避免把 nIDEvent 误当 uElapse（例: 0x5F7437 真实 uElapse=0x1F4，旧口径误报 0x1267）。

用法: python -X utf8 _re_timer.py <exe> [SetTimer|KillTimer]
"""
import struct
import sys

import pe_re
import _re_iat
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM

X86 = (CS_ARCH_X86, CS_MODE_32)


def _pushes_before(pe, md, call_va, window=64):
    """call 点前 window 字节内、按地址升序的 push 列表 [(va, text, imm_or_None)]；
    只取最后 4 条作为参数（跳过无关的更早 push）。"""
    start_va = call_va - window
    o = pe.va2o(start_va)
    if o is None:
        start_va = call_va
        o = pe.va2o(start_va)
    if o is None:
        return []
    end = pe.va2o(call_va)
    if end is None:
        end = o + window
    pushes = []
    for ins in md.disasm(pe.d[o:end], start_va):
        if ins.address >= call_va:
            break
        if ins.mnemonic == 'push':
            imm = None
            if ins.operands and ins.operands[0].type == CS_OP_IMM:
                imm = ins.operands[0].imm & 0xFFFFFFFF
            pushes.append((ins.address, ins.mnemonic + ' ' + ins.op_str, imm))
    return pushes[-4:]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    path = argv[1]
    want = (argv[2] if len(argv) > 2 else 'SetTimer').lower()
    pe = pe_re.PE(path)
    md = Cs(*X86)
    md.detail = True
    ents = _re_iat.imports_full(pe)
    d = pe.d
    targets = [e for e in ents if (e[1] or '').lower() == want]
    if not targets:
        targets = [e for e in ents if want in (e[1] or '').lower()]
    for dll, nm, ordv, iat in targets:
        print('== %s!%s IAT=0x%08X ==' % (dll, nm or ('#%d' % ordv), iat))
        pat = b'\xFF\x15' + struct.pack('<I', iat)
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            r = pe.o2r(k)
            if r is None:
                continue
            va = pe.imagebase + r
            args = _pushes_before(pe, md, va)
            seq = ' | '.join('%s%s' % (t, ('(0x%X)' % i) if i is not None else '')
                             for _a, t, i in args)
            e_arg = args[1][2] if len(args) >= 2 else None          # SetTimer: args[1]=uElapse
            e_near = next((i for _a, _t, i in reversed(args) if i), None)  # 旧口径
            el = ('%d (0x%X)' % (e_arg, e_arg)) if e_arg is not None else '?'
            alt = ('%d (0x%X)' % (e_near, e_near)) if e_near is not None else '?'
            print('  call @0x%08X  uElapse[push#2]=%s  alt[near non-zero imm]=%s'
                  % (va, el, alt))
            print('      push seq(asc): %s' % seq)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
