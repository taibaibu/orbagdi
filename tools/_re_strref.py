"""定位 ASCII 字符串的出现点，并找出代码里对该字符串 VA 的引用（push/mov offset）+ 反汇编窗口。

用途：判断某 DLL 导出名（如 EnumUvARM7）是否被本 EXE 以**名字**引用（GetProcAddress 前的
`push offset "Name"`），以及调用点的实参（如 mode/ordinal）。

注意：若目标只以**序号**引用（push ordinal；EAT 里存 RVA），本工具找不到 —— 那时需改用
scan 导入手册/序号常量。

复扫要求（方法论）：判"某名是否被引用/是否存在"必须**四路复扫** ——
ASCII 精确 + ASCII 子串 + UTF-16LE 精确 + UTF-16LE 子串；
只跑 ASCII 精确会因截断/宽字符编码而漏，"名字零命中"结论必须建立在四路皆空之上。
（序号命中另有陷阱：见 `_re_iatcall.py` —— 序号必须连带模块归属判。）

用法:
    _re_strref.py <exe> <string> [-n N] [-w 0x40]
"""
import re
import struct
import sys

import capstone
import pe_re


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return
    exe = args[0]
    s = args[1].encode('ascii')
    n = 20
    win = 0x40
    i = 2
    while i < len(args):
        if args[i] == '-n':
            n = int(args[i + 1], 0)
            i += 2
        elif args[i] == '-w':
            win = int(args[i + 1], 0)
            i += 2
        else:
            i += 1

    pe = pe_re.PE(exe)
    d = pe.d
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    strs = [m.start() for m in re.finditer(re.escape(s), d)]
    print('== string %r  hits=%d' % (s.decode('ascii', 'replace'), len(strs)))
    for off in strs:
        r = pe.o2r(off)
        if r is None:          # 非节区 → 跳过，避免 int + None
            continue
        print('   str @0x%08X  (file 0x%X)' % (pe.imagebase + r, off))

    refs = []
    for off in strs:
        r = pe.o2r(off)
        if r is None:
            continue
        va = pe.imagebase + r
        for m in re.finditer(re.escape(struct.pack('<I', va)), d):
            k = m.start()
            rk = pe.o2r(k)
            if rk is None:
                continue
            ref_va = pe.imagebase + rk
            txt = None
            for back in range(1, 16):
                start = k - back
                if start < 0:
                    break
                rs = pe.o2r(start)
                if rs is None:
                    continue
                for ins in md.disasm(d[start:start + 16], pe.imagebase + rs, count=1):
                    if ins.address + ins.size != ref_va + 4:
                        continue
                    for op in ins.operands:
                        if op.type == capstone.x86.X86_OP_IMM and op.imm == va:
                            txt = (pe.imagebase + rs, ins.mnemonic + ' ' + ins.op_str)
                    if txt:
                        break
                if txt:
                    break
            if txt:
                refs.append((va, txt))

    print('== refs=%d' % len(refs))
    for va, (ins_va, ins_txt) in refs[:n]:
        print('   ref 0x%08X  %s   (-> str 0x%08X)' % (ins_va, ins_txt, va))

    for va, (ins_va, ins_txt) in refs[:n]:
        lo = ins_va - win
        hi = ins_va + win + 0x20
        print('---- disasm around 0x%08X (%s) ----' % (ins_va, ins_txt))
        rlo, rhi = pe.va2o(lo), pe.va2o(hi)
        if rlo is None or rhi is None:
            continue
        for ins in md.disasm(d[rlo:rhi], lo):
            print('   0x%08X  %-24s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str))


if __name__ == '__main__':
    main()
