#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""全盘扫 op_x64.dll / op_c_api_x64.dll 副本，比对 sha1（工作区脚本，不入库）。

「修复没生效」的头号嫌疑就是**旧 DLL**，而这个项目有 N 处副本。
本脚本列出全部副本 + sha1 + 是否与当前构建源一致。

用法：python workbench/scan_release_copies.py
"""
import hashlib
import os

SRC = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
NAMES = ["op_x64.dll", "op_c_api_x64.dll"]
ROOTS = [r"D:\AutoPro", r"D:\AgentWork"]
SKIP_DIRS = {"_deps", "backup", "isolated", ".git", "__pycache__"}


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


src_sha = {n: sha1(os.path.join(SRC, n)) for n in NAMES}
print("源：", src_sha)

same = {n: [] for n in NAMES}
diff = {n: [] for n in NAMES}
for root in ROOTS:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn not in NAMES:
                continue
            p = os.path.join(dirpath, fn)
            if os.path.abspath(dirpath) == os.path.abspath(SRC):
                continue
            try:
                s = sha1(p)
            except OSError:
                continue
            (same if s == src_sha[fn] else diff).setdefault(fn, []).append((p, s))

for n in NAMES:
    print(f"\n=== {n} ===")
    print(f"  与源一致 {len(same[n])} 处")
    print(f"  与源不同 {len(diff[n])} 处")
    for p, s in diff[n]:
        print(f"    ! {s}  {p}")
