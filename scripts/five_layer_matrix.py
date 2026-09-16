# -*- coding: utf-8 -*-
"""五层覆盖矩阵：以 C-API 导出名为主键，追踪每个 API 在
契约层(include/) -> 实现层(libop/op/) -> 出口层(c_api+com) -> 绑定层(python/go)
的落地情况，并定位它调用的能力层服务。

用途：批 0「跨层硬伤台账校验」的自动化底座。缺口清单可直接作为优化输入。

用法：
    python scripts/five_layer_matrix.py [--json path] [--md path]
默认在脚本所在目录输出 five_layer_matrix.md / .json
"""
import os, re, sys, json, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # op/


def rd(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def norm(n):
    """OpFindLineExS -> FindLineExS（跨层比较用的核心名）"""
    return n[2:] if n.startswith("Op") and len(n) > 2 and n[2].isupper() else n


def build(root):
    rows = []

    # ---------- 契约层 ----------
    h_lib = rd(os.path.join(root, "include", "libop.h"))
    set_libh = set(re.findall(
        r'^\s{4}(?:void|long|int|bool|double|float|HRESULT|LONG|BSTR|std::wstring|unsigned)\s+(\w+)\s*\(',
        h_lib, re.M))

    h_capi = rd(os.path.join(root, "include", "op_c_api.h"))
    capi_names = list(dict.fromkeys(re.findall(r'OP_CALL\s+(Op\w+)\s*\(', h_capi)))
    capi_names = [x for x in capi_names if x != "OpCreate"]

    # ---------- 出口层 ----------
    c_capi = rd(os.path.join(root, "libop", "c_api", "op_c_api.cpp"))
    set_capidef = set(re.findall(r'OP_CALL\s+(Op\w+)\s*\(', c_capi))
    # 宏批量生成：OP_MOUSE_RET(OpLeftClick, LeftClick) / OP_CV_FILE_RET(OpCvToGray, CvToGray)
    set_capidef |= set(re.findall(r'^\s*[A-Z][A-Z0-9_]*\(\s*(Op\w+)\s*,', c_capi, re.M))

    h_com = rd(os.path.join(root, "libop", "com", "OpAutomation.h"))
    set_comh = set(re.findall(r'STDMETHOD\((\w+)\)', h_com))
    c_com = rd(os.path.join(root, "libop", "com", "OpAutomation.cpp"))
    set_comdef = set(re.findall(r'STDMETHODIMP\s+\w*OpAutomation::(\w+)\s*\(', c_com))

    # ---------- 实现层 op/ ----------
    impl_map = {}
    opdir = os.path.join(root, "libop", "op")
    if os.path.isdir(opdir):
        for f in os.listdir(opdir):
            if not (f.startswith("Op") and f.endswith(".cpp")):
                continue
            src = rd(os.path.join(opdir, f))
            for m in re.finditer(r'op::Op::(\w+)\s*\([^)]*\)\s*\{', src):
                body = src[m.end():m.end() + 3000]
                end = body.find("\n}\n")
                if end > 0:
                    body = body[:end]
                impl_map[norm(m.group(1))] = (f, sorted(set(re.findall(r'm_context->(\w+)', body))))

    # ---------- 绑定层 ----------
    ffi = rd(os.path.join(root, "bindings", "python", "op", "_ffi.py"))
    set_ffi = set(norm(x) for x in re.findall(r'"(Op\w+)"', ffi))

    apipy = rd(os.path.join(root, "bindings", "python", "op", "api.py"))
    set_py = set(norm(x) for x in re.findall(r'"(Op\w+)"', apipy))
    # 直接 self._dll.OpXxx(...) 的裸调用（绕过 _call_* 包装）也必须算已暴露
    set_py |= set(norm(x) for x in re.findall(r'\.(Op\w+)\s*\(', apipy))

    goset = set()
    gdir = os.path.join(root, "bindings", "go")
    if os.path.isdir(gdir):
        for f in os.listdir(gdir):
            if f.endswith(".go"):
                goset |= set(norm(x) for x in re.findall(r'Op([A-Z]\w+)', rd(os.path.join(gdir, f))))

    # ---------- 组装 ----------
    for capi in capi_names:
        core = norm(capi)
        impl_file, svc = impl_map.get(core, ("", []))
        rows.append({
            "capi": capi, "core": core,
            "libh": core in set_libh,
            "impl": core in impl_map, "impl_file": impl_file,
            "svc": ",".join(svc),
            "com": core in set_comh or core in set_comdef,
            "capi_def": capi in set_capidef,
            "ffi": core in set_ffi,
            "py": core in set_py,
            "go": core in goset,
        })
    return rows


def report(rows):
    out = []
    out.append("# 五层覆盖矩阵（主键 = C-API 导出名）")
    out.append("")
    out.append("生成方式：`python scripts/five_layer_matrix.py`（自动扫描，勿手改）")
    out.append("")
    out.append("当前接口面：c_api 声明 **%d** · 契约 libop.h **%d** 方法 · op/ 实现 **%d**"
               % (len(rows), sum(1 for r in rows if r["libh"]), sum(1 for r in rows if r["impl"])))
    out.append("")

    gaps = [
        ("op/ 实现层未转发", [r for r in rows if not r["impl"]], "契约有、实现缺 → 真断链"),
        ("c_api.cpp 无定义", [r for r in rows if not r["capi_def"]], "头文件声明但未实现（若>0 通常是宏生成未识别）"),
        ("COM 出口无对应", [r for r in rows if not r["com"]], "C-API 有、COM 没有"),
        ("python api.py 未暴露", [r for r in rows if not r["py"]], "绑定层缺口"),
        ("_ffi.py 无 ctypes 签名", [r for r in rows if not r["ffi"]], "调不通"),
        ("go 未覆盖", [r for r in rows if not r["go"]], ""),
        ("契约 libop.h 无声明", [r for r in rows if not r["libh"]], "仅 C-API 独有（生命周期/测试钩子/别名）"),
    ]
    out.append("## 缺口汇总")
    out.append("")
    out.append("| 层 | 缺口数 | 说明 |")
    out.append("|---|---|---|")
    for name, lst, note in gaps:
        out.append("| %s | %d | %s |" % (name, len(lst), note))
    out.append("")

    for name, lst, note in gaps:
        if not lst or name == "契约 libop.h 无声明":
            continue
        out.append("## 明细：%s" % name)
        out.append("")
        for r in lst:
            out.append("- `%s` (impl=%s)" % (r["capi"], r["impl_file"] or "无"))
        out.append("")

    out.append("## 明细：契约 libop.h 无声明")
    out.append("")
    out.append(", ".join("`%s`" % r["capi"] for r in rows if not r["libh"]))
    out.append("")

    svc = {}
    for r in rows:
        for s in [x for x in r["svc"].split(",") if x]:
            svc[s] = svc.get(s, 0) + 1
    out.append("## 能力层服务被调用频次（经 op/ 门面统计）")
    out.append("")
    out.append("| m_context 服务 | 承载 API 数 |")
    out.append("|---|---|")
    for k, v in sorted(svc.items(), key=lambda x: -x[1]):
        out.append("| %s | %d |" % (k, v))
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--md", default=os.path.join(HERE, "five_layer_matrix.md"))
    ap.add_argument("--json", default=os.path.join(HERE, "five_layer_matrix.json"))
    a = ap.parse_args()

    rows = build(a.root)
    with open(a.md, "w", encoding="utf-8") as fh:
        fh.write(report(rows))
    with open(a.json, "w", encoding="utf-8") as fh:
        json.dump(rows, fh, ensure_ascii=False, indent=1)
    print("rows=%d -> %s" % (len(rows), a.md))


if __name__ == "__main__":
    main()
