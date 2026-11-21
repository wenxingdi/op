# -*- coding: utf-8 -*-
"""生成 bindings/go/extra_windows.go：补齐 C API 已有、go 绑定缺失的接口。

背景：go 绑定覆盖率不到 2/3（`five_layer_matrix.py` 报 127 个未覆盖），
而且当时**根本编不过**（`procFindColorBlockExS` / `procFindLineExS` 只在
bindProcs 里赋值、var 块里没声明）。本脚本按 op_c_api.h 的签名机械生成，
再由 `go build` 兜底验证（这是唯一可信的检查方式）。

规则（映射自既有的 *_windows.go 写法）：
    int              -> int        / uintptr(x)
    intptr_t         -> uintptr    / x
    long long        -> int64      / uintptr(x)
    double           -> float64    / f64Arg(x)
    const wchar_t *  -> string     / strArg(x)
    const void */void * + size -> []byte / bytesPtr(b), uintptr(len(b))
    int *X           -> 追加 out int（int32 局部变量）
返回：
    int              -> int
    const wchar_t *  -> string (wcharString)
    void             -> 无返回值

用法：python scripts/gen_go_bindings.py
"""
from __future__ import annotations

import io
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "op_c_api.h")
GO_DIR = os.path.join(ROOT, "bindings", "go")
OUT = os.path.join(GO_DIR, "extra_windows.go")

GO_KEYWORDS = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough",
    "for", "func", "go", "goto", "if", "import", "interface", "map", "package", "range",
    "return", "select", "struct", "switch", "type", "var",
}

# 不生成：生命周期/测试钩子（go 侧要么已绑定，要么不该暴露）
SKIP = {"OpCreate", "OpDestroy", "OpVer", "OpRequestCaptureForTest"}


def parse_header():
    src = io.open(HEADER, encoding="utf-8", errors="replace").read()
    flat = " ".join(src.replace("\\\n", " ").split())
    decls = {}
    for part in flat.split(";"):
        m = re.search(r"OP_C_API\s+(.+?)\s*OP_CALL\s+(Op\w+)\s*\((.*)\)\s*$", part.strip())
        if m:
            decls[m.group(2)] = (" ".join(m.group(1).split()), " ".join(m.group(3).split()))
    return decls


def parse_params(args_c: str):
    """把 C 形参串拆成 [(ctype, name)]；ct 里带 "*" 表示指针。"""
    out = []
    for p in (x.strip() for x in args_c.split(",")):
        if not p or p == "void":
            continue
        m = re.match(r"^(.*?)\s*(\*)?\s*(\w+)\s*(\[\s*\d+\s*\])?$", p)
        if not m:
            raise SystemExit("cannot parse param: %r" % p)
        base = re.sub(r"\s+", " ", m.group(1)).strip()
        is_ptr = bool(m.group(2)) or bool(m.group(4))
        ctype = base + (" *" if is_ptr else "")
        out.append((ctype, m.group(3)))
    return out


def go_name(cname: str) -> str:
    return cname[2:] if cname.startswith("Op") else cname


def safe_param(name: str) -> str:
    return name + "V" if name in GO_KEYWORDS else name


def camel(name: str) -> str:
    """x1 / offset_x -> x1 / offsetX"""
    head, *rest = name.split("_")
    return head + "".join(w[:1].upper() + w[1:] for w in rest if w)


