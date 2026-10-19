"""扫描 disp32 引用点（成员位移 / 绝对寻址），打印 VA 与上下文字节。

补 `_re_callslot.py` 的盲区：`mov edx,[ecx+0x152]; call edx` 这类
"先 load 槽位、再 call 寄存器"的间接调用，不会出现 `call [reg+slot]`，
因此必须按 disp32 引用点扫。

用法:
    _re_disp32ref.py <exe> <disp32> [disp32 ...] [-n N]

输出每行: VA  前后各 8 字节上下文(hex)，用于按前导操作码过滤假阳性
          (如 `0f 84 3a 01 00 00` = jz rel32，非成员访问)。
"""
import sys
import re
import struct

import pe_re


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    exe = args[0]
    n = 14
    offs = []
    i = 1
    while i < len(args):
        if args[i] == '-n':
            n = int(args[i + 1], 0)
            i += 2
            continue
        offs.append(int(args[i], 0))
        i += 1

    pe = pe_re.PE(exe)
    d = pe.d
    for o in offs:
        t = struct.pack('<I', o)
        locs = [m.start() for m in re.finditer(re.escape(t), d)]
        print('--- disp32=0x%03X  n=%d' % (o, len(locs)))
        for k in locs[:n]:
            print('   0x%08X  %s' % (pe.imagebase + pe.o2r(k), d[k - 8:k + 10].hex()))


if __name__ == '__main__':
    main()
