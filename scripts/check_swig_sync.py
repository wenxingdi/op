#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_swig_sync.py — 检查 SWIG 绑定是否与契约层 libop.h 同步

背景
    swig/op_wrap.cxx + swig/pyop.py 是 SWIG 4.4.1 的生成物且已入库，
    python/pyop/_binding.py 是 pyop.py 的副本（wheel 包 python/pyop 的实际内容）。
    三者一旦落后于 include/libop.h，pip 包 op-plugins 就会静默缺 API
    （历史事故：落后 12 个方法，含 SetDxAttr/GetDxAttr/FindLineExS/AutoOcr*）。
    2026-09-16 已用 SWIG 4.4.1 重新生成并补齐。

用法
    python scripts/check_swig_sync.py

退出码
    0 = 三层同步
    1 = 存在漂移（需重新生成，见下）
    2 = 文件缺失

修复方式（需 SWIG 4.4.1）
    pip install "swig==4.4.1"          # pip 提供 Windows 二进制
    cd swig && swig -c++ -python -o op_wrap.cxx op.i
    copy /Y pyop.py ..\\python\\pyop\\_binding.py
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBOP = os.path.join(ROOT, "include", "libop.h")
PYOP = os.path.join(ROOT, "swig", "pyop.py")
WRAP = os.path.join(ROOT, "swig", "op_wrap.cxx")
BINDING = os.path.join(ROOT, "python", "pyop", "_binding.py")

EXPECT_SWIG = "4.4.1"


def read(p: str) -> str:
    if not os.path.exists(p):
        print("[FAIL] 缺失文件:", p)
        sys.exit(2)
    return open(p, encoding="utf-8", errors="replace").read()


def main() -> int:
    libop = read(LIBOP)
    pyop = read(PYOP)
    wrap = read(WRAP)
    binding = read(BINDING)

    methods = set(re.findall(r"^\s+void\s+(\w+)\s*\(", libop, re.M))
    py_defs = set(re.findall(r"^\s+def\s+(\w+)\s*\(", pyop, re.M))
    binding_defs = set(re.findall(r"^\s+def\s+(\w+)\s*\(", binding, re.M))
    wraps = set(re.findall(r"_wrap_Op_(\w+)\s*\(", wrap))

    print("libop.h 方法数        =", len(methods))
    print("swig/pyop.py def 数   =", len(py_defs))
    print("_binding.py def 数    =", len(binding_defs))
    print("op_wrap.cxx _wrap_ 数 =", len(wraps))
    print("pyop.py 与 _binding.py 内容一致 =", pyop == binding)

    mver = re.search(r"Version (\d+\.\d+\.\d+)", wrap)
    ver = mver.group(1) if mver else "?"
    print("wrapper 生成版本      =", ver, "(期望 %s)" % EXPECT_SWIG)
    print()

    bad = 0

    miss_py = sorted(methods - py_defs)
    if miss_py:
        bad = 1
        print("[DRIFT] libop.h 有、pyop.py 无 (%d):" % len(miss_py))
        for n in miss_py:
            print("   ", n)
    else:
        print("[OK] pyop.py 覆盖 libop.h 全部方法")

    if pyop != binding:
        bad = 1
        print("[DRIFT] python/pyop/_binding.py 与 swig/pyop.py 不一致")
        only_swig = sorted(py_defs - binding_defs)
        only_bind = sorted(binding_defs - py_defs)
        if only_swig:
            print("   _binding.py 缺:", only_swig)
        if only_bind:
            print("   _binding.py 多:", only_bind)
    else:
        print("[OK] _binding.py 与 pyop.py 完全一致")

    miss_wrap = sorted(methods - wraps)
    if miss_wrap:
        bad = 1
        print("[DRIFT] libop.h 有、wrapper 无 (%d):" % len(miss_wrap))
        for n in miss_wrap:
            print("   ", n)
    else:
        print("[OK] wrapper 覆盖 libop.h 全部方法")

    if ver != EXPECT_SWIG:
        bad = 1
        print("[DRIFT] wrapper 由 SWIG %s 生成，期望 %s（版本不一致会产生巨量噪声 diff）" % (ver, EXPECT_SWIG))
    else:
        print("[OK] wrapper 生成版本与基准一致")

    print()
    print("结论：", "存在漂移，需重新生成" if bad else "三层同步，无缺口")
    return bad


if __name__ == "__main__":
    raise SystemExit(main())
