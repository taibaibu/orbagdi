#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_refs.py — 列出某个 VA 在模块里的所有引用点：绝对字面量引用 + rel32 分支目标。

重要（旧版陷阱）：旧版只扫"绝对地址字面量"（4 字节 == 目标 VA）。但 `call rel32` /
`jmp rel32` 的机器码里**只有相对位移、不含目标绝对地址**，所以**直接 call 与尾调用
jmp 都扫不到** → 显示"0 引用"是**假阴性**，绝不能据此判死代码。
（例: UpdateClockSWO 的调用者全是 `jmp 0x1008C2D0`，旧版报 0；本版已复现
在 ULP2CM3.dll 得 2 条 jmp @0x1008C395 / 0x1008C4CA。）

方法论（函数边界判定三管齐下）: 必须同时用 ①序言扫描(55 8B EC 等)
②int3/CC 填充 ③`jmp`/`call` 目标；叶函数不以 55 8B EC 开头、纯尾调用函数
只会被 `jmp` 命中，因此本工具 0 引用**不能**当作"该函数是死代码"的依据。

本版两通道:
  abs : 绝对字面量引用（push/mov imm/函数指针表项等），保留原行为
  rel : capstone 反汇编可执行节，收集 call/jmp/jcc 的**相对目标 == 该 VA** 的点

用法: python -X utf8 _re_refs.py <dll> <va_hex> [-n N] [--no-rel]
"""
import struct
import sys

import capstone
import pe_re


def _secname(name):
    return name.decode('latin1') if isinstance(name, (bytes, bytearray)) else str(name)


def _is_exec(name):
    n = _secname(name).lower()
    return n.startswith('.text') or n.startswith('.itext') or 'code' in n or n.startswith('page')


def find_refs(pe, addr, do_rel=True):
    d = pe.d
    out = []
    # 通道 1: 绝对字面量
    pat = struct.pack('<I', addr)
    j = 0
    while True:
        k = d.find(pat, j)
        if k < 0:
            break
        j = k + 1
        r = pe.o2r(k)
        if r is None:
            continue
        out.append((pe.imagebase + r, 'abs',
                    d[max(0, k - 3):k].hex(), d[k + 4:k + 10].hex()))
    # 通道 2: rel32 分支目标
    if do_rel:
        secs = [s for s in pe.secs if _is_exec(s[0])]
        if not secs:
            print('  WARN: 未找到可执行节(.text/.itext/CODE/PAGE)，跳过 rel32 通道')
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        md.skipdata = True   # 关键: .text 里有数据/填充，否则线性反汇编在首个坏字节即停(假阴性)
        for name, va, vsize, raw, rsize in secs:
            for ins in md.disasm(d[raw:raw + rsize], pe.imagebase + va):
                if not (ins.mnemonic == 'call' or ins.mnemonic.startswith('j')):
                    continue
                for op in ins.operands:
                    if op.type == capstone.CS_OP_IMM and (op.imm & 0xFFFFFFFF) == addr:
                        out.append((ins.address, ins.mnemonic, '', ''))
                        break
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    addr = int(argv[2], 0)
    n = 40
    do_rel = '--no-rel' not in argv
    if '-n' in argv:
        n = int(argv[argv.index('-n') + 1], 0)
    pe = pe_re.PE(path)
    refs = find_refs(pe, addr, do_rel)
    refs.sort(key=lambda x: (x[0], x[1]))
    print('refs to 0x%08X in %s  (abs+rel, rel=%s)' % (addr, path, do_rel))
    for va, kind, prev, nxt in refs[:n]:
        if kind == 'abs':
            print('  0x%08X  abs        prev=%s next=%s' % (va, prev, nxt))
        else:
            print('  0x%08X  %-10s (rel32: call/jmp/jcc)' % (va, kind))
    print('total %d' % len(refs))
    if not refs:
        print('  NOTE: 0 refs 不是"死代码"证据；rel32 通道需可执行节解析。')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
