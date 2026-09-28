#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CMake 显式源清单同步门禁。

背景
----
`libop/CMakeLists.txt` 与 `tests/CMakeLists.txt` 用的是**显式源列表**（非 GLOB）。
新增 .cpp 忘了登记 = **静默不编译**：既不报错，功能也不存在，
等到某天发现"改了代码没生效"才回头查（本项目已吃过一次亏）。

本脚本比对「磁盘上实际存在的 .cpp」与「CMakeLists 里登记的路径」：

- MISSING：磁盘有、清单无  → **危险**（静默不编译）
- STALE  ：清单有、磁盘无  → 构建直接失败（通常是删文件没同步）

用法
----
    python scripts/check_cmake_sources.py            # 人类可读
    python scripts/check_cmake_sources.py --strict    # 有 MISSING/STALE 就退出码 1（可挂 CI）
"""
from __future__ import annotations

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = [
    ("libop/CMakeLists.txt", "libop"),
    ("tests/CMakeLists.txt", "tests"),
]

# 生成物 / 非本目录源码：清单里有、磁盘上未必有，跳过 STALE 判定
GENERATED_HINTS = ("op_com_i.c", "_wrap", "generated", "$(", "${")

# 明确不参与构建的子目录（独立可执行 / 隔离区 / 备份）
EXCLUDE_DIRS = {"build", "_deps", "backup", "isolated", ".git", "__pycache__", "workbench"}


def listed_sources(cmake_path: str) -> set:
    text = open(cmake_path, "r", encoding="utf-8", errors="replace").read()
    names = set()
    for m in re.finditer(r'"([^"\n]+\.(?:cpp|c|cc|h))"', text):
        rel = m.group(1).replace("\\", "/")
        if any(h in rel for h in GENERATED_HINTS):
            continue
        names.add(os.path.normpath(rel))
    return names


def disk_sources(root: str) -> set:
    out = set()
    abs_root = os.path.join(REPO, root)
    for dirpath, dirnames, filenames in os.walk(abs_root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        for fn in filenames:
            if fn.endswith((".cpp", ".c", ".cc")):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, REPO).replace("\\", "/")
                out.add(os.path.normpath(rel))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    total_missing, total_stale = 0, 0
    for cmake_rel, root in TARGETS:
        cmake_path = os.path.join(REPO, cmake_rel)
        listed = listed_sources(cmake_path)
        # 清单里的路径有 "../libop/xxx" 这种相对当前 CMakeLists 的写法
        normalized = set()
        base = os.path.dirname(cmake_rel)
        for p in listed:
            p = p.replace("\\", "/")
            # 清单里的路径一律相对该 CMakeLists 所在目录（"main.cpp" / "../libop/base/Utils.cpp"）
            p = os.path.normpath(os.path.join(base, p)).replace("\\", "/")
            normalized.add(os.path.normpath(p))
        on_disk = disk_sources(root)

        missing = sorted(d for d in on_disk if d not in normalized)
        stale = sorted(n for n in normalized
                       if n.startswith(root.replace("\\", "/") + "/") and n not in on_disk
                       and not any(h in n for h in GENERATED_HINTS))

        print(f"### {cmake_rel}（{root}/ 下实际 {len(on_disk)} 个源文件）")
        if missing:
            print(f"  MISSING {len(missing)} —— 磁盘有但清单未登记（**静默不编译**）：")
            for m in missing:
                print(f"    - {m}")
        else:
            print("  MISSING 0")
        if stale:
            print(f"  STALE {len(stale)} —— 清单登记但磁盘不存在：")
            for s in stale:
                print(f"    - {s}")
        else:
            print("  STALE 0")
        print()
        total_missing += len(missing)
        total_stale += len(stale)

    print(f"合计：MISSING {total_missing} / STALE {total_stale}")
    if args.strict and (total_missing or total_stale):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
