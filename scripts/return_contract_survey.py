#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""出口层返回值契约普查。

背景
----
对外契约（C API / COM / Python / 大漠语义）：**0 = 失败，非 0 = 成功**。
宿主侧 `_ok()` 用 `if value:` 判定，因此内部私有错误码（-1 ~ -7 之类）
一旦透传到返回值，就会被当成「成功」。

本脚本扫 `libop/op/*.cpp` 里的 `internal::set_result(target, expr)` 调用点，
把 `expr` 分类，并对**透传型**（直接把能力层函数返回值写进对外返回值）
回溯能力层函数定义，看它是否存在 `return -N` 路径。

分类
----
- LITERAL      : expr 是数字字面量（-1L 之类会被单独标红）
- TERNARY      : 三元/比较表达式（人工看分支值是否为负）
- PASSTHROUGH  : 直接函数调用（本脚本重点，回溯定义查负返回）
- VAR / OTHER  : 变量或其它表达式

用法
----
    python scripts/return_contract_survey.py                 # 打印摘要
    python scripts/return_contract_survey.py --md out.md     # 落 markdown
    python scripts/return_contract_survey.py --json out.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBOP = os.path.join(REPO, "libop")
OP_DIR = os.path.join(LIBOP, "op")


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def match_paren(text: str, open_idx: int) -> int:
    """给定 '(' 的下标，返回配对 ')' 的下标；跳过字符串/字符字面量与注释。"""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def match_brace(text: str, open_idx: int) -> int:
    """给定 '{' 的下标，返回配对 '}' 的下标。"""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_top_level_commas(s: str) -> list:
    out, depth, last = [], 0, 0
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if s[i] == "\\":
                    i += 2
                    continue
                if s[i] == quote:
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
    return out


CALLEE_RE = re.compile(r"([A-Za-z_]\w*)\s*\(")


def collect_calls(expr: str) -> list:
    """取表达式里出现的所有调用名（去掉控制关键字）。"""
    skip = {"if", "for", "while", "switch", "sizeof", "static_cast", "reinterpret_cast",
            "const_cast", "dynamic_cast", "return"}
    names = []
    for m in CALLEE_RE.finditer(expr):
        nm = m.group(1)
        if nm not in skip:
            names.append(nm)
    return names


# ---------------------------------------------------------------- 能力层定义回溯

