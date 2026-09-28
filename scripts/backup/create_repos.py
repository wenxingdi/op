# -*- coding: utf-8 -*-
"""create_repos.py — 在 wenxingdi 账号下创建 op(fork,私有) 与 OPTool(私有) 两个仓库。

token 来自 GCM（git credential fill），不落盘、不打印。
"""
import json, subprocess, sys, time, urllib.request, urllib.error

API = "https://api.github.com"
OWNER = "wenxingdi"


def token():
    r = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("GCM 取凭据失败: " + r.stderr)
    kv = dict(line.split("=", 1) for line in r.stdout.strip().splitlines() if "=" in line)
    return kv.get("username"), kv.get("password")


def call(method, path, tok, body=None, timeout=60):
    req = urllib.request.Request(API + path, method=method)
    req.add_header("Authorization", "token " + tok)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    data = json.dumps(body).encode() if body is not None else None
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data=data, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait_repo(name, tok, timeout=300):
    t0 = time.time()
    while time.time() - t0 < timeout:
        code, _ = call("GET", f"/repos/{OWNER}/{name}", tok)
        if code == 200:
            return True
        time.sleep(5)
    return False


def main():
    user, tok = token()
    print(f"凭据用户: {user}")

    # 1) fork WallBreaker2/op -> wenxingdi/op (private fork)
    code, resp = call("POST", "/repos/WallBreaker2/op/forks", tok,
                      {"name": "op", "private": True, "default_branch_only": False})
    if code in (202, 422) and (code == 202 or "already exists" in json.dumps(resp)):
        print(f"fork op: HTTP {code}（202=后台创建中, 422=可能已存在）")
    else:
        print(f"fork op: HTTP {code} -> {json.dumps(resp)[:300]}")
        if code not in (202, 422):
            sys.exit(1)

    # 2) 新建 OPTool 私有空仓库
    code, resp = call("POST", "/user/repos", tok,
                      {"name": "OPTool", "private": True,
                       "description": "OP 插件测试工具（自用备份）",
                       "has_issues": False, "has_wiki": False, "auto_init": False})
    if code == 201:
        print("OPTool 仓库: 创建成功(私有)")
    elif code == 422 and "name already exists" in json.dumps(resp):
        print("OPTool 仓库: 已存在，跳过")
    else:
        print(f"OPTool 仓库: HTTP {code} -> {json.dumps(resp)[:300]}")
        if code != 422:
            sys.exit(1)

    # 3) 轮询 fork 就绪
    print("等待 op fork 就绪...", flush=True)
    ok = wait_repo("op", tok)
    print("op fork 就绪" if ok else "op fork 轮询超时（可稍后重试 push，fork 仍在后台创建）")


if __name__ == "__main__":
    main()
