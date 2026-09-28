#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 C API「null-handle 契约」冒烟测试。

为什么
------
256 个 C API 里只有极少数进过 gtest（`C_API异常覆盖普查_20260928.md`）。
一旦某 API 忘了 null 检查、忘了异常护栏、或把内部负错误码透出去，
回归网是**抓不到**的——因为它压根没被调用过。

本脚本从 `include/op_c_api.h` 解析全部声明，生成
`tests/c_api_null_handle_test.cpp`：

- 凡带 `op_handle` 参数的 API，用 `nullptr` 调用，断言**失败值**
  （int/intptr → 0，const wchar_t* → 空串）
- 索引语义 API（大漠 FindPic 返回命中序号）例外：期望 `-1`
- 无 handle 的全局函数（OpVer / OpCreate / OpDestroy / OpRequestCaptureForTest …）
  单独一组，断言"不崩溃 + 契约值"

这样就给每个 C API 至少挂上一条回归，且新增 API 忘登记会立刻暴露。

用法
----
    python scripts/gen_c_api_null_handle_test.py [--out tests/c_api_null_handle_test.cpp]
"""
from __future__ import annotations

import argparse
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(REPO, "include", "op_c_api.h")

# 索引语义例外：返回值是"命中序号"而不是布尔，-1 = 未找到。
INDEX_SEMANTICS = {"OpFindPic": -1}

# 有真实副作用 / 契约已在别处钉住的函数：只调用不断言具体值
NO_ASSERT = {"OpRequestCaptureForTest"}

# 无 handle 参数但**正常应返回非空**的函数
NON_EMPTY = {"OpVer"}

# JSON 结果型 API 的失败串（op_c_api.cpp: kJsonFailure）
JSON_FAILURE = 'L"{\\"ok\\":0}"'


def json_funcs() -> set:
    """从 op_c_api.cpp 自动识别走 call_json_string 的 API：它们的失败值是 JSON 串而非空串。"""
    src = os.path.join(REPO, "libop", "c_api", "op_c_api.cpp")
    text = open(src, "r", encoding="utf-8", errors="replace").read()
    out = set()
    for m in re.finditer(r"OP_CALL\s+(Op\w+)\s*\(", text):
        window = text[m.end(): m.end() + 700]
        if "call_json_string" in window:
            out.add(m.group(1))
    return out


def split_top_level_commas(s: str) -> list:
    out, depth, last = [], 0, 0
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in "\"'":
            q = c
            i += 1
            while i < n:
                if s[i] == "\\":
                    i += 2
                    continue
                if s[i] == q:
                    break
                i += 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(s[last:i].strip())
            last = i + 1
        i += 1
    out.append(s[last:].strip())
    return [x for x in out if x]


ARRAY_RE = re.compile(r"^(.*?)\s*(\w+)\s*\[\s*(\d+)\s*\]$")


def parse_params(args: str) -> list:
    params = []
    for raw in split_top_level_commas(args):
        raw = " ".join(raw.split())
        if raw in ("void", ""):
            continue
        m = ARRAY_RE.match(raw)
        if m:
            params.append({"type": m.group(1).strip(), "name": m.group(2),
                           "array": int(m.group(3))})
            continue
        # 形如 "const wchar_t *path" / "int x1" / "op_handle handle"
        m = re.match(r"^(.*?)(\w+)$", raw)
        if not m:
            params.append({"type": raw, "name": "arg%d" % len(params), "array": 0})
            continue
        ptype = m.group(1).strip()
        pname = m.group(2)
        params.append({"type": ptype, "name": pname, "array": 0})
    return params


def arg_expr(p: dict) -> str:
    if p["array"]:
        return "g_pixel4" if "char" in p["type"] else "g_words4"
    t = p["type"]
    if t == "op_handle":
        return "nullptr"
    if "*" in t:
        if "wchar_t" in t:
            return 'L""'
        # 仅 plain char*（不是 unsigned char* / signed char*）才给空字符串
        if re.fullmatch(r"(const\s+)?char\s*\*+", t):
            return '""'
        return "nullptr"
    if "double" in t or "float" in t:
        return "0.0"
    return "0"


DECL_RE = re.compile(
    r"OP_C_API\s+([A-Za-z_][\w\s\*]*?)\s*OP_CALL\s+(\w+)\s*\((.*?)\)\s*;", re.S)


def parse_header() -> list:
    text = open(HEADER, "r", encoding="utf-8", errors="replace").read()
    # 去掉 // 行注释，避免误伤
    text = re.sub(r"//[^\n]*", "", text)
    funcs = []
    for m in DECL_RE.finditer(text):
        ret = " ".join(m.group(1).split())
        name = m.group(2)
        params = parse_params(m.group(3))
        funcs.append({"ret": ret, "name": name, "params": params})
    return funcs


def gen(funcs: list) -> str:
    handle_fns = [f for f in funcs if any(p["type"] == "op_handle" for p in f["params"])]
    free_fns = [f for f in funcs if f not in handle_fns]
    json_api = json_funcs()

    L = []
    L.append("// 本文件由 scripts/gen_c_api_null_handle_test.py 从 include/op_c_api.h 生成，**勿手改**。")
    L.append("// 改契约请改生成器或头文件后重跑：python scripts/gen_c_api_null_handle_test.py")
    L.append("//")
    L.append("// 契约：带 op_handle 的 C API 在 handle=nullptr 时必须返回**失败值**且不崩溃")
    L.append("//       （int/intptr_t → 0，const wchar_t* → 空串）；索引语义 API 例外（-1）。")
    L.append("")
    L.append('#include "op_c_api.h"')
    L.append("")
    L.append("#include <gtest/gtest.h>")
    L.append("")
    L.append("namespace {")
    L.append("")
    L.append("unsigned char g_pixel4[4] = {0, 0, 0, 0};")
    L.append("long g_words4[4] = {0, 0, 0, 0};")
    L.append("")
    L.append("void expect_empty(const wchar_t *s, const char *api) {")
    L.append("    ASSERT_NE(s, nullptr) << api << \" returned null instead of empty string\";")
    L.append("    EXPECT_EQ(s[0], L'\\0') << api << \" returned non-empty string for null handle\";")
    L.append("}")
    L.append("")
    L.append("void expect_non_empty(const wchar_t *s, const char *api) {")
    L.append("    ASSERT_NE(s, nullptr) << api;")
    L.append("    EXPECT_NE(s[0], L'\\0') << api << \" returned empty\";")
    L.append("}")
    L.append("")
    L.append("// JSON 结果型 API 的失败串：至少含 \"ok\":0。不同 API 字段数不同")
    L.append("// （如 OpYoloDetectFromFile 返回 {\"ok\":0,\"code\":-1,\"results\":[]}），")
    L.append("// 只卡 \"ok\":0 这个不变量，避免以后加字段就误报。")
    L.append("void expect_json_failure(const wchar_t *s, const char *api) {")
    L.append("    ASSERT_NE(s, nullptr) << api;")
    L.append("    EXPECT_NE(std::wstring(s).find(L\"\\\"ok\\\":0\"), std::wstring::npos) << api << \" -> \" << s;")
    L.append("}")
    L.append("")
    L.append("} // namespace")
    L.append("")

    def emit(f: dict, indent: str) -> list:
        out = []
        name = f["name"]
        args = ", ".join(arg_expr(p) for p in f["params"])
        call = f"{name}({args})"
        ret = f["ret"]
        if name in NO_ASSERT:
            out.append(f"{indent}// 有真实副作用，契约由 image_color_test 钉住，此处只保证不崩溃")
            out.append(f"{indent}(void){call};")
        elif ret in ("void",):
            out.append(f"{indent}{call};  // void：只保证不崩溃")
        elif ret in ("int", "long"):
            exp = INDEX_SEMANTICS.get(name, 0)
            note = " // 索引语义：-1=未找到" if exp == -1 else ""
            out.append(f"{indent}EXPECT_EQ({call}, {exp}) << \"{name}\";{note}")
        elif "intptr_t" in ret or "LONG_PTR" in ret or "long long" in ret:
            out.append(f"{indent}EXPECT_EQ({call}, 0) << \"{name}\";")
        elif "wchar_t" in ret and "*" in ret:
            if name in json_api:
                # JSON 结果型：失败值是 kJsonFailure（op_c_api.cpp），不是空串
                out.append(f"{indent}expect_json_failure({call}, \"{name}\");")
            elif name in NON_EMPTY:
                out.append(f"{indent}expect_non_empty({call}, \"{name}\");")
            else:
                out.append(f"{indent}expect_empty({call}, \"{name}\");")
        elif "char" in ret and "*" in ret:
            out.append(f"{indent}EXPECT_TRUE({call} != nullptr) << \"{name}\";")
        elif ret == "op_handle":
            out.append(f"{indent}EXPECT_EQ({call}, nullptr) << \"{name}\";")
        else:
            out.append(f"{indent}(void){call};  // 未知返回类型：只保证不崩溃")
        return out

    L.append(f"TEST(CApiNullHandle, AllHandleFunctionsReturnFailure) {{")
    for f in handle_fns:
        L.extend(emit(f, "    "))
    L.append("}")
    L.append("")
    L.append("TEST(CApiNullHandle, FreeFunctionsAreSafe) {")
    for f in free_fns:
        if f["name"] == "OpCreate":
            L.append("    // OpCreate 无 handle 参数：正常应拿到句柄，销毁后不得崩")
            L.append("    op_handle h = OpCreate();")
            L.append("    EXPECT_NE(h, nullptr) << \"OpCreate\";")
            L.append("    OpDestroy(h);")
            continue
        L.extend(emit(f, "    "))
    L.append("}")
    L.append("")
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "tests", "c_api_null_handle_test.cpp"))
    ap.add_argument("--check", action="store_true", help="只比对生成物是否最新，不写文件")
    args = ap.parse_args()

    funcs = parse_header()
    code = gen(funcs)
    n_handle = sum(1 for f in funcs if any(p["type"] == "op_handle" for p in f["params"]))
    print(f"解析声明 {len(funcs)} 个：带 handle {n_handle}，无 handle {len(funcs) - n_handle}")

    if args.check:
        if not os.path.exists(args.out):
            print(f"MISSING {args.out}")
            return 1
        old = open(args.out, "r", encoding="utf-8").read()
        if old != code:
            print(f"STALE {args.out} —— 头文件已变，请重跑生成器")
            return 1
        print("OK 生成物与头文件一致")
        return 0

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(code)
    print(f"已生成 {args.out}（{len(code.splitlines())} 行）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
