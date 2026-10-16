#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_swig_sync.py — 检查 SWIG / ctypes 绑定是否与契约层 libop.h 同步

背景
    swig/op_wrap.cxx + swig/pyop.py 是 SWIG 4.4.1 的生成物且已入库，
    python/pyop/_binding.py 是 pyop.py 的副本（wheel 包 python/pyop 的实际内容）。
    三者一旦落后于 include/libop.h，pip 包 op-plugins 就会静默缺 API
    （历史事故：落后 12 个方法，含 SetDxAttr/GetDxAttr/FindLineExS/AutoOcr*）。
    2026-09-16 已用 SWIG 4.4.1 重新生成并补齐。

    bindings/python/op/api.py（ctypes 绑定）同理——2026-10-16 曾落后 55 个方法
    （文件/JSON/HTTP/QR/Draw 等整批缺失），本脚本第二段对其做同样的漂移拦截。

用法
    python scripts/check_swig_sync.py

退出码
    0 = 各层同步
    1 = 存在漂移（需重新生成，见下）
    2 = 文件缺失

修复方式（需 SWIG 4.4.1）
    pip install "swig==4.4.1"          # pip 提供 Windows 二进制
    cd swig && swig -c++ -python -o op_wrap.cxx op.i
    copy /Y pyop.py ..\\python\\pyop\\_binding.py

    ctypes 层漂移则对照 include/op_c_api.h 补 bindings/python/op/_ffi.py 签名与
    api.py 方法包装；命名特殊表 _PY_SNAKE_SPECIAL 需与
    scripts/gen_api_reference.py 保持一致。
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
CTYPES_API = os.path.join(ROOT, "bindings", "python", "op", "api.py")

EXPECT_SWIG = "4.4.1"

# ctypes 绑定的手写命名特殊表（与 scripts/gen_api_reference.py 的 _PY_SNAKE_SPECIAL
# 保持一致；新增非常规命名时两处同步改）。Ver 映射为 version 属性。
_PY_SNAKE_SPECIAL = {
    "Ver": "version",
    "SetAStarMap": "set_astar_map",
    "SetAStarMapData": "set_astar_map_data",
    "UnBindWindow": "unbind_window",
    "HWheel": "hwheel",
    "XButton1Click": "xbutton1_click",
    "XButton1DoubleClick": "xbutton1_double_click",
    "XButton1Down": "xbutton1_down",
    "XButton1Up": "xbutton1_up",
    "XButton2Click": "xbutton2_click",
    "XButton2DoubleClick": "xbutton2_double_click",
    "XButton2Down": "xbutton2_down",
    "XButton2Up": "xbutton2_up",
    "AutoOcr": "autoocr",
    "DecodeQRCode": "decode_qrcode",
    "DecodeQRCodeFromFile": "decode_qrcode_from_file",
    "CreateQRCode": "create_qrcode",
}


def snake_acronym(n: str) -> str:
    n2 = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", n)
    n2 = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", n2)
    return n2.lower()


def ctypes_name(n: str) -> str:
    return _PY_SNAKE_SPECIAL.get(n, snake_acronym(n))


# 全返回型方法提取：void-only 正则会漏掉 long/指针/string 返回的方法
# （JsonIsValid/SetOcrEngine/Ver 等 6 个），导致非 void 方法漂移不被发现。
_CPP_NOISE = {"Op", "operator", "runtime", "window", "binding", "input", "image", "ocr",
              "opencv", "yolo", "memory"}


def libop_methods(src: str) -> set:
    names = set()
    for m in re.finditer(r"^\s+(?:std::)?[\w:<>,\s\*&]+?\b(\w+)\s*\(", src, re.M):
        n = m.group(1)
        if n not in _CPP_NOISE:
            names.add(n)
    return names


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

    methods = libop_methods(libop)
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

    # ---- ctypes 绑定（bindings/python）漂移检查 ----
    ctypes_src = read(CTYPES_API)
    ctypes_defs = set(re.findall(r"^\s+def\s+(\w+)\s*\(", ctypes_src, re.M))
    expect = {ctypes_name(m) for m in methods}
    # version 是 @property（def version(self) 已能被上面的正则捕获），无需特判
    miss_ct = sorted(n for n in (expect - ctypes_defs) if not n.startswith("_"))
    extra_ct = sorted(n for n in (ctypes_defs - expect) if not n.startswith("_"))
    print("libop.h 期望 ctypes 方法数 =", len(expect))
    print("api.py 公开 def 数        =", len([n for n in ctypes_defs if not n.startswith("_")]))
    if miss_ct:
        bad = 1
        print("[DRIFT] libop.h 有、api.py 无 (%d):" % len(miss_ct))
        for n in miss_ct:
            print("   ", n)
    else:
        print("[OK] api.py 覆盖 libop.h 全部 ctypes 方法")
    if extra_ct:
        print("[WARN] api.py 有、libop.h 无（多为别名/辅助方法，不拦截）:", extra_ct)

    print()
    print("结论：", "存在漂移，需重新生成" if bad else "各层同步，无缺口")
    return bad


if __name__ == "__main__":
    raise SystemExit(main())
