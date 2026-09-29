#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""补同步「还在用的运行时目录」里残留的旧 DLL（工作区脚本，不入库）。

_sync_release_b2.py 只覆盖 6 处主副本；全盘扫（`scan_release_copies.py`）会发现
6 还有若干**仍在使用的运行时目录**停留在旧版本 —— 「修复没生效」的头号嫌疑就是它们。

本次补：OPTool\\Common\\bin（OPTool 运行输出目录）+ optool_verify 的 exe 同目录。
一次性探针目录（op_audit / portable_probe / wordcut_verify）已完成使命，不同步。
外来项目（COP / PPOCR_v6_ncnn）不动。

⚠ 2026-09-29：这些目录**原本没有 x86 hook dll**（x64 宿主绑 32 位窗口走 dx 注入时才有用），
   故 x86 分支改为「源存在就补建」，x64 分支保持「只刷新已存在」（不无谓污染目录）。
"""
import hashlib
import os
import shutil

SRC_X64 = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
SRC_X86 = r"D:\AutoPro\op-master\op\build\ninja-x86-Release\libop"

TARGETS = [
    r"D:\AutoPro\OPTool\Common\bin\Release\net10.0-windows7.0\Dll",
    r"D:\AgentWork\WorkBuddy\2026-08-04-11-41-10\optool_verify\bin\Release\net10.0-windows7.0",
]
# x64：仅刷新已存在的旧副本；x86：源存在即补建（跨位数 dx 注入必需）。
X64_REFRESH = ["op_x64.dll", "op_c_api_x64.dll"]
X86_ADD = ["op_x86.dll", "op_c_api_x86.dll"]


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()[:12]


ok = True
for t in TARGETS:
    if not os.path.isdir(t):
        print(f"SKIP（目录不存在）{t}")
        continue
    print("===", t)
    # x64：只刷新已存在
    for n in X64_REFRESH:
        src = os.path.join(SRC_X64, n)
        dst = os.path.join(t, n)
        if not os.path.exists(dst):
            print(f"  {n}: 目标不存在，跳过")
            continue
        shutil.copy2(src, dst)
        s_src, s_dst = sha1(src), sha1(dst)
        flag = "OK" if s_src == s_dst else "MISMATCH"
        if flag != "OK":
            ok = False
        print(f"  {n}: {flag} src={s_src} dst={s_dst}")
    # x86：源存在即补建/刷新
    for n in X86_ADD:
        src = os.path.join(SRC_X86, n)
        if not os.path.exists(src):
            print(f"  {n}: 源缺失，跳过")
            continue
        dst = os.path.join(t, n)
        shutil.copy2(src, dst)
        s_src, s_dst = sha1(src), sha1(dst)
        flag = "OK" if s_src == s_dst else "MISMATCH"
        if flag != "OK":
            ok = False
        print(f"  {n}: {flag} src={s_src} dst={s_dst}")

print("\n结论:", "全部一致" if ok else "存在不一致")
