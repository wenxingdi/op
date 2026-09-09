# -*- coding: utf-8 -*-
"""Module inventory: lines/files/cross-module include deps for libop.
Usage: python module_inventory.py -> prints TSV rows"""
import os, re, collections, sys

ROOT = r"D:/AutoPro/op-master/op/libop"
skip_dirs = {"com_generated", "__pycache__"}
inc_re = re.compile(r'#include\s*[<"]([^>"]+)[>"]')

mods = collections.OrderedDict()
for dirpath, dirnames, filenames in os.walk(ROOT):
    dirnames[:] = [d for d in dirnames if d not in skip_dirs]
    rel = os.path.relpath(dirpath, ROOT)
    if rel == ".": continue
    top = rel.split(os.sep)[0]
    m = mods.setdefault(top, {"files": 0, "lines": 0, "dirs": set(), "incs": collections.Counter()})
    for fn in filenames:
        if fn.endswith((".cpp", ".h")):
            p = os.path.join(dirpath, fn)
            m["files"] += 1
            m["dirs"].add(rel)
            try:
                with open(p, "r", encoding="utf-8", errors="ignore") as f:
                    m["lines"] += sum(1 for _ in f)
                    f.seek(0)
                    for line in f:
                        mm = inc_re.search(line)
                        if mm:
                            t = mm.group(1)
                            if "libop/" in t or t.startswith("../") or t.startswith("..\\"):
                                m["incs"][t] += 1
            except Exception:
                pass

print("module\tfiles\tlines\tdirs\tcross_include_targets")
for name, m in mods.items():
    # top include targets = which other top modules' headers this module includes
    tops = collections.Counter()
    for t, c in m["incs"].items():
        norm = t.replace("\\", "/")
        if "libop/" in norm:
            seg = norm.split("libop/")[-1]
        else:
            seg = norm.lstrip("../").lstrip("./")
        top = seg.split("/")[0]
        if top != name and top in mods:
            tops[top] += c
    cross = ",".join(f"{k}x{v}" for k, v in tops.most_common(6)) or "-"
    print(f"{name}\t{m['files']}\t{m['lines']}\t{len(m['dirs'])}\t{cross}")
