"""
traceDec / 一次性移植工具：展开 GCC 的 `case A ... B:` 范围扩展。

orbuculum 的 `traceDecoder_etm4.c` 用了 GCC 专有的 case-range 语法
（`case 0b11011000 ... 0b11011011:`），标准 C 和 C++ 都没有，MSVC 报
"C2143: 缺少 ':'(在 '...' 的前面)"。

这里把它机械展开成等价的显式标签序列：

    case 0b11011000 ... 0b11011011:   ->   case 0b11011000: case 0b11011001:
                                           case 0b11011010: case 0b11011011:

**语义完全等价**（原语法就是"这一串值走同一个分支"），只是标签变多了。
后面的注释原样保留（它在冒号之后，不受影响）。

用法：
    python fix_gcc_ranges.py [文件]
默认处理同目录的 traceDecoder_etm4.cpp。就地修改，先备份为 .orig。
"""
import io
import os
import re
import sys

CASE_RANGE = re.compile(r'case\s+(0b[01]+)\s*\.\.\.\s*(0b[01]+)\s*:')


def expand(match):
    lo = int(match.group(1), 2)
    hi = int(match.group(2), 2)
    if hi < lo:
        raise ValueError('empty range: %s' % match.group(0))
    return ' '.join('case 0b%s:' % format(v, '08b') for v in range(lo, hi + 1))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'traceDecoder_etm4.cpp')
    with io.open(path, 'r', encoding='utf-8', newline='') as f:
        src = f.read()

    n = [0]
    lo_sum = [0]

    def counted(match):
        n[0] += 1
        lo_sum[0] += int(match.group(2), 2) - int(match.group(1), 2) + 1
        return expand(match)

    out = CASE_RANGE.sub(counted, src)

    if not n[0]:
        print('no GCC case ranges found in %s' % path)
        return 0

    backup = path + '.orig'
    if not os.path.exists(backup):
        with io.open(backup, 'w', encoding='utf-8', newline='') as f:
            f.write(src)

    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(out)

    print('expanded %d range(s) into %d explicit case label(s) in %s'
          % (n[0], lo_sum[0], os.path.basename(path)))
    print('backup: %s' % os.path.basename(backup))
    return 0


if __name__ == '__main__':
    sys.exit(main())
