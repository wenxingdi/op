# -*- coding: utf-8 -*-
"""A/B 跑任意 op_test.exe（含备份件），用于判定"新失败是否本轮改动引入"。

用法：
    python workbench/_t_ab_run.py <exe路径> <dll目录> ["过滤器"]

判定原则：同一台机器状态、同一时刻跑新旧两个二进制，失败集合一致 ⇒ 环境/抖动；
只有新二进制多出的失败才是回归。
"""
import os
import subprocess
import sys

REPO = r"D:\AutoPro\op-master\op"

exe = sys.argv[1]
dll_dir = sys.argv[2]
flt = sys.argv[3] if len(sys.argv) > 3 else "*"

env = os.environ.copy()
env["PATH"] = dll_dir + ";" + env["PATH"]

# cwd 必须是仓库根：OpenCvTest 的 FindDemoAsset 以 current_path() 找 assets/
proc = subprocess.run([exe, "--gtest_filter=" + flt], cwd=REPO, env=env,
                      capture_output=True, text=True, encoding="utf-8", errors="replace")
out = (proc.stdout or "") + (proc.stderr or "")
tag = os.path.basename(os.path.dirname(exe)) or "cur"
out_path = os.path.join(REPO, "workbench", f"_t_ab_{tag}.txt")
with open(out_path, "w", encoding="utf-8") as f:
    f.write(out)

fails = [ln for ln in out.splitlines() if ln.startswith("[  FAILED  ]") and "listed below" not in ln]
skips = [ln for ln in out.splitlines() if ln.startswith("[  SKIPPED  ]")]
passed = [ln for ln in out.splitlines() if ln.startswith("[  PASSED  ]")]
print("EXE =", exe)
print("OUT =", out_path)
for ln in passed + skips + fails:
    print(ln)
print("EXIT =", proc.returncode)
