#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_slotcall.py - 找 "mov r32,[reg+disp] ... call r32" 形式的间接槽调用
                  （这是 UV4 调用 TDI 缓存成员指针的真实形态，
                    而 _re_callslot.py 只能看 call [reg+disp]，看不到这种）

用法: python -X utf8 _re_slotcall.py <exe> <disp_hex> [disp_hex ...]

原理:
  1) 裸字节找  mov r32,[rm+disp] : 8B /r, mod=01(0x40) disp8 / mod=10(0x80) disp32
     （reg 字段=目标寄存器, rm=基址寄存器），随后用 capstone 做**边界校验**：
     k 必须是真指令起点、指令长度恰等于 `8B/…+disp` 编码长度、mem.disp == 目标。
  2) 从该 mov 之后做**寄存器 def-use 追踪**，直到：
       · 遇 `call reg` / `jmp reg`  → 记一对(命中)；
       · 遇重新写 reg（regs_write 含 reg）→ 停；
       · 遇 `ret`/`hlt`/`int3`/无条件 `jmp` → 停（基本块结束）；
       · reg 为 caller-saved(eax/ecx/edx) 且中途遇 `call` → 停（被被调方破坏）。
     允许中间任意 `push/mov/test/je` 等。

⚠ 四种**各自独立**的假阴性/误读来源（同族工具，交叉参照）：
  ① 通道错：`call [reg+disp]`(虚表风格) 要用 `_re_callslot.py`；本工具只管
     "先 load 到寄存器再 call reg"。两者互补、非 bug。
  ② 边界错：裸字节会命中落在别的指令中间的 `8B 80..` 序列 → 已用 capstone 边界校验剔除
     （看 raw/real 差值）。
  ③ **窗口错**（本条，历史 bug）：旧版只向前扫 48 字节，而编译器会在 load 与 call 之间
     插入**实参 push**，参数越多距离越远 ⇒ 系统性漏检（恰恰参数多的旧式 API 是重点靶）。
     实证 UV4 `mov ecx,[ecx+0x11a]@0x45EF69 → call ecx@0x45EFAA` 相距 **0x41=65 字节**
     （中间 11 条 mov 装栈 + test ecx,ecx/je + 4 条 push），48 窗口必漏。
     现改为 def-use 追踪，无窗口参数。
  ④ **判据覆盖边界**（近邻 vs 入口级，**误读风险，务必读**）：
     `[gate test reg,reg]` 只覆盖"调用点**近邻**"的门（同基本块、load 与 call 之间）；
     **没有该标注 ≠ 裸调用** —— 函数入口级 / 跨基本块的门本工具**不判**。
     实证（用同一份 0x11A 输出即可反证）：real=4 中仅 `0x45EF69` 带 [gate]，但
     `0x45F534/0x45F704/0x45F833` **同样有门** —— 门在函数入口
     `0x45F46C cmp dword [ecx+0x11a],0 / jne 0x45F483`（为空则 `mov eax,4; ret 0x18`
     失败返回）；`0x45F704` 距该门约 **0xE8 字节**，且门检查的 `ecx` 与调用点的 `edi`
     是**同一对象**（序言 `0x45F410 mov [ebp-4],ecx` → `0x45F6C2 mov edi,[ebp-4]`）。
     若见"无 [gate]"就判成裸调用，会重犯 `re-tracepath` 已定性清楚的那个错。
     ⇒ 要判入口级门：配 `_re_dispref.py` 找该槽的 `cmp [reg+disp],imm` 门，
       并确认基址是**同一对象**（经 `[ebp-x]` 中转时需追活跃区间）。
"""
import struct
import sys

import capstone
import pe_re

REGNAME = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REGID = {i: getattr(capstone.x86, 'X86_REG_' + n.upper()) for i, n in enumerate(REGNAME)}
CALLER_SAVED = {REGID[0], REGID[1], REGID[2]}          # eax/ecx/edx
MAX_SPAN = 0x100                                        # 仅作安全上限(def-use 通常会先停)


def _patterns(disp):
    """(pattern_bytes, target_reg, instr_len)。"""
    out = []
    for reg in range(8):
        for rm in range(8):
            out.append((bytes([0x8B, 0x80 | (reg << 3) | rm])
                        + struct.pack('<I', disp), reg, 6))
            if 0 <= disp <= 0x7F:
                out.append((bytes([0x8B, 0x40 | (reg << 3) | rm, disp]), reg, 3))
    return out


def _follow(d, pe, md, pos0, reg):
    """def-use: 从 pos0 起找 `call/jmp reg`，返回 (call_off, kind, gated) 或 None。"""
    reg_id = REGID[reg]
    pos = pos0
    end = min(pos0 + MAX_SPAN, len(d))
    saw_test = False
    while pos < end:
        rk = pe.o2r(pos)
        if rk is None:
            return None
        ins = next(md.disasm(d[pos:pos + 16], pe.imagebase + rk, count=1), None)
        if ins is None:
            return None
        if ins.mnemonic in ('call', 'jmp') and ins.operands:
            op = ins.operands[0]
            if op.type == capstone.x86.X86_OP_REG and op.reg == reg_id:
                return (pos, ins.mnemonic, saw_test)
        if ins.mnemonic.startswith('ret') or ins.mnemonic in ('hlt', 'ud2', 'int3'):
            return None
        if ins.mnemonic == 'jmp':          # 无条件 jmp(非本寄存器) 结束基本块
            return None
        try:
            written = ins.regs_access()[1]
        except Exception:
            written = []
        if reg_id in written:              # 寄存器被重写 ⇒ 前一次 load 作废
            return None
        if ins.mnemonic == 'call' and reg_id in CALLER_SAVED:
            return None                    # 被中间调用破坏
        if (ins.mnemonic == 'test' and len(ins.operands) == 2
                and all(o.type == capstone.x86.X86_OP_REG and o.reg == reg_id
                        for o in ins.operands)):
            saw_test = True
        pos += ins.size
    return None


def scan(d, pe, md, disp):
    raw = 0
    hits = []
    for pat, reg, ilen in _patterns(disp):
        j = 0
        while True:
            k = d.find(pat, j)
            if k < 0:
                break
            j = k + 1
            raw += 1
            rk = pe.o2r(k)
            if rk is None:
                continue
            va = pe.imagebase + rk
            ins = next(md.disasm(d[k:k + 16], va, count=1), None)
            if ins is None or ins.size != ilen or ins.mnemonic != 'mov':
                continue
            memop = None
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM:
                    memop = op
            if memop is None or memop.mem.disp != disp:
                continue
            got = _follow(d, pe, md, k + ilen, reg)
            if got is None:
                continue
            call_off, kind, gated = got
            hits.append((k, va, reg, call_off, kind, memop.size, gated))
    return raw, hits


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    pe = pe_re.PE(argv[1])
    d = pe.d
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    for a in argv[2:]:
        disp = int(a, 0)
        raw, hits = scan(d, pe, md, disp)
        print('disp=0x%X  raw=%d  real=%d' % (disp, raw, len(hits)))
        seen = set()
        for k, va, reg, c, kind, width, gated in sorted(hits):
            rc = pe.o2r(c)
            if rc is None:
                continue
            key = (va, pe.imagebase + rc)
            if key in seen:
                continue
            seen.add(key)
            print('    mov  @0x%08X  [dword ]  %-4s %s @0x%08X   (reg=%s)%s'
                  % (key[0], kind, REGNAME[reg], key[1], REGNAME[reg],
                     '  [gate test %s,%s]' % (REGNAME[reg], REGNAME[reg]) if gated else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
