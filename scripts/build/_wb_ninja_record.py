"""ninja 编译器包装（launcher 版）：把编译器输出落到日志文件，然后原样转发退出码。

背景（DSH 沙箱下的构建系统坑，2026-11-21）：
  - 沙箱下 ninja 无法把子进程输出接进自己的管道：一旦有编译动作要跑，ninja 就卡在
    等待子进程 I/O 上（实测 CPU 0.06s、线程 WaitReason=15、从不 spawn cl.exe）。
    `subprocess.PIPE` / `> file` 都不行——ninja 会替换 stdout 句柄。
  - 解决办法：注入 CMAKE_CXX_COMPILER_LAUNCHER，先由本脚本把 stdout/stderr 重定向到
    **真实文件句柄**（不是管道），再把命令原样 spawn 出去。

⚠ 不要用 os.execv 做"进程替换"（2026-11-21 踩过）：
  Windows 的 CRT `_wexecv` 只是把 argv **用空格拼成命令行、不加任何引号转义**。
  于是 `-DOP_VERSION="5.2.0"` 这种带引号的参数被子进程重新解析时引号被吃掉，
  变成 `-DOP_VERSION=5.2.0` → `OP_WIDEN_TEXT(OP_VERSION)` 展开成 `L5.2.0`
  → `error C2065: "L5": 未声明的标识符`。
  `subprocess` 走 list2cmdline 会正确转义，所以这里用它，退出码照样精确转发。
  （曾担心"spawn 再返回会丢退出码"→ 不会：sys.exit(returncode) 就是编译器的码。）

日志路径可由环境变量 WB_COMPILE_LOG 覆盖。
"""
from __future__ import annotations

import os
import subprocess
import sys

_DEFAULT_LOG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "workbench", "tmp", "ninja_compile.log")


def _raw_command_line_tail() -> str:
    """取**原始**命令行，砍掉 "python.exe <本脚本>" 前缀，原样返回后面的部分。

    为什么不直接用 sys.argv：CRT 解析 argv 时会把 `/DOP_VERSION="5.2.0"` 里的
    引号当成**语法引号**吃掉，argv 里只剩 `5.2.0`；再用 list2cmdline 拼回去时
    这个参数已经没有需要转义的字符，于是不再补引号 → 子进程收到
    `/DOP_VERSION=5.2.0` → `_T(OP_VERSION)` 展开成 `L5.2.0`
    → `error C2065: "L5": 未声明的标识符`。
    （2026-11-23 实测：x86 ninja 构建正是在 OpRuntime.cpp / op_c_api.cpp 上挂的。）

    走 GetCommandLineW 拿到的是**带引号的原始串**，直接作为 lpCommandLine 传给
    CreateProcess 就完全不经过"解析→重新拼接"，引号原样保留。
    """
    try:
        import ctypes
        raw = ctypes.wstring_at(ctypes.windll.kernel32.GetCommandLineW())
    except Exception:
        return ""
    for marker in (os.path.abspath(__file__), __file__):
        idx = raw.find(marker)
        if idx >= 0:
            return raw[idx + len(marker):].lstrip()
    return ""


def main() -> int:
    cmd = sys.argv[1:]
    if not cmd:
        return 2
    log = os.environ.get("WB_COMPILE_LOG") or _DEFAULT_LOG
    os.makedirs(os.path.dirname(log), exist_ok=True)
    # 优先用原始命令行（保住宏定义里的引号）；拿不到再退回 argv 重拼
    tail = _raw_command_line_tail()
    with open(log, "ab") as fh:
        # 每个编译单元前打一行分隔，便于定位
        fh.write(("\n### " + (tail or " ".join(cmd)) + "\n").encode("utf-8", "replace"))
        fh.flush()
        if tail:
            # str 形式在 Windows 上作为 lpCommandLine **原样**传入，不经 list2cmdline
            p = subprocess.run(tail, stdout=fh, stderr=fh, stdin=subprocess.DEVNULL)
        else:
            p = subprocess.run(cmd, stdout=fh, stderr=fh, stdin=subprocess.DEVNULL)
    return p.returncode


if __name__ == "__main__":
    raise SystemExit(main())
