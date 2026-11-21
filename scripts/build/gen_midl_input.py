# -*- coding: utf-8 -*-
"""生成 midl 能安全读的 op.idl 副本（纯 ASCII，字节数/行号不变）。

## 为什么需要这一步（2026-11-21 实测，真实缺陷）

`libop/com/op.idl` 是 UTF-8，注释里带中文。MIDL 通过 **cl 预处理器**（代码页 936）
读这个文件，于是：

1. UTF-8 的多字节序列被按 GBK 双字节配对，**错位**；
2. 错位后偶发地把某个字节配成 `0x5C`（`\\`），落在 `//` 注释行尾 -> **续行**；
3. 紧邻的下一行声明被整行并入注释，**静默消失**，midl 仍 exit=0。

实测后果（2026-11-21 发布验证发现）：`OcrEx2`(440) / `OcrLastProfile`(444) /
`OcrFromMemEx2`(464) 三个方法**从来没进过类型库** —— 易语言/IDispatch 报
`DISP_E_UNKNOWNNAME`。而 C API、C++ 实现、Python 绑定都是好的，所以
"编译通过 + 单元测试全绿"完全掩盖了它。唯一线索是长期被忽略的
`warning C4819`（该文件包含不能在当前代码页(936)中表示的字符）。

## 做法

把非 ASCII **字节**逐个替换成 `X`（1:1，不增删字节）：
- 行号完全不变 -> midl 报错行号仍能对回源文件
- idl 里所有非 ASCII 都在 `//` 注释中（无 helpstring / 字符串字面量，已校验），
  替换不会影响任何声明或类型库内容
- 校验：替换后必须是纯 ASCII，且行数与源文件一致

用法：gen_midl_input.py <源 op.idl> <目标 op_midl.idl>
退出码：0 成功；非 0 = 输入不合法（含非 ASCII 的字符串字面量等）
"""
import sys

# idl 里出现这些模式就说明非 ASCII 可能不在注释里，直接拒绝（避免静默改坏内容）
RISKY_MARKERS = ('helpstring', 'L"', '[string]')


def convert(src_path, dst_path):
    with open(src_path, 'rb') as f:
        raw = f.read()

    if raw[:3] == b'\xef\xbb\xbf':
        raise SystemExit('输入带 UTF-8 BOM，请先去掉：%s' % src_path)

    text = raw.decode('utf-8', 'strict')
    for marker in RISKY_MARKERS:
        if marker in text:
            raise SystemExit(
                'idl 里出现 %r，说明非 ASCII 可能不在注释中；请人工确认后再改本脚本' % marker)

    out = bytes(b if b < 0x80 else 0x58 for b in raw)  # 0x58 = 'X'

    src_lines = raw.count(b'\n')
    dst_lines = out.count(b'\n')
    if src_lines != dst_lines:
        raise SystemExit('行数不一致（%d vs %d），拒绝写出' % (src_lines, dst_lines))

    with open(dst_path, 'wb') as f:
        f.write(out)

    replaced = sum(1 for b in raw if b >= 0x80)
    print('gen_midl_input: %s -> %s (%d 字节非 ASCII 已替换，%d 行)'
          % (src_path, dst_path, replaced, dst_lines + 1))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('用法: gen_midl_input.py <源 op.idl> <目标 op_midl.idl>')
    convert(sys.argv[1], sys.argv[2])
