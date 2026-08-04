#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""L3-C: 将 COM 层手写的 BSTR* 出参构造(CComBSTR + Append + CopyTo + return S_OK)
统一替换为已有的 CopyOutBstr(target, s) helper(含 NULL 检查 + 返回真实 hr)。

逐行状态机, 稳健不依赖多行正则。仅转换严格匹配的反模式, 不命中者保留原样。
用法: python l3_bstr_refactor.py        # dry-run, 打印统计
      python l3_bstr_refactor.py --apply # 实际改写并写回
"""
import re
import sys

SRC = "D:/AutoPro/op-master/op/libop/com/OpAutomation.cpp"
APPLY = "--apply" in sys.argv

with open(SRC, "rb") as f:
    text = f.read().decode("utf-8").replace("\r\n", "\n")

lines = text.split("\n")
out = []
i = 0
n = 0
pending = None   # (var, src)   src 为 None 表示还没遇到 Append
buffer = []      # 暂存待丢弃的行(CComBSTR 声明 / HRESULT hr; / Append 行)

re_c = re.compile(r"\s*CComBSTR\s+(\w+);\s*$")
re_app = lambda v: re.compile(r"\s*" + re.escape(v) + r"\.Append\((.*)\);\s*$")
re_cp = lambda v: re.compile(r"\s*(?:hr\s*=\s*|return\s+)?\s*" + re.escape(v) + r"\.CopyTo\((\w+)\);\s*$")
re_hr = re.compile(r"\s*HRESULT\s+hr;\s*$")
re_hr_a = lambda v: re.compile(r"\s*auto\s+hr\s*=\s*" + re.escape(v) + r"\.Append\((.*)\)")
re_hr_c = lambda v: re.compile(r"\s*hr\s*=\s*" + re.escape(v) + r"\.CopyTo")

while i < len(lines):
    line = lines[i]
    m_c = re_c.match(line)
    if m_c and pending is None:
        pending = (m_c.group(1), None)
        buffer = [line]
        i += 1
        continue

    if pending is not None:
        v = pending[0]
        # pending 激活时, 块内空行也收入 buffer(不重置状态)
        if line.strip() == "":
            buffer.append(line)
            i += 1
            continue
        ma = re_app(v).match(line)
        mc = re_cp(v).match(line)
        if ma:
            src = ma.group(1).strip()
            # 还原为 wstring 变量名: s.data() / s.c_str() -> s
            if src.endswith(".data()"):
                src = src[: -len(".data()")]
            elif src.endswith(".c_str()"):
                src = src[: -len(".c_str()")]
            pending = (v, src)
            buffer.append(line)
            i += 1
            continue
        if mc:
            tgt = mc.group(1)
            if pending[1] and re.fullmatch(r"\w+", pending[1]):
                out.append("    return CopyOutBstr(%s, %s);" % (tgt, pending[1]))
                n += 1
                i += 1
                # 跳过后续空行 + 死代码 return (S_OK / hr, 二者均已被 CopyOutBstr 取代)
                while i < len(lines) and lines[i].strip() == "":
                    i += 1
                if i < len(lines) and lines[i].strip() in ("return S_OK;", "return hr;"):
                    i += 1
                pending = None
                buffer = []
                continue
            else:
                # 无法安全转换, 回吐 buffer
                out.extend(buffer)
                pending = None
                buffer = []
        elif re_hr.match(line):
            buffer.append(line)
            i += 1
            continue
        elif re_hr_a(v).match(line):
            # auto hr = var.Append(SRC);  提取 SRC 以还原 wstring 变量名
            m = re_hr_a(v).match(line)
            src = m.group(1).strip()
            if src.endswith(".data()"):
                src = src[: -len(".data()")]
            elif src.endswith(".c_str()"):
                src = src[: -len(".c_str()")]
            pending = (v, src)
            buffer.append(line)
            i += 1
            continue
        elif re_hr_c(v).match(line):
            buffer.append(line)
            i += 1
            continue
        else:
            # 不符合预期结构, 回吐 buffer
            out.extend(buffer)
            pending = None
            buffer = []

    out.append(line)
    i += 1

new_text = "\n".join(out)

if not APPLY:
    print("DRY-RUN: transformed %d blocks" % n)
    remain = re.findall(r"\.CopyTo\((ret|retstr|retstring|path|ret_str|retjson)\)", new_text)
    print("remaining manual CopyTo(BSTR*) sites: %d" % len(remain))
    print("CopyOutBstr calls after transform:", new_text.count("CopyOutBstr("))
else:
    with open(SRC, "w", encoding="utf-8", newline="\n") as f:
        f.write(new_text)
    print("APPLIED: transformed %d blocks" % n)
