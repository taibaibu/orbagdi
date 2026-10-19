#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pe_re.py — Keil AGDI / RDDI DLL 逆向分析小工具

用于分析 32 位 PE（典型目标）：
    D:\\MDK5\\ARM\\BIN\\CMSIS_AGDI.dll       官方 ARM CMSIS-AGDI（AGDI 层）
    D:\\MDK5\\ARM\\BIN\\CMSIS_DAP.dll        实际生效的 RDDI 层（可能是 ORBMDK）
    D:\\MDK5\\ARM\\BIN\\CMSIS_DAP.dll.bak    官方 RDDI 层备份（对照基准）

依赖：仅 capstone（PE 解析自带，无 pefile 依赖）
    python -m pip install capstone

子命令：
    exports <dll>                    列出导出表（名称 / RVA / VA）
    imports <dll>                    列出导入的 DLL 名
    names   <dll> [regex]            扫描文件内符号名（ASCII + UTF-16LE）
    dis     <dll> <VA|导出名> [len]  按 VA 或导出名线性反汇编（注释字符串立即数）
    xref    <dll> <子串>             找字符串及其代码引用点
    slots   <dll> <起VA> <止VA>      AGDI 的 GetProcAddress 指针槽映射 + 调用点统计

示例：
    python pe_re.py exports "D:\\MDK5\\ARM\\BIN\\CMSIS_DAP.dll.bak"
    python pe_re.py names   "D:\\MDK5\\ARM\\BIN\\CMSIS_AGDI.dll" "^(CMSIS_DAP|DAP|RDDI)_"
    python pe_re.py dis     "D:\\MDK5\\ARM\\BIN\\CMSIS_DAP.dll" CMSIS_DAP_GetDeviceIDList 400
    python pe_re.py dis     "D:\\MDK5\\ARM\\BIN\\CMSIS_AGDI.dll" 0x10022A60 0xD0
    python pe_re.py xref    "D:\\MDK5\\ARM\\BIN\\CMSIS_AGDI.dll" CMSIS_DAP_Disconnect
    python pe_re.py slots   "D:\\MDK5\\ARM\\BIN\\CMSIS_AGDI.dll" 0x1002C000 0x1002CA00