class DefIndex:
    """在 libop 下建立「函数名 -> 定义位置」索引（粗粒度，够用即可）。"""

    def __init__(self) -> None:
        self.funcs = {}  # name -> list[(file, idx)]
        self.cache = {}
        for root, _dirs, files in os.walk(LIBOP):
            for fn in files:
                if not fn.endswith((".cpp", ".h", ".hpp", ".inl")):
                    continue
                p = os.path.join(root, fn)
                text = self._load(p)
                # 定义形如  [RetType] [Class::]name(  ...  ) ... {
                for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{", text):
                    name = m.group(1)
                    line_prev = text.rfind("\n", 0, m.start()) + 1
                    # 简单过滤：前面必须是标识符/类型或 ::（排除 if( 之类已在 collect 里跳过）
                    self.funcs.setdefault(name, []).append((p, m.start(), line_prev))

    def _load(self, path: str) -> str:
        if path not in self.cache:
            self.cache[path] = read(path)
        return self.cache[path]

    def body(self, file: str, idx: int) -> str:
        text = self._load(file)
        open_paren = text.index("(", idx)
        close_paren = match_paren(text, open_paren)
        if close_paren < 0:
            return ""
        brace = text.find("{", close_paren)
        if brace < 0:
            return ""
        end = match_brace(text, brace)
        return text[brace:end] if end > 0 else text[brace:]

    def neg_returns(self, name: str) -> list:
        """返回 [(file, line, snippet)]，列出该函数定义体里的 `return -`。"""
        out = []
        for (file, idx, line_start) in self.funcs.get(name, []):
            body = self.body(file, idx)
            if not body:
                continue
            for m in re.finditer(r"return\s+-\s*\w+", body):
                off = body[: m.start()].count("\n")
                snippet = body.splitlines()[off].strip() if off < len(body.splitlines()) else ""
                out.append({
                    "file": os.path.relpath(file, REPO).replace("\\", "/"),
                    "line": self._load(file)[: idx].count("\n") + 1 + off + 1,
                    "snippet": snippet,
                })
        return out


# ---------------------------------------------------------------- set_result 扫描

SET_RESULT_RE = re.compile(r"(?:internal::)?set_result\s*\(")


def scan_file(path: str, index: DefIndex) -> list:
    text = read(path)
    rel = os.path.relpath(path, REPO).replace("\\", "/")
    rows = []
    for m in SET_RESULT_RE.finditer(text):
        open_paren = text.index("(", m.start())
        close_paren = match_paren(text, open_paren)
        if close_paren < 0:
            continue
        args = split_top_level_commas(text[open_paren + 1: close_paren])
        if len(args) < 2:
            continue
        target, expr = args[0], " ".join(args[1:])
        line = text[: m.start()].count("\n") + 1
        flat = " ".join(expr.split())

        if re.fullmatch(r"-?\d+[ULull]*", flat):
            kind = "LITERAL"
        elif "?" in expr:
            kind = "TERNARY"
        elif "(" in expr:
            kind = "PASSTHROUGH"
        else:
            kind = "OTHER"

        callees = []
        if kind in ("PASSTHROUGH", "OTHER", "TERNARY"):
            for nm in dict.fromkeys(collect_calls(expr)):
                negs = index.neg_returns(nm)
                if negs:
                    callees.append({"name": nm, "neg_returns": negs[:6]})

        rows.append({
            "file": rel,
            "line": line,
            "target": target,
            "expr": flat[:160],
            "kind": kind,
            "callees": callees,
        })
    return rows


def enclosing_function(text: str, pos: int) -> dict:
    """找出 pos 所属的最内层函数定义（返回 {name, line}）。粗粒度但够用。"""
    best = None
    for m in re.finditer(r"([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{", text):
        if m.start() > pos:
            break
        open_paren = text.index("(", m.start())
        close_paren = match_paren(text, open_paren)
        brace = text.find("{", close_paren)
        if brace < 0:
            continue
        end = match_brace(text, brace)
        if end >= pos:
            best = {"name": m.group(1), "line": text[: m.start()].count("\n") + 1}
    return best or {"name": "?", "line": 0}


def negative_sites(root: str) -> list:
    """扫 root 下全部 `return -N`，并反推所属函数。"""
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            if not fn.endswith((".cpp", ".h", ".hpp", ".inl")):
                continue
            p = os.path.join(dirpath, fn)
            text = read(p)
            for m in re.finditer(r"return\s+-\s*\w+", text):
                enc = enclosing_function(text, m.start())
                out.append({
                    "file": os.path.relpath(p, REPO).replace("\\", "/"),
                    "line": text[: m.start()].count("\n") + 1,
                    "snippet": " ".join(m.group(0).split()),
                    "func": enc["name"],
                    "func_line": enc["line"],
                })
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--md")
    ap.add_argument("--json")
    ap.add_argument("--trace-neg", action="store_true",
                    help="反向模式：从能力层 return -N 出发，找被出口层透传的点")
    args = ap.parse_args()

    if args.trace_neg:
        sites = negative_sites(LIBOP)
        op_text = {}
        for fn in sorted(os.listdir(OP_DIR)):
            if fn.endswith(".cpp"):
                op_text[os.path.join(OP_DIR, fn)] = read(os.path.join(OP_DIR, fn))
        # 出口层里出现同名调用的行
        hits = []
        for s in sites:
            name = s["func"]
            if name in ("?", ""):
                continue
            for path, text in op_text.items():
                for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
                    line_no = text[: m.start()].count("\n") + 1
                    line = text.splitlines()[line_no - 1].strip()
                    if "set_result" in line:
                        hits.append({
                            "neg_func": name,
                            "neg_site": f"{s['file']}:{s['line']} ({s['snippet']})",
                            "exit_site": f"{os.path.relpath(path, REPO).replace(chr(92), '/')}:{line_no}",
                            "exit_line": line[:160],
                        })
        print(f"能力层 return -N 站点：{len(sites)}；其中被 op 层 set_result 直接透传：{len(hits)}")
        print()
        by_func = {}
        for s in sites:
            by_func.setdefault(s["func"], []).append(s)
        print("| 函数 | 负返回站点 | 是否被出口层透传 |")
        print("|---|---|---|")
        for name, ss in sorted(by_func.items()):
            exit_sites = [h for h in hits if h["neg_func"] == name]
            flag = "**是** " + "; ".join(h["exit_site"] for h in exit_sites[:4]) if exit_sites else "否"
            sites_txt = "; ".join(f"{x['file'].split('/')[-1]}:{x['line']}" for x in ss[:4])
            print(f"| `{name}` | {sites_txt} | {flag} |")
        return 0

    index = DefIndex()
    rows = []
    for fn in sorted(os.listdir(OP_DIR)):
        if fn.endswith(".cpp"):
            rows.extend(scan_file(os.path.join(OP_DIR, fn), index))

    risky = [r for r in rows if r["callees"]]
    neg_literal = [r for r in rows if r["kind"] == "LITERAL" and
                   re.match(r"-\s*\d", r["expr"])]

    summary = {
        "total_set_result": len(rows),
        "risky_passthrough": len(risky),
        "negative_literal": len(neg_literal),
    }

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"summary": summary, "risky": risky,
                       "negative_literal": neg_literal, "all": rows}, f,
                      ensure_ascii=False, indent=2)

    lines = ["# 出口层返回值契约普查", ""]
    lines.append(f"- set_result 调用点：**{len(rows)}**")
    lines.append(f"- 透传且被调方存在 `return -N`：**{len(risky)}**（重点）")
    lines.append(f"- 直接写负字面量：**{len(neg_literal)}**")
    lines.append("")
    lines.append("## 一、透传型风险点（被调方存在负返回）")
    lines.append("")
    lines.append("| 文件:行 | 写入 | 表达式 | 被调方负返回 |")
    lines.append("|---|---|---|---|")
    for r in risky:
        neg = "; ".join(f"`{c['name']}`→{c['neg_returns'][0]['file']}:{c['neg_returns'][0]['line']}"
                        for c in r["callees"])
        lines.append(f"| {r['file']}:{r['line']} | `{r['target']}` | `{r['expr']}` | {neg} |")
    lines.append("")
    lines.append("## 二、直接写负字面量")
    lines.append("")
    lines.append("| 文件:行 | 写入 | 值 |")
    lines.append("|---|---|---|")
    for r in neg_literal:
        lines.append(f"| {r['file']}:{r['line']} | `{r['target']}` | `{r['expr']}` |")

    md = "\n".join(lines) + "\n"
    if args.md:
        os.makedirs(os.path.dirname(os.path.abspath(args.md)), exist_ok=True)
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(md)
    print(json.dumps(summary, ensure_ascii=False))
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
