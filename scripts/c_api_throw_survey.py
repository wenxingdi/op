# -*- coding: utf-8 -*-
"""H14 可抛点普查：枚举 op_c_api.cpp 的全部 C 导出，分类其异常护栏状态。

分类口径：
  guarded_helper  —— 函数体走 call_int/call_ret/call_intptr/call_string/
                     call_json_string/call_memory（这些 helper 内部都有 try）
  guarded_own     —— 函数体自带 try/catch
  UNGUARDED       —— 既无自有 try，也不走 helper  → 异常会穿 extern "C" 边界

用法： python c_api_throw_survey.py [--json out.json]
"""
import json
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "libop" / "c_api" / "op_c_api.cpp"

HELPERS = (
    "call_int(", "call_ret(", "call_intptr(", "call_string(",
    "call_json_string(", "call_memory(",
)

# 在函数体内出现即可能抛（bad_alloc / invalid_argument / out_of_range …）
THROW_TOKENS = re.compile(
    r"\bstd::(stoi|stol|stoul|stoll|stod|stof|stoi|to_string|to_wstring|"
    r"wstring|string|vector|map|unordered_map|stoi)\b"
    r"|\.at\(|\.substr\(|\.resize\(|\.reserve\(|\[[^\]]*\]\s*=|new \w"
    r"|std::dynamic_pointer_cast|dynamic_cast<"
)

RE_SIG = re.compile(r"^(?P<ret>[A-Za-z_][\w:<>\*&\s]*?)\s+OP_CALL\s+(?P<name>Op\w+)\s*\(")
RE_ARGS = re.compile(r"^[A-Za-z_][\w:<>\*&\s]*?\s+(?P<name>Op\w+)\s*\(")  # 无 OP_CALL 的覆写/别名
RE_CALL_OP = re.compile(r"\bop\.[A-Za-z_]\w*\s*\(")
RE_HANDLE_OP = re.compile(r"\bhandle->op\.[A-Za-z_]\w*\s*\(")


def collect_macros(text):
    """收集 #define NAME(...) <body>（含反斜杠续行），返回 {NAME: (line_no, body)}。

    必要性：op_c_api.cpp 用 OP_MOUSE_RET / OP_CV_FILE_RET 这类宏批量生成导出，
    展开点文本里**不含** "OP_CALL"，只按字面扫会漏掉这些导出（首版就漏了）。
    """
    lines = text.splitlines()
    macros = {}
    i = 0
    while i < len(lines):
        m = re.match(r"#define\s+(\w+)\s*\(([^)]*)\)\s*(.*)", lines[i])
        if m:
            name = m.group(1)
            body = m.group(3)
            j = i
            while body.rstrip().endswith("\\") and j + 1 < len(lines):
                j += 1
                body += "\n" + lines[j]
            macros[name] = (i + 1, body)
            i = j
        i += 1
    return macros


def expand_macro_rows(text, macros):
    """把宏展开点也当成函数行，body 用宏定义体（护栏分类看的就是宏体）。"""
    rows = []
    for name, (dline, body) in macros.items():
        if "OP_CALL" not in body:
            continue
        for idx, line in enumerate(text.splitlines()):
            if re.match(r"^\s*" + re.escape(name) + r"\s*\(", line):
                rows.append((idx + 1, line.strip().rstrip("/").strip(), body, name, dline))
    return rows


def split_functions(text):
    """按顶层 '}' 归零切分函数块，返回 [(start_line, name, body)]。"""
    lines = text.splitlines()
    funcs = []
    i = 0
    n = len(lines)
    while i < n:
        m = RE_SIG.match(lines[i]) or RE_ARGS.match(lines[i])
        if m and "OP_CALL" in lines[i]:
            name = m.group("name")
            # 找签名结束（')' + '{'）
            j = i
            while j < n and "{" not in lines[j]:
                j += 1
            if j >= n:
                break
            depth = 0
            body = []
            k = j
            while k < n:
                depth += lines[k].count("{") - lines[k].count("}")
                body.append(lines[k])
                if depth <= 0:
                    break
                k += 1
            funcs.append((i + 1, name, "\n".join(body)))
            i = k + 1
        else:
            i += 1
    return funcs


def classify(body):
    if "try" in re.sub(r"//.*", "", body):
        return "guarded_own", []
    hits = [h for h in HELPERS if h in body]
    if hits:
        return "guarded_helper", hits
    return "UNGUARDED", []


def main():
    text = SRC.read_text(encoding="utf-8", errors="replace")
    funcs = split_functions(text)
    rows = []
    for line, name, body in funcs:
        kind, hits = classify(body)
        toks = sorted(set(THROW_TOKENS.findall(body)))
        # 是否直接调用 op::Op 方法（其内部异常一律外泄）
        opcalls = len(RE_CALL_OP.findall(body)) + len(RE_HANDLE_OP.findall(body))
        rows.append({
            "line": line, "name": name, "kind": kind,
            "helper": hits, "throw_tokens": [t for t in toks if t],
            "op_calls": opcalls,
            "body_lines": body.count("\n") + 1,
        })
    macros = collect_macros(text)
    macro_rows = expand_macro_rows(text, macros)
    for line, name, body, macro, dline in macro_rows:
        kind, hits = classify(body)
        toks = sorted(set(THROW_TOKENS.findall(body)))
        rows.append({
            "line": line, "name": name, "kind": kind,
            "helper": hits, "throw_tokens": [t for t in toks if t],
            "op_calls": len(RE_CALL_OP.findall(body)) + len(RE_HANDLE_OP.findall(body)),
            "body_lines": body.count("\n") + 1,
            "via_macro": "%s (L%d)" % (macro, dline),
        })

    summary = {}
    for r in rows:
        summary[r["kind"]] = summary.get(r["kind"], 0) + 1

    print("=== 导出总数：%d（其中宏生成 %d）===" % (len(rows), len(macro_rows)))
    for k in ("guarded_helper", "guarded_own", "UNGUARDED"):
        print("  %-16s %d" % (k, summary.get(k, 0)))
    un = [r for r in rows if r["kind"] == "UNGUARDED"]
    print("\n=== UNGUARDED 明细（%d 条）===" % len(un))
    for r in un:
        tok = ",".join(r["throw_tokens"]) or "-"
        print("  L%-5d %-34s op_calls=%-3d tokens=%s" % (r["line"], r["name"], r["op_calls"], tok))
    if "--json" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--json") + 1])
        out.write_text(json.dumps({"summary": summary, "rows": rows}, ensure_ascii=False, indent=2),
                       encoding="utf-8")
        print("\nJSON -> %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