def build(cname, cret, cargs):
    params = parse_params(cargs)
    params = [(t, n) for (t, n) in params if n != "handle"]

    go_params, call_args, out_vars, out_returns = [], [], [], []
    i = 0
    while i < len(params):
        ctype, name = params[i]
        pname = safe_param(camel(name))
        low = ctype.lower()
        if "wchar_t" in ctype:
            go_params.append("%s string" % pname)
            call_args.append("strArg(%s)" % pname)
        elif ("void" in ctype or "char" in ctype) and "*" in ctype:
            # 下一个参数是 size
            size_name = None
            if i + 1 < len(params) and "int" in params[i + 1][0]:
                size_name = params[i + 1][1]
                i += 1
            go_params.append("%s []byte" % pname)
            call_args.append("bytesPtr(%s)" % pname)
            call_args.append("uintptr(len(%s))" % pname)
        elif "double" in ctype and "*" not in ctype:
            go_params.append("%s float64" % pname)
            call_args.append("f64Arg(%s)" % pname)
        elif "long long" in ctype:
            go_params.append("%s int64" % pname)
            call_args.append("uintptr(%s)" % pname)
        elif "intptr_t" in ctype:
            go_params.append("%s uintptr" % pname)
            call_args.append(pname)
        elif "double" in ctype and "*" in ctype:
            v = pname + "Out"
            out_vars.append((v, "float64"))
            call_args.append("float64Ptr(&%s)" % v)
            out_returns.append(("float64", "float64(%s)" % v))
        elif "float" in ctype and "*" in ctype:
            v = pname + "Out"
            out_vars.append((v, "float32"))
            call_args.append("float32Ptr(&%s)" % v)
            out_returns.append(("float32", "float32(%s)" % v))
        elif "int" in low and "*" in ctype:
            v = pname + "Out"
            out_vars.append((v, "int32"))
            call_args.append("int32Ptr(&%s)" % v)
            out_returns.append(("int", "int(%s)" % v))
        elif "int" in low or low.startswith("long"):
            go_params.append("%s int" % pname)
            call_args.append("uintptr(%s)" % pname)
        else:
            raise SystemExit("unmapped param type %r in %s" % (ctype, cname))
        i += 1

    rets = []
    tail = ""
    uses_ret = False
    out_convs = [conv for _t, conv in out_returns]
    if "wchar_t" in cret:
        uses_ret = True
        rets.append("string")
        tail = "return wcharString(ret)" + ("" if not out_convs else ", " + ", ".join(out_convs))
    elif cret.strip() == "intptr_t":
        uses_ret = True
        rets.append("int64")
        tail = "return int64(ret)" + ("" if not out_convs else ", " + ", ".join(out_convs))
    elif cret.strip() == "int":
        uses_ret = True
        rets.append("int")
        tail = "return int(ret)" + ("" if not out_convs else ", " + ", ".join(out_convs))
    else:
        tail = "return " + ", ".join(out_convs) if out_convs else ""
    rets += [t for t, _c in out_returns]
    return go_params, call_args, out_vars, rets, tail, uses_ret


def main() -> int:
    decls = parse_header()
    bound = set()
    for fn in os.listdir(GO_DIR):
        # 跳过本脚本的产物：否则第二次运行会把上次生成的当成"已绑定"，生成 0 个方法
        if fn.endswith(".go") and os.path.join(GO_DIR, fn) != OUT:
            text = io.open(os.path.join(GO_DIR, fn), encoding="utf-8", errors="replace").read()
            bound.update(re.findall(r'NewProc\("(Op\w+)"\)', text))

    todo = [(n, r, a) for n, (r, a) in sorted(decls.items()) if n not in bound and n not in SKIP]

    methods, proc_decls, proc_inits = [], [], []
    for cname, cret, cargs in todo:
        gname = go_name(cname)
        go_params, call_args, out_vars, rets, tail, uses_ret = build(cname, cret, cargs)
        proc = "proc" + gname
        proc_decls.append(proc)
        proc_inits.append('\t%s = dll.NewProc("%s")' % (proc, cname))

        sig = ", ".join(go_params)
        ret_sig = ""
        if len(rets) == 1:
            ret_sig = " " + rets[0]
        elif len(rets) > 1:
            ret_sig = " (" + ", ".join(rets) + ")"

        body = []
        if rets:
            zeros = ", ".join('""' if r == "string" else "0" for r in rets)
            body.append("\tif !o.valid() {")
            body.append("\t\treturn " + zeros)
            body.append("\t}")
        else:
            body.append("\tif !o.valid() {")
            body.append("\t\treturn")
            body.append("\t}")
        if out_vars:
            for v, t in out_vars:
                body.append("\tvar %s %s" % (v, t))
        call = "\t%s%s.Call(o.handle%s)" % (
            "ret, _, _ := " if uses_ret else "", proc, "".join(", " + a for a in call_args))
        body.append(call)
        if tail:
            body.append("\t" + tail)

        methods.append(
            "// %s 由 scripts/gen_go_bindings.py 生成（对应 C API %s）。\n"
            "func (o *Op) %s(%s)%s {\n%s\n}\n" % (gname, cname, gname, sig, ret_sig, "\n".join(body))
        )

    header = '''//go:build windows

// 代码生成，勿手改：python scripts/gen_go_bindings.py
//
// 补齐 C API 已有、go 绑定此前缺失的接口（five_layer_matrix.py 报 127 个未覆盖）。
// 生成后必须 `go build ./...` 验证 —— 类型不对编译器当场就报。

package opcapi

import (
	"golang.org/x/sys/windows"
)

var (
%s
	// 以下两个此前只在 bindProcs 里赋值、var 块漏了声明，导致整包编不过（2026-11-21 修）。
	procFindColorBlockExS *windows.LazyProc
	procFindLineExS        *windows.LazyProc
)

func bindExtraProcs() {
%s
}

''' % ("\n".join("\t%s *windows.LazyProc" % p for p in proc_decls), "\n".join(proc_inits))

    io.open(OUT, "w", encoding="utf-8", newline="\n").write(header + "\n".join(methods))
    print("generated %d methods -> %s" % (len(methods), OUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
