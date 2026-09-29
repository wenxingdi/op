#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跑 op_test.exe（工作区脚本，不入库）。

Git Bash 里 `PATH=...:$PATH ./op_test.exe` 的 PATH 转换不可靠（会报
`op_c_api_x64.dll: cannot open shared object file`），PowerShell 直管道也拿不到输出。
统一用 Python 拼 Windows 路径 + cwd=仓库根，输出落文件再读。

用法：
    python workbench/run_optest.py                      # 全量
    python workbench/run_optest.py --filter CApiNullHandle.*
"""
import os
import subprocess
import sys

REPO = r"D:\AutoPro\op-master\op"
BUILD = os.path.join(REPO, "build", "nmake-x64-Release")
EXE = os.path.join(BUILD, "tests", "op_test.exe")
DLL_DIR = os.path.join(BUILD, "libop")

args = [EXE]
if len(sys.argv) > 1 and sys.argv[1] == "--filter":
    args.append("--gtest_filter=" + sys.argv[2])

env = os.environ.copy()
env["PATH"] = DLL_DIR + ";" + env["PATH"]

proc = subprocess.run(args, cwd=REPO, env=env, capture_output=True, text=True,
                      encoding="utf-8", errors="replace")
out = (proc.stdout or "") + (proc.stderr or "")

# 全量输出落文件（cwd 必须是仓库根，否则 OpenCvTest 的 assets 相对路径找不到 →
# 静默 SKIP 6~7 条，看起来像"环境缺资源"）。print 只留尾部避免刷屏。
full_out = os.path.join(REPO, "workbench", "_last_optest.txt")
with open(full_out, "w", encoding="utf-8") as f:
    f.write(out)
print(out[-8000:])
print("FULL_OUT=", full_out)
print("EXIT=", proc.returncode)
