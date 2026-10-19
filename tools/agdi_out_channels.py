#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
agdi_out_channels.py — 从 AGDI 驱动 DLL 里提取「回报到 uVision 主界面」的通道参数

背景：AGDI 公开头文件（AGDI.H）没有定义 MSG_UV2_* 这套消息码，但官方驱动
（ULP2CM3.dll / DbgCM 系列）用它把文本打到 uVision 的命令窗口 / 编译输出窗口：

    SendMessage(hMfrm,          // AG_Init(AG_INITITEM|AG_INITPHANDLEP, &hwnd)
                Uv2Msg,         // AG_Init(AG_INITITEM|AG_INITUSRMSG,   msg)
                pio->FlashLoad ? MSG_UV2_TRNLINE : MSG_UV2_CMDLINE,
                (LPARAM)text);

本脚本从二进制里把这两个常量挖出来：先定位 user32!SendMessageA 的 IAT 槽，
再扫描 .text 里 `call dword ptr [IAT]`（FF 15 <va>）的调用点，把调用前压栈的
立即数全部列出——其中成对出现、且只在 FlashLoad 分支上区分的两个值就是
MSG_UV2_CMDLINE / MSG_UV2_TRNLINE。

用法：
    python agdi_out_channels.py ULP2CM3.dll
    python agdi_out_channels.py ULP2CM3.dll SendMessageW
"""

import struct
import sys

sys.path.insert(0, '.')
import pe_re  # noqa: E402

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    print("需要 capstone：python -m pip install capstone", file=sys.stderr)
    raise


def iat_of(pe, dll, func):
    """返回 <dll>!<func> 的 IAT 槽 VA（绝对地址）。"""
    d = pe.d
    imp_rva = struct.unpack_from('<I', d, pe.ddoff + 8)[0]
    if not imp_rva:
        return None
    io = pe.r2o(imp_rva)
    want = dll.lower()
    while True:
        oft, _ts, _fw, name_rva, first_thunk = struct.unpack_from('<IIIII', d, io)
        if name_rva == 0:
            return None
        no = pe.r2o(name_rva)
        nm = d[no:d.index(b'\0', no)].decode('latin1').lower()
        if nm == want:
            orig = pe.r2o(oft) if oft else pe.r2o(first_thunk)
            i = 0
            while True:
                v = struct.unpack_from('<I', d, orig + 4 * i)[0]
                if v == 0:
                    break
                if not (v & 0x80000000):
                    o = pe.r2o(v)
                    if o is not None:
                        fn = d[o + 2:d.index(b'\0', o + 2)].decode('latin1')
                        if fn == func:
                            return pe.imagebase + first_thunk + 4 * i
                i += 1
        io += 20


def call_sites(pe, iat_va):
    """扫描 .text 里 call dword ptr [iat_va]（FF 15 <va>）的位置。"""
    pat = b'\xff\x15' + struct.pack('<I', iat_va)
    text = [s for s in pe.secs if s[0] == '.text'][0]
    tstart, tsize, traw = text[1], text[4], text[3]
    tb = pe.d[traw:traw + tsize]
    hits = []
    j = 0
    while True:
        k = tb.find(pat, j)
        if k < 0:
            break
        j = k + 1
        hits.append(pe.imagebase + tstart + k)
    return hits


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    path = argv[1]
    func = argv[2] if len(argv) > 2 else 'SendMessageA'

    pe = pe_re.PE(path)
    iat = iat_of(pe, 'user32.dll', func)
    if iat is None:
        print('找不到 user32!%s 的 IAT 槽' % func)
        return 1
    print('user32!%s  IAT = 0x%08X' % (func, iat))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    hits = call_sites(pe, iat)
    print('调用点 = %d\n' % len(hits))

    for va in hits:
        o = pe.va2o(va)
        back = pe.d[max(0, o - 40):o]
        start = va - len(back)
        ins = list(md.disasm(back, start))
        # 只保留最后几条（压参数的那几条）
        tail = ins[-6:]
        imm = []
        for x in tail:
            if x.mnemonic == 'push':
                for op in x.operands:
                    if op.type == 2:          # CS_OP_IMM
                        imm.append(op.imm & 0xFFFFFFFF)
        print('@0x%08X  push 立即数: %s' % (va, ' '.join('0x%X' % v for v in imm)))
        for x in tail:
            print('      0x%08X  %s %s' % (x.address, x.mnemonic, x.op_str))
        print()

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
