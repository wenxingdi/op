# -*- coding: utf-8 -*-
"""三层 API 面对照：COM(OpAutomation.h) / C-API(op_c_api.h) / Python(api.py)"""
import re, json, io, sys
from pathlib import Path

ROOT = Path(r"D:\AutoPro\op-master\op")
OUT = Path(r"D:\AgentWork\WorkBuddy\2026-08-04-11-41-10\_api_surface.json")

def read(p):
    return p.read_text(encoding="utf-8", errors="replace")

# --- 1. COM: OpAutomation.h -> STDMETHOD(Name)(...)
com_src = read(ROOT / "libop/com/OpAutomation.h")
com = []
for m in re.finditer(r"STDMETHOD\((\w+)\)\s*\(", com_src):
    com.append(m.group(1))

# --- 2. C-API: op_c_api.h -> OP_C_API <type> OP_CALL OpXxx(
capi_src = read(ROOT / "include/op_c_api.h")
capi = []
for m in re.finditer(r"OP_C_API\s+[^;\n]*?OP_CALL\s+(Op\w+)\s*\(", capi_src):
    capi.append(m.group(1))

# --- 3. Python: api.py -> def name(
py_src = read(ROOT / "bindings/python/op/api.py")
py_ms = []
for m in re.finditer(r"^\s{4}def\s+(\w+)\s*\(", py_src, re.M):
    py_ms.append(m.group(1))
# 属性/私有过滤
py_pub = [n for n in py_ms if not n.startswith("_")]

com_set, capi_set, py_set = set(com), set(capi), set(py_pub)

def norm(n):
    # CapName <-> snake_case
    return re.sub(r"[^a-z0-9]", "", n.lower())

# python 方法名多为 snake_case，转驼峰后再比
def to_camel(s):
    parts = re.split(r"[_]+", s)
    return parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])
# 但 api.py 也可能直接是驼峰；两种都试
py_camel = {}
for n in py_pub:
    py_camel.setdefault(norm(to_camel(n)), set()).add(n)
    py_camel.setdefault(norm(n), set()).add(n)

com_norm = {norm(n): n for n in com}
capi_norm = {re.sub(r"^op", "", norm(n)): n for n in capi}

res = {
    "counts": {"COM": len(com), "CAPI": len(capi), "PythonPublic": len(py_pub)},
    "com_only": [],       # COM 有，C-API 无
    "capi_only": [],      # C-API 有，COM 无
    "py_only": [],        # Python 有，C-API 无
    "capi_no_py": [],     # C-API 有，Python 无
}

for n in sorted(com):
    key = norm(n)
    ck = re.sub(r"^op", "", key)
    if ck not in capi_norm:
        res["com_only"].append(n)
for n in sorted(capi):
    key = re.sub(r"^op", "", norm(n))
    if key not in com_norm:
        res["capi_only"].append(n)
    if key not in py_camel:
        res["capi_no_py"].append(n)
for n in sorted(py_pub):
    key = norm(to_camel(n))
    if key not in capi_norm and norm(n) not in {norm(x) for x in capi} and key not in {
        re.sub(r"^op", "", k) for k in capi_norm}:
        if key not in capi_norm:
            res["py_only"].append(n)

OUT.write_text(json.dumps(res, ensure_ascii=False, indent=2), encoding="utf-8")
print("counts:", res["counts"])
for k in ("com_only", "capi_only", "capi_no_py"):
    print(f"\n--- {k} ({len(res[k])}) ---")
    print(", ".join(res[k][:80]))
print(f"\n--- py_only ({len(res['py_only'])}) ---")
print(", ".join(res["py_only"][:80]))
print("\nwritten:", OUT)
