"""扫描 disp32 的**真实成员访问**点（capstone 校验，剔除 jcc/jmp/push 的 rel32/imm32 假阳性）。

动机：`_re_callslot.py` 只能抓 `call [reg+slot]`（直接内存间接调用），
抓不到 `mov edx,[ecx+0x152]; call edx` 这类"先 load 槽位再 call 寄存器"的消费方式；
而 `_re_disp32ref.py` 的裸字节扫描里 ~90% 命中是 `0f 84 xx xx xx xx`(=jz rel32)。

判据（capstone 校验，精确）：对每个 disp32 命中点 k，向前 1..15 字节尝试对齐，
接受满足以下两条的指令：
  ① 有内存操作数且 其 disp == 目标值；
  ② 其 disp32 字段的起点**正好落在 k**：用 capstone `ins.encoding.disp_offset/disp_size`
     判定 `ins.address + disp_offset == k 且 disp_size == 4`。
② 用 encoding 精确定位 disp 字段，因此同时覆盖且只覆盖：
     mov r/m,[reg+disp]            (disp 居中,无尾随)、
     cmp r/m32,[reg+disp],imm8     (83 /7,disp 后有 1 字节 imm8)、
     cmp r/m32,[reg+disp],imm32    (81 /7,disp 后有 4 字节 imm32)、
     mov r/m32,[reg+disp],imm32    (c7 /0) 等。
  （注意:不能用"操作数 op.size"当立即数编码长度——capstone 对 rel8 分支也报 imm size=4，
    必须用 encoding 字段定位。）
  ⚠ 早期版本要求"指令恰好结束于 k+4"，会**系统性漏掉 `cmp mem,imm8/imm32`** ——
     而这正是"判空门 / 裸调用优雅失败"最常用的惯用法(静默假阴性)。
     实测 UV4 disp=0x11A 的两处空指针门 0x0045F0F7(`83 ba 1a010000 00`) /
     0x0045F46C(`83 b9 1a010000 00`) 曾被漏检。

访问宽度：每条命中输出内存操作数宽度(byte/word/dword/qword/xmmword)，可 -w 过滤。
    同一位移常被"字节 RMW"(adc byte [reg+disp],al) 与真成员访问共用 →
    只报 disp32 不报宽度会产生大量假阳性；用 `-w 4` 只看 dword 成员访问即可剔除。

用法:
    _re_dispref.py <exe> <disp32> [disp32 ...] [-n N] [-w W [W ...]]
      -w 只保留指定访问宽度(字节数): 1=byte 2=word 4=dword 8=qword 16=xmmword
"""
import sys
import re
import struct

import capstone
import pe_re

WLAB = {1: 'byte', 2: 'word', 4: 'dword', 8: 'qword', 10: 'tbyte',
        16: 'xmmword', 32: 'ymmword'}


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    exe = args[0]
    n = 12
    offs = []
    widths = []
    i = 1
    while i < len(args):
        if args[i] == '-n':
            n = int(args[i + 1], 0)
            i += 2
            continue
        if args[i] == '-w':
            i += 1
            while i < len(args) and re.match(r'^(0x)?[0-9A-Fa-f]+$', args[i]):
                widths.append(int(args[i], 0))
                i += 1
            continue
        offs.append(int(args[i], 0))
        i += 1

    pe = pe_re.PE(exe)
    d = pe.d
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    for o in offs:
        locs = [m.start() for m in re.finditer(re.escape(struct.pack('<I', o)), d)]
        hits = {}
        for k in locs:
            rk = pe.o2r(k)
            if rk is None:
                continue
            kva = pe.imagebase + rk
            cands = []
            for back in range(1, 16):
                s = k - back
                if s < 0:
                    break
                rs = pe.o2r(s)
                if rs is None:
                    continue
                for ins in md.disasm(d[s:s + 16], pe.imagebase + rs, count=1):
                    memop = None
                    for op in ins.operands:
                        if op.type == capstone.x86.X86_OP_MEM:
                            memop = op
                    if memop is None or memop.mem.disp != o:
                        continue
                    enc = ins.encoding
                    if enc is None or enc.disp_size != 4:
                        continue
                    # disp32 字段起点必须正好落在命中处 k（capstone encoding 精确定位，
                    # 覆盖 cmp mem,imm8/imm32 等 disp 后带尾随立即数的形态）
                    if ins.address + enc.disp_offset != kva:
                        continue
                    cands.append((s, pe.imagebase + rs,
                                  ins.mnemonic + ' ' + ins.op_str,
                                  ins.size, memop.size))
            if cands:
                # 同 k 可能有多条合法解码(如 0f 11 80.. 的 movups vs 11 80.. 的 adc)，
                # 取"最早起点/最长指令"作最可能真身(与 uv4-fetch 口径一致)。
                cands.sort(key=lambda c: (-c[3], c[0]))
                s, va, txt, _isz, w = cands[0]
                if widths and w not in widths:
                    continue
                hits[va] = (txt, w)
        print('--- disp32=0x%03X  raw=%d  real=%d  (width filter: %s)'
              % (o, len(locs), len(hits), widths or 'all'))
        for va in sorted(hits)[:n]:
            txt, w = hits[va]
            print('   0x%08X  [%-7s] %s' % (va, WLAB.get(w, '%dB' % w), txt))


if __name__ == '__main__':
    main()
