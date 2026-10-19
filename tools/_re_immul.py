#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_immul.py - 扫描"乘以某个立即数"的指令，用于定位 nCount*22 (0x16) 缓冲分配。

覆盖：
    imul r32, r/m32, imm8     6B <modrm> <imm8>
    imul r32, r/m32, imm32    69 <modrm> <imm32>
    lea  r32, [r+r*k] 常见缩放（这里是启发式，仅注明）
另可给出该点向后 / 前若干字节附近的 malloc/new/FF15 目标。

复用 pe_re.PE。

用法: python -X utf8 _re_immul.py <exe> <imm_dec_or_hex> [context_bytes]
"""
import struct
import sys

import pe_re


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    imm = int(argv[2], 0)
    ctx = int(argv[3], 0) if len(argv) > 3 else 0
    pe = pe_re.PE(path)
    d = pe.d
    n = 0
    if imm <= 0xFF:
        op1 = bytes([0x6B])
        for rm in range(0x100):
            pat = bytes([0x6B, rm, imm])
            j = 0
            while True:
                k = d.find(pat, j)
                if k < 0:
                    break
                j = k + 1
                if pe.o2r(k) is None:
                    continue
                # 仅保留 mod != 3 (内存操作数) 或任意，全部列出
                n += 1
                print('  imul r,r/m,0x%X (6B %02X imm8)  @0x%08X'
                      % (imm, rm, pe.imagebase + pe.o2r(k)))
    if imm <= 0xFFFFFFFF:
        tail = struct.pack('<I', imm)
        for rm in range(0x100):
            pat = bytes([0x69, rm]) + tail
            j = 0
            while True:
                k = d.find(pat, j)
                if k < 0:
                    break
                j = k + 1
                if pe.o2r(k) is None:
                    continue
                n += 1
                print('  imul r,r/m,0x%X (69 %02X imm32)  @0x%08X'
                      % (imm, rm, pe.imagebase + pe.o2r(k)))
    print('total %d' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
