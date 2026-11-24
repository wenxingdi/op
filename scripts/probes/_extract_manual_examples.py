"""从生成的 C API 手册里抽出使用案例的 C 代码块，落盘待编译验证。

为什么需要：手册是「文档」，但其中案例承诺「可运行」。手写案例最容易错在
参数个数/类型/顺序（真踩过：`OpFreeString` 根本不存在、Python 的
`get_client_size` 返回 2 值而非 3 值）。所以案例必须能被机械地提出来编译一遍。

用法：python scripts/probes/_extract_manual_examples.py
输出：workbench/tmp/_manual_case<N>.c
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANUAL = os.path.join(ROOT, "docs", "2026-11", "OP_API手册_20261123.md")
OUT_DIR = os.path.join(ROOT, "workbench", "tmp")

FENCE_C = "```c"


def main() -> int:
    src = io.open(MANUAL, encoding="utf-8").read()
    os.makedirs(OUT_DIR, exist_ok=True)

    # 按 "### 案例 N" 切块，取每块里第一个 ```c 围栏
    blocks = re.split(r"(?m)^### 案例 ", src)
    extracted = []
    for block in blocks[1:]:
        num = block.split("：", 1)[0].strip()
        if not num.isdigit():
            continue
        start = block.find(FENCE_C)
        if start < 0:
            continue
        start += len(FENCE_C)
        end = block.find("```", start)
        if end < 0:
            continue
        code = block[start:end].strip("\n")
        # 片段类案例（2~6）不自带 include，统一补齐头文件以便独立编译验证。
        # 注意：这里**不能**去掉片段里已有的 `#include "op_c_api.h"` —— 去掉后
        # op_handle/OP_CALL 全都不认识，报一堆「未声明的标识符」，看起来像案例写错了。
        is_fragment = "int main(" not in code and "DWORD WINAPI worker" not in code
        if is_fragment:
            body = "\n".join("    " + ln if ln.strip() else ln for ln in code.splitlines())
            body = re.sub(r'(?m)^\s*#include\s+[<"].*[>"]\s*$\n?', "", body)
            # 不再自动推断该补哪些局部变量 —— 那套推断（判断 int x,y 逗号连声明）
            # 反复出错且越写越复杂。改为给片段提供一个**固定且完整**的外层环境：
            # 所有片段都可能用到的量一次性声明好，片段自己再声明就会 C2374 重定义，
            # 所以片段内部**约定不自带声明**（见 USAGE_EXAMPLES 的写法）。
            code = (
                '#include <windows.h>\n#include <stdio.h>\n#include "op_c_api.h"\n'
                "/* 片段编译校验外壳：提供片段约定的外层变量 */\n"
                "int main(void) {\n"
                "    op_handle h = OpCreate();\n"
                "    intptr_t hwnd = 0;\n"
                "    int x = 0, y = 0, w = 0, hgt = 0, ret = 0;\n"
                f"{body}\n"
                "    return 0;\n}\n"
            )
        elif "op_c_api.h" not in code:
            code = '#include "op_c_api.h"\n' + code
        path = os.path.join(OUT_DIR, f"_manual_case{num}.c")
        with io.open(path, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(code if code.endswith("\n") else code + "\n")
        extracted.append(path)
        print("[extracted]", os.path.relpath(path, ROOT))

    if not extracted:
        print("[FAIL] 手册里没找到任何 ```c 案例", file=sys.stderr)
        return 1
    print(f"共导出 {len(extracted)} 个 C 案例")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
