# -*- coding: utf-8 -*-
"""push.py — 把 op 仓库 master 推到 GitHub，并**硬校验**远端 sha == 本地 sha。

为什么不用普通 `git push`：
  1. 默认 credential.helper 会派生 GCM 子进程，在沙箱 shell 下会**静默崩掉整个 git**
     （表现为 git 直接段错误/无输出）。改为 `git -c credential.helper= push https://user:TOKEN@…`，
     token 由 GCM exe **直取**（`git credential fill` 同样会崩）。
  2. 走内联 auth URL 的推送**不会更新**本地 `refs/remotes/origin/master` 跟踪引用
     （git 视其为匿名 URL）→ 之后 `git bundle create --all` 会带上**过期**的 origin/master。
     本脚本推送成功后显式 `git update-ref` 同步，避免冷备份里出现误导性旧引用。

其它工程细节：
  - 输出**全程脱敏**（token 及其前缀替换为 `***`），凭据不落日志。
  - 代理下 `ls-remote` 有间歇 `schannel handshake failed` → 内置 5 次重试。
  - 校验一律以 `ls-remote` 为准（远端权威），不信本地跟踪引用。

用法:
    python scripts/push.py                 # 推送 op 仓库 master
    python scripts/push.py --dir D:\\AutoPro\\OPTool
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "build"))
from _root import repo_root  # noqa: E402

REMOTE = "https://github.com/wenxingdi/op.git"
GCM = (r"C:\Users\lc\.workbuddy\binaries\PortableGit\versions\1.2.0"
       r"\mingw64\bin\git-credential-manager.exe")


def token() -> str:
    r = subprocess.run([GCM, "get"], input="protocol=https\nhost=github.com\n\n",
                       capture_output=True, text=True)
    kv = dict(l.split("=", 1) for l in r.stdout.strip().splitlines() if "=" in l)
    if "password" not in kv:
        sys.exit("未能从 GCM 取到 token（检查 git-credential-manager.exe 路径与登录态）")
    return kv["password"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=None, help="仓库目录（默认自动向上找 .git）")
    ap.add_argument("--branch", default="master")
    ap.add_argument("--remote", default=REMOTE)
    a = ap.parse_args()

    repo = os.path.abspath(a.dir) if a.dir else repo_root()
    TOK = token()
    scrub = [(TOK, "***"), (TOK[:12], "***")]

    def safe(s: str) -> str:
        for x, y in scrub:
            if x:
                s = s.replace(x, y)
        return s

    def git(*args, **kw):
        return subprocess.run(["git", "-C", repo, *args], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", **kw)

    local = git("rev-parse", a.branch).stdout.strip()
    if not local:
        sys.exit("取不到本地 %s sha（仓库: %s）" % (a.branch, repo))
    print("repo  =", repo)
    print("local %s = %s" % (a.branch, local[:12]))

    auth = re.sub(r"^https://", "https://wenxingdi:%s@" % TOK, a.remote)
    ok = False
    for attempt in (1, 2, 3):
        r = git("-c", "credential.helper=", "push", auth, a.branch)
        print("[push try%d] exit=%d" % (attempt, r.returncode))
        if r.returncode == 0:
            print(safe((r.stderr or r.stdout).strip()))
            ok = True
            break
        print(safe((r.stderr or r.stdout).strip()[:400]))
        time.sleep(3)
    if not ok:
        sys.exit("push 失败")

    # 校验用**无凭据** URL（公开仓库）+ 容忍代理下间歇 SSL 抖动
    remote = ""
    for attempt in range(1, 6):
        r = git("ls-remote", a.remote, "refs/heads/%s" % a.branch)
        out = r.stdout.strip()
        if out:
            remote = out.split()[0]
            break
        print("[ls-remote try%d] exit=%d，重试…" % (attempt, r.returncode))
        time.sleep(3)

    print("remote %s = %s" % (a.branch, remote[:12] if remote else "(空)"))
    if remote == local:
        # 内联 auth URL 推送不更新本地跟踪引用 → 显式同步（见模块 docstring 第 2 条）
        u = git("update-ref", "refs/remotes/origin/%s" % a.branch, local)
        print("update-ref origin/%s -> %s exit=%d" % (a.branch, local[:12], u.returncode))
    print("MATCH" if remote == local else "MISMATCH")
    sys.exit(0 if remote == local else 1)


if __name__ == "__main__":
    main()