"""

import re
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_OP_REG
except ImportError:
    print("需要 capstone：python -m pip install capstone", file=sys.stderr)
    raise

X86 = (CS_ARCH_X86, CS_MODE_32)


# ---------------------------------------------------------------------------
# PE 解析
# ---------------------------------------------------------------------------
class PE:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.d = f.read()
        d = self.d
        e = struct.unpack_from('<I', d, 0x3C)[0]
        if d[e:e + 4] != b'PE\0\0':
            raise ValueError('not a PE file')
        coff = e + 4
        self.nsec = struct.unpack_from('<H', d, coff + 2)[0]
        optsize = struct.unpack_from('<H', d, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from('<H', d, opt)[0]
        self.imagebase = struct.unpack_from('<I', d, opt + 28)[0]
        self.ddoff = opt + (96 if magic == 0x10b else 112)
        self.secs = []
        so = opt + optsize
        for i in range(self.nsec):
            b = so + i * 40
            nm = d[b:b + 8].rstrip(b'\0').decode('latin1')
            vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', d, b + 8)
            self.secs.append((nm, vaddr, vsize, raddr, rsize))

    def r2o(self, rva):
        for nm, vaddr, vsize, raddr, rsize in self.secs:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raddr + (rva - vaddr)
        return None

    def o2r(self, ofs):
        for nm, vaddr, vsize, raddr, rsize in self.secs:
            if raddr <= ofs < raddr + rsize:
                return vaddr + (ofs - raddr)
        return None

    def va2o(self, va):
        return self.r2o(va - self.imagebase)

    def cstr(self, va, maxlen=120):
        """把 VA 处的 NUL 结尾可打印字符串取出，失败返回 None。"""
        o = self.va2o(va)
        if o is None:
            return None
        end = self.d.find(b'\0', o)
        if end < 0 or end == o or end - o > maxlen:
            return None
        s = self.d[o:end]
        if any(c < 0x20 or c > 0x7e for c in s):
            return None
        return s.decode('latin1')

    # --- 导出表 ---------------------------------------------------------
    def exports(self):
        d = self.d
        exp_rva = struct.unpack_from('<I', d, self.ddoff)[0]
        out = []
        if not exp_rva:
            return out
        eo = self.r2o(exp_rva)
        nfunc = struct.unpack_from('<I', d, eo + 20)[0]
        nnames = struct.unpack_from('<I', d, eo + 24)[0]
        funcs_rva = struct.unpack_from('<I', d, eo + 28)[0]
        names_rva = struct.unpack_from('<I', d, eo + 32)[0]
        ord_rva = struct.unpack_from('<I', d, eo + 36)[0]
        fo, no, oo = self.r2o(funcs_rva), self.r2o(names_rva), self.r2o(ord_rva)
        for i in range(nnames):
            nr = struct.unpack_from('<I', d, no + i * 4)[0]
            o = self.r2o(nr)
            end = d.index(b'\0', o)
            name = d[o:end].decode('latin1')
            ordv = struct.unpack_from('<H', d, oo + i * 2)[0]
            rva = struct.unpack_from('<I', d, fo + ordv * 4)[0]
            out.append((name, ordv, rva))
        return sorted(out, key=lambda x: x[2])

    def export_rva(self, name):
        for n, _, rva in self.exports():
            if n == name:
                return rva
        return None

    # --- 导入表 ---------------------------------------------------------
    def imports(self):
        d = self.d
        imp_rva = struct.unpack_from('<I', d, self.ddoff + 8)[0]
        out = []
        if not imp_rva:
            return out
        io = self.r2o(imp_rva)
        while True:
            name_rva = struct.unpack_from('<I', d, io + 12)[0]
            if name_rva == 0:
                break
            o = self.r2o(name_rva)
            end = d.index(b'\0', o)
            out.append(d[o:end].decode('latin1'))
            io += 20
        return out


def names_all(pe, pattern=None):
    """扫描 ASCII + UTF-16LE 符号名，可按正则过滤。"""
    res = set()
    for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_]{2,}', pe.d):
        res.add(m.group(0).decode('latin1'))
    for off in (0, 1):
        txt = pe.d[off:].decode('utf-16-le', errors='ignore')
        for m in re.finditer(r'[A-Za-z_][A-Za-z0-9_]{2,}', txt):
            res.add(m.group(0))
    if pattern:
        rx = re.compile(pattern)
        res = {n for n in res if rx.search(n)}
    return sorted(res)


def disasm(pe, start_va, length):
    md = Cs(*X86)
    md.detail = True
    o = pe.va2o(start_va)
    if o is None:
        print('VA 0x%08X 不在任何节内' % start_va)
        return
    for ins in md.disasm(pe.d[o:o + length], start_va):
        note = ''
        for op in ins.operands:
            if op.type == CS_OP_IMM:
                s = pe.cstr(op.imm & 0xFFFFFFFF)
                if s:
                    note = '   ; "%s"' % s
                    break
        print('0x%08X  %-22s %s%s'
              % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str, note))


# ---------------------------------------------------------------------------
# 子命令
# ---------------------------------------------------------------------------
def cmd_exports(path):
    pe = PE(path)
    print('imagebase = 0x%08X' % pe.imagebase)
    exps = pe.exports()
    print('exports = %d' % len(exps))
    for name, ordv, rva in exps:
        print('  %-40s ord=%-5d rva=0x%08X va=0x%08X' % (name, ordv, rva, pe.imagebase + rva))


def cmd_imports(path):
    pe = PE(path)
    for n in pe.imports():
        print(n)


def cmd_names(path, pattern=None):
    pe = PE(path)
    for n in names_all(pe, pattern):
        print(n)


def cmd_dis(path, target, length=256):
    pe = PE(path)
    if target.lower().startswith('0x'):
        va = int(target, 16)
    else:
        rva = pe.export_rva(target)
        if rva is None:
            print('导出未找到：%s' % target)
            return
        va = pe.imagebase + rva
        print('导出 %s -> rva=0x%08X va=0x%08X' % (target, rva, va))
    disasm(pe, va, length)


def cmd_xref(path, needle):
    pe = PE(path)
    pat = needle.encode('latin1')
    found = 0
    start = 0
    while True:
        i = pe.d.find(pat, start)
        if i < 0:
            break
        start = i + 1
        if pe.d[i + len(pat)] != 0:      # 只认精确匹配（NUL 结尾）
            continue
        rva = pe.o2r(i)
        if rva is None:
            continue
        va = pe.imagebase + rva
        found += 1
        print('str @file 0x%06X va 0x%08X  "%s"' % (i, va, needle))
        # 找引用该 VA 的代码（push imm32 / mov reg,imm32 都编码为绝对 VA）
        ref = struct.pack('<I', va)
        j = 0
        n = 0
        while True:
            k = pe.d.find(ref, j)
            if k < 0:
                break
            j = k + 1
            r = pe.o2r(k)
            if r is None:
                continue
            n += 1
            if n > 16:
                print('      ... 引用过多，已截断')
                break
            print('      ref va 0x%08X' % (pe.imagebase + r))
    if not found:
        print('未找到字符串：%s' % needle)


def cmd_slots(path, start_va, end_va):
    """还原 AGDI 的 GetProcAddress 槽位表并统计每个槽的调用点。

    识别模式（x86 32 位，__cdecl）：
        push <字符串VA>            ; 函数名
        push dword ptr [hModule]
        mov  dword ptr [slot], eax ; 保存「上一次」GetProcAddress 的结果
        call esi                   ; GetProcAddress
    因此 slot 与 name 的对应关系要靠「先记下 call 时的名字，再在下一条
    mov [slot],eax 时落表」，不能直接看相邻的 push。
    """
    pe = PE(path)
    md = Cs(*X86)
    md.detail = True
    o = pe.va2o(start_va)
    code = pe.d[o:o + (end_va - start_va)]

    last_str = None
    pending = None
    order = []
    for ins in md.disasm(code, start_va):
        m = ins.mnemonic
        if m == 'push':
            for op in ins.operands:
                if op.type == CS_OP_IMM:
                    s = pe.cstr(op.imm & 0xFFFFFFFF)
                    if s:
                        last_str = s
                        break
        elif m == 'call':
            if last_str:
                pending = last_str
            last_str = None
        elif m == 'mov' and len(ins.operands) == 2:
            dst, src = ins.operands
            if dst.type == CS_OP_MEM and src.type == CS_OP_REG and ins.reg_name(src.reg) == 'eax':
                if pending and pending.startswith(('RDDI_', 'DAP_', 'CMSIS_DAP_', 'ULINKPLUS_')):
                    order.append((pending, dst.mem.disp & 0xFFFFFFFF, ins.address))
                pending = None

    print('=== name -> pointer slot (%d) ===' % len(order))
    for nm, slot, at in order:
        print('  %-38s slot 0x%08X  (stored @0x%08X)' % (nm, slot, at))

    text = [s for s in pe.secs if s[0] == '.text']
    if not text:
        return
    tstart, tsize, traw = text[0][1], text[0][4], text[0][3]
    tbytes = pe.d[traw:traw + tsize]

    print()
    print('=== 每个槽在 .text 中的引用统计 ===')
    for nm, slot, at in order:
        pat = struct.pack('<I', slot)
        hits = []
        j = 0
        while True:
            k = tbytes.find(pat, j)
            if k < 0:
                break
            j = k + 1
            hits.append(pe.imagebase + tstart + k)
        calls = []
        for h in hits:
            fo = pe.va2o(h) - 2
            if fo >= 0 and pe.d[fo] == 0xFF:
                calls.append(h)
        flag = '   <== 会被间接调用' if calls else ''
        print('  %-38s slot 0x%08X refs=%2d call/jmp=%2d%s'
              % (nm, slot, len(hits), len(calls), flag))
        for h in calls[:4]:
            print('        call/jmp @0x%08X' % h)


# ---------------------------------------------------------------------------
USAGE = __doc__


def main(argv):
    if len(argv) < 3:
        print(USAGE)
        return 1
    cmd, path = argv[1], argv[2]
    rest = argv[3:]
    if cmd == 'exports':
        cmd_exports(path)
    elif cmd == 'imports':
        cmd_imports(path)
    elif cmd == 'names':
        cmd_names(path, rest[0] if rest else None)
    elif cmd == 'dis':
        if not rest:
            print('需要 <VA|导出名>')
            return 1
        cmd_dis(path, rest[0], int(rest[1], 0) if len(rest) > 1 else 256)
    elif cmd == 'xref':
        if not rest:
            print('需要 <子串>')
            return 1
        cmd_xref(path, rest[0])
    elif cmd == 'slots':
        if len(rest) < 2:
            print('需要 <起VA> <止VA>')
            return 1
        cmd_slots(path, int(rest[0], 16), int(rest[1], 16))
    else:
        print(USAGE)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
