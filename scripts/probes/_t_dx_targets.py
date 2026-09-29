# -*- coding: utf-8 -*-
"""扫描本机可作为 dx（D3D9/DXGI）测试靶子的现成程序。

判据：
  - 位数：PE header machine（0x14C=i386/32 位、0x8664=AMD64）
  - 图形 API：导入表（含延迟导入）里是否出现 d3d9.dll / dxgi.dll / d3d11.dll /
    d3d12.dll / ddraw.dll
注意：导入表是**静态线索**。真正能否当 dx 靶子要看运行后**实际加载的模块**
（用 workbench/_t_mods.py 枚举）。很多程序是延迟加载或动态 LoadLibrary。

用法：
    python workbench/_t_dx_targets.py                 # 扫系统目录
    python workbench/_t_dx_targets.py <目录> [深度]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_imports  # noqa: E402  （同目录工具）

GRAPHICS = ("d3d9.dll", "dxgi.dll", "d3d11.dll", "d3d12.dll", "ddraw.dll", "d3d8.dll")
MAX_MB = 60  # 超过就不读（导入表解析需要整文件）


def machine_of(path):
    try:
        with open(path, "rb") as f:
            head = f.read(0x400)
    except OSError:
        return None
    if head[:2] != b"MZ":
        return None
    try:
        e = struct.unpack_from("<I", head, 0x3C)[0]
        m, = struct.unpack_from("<H", head, e + 4)
        return m
    except struct.error:
        return None


def scan(root, depth=1):
    root = os.path.abspath(root)
    base_depth = root.rstrip("\\").count("\\")
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        if dirpath.count("\\") - base_depth >= depth:
            dirnames[:] = []
        for fn in filenames:
            if not fn.lower().endswith((".exe", ".scr")):
                continue
            p = os.path.join(dirpath, fn)
            mach = machine_of(p)
            if mach not in (0x14C, 0x8664):
                continue
            try:
                if os.path.getsize(p) > MAX_MB * 1024 * 1024:
                    continue
            except OSError:
                continue
            try:
                r = pe_imports.parse(p)
            except Exception:
                continue
            if not r:
                continue
            low = {n.lower() for n in r["imports"]} | {n.lower() for n in r["delay"]}
            g = sorted(low & set(GRAPHICS))
            if g:
                hits.append({
                    "path": p,
                    "arch": "x86" if mach == 0x14C else "x64",
                    "apis": g,
                    "direct": sorted(set(n.lower() for n in r["imports"]) & set(GRAPHICS)),
                })
    return hits


if __name__ == "__main__":
    if len(sys.argv) > 1:
        roots = [(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1)]
    else:
        roots = [
            (os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "SysWOW64"), 1),
            (os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32"), 1),
        ]

    all_hits = []
    for root, d in roots:
        if not os.path.isdir(root):
            continue
        h = scan(root, d)
        print(f"### {root}  (深度 {d}) → {len(h)} 个命中")
        all_hits += h

    print("\n==== 32 位（x86）候选 —— dx 注入需要它 ====")
    for it in sorted([x for x in all_hits if x["arch"] == "x86"], key=lambda x: x["path"]):
        print(f"  {'直接' if it['direct'] else '延迟'}  {','.join(it['apis']):28s} {it['path']}")
    print("\n==== 64 位（x64）候选 ====")
    for it in sorted([x for x in all_hits if x["arch"] == "x64"], key=lambda x: x["path"]):
        print(f"  {'直接' if it['direct'] else '延迟'}  {','.join(it['apis']):28s} {it['path']}")
