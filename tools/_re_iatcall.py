"""列出对某 IAT 槽的全部间接调用点（call/jmp [IAT]），并反汇编其前置指令。

用途：判断 GetProcAddress 是否被**按序号**调用 —— 名字形式前置指令里是
`push <字符串VA>`（会被注释出字符串），序号形式则是 `push imm16/imm8`（小整数无注释）。

判据边界（重要，方法论）："序号命中"必须**连带模块归属**一起判 —— 同一序号在不同
DLL 里意义不同。实证：`EnumUvARM7` 在 ULP2CM3 导出序=14；UV4 里唯一带序号的
GetProcAddress 点 `0x7F70F3` 压 `0xE`(=14)，但该点属 **hhctrl.ocx**，不能据此认定
"UV4 按序号调 ULP2CM3:EnumUvARM7"。序号结论必须与承载它的模块配对。

用法:
    _re_iatcall.py <exe> <iat_va> [-w 32] [-q]
    -w N  每个调用点向前反汇编的最大字节数（默认 32）
    -q    摘要模式：每点只打印前置 push 还原结果（`"Name"` 或 `!IMM:0x..`）
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
    iat = int(args[1], 0)
    win = 32
    quiet = False
    i = 2
    while i < len(args):
        if args[i] == '-w':
            win = int(args[i + 1], 0)
            i += 2
        elif args[i] == '-q':
            quiet = True
            i += 1
        else:
            i += 1

    pe = pe_re.PE(exe)
    d = pe.d
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    sites = []
    for p in (b'\xff\x15' + struct.pack('<I', iat), b'\xff\x25' + struct.pack('<I', iat)):
        for m in re.finditer(re.escape(p), d):
            r = pe.o2r(m.start())
            if r is None:          # 非节区命中 → 跳过，避免 int + None
                continue
            sites.append((pe.imagebase + r, m.start()))
    sites.sort()
    print('IAT 0x%08X  call sites=%d' % (iat, len(sites)))

    for va, off in sites:
        best = None
        for back in range(2, win + 1):
            s = off - back
            if s < 0:
                break
            rs = pe.o2r(s)
            if rs is None:
                continue
            insns = list(md.disasm(d[s:off + 8], pe.imagebase + rs))
            if any(i.address == va for i in insns):
                best = (s, insns)
        if best is None:
            print('  0x%08X  (无法对齐)' % va)
            continue
        _, insns = best
        if quiet:
            toks = []
            for ins in insns:
                if ins.address >= va:
                    break
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_IMM:
                        imm = op.imm & 0xFFFFFFFF
                        c = pe.cstr(imm)
                        toks.append('"%s"' % c if c else '!IMM:0x%X' % imm)
            print('  0x%08X  %s' % (va, ' | '.join(toks)))
            continue
        print('  0x%08X:' % va)
        for ins in insns:
            note = ''
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_IMM:
                    c = pe.cstr(op.imm & 0xFFFFFFFF)
                    if c:
                        note = '   ; "%s"' % c
            print('      0x%08X  %-20s %s%s'
                  % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str, note))
            if ins.address >= va:
                break


if __name__ == '__main__':
    main()
