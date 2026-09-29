#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""运行时覆盖缺口普查。

以 C-API 导出面（include/op_c_api.h 的 Op*）为主键，检查每个 API 是否被
**任一运行时用例**真正调用过：
  · tests/*.cpp        —— gtest（进程内直调 C API / 内部层）
  · scripts/probes/*.py —— Python 探针（经 ctypes 走 op_c_api_x64.dll）
两者都没出现 ⇒ 该 API 无任何端到端证据，是真机/冒烟覆盖缺口。

用法：
    python scripts/probes/coverage_gaps.py [--md path]
"""
import os
import re
import sys
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # op/

# 域划分（按名字前缀/关键字），用于分组输出
DOMAINS = [
    ("窗口/进程", r"Window|Process|RunApp|Clipboard|Inject|Dll|Enum|Find|SetWindow|GetWindow|Move|Client|Screen|Desktop|Path|Special"),
    ("图色基础", r"Capture|Color|FindColor|FindPic|FindMultiColor|FindShape|CmpColor|Pixel|Image|Save|Load|Bmp"),
    ("高级图色", r"FindLine|FindCircle|FindShapeEx|IsLineBlocked|SimplifyPath|GetPointInfo|FindPicExS|FindMultiColorEx|Contour|Skeleton"),
    ("OpenCV", r"^OpCv"),
    ("OCR/字库", r"Ocr|Dict|Word|Char|Font|Line"),
    ("键鼠", r"Key|Mouse|Click|Wheel|Input|Drag|Cursor"),
    ("内存", r"^OpMemory|^OpRead|^OpWrite|^OpFind"),
    ("算法/A星", r"^OpAStar|AStar|Algorithm|Path|Aes|Md5|Hash|Base64|Encrypt"),
    ("YOLO/gl", r"Yolo|^OpGl"),
]


def rd(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def api_names():
    h = rd(os.path.join(ROOT, "include", "op_c_api.h"))
    names = re.findall(r'OP_CALL\s+(Op\w+)\s*\(', h)
    return [n for n in dict.fromkeys(names) if n != "OpCreate"]


# 排除「自动生成」的全量空句柄冒烟测试：它对每个 API 都只是 null handle 调用，
# 只能证明「不崩」，不能证明功能正确 ⇒ 不算端到端证据。
EXCLUDE = {"c_api_null_handle_test.cpp"}

# 只统计「真实调用」的 gtest（排除空句柄冒烟后）仍会用宏批量引用 API 名，
# 因此额外要求：出现次数 ≥1 且**不在** EXCLUDE 文件里。


def runtime_corpus():
    txt = []
    for d in (os.path.join(ROOT, "tests"), os.path.join(ROOT, "scripts", "probes")):
        for fn in sorted(os.listdir(d)):
            if fn in EXCLUDE:
                continue
            if fn.endswith((".cpp", ".py", ".h")) and not fn.startswith("__"):
                txt.append(rd(os.path.join(d, fn)))
    return "\n".join(txt)


def domain_of(name):
    core = name[2:] if name.startswith("Op") else name
    for dname, pat in DOMAINS:
        if re.search(pat, core):
            return dname
    return "其他"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", default="")
    a = ap.parse_args()

def snake(name):
    """OpFindPicExS -> find_pic_ex_s（Python 绑定层命名）"""
    core = name[2:] if name.startswith("Op") else name
    s = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', '_', core)
    return s.lower()


def py_name_map():
    """Python 绑定名 -> 它实际调用的 C API 名集合。

    bindings/python/op/api.py 里 def 名是「产品口径」（autoocr_ex / find_str），
    与 C 名（OpAutoOcrEx / OpFindStr）不是简单蛇形转换 ⇒ 必须靠函数体里的
    "OpXxx" 字面量建立映射，否则会大量误报缺口。
    """
    txt = rd(os.path.join(ROOT, "bindings", "python", "op", "api.py"))
    m = {}
    # 按 def 切块
    marks = [(mo.start(), mo.group(1)) for mo in re.finditer(r'^\s{4}def (\w+)\(', txt, re.M)]
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(txt)
        body = txt[pos:end]
        for capi in re.findall(r'"(Op\w+)"', body):
            m.setdefault(name, set()).add(capi)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", default="")
    a = ap.parse_args()

    names = api_names()
    corpus = runtime_corpus()
    pmap = py_name_map()
    c2py = {}
    for py, caps in pmap.items():
        for c in caps:
            c2py.setdefault(c, []).append(py)

    covered, gaps = [], []
    for n in names:
        # ① C 名整体匹配（gtest 里 OpXxx(）
        hit = re.search(r'\b' + re.escape(n) + r'\b', corpus) is not None
        # ② Python 绑定层名（产品口径，非蛇形）—— 权威映射
        if not hit:
            for py in c2py.get(n, []):
                if re.search(r'\b' + re.escape(py) + r'\b', corpus):
                    hit = True
                    break
        # ③ 兜底：蛇形名
        if not hit:
            sk = snake(n)
            hit = re.search(r'\b' + re.escape(sk) + r'\b', corpus) is not None
        (covered if hit else gaps).append(n)

    by = {}
    for n in gaps:
        by.setdefault(domain_of(n), []).append(n)

    lines = []
    lines.append("# 运行时覆盖缺口（无任何端到端调用证据的 C API）")
    lines.append("")
    lines.append("- 主键：`include/op_c_api.h` 导出面 共 %d 个" % len(names))
    lines.append("- 已覆盖（tests/*.cpp 或 scripts/probes/*.py 中出现）：%d" % len(covered))
    lines.append("- **缺口：%d**" % len(gaps))
    lines.append("")
    for d, _ in DOMAINS + [("其他", "")]:
        ns = by.get(d) or []
        if not ns:
            continue
        lines.append("## %s（%d）" % (d, len(ns)))
        lines.append("")
        for n in ns:
            lines.append("- `%s`" % n)
        lines.append("")

    out = "\n".join(lines)
    print(out)
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write(out)
        print("写入 %s" % a.md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
