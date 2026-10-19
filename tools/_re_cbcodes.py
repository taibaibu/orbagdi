#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
_re_cbcodes.py — 列出某个 DLL 里所有"push <码>; call dword ptr [回调指针]"的通知码。

AGDI 驱动通过 pCbFunc(code, vp) 与 uVision 通信。把 ULP2CM3 用到的码全列出来，
就能和本驱动逐条对齐，找出遗漏。

用法: python _re_cbcodes.py <dll> [回调指针 VA 十六进制]
"""
import struct
import sys
import pe_re

# 常见的回调指针全局量（在 ULP2CM3 里由 AG_CB_GETBOOCBPTR 之类填入）
DEFAULT_PTRS = [0x102A3C94, 0x102A3C98, 0x102A3C9C, 0x102A3C90]


def scan_ptr(pe, d, ptr_va, out):
    # call dword ptr [ptr] = FF 15 <imm32>
    pat = b'\xFF\x15' + struct.pack('<I', ptr_va)
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
        # 往前找最近的 push 立即数（push imm8 = 6A xx / push imm32 = 68 xx xx xx xx）
        code = None
        for back in (2, 5):
            s = k - back
            if s < 0:
                continue
            if back == 2 and d[s] == 0x6A:
                code = d[s + 1]
                break
            if back == 5 and d[s] == 0x68:
                code = struct.unpack_from('<I', d, s + 1)[0]
                break
        out.append((va, code))


def main(argv):
    path = argv[1]
    ptrs = [int(argv[2], 0)] if len(argv) > 2 else DEFAULT_PTRS
    pe = pe_re.PE(path)
    d = pe.d
    out = []
    for p in ptrs:
        scan_ptr(pe, d, p, out)
    print('callback invocations in %s: %d' % (path, len(out)))
    for va, code in sorted(out):
        print('  0x%08X  code=%s' % (va, ('%u (0x%X)' % (code, code)) if code is not None else '?'))
    codes = sorted(set(c for _, c in out if c is not None))
    print('distinct codes:', ' '.join(str(c) for c in codes))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
