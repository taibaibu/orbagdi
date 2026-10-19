#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_afc.py — 找 [reg+disp32] 形式访问某个位移的指令（modrm 的 reg/opcode 字段可枚举）。

用法: python _re_afc.py <exe> <disp_hex>
"""
import sys
import pe_re

DISP = 0x00000AFC


def main(argv):
    path = argv[1] if len(argv) > 1 else r'D:\MDK5\UV4\UV4.exe'
    disp = int(argv[2], 0) if len(argv) > 2 else DISP
    pe = pe_re.PE(path)
    d = pe.d
    tail = disp.to_bytes(4, 'little')

    # 形如  <opcode> <modrm:mod=10 reg=xxx rm=xxx> <disp32> ...
    # mod=10 表示 [reg+disp32]，rm=0..7 对应 eax/ecx/edx/ebx/esp/ebp/esi/edi
    groups = {
        'or  byte [r+d], imm8   (80 /1)': (0x80, 1),
        'and byte [r+d], imm8   (80 /4)': (0x80, 4),
        'mov byte [r+d], imm8   (C6 /0)': (0xC6, 0),
        'test byte [r+d], imm8  (F6 /0)': (0xF6, 0),
        'mov dword [r+d], imm32 (C7 /0)': (0xC7, 0),
        'or  dword [r+d], imm32 (81 /1)': (0x81, 1),
        'and dword [r+d], imm32 (81 /4)': (0x81, 4),
        'test dword [r+d], imm32( F7 /0)': (0xF7, 0),
    }

    for label, (op, reg) in groups.items():
        hits = []
        for rm in range(8):
            modrm = 0x80 | (reg << 3) | rm
            pat = bytes([op, modrm]) + tail
            j = 0
            while True:
                k = d.find(pat, j)
                if k < 0:
                    break
                j = k + 1
                r = pe.o2r(k)
                if r is None:
                    continue
                hits.append(pe.imagebase + r)
        print('%-34s -> %d' % (label, len(hits)))
        for h in hits[:16]:
            print('      0x%08X' % h)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
