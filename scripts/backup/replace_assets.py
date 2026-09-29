# -*- coding: utf-8 -*-
"""replace_assets.py — 替换已发布 Release 上的指定附件（先 DELETE 再上传）。

与 `release_assets.py` 的分工：
  - `release_assets.py`：**幂等补齐**（已存在则跳过），用于首次发布。
  - `replace_assets.py`：**强制替换**，用于补打 bundle / 修正文档后刷新线上附件。

用法:
    python replace_assets.py <备份目录> <附件名> [<附件名> ...]

`<附件名>` 用本地文件名（如 `op-repo.bundle`、`恢复说明.md`）；上传名按
`UPLOAD_NAME` 映射为 ASCII（GitHub 附件名不支持非 ASCII）。
tag 由目录名日期推导：`2026-09-29_stable_3` → `backup-2026-09-29`。
token 走 GCM，不落盘、不打印。
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

OWNER_REPO = "wenxingdi/op"
UPLOAD_NAME = {"恢复说明.md": "RESTORE.md"}
GCM = (r"C:\Users\lc\.workbuddy\binaries\PortableGit\versions\1.2.0"
       r"\mingw64\bin\git-credential-manager.exe")


def token() -> str:
    if os.path.isfile(GCM):
        r = subprocess.run([GCM, "get"], input="protocol=https\nhost=github.com\n\n",
                           capture_output=True, text=True)
        kv = dict(l.split("=", 1) for l in r.stdout.strip().splitlines() if "=" in l)
        if "password" in kv:
            return kv["password"]
    r = subprocess.run(["git", "credential", "fill"],
                       input="protocol=https\nhost=github.com\n\n",
                       capture_output=True, text=True)
    kv = dict(l.split("=", 1) for l in r.stdout.strip().splitlines() if "=" in l)
    return kv["password"]


def api(method, url, tok, body=None, raw=None, ctype="application/json", timeout=900):
    req = urllib.request.Request(url, method=method)
    req.add_header("Authorization", "token " + tok)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    data = raw
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", ctype)
    elif raw is not None:
        req.add_header("Content-Type", ctype)
        req.add_header("Content-Length", str(len(raw)))
    try:
        with urllib.request.urlopen(req, data=data, timeout=timeout) as resp:
            payload = resp.read()
            try:
                return resp.status, json.loads(payload)
            except Exception:
                return resp.status, payload
    except urllib.error.HTTPError as e:
        return e.code, e.read()[:400]


def get_release(tag, tok):
    code, resp = api("GET", f"https://api.github.com/repos/{OWNER_REPO}/releases/tags/{tag}", tok)
    return resp if code == 200 else None


def upload(rel, bk: Path, local: str, tok) -> bool:
    up_name = UPLOAD_NAME.get(local, local)
    base = rel["upload_url"].split("{")[0]
    url = f"{base}?name={urllib.parse.quote(up_name)}"
    p = bk / local
    size = p.stat().st_size
    for attempt in (1, 2, 3):
        t0 = time.time()
        with open(p, "rb") as f:
            code, resp = api("POST", url, tok, raw=f.read(), ctype="application/octet-stream")
        dt = time.time() - t0
        if code in (200, 201):
            got = resp.get("name", "") if isinstance(resp, dict) else ""
            if isinstance(resp, dict) and got != up_name:
                c2, _ = api("PATCH",
                            f"https://api.github.com/repos/{OWNER_REPO}/releases/assets/{resp['id']}",
                            tok, body={"name": up_name})
                got = up_name if c2 in (200, 201) else got
            print(f"  OK {up_name}  {size/1048576:.1f}MB  {dt:.0f}s"
                  + ("" if got == up_name else f"  !! 名不符: {got}"))
            return True
        print(f"  重试{attempt} {up_name}: HTTP {code} {resp}")
        time.sleep(5)
    return False


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bk = Path(sys.argv[1])
    targets = sys.argv[2:]
    m = re.search(r"(\d{4}-\d{2}-\d{2})", bk.name)
    if not m:
        sys.exit(f"备份目录名须含日期，实际: {bk.name}")
    tag = f"backup-{m.group(1)}"

    tok = token()
    rel = get_release(tag, tok)
    if not rel:
        sys.exit(f"release {tag} 不存在")
    print(f"release {tag} id={rel['id']}  现有 = {[a['name'] for a in rel['assets']]}")

    by_name = {a["name"]: a["id"] for a in rel.get("assets", [])}
    ok = True
    for local in targets:
        p = bk / local
        if not p.is_file():
            print(f"  本地缺失，跳过: {p}")
            ok = False
            continue
        up = UPLOAD_NAME.get(local, local)
        aid = by_name.get(up)
        if aid:
            code, _ = api("DELETE",
                          f"https://api.github.com/repos/{OWNER_REPO}/releases/assets/{aid}", tok)
            print(f"  DELETE {up} -> HTTP {code}")
        ok = upload(rel, bk, local, tok) and ok

    print("== 复核 ==")
    rel2 = get_release(tag, tok)
    for a in sorted(rel2["assets"], key=lambda x: x["name"]):
        print(f"  {a['name']:24} {a['size']/1048576:8.1f} MB  state={a['state']}")
    print("全部成功" if ok else "存在失败")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
