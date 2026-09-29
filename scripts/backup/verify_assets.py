# -*- coding: utf-8 -*-
"""verify_assets.py — 硬校验线上 Release 附件 == 本地备份目录文件（sha256 + size）。

判据只有一条：GitHub Release asset 的 `digest`（sha256）**且** `size` 都必须与本地
逐字节 sha256 / 字节数相等。**不认**上传返回值，也不认时间戳（都不可靠）。

工程注意：
  - 刚上传完立刻查询 **可能拿到缓存的旧 digest**（实测误报 MISMATCH，重查即一致）
    → 内置最多 3 轮重试、间隔 8s，全等才退出 0。
  - 线上附件名可能是 ASCII 映射名（如 `RESTORE.md` ← `恢复说明.md`，GitHub 附件名不支持
    非 ASCII）；本地对照文件按 `LOCAL_NAME` 反查。
  - token 走 GCM，不落盘、不打印。

用法:
    python verify_assets.py <备份目录>
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

OWNER_REPO = "wenxingdi/op"
LOCAL_NAME = {"RESTORE.md": "恢复说明.md"}
GCM = (r"C:\Users\lc\.workbuddy\binaries\PortableGit\versions\1.2.0"
       r"\mingw64\bin\git-credential-manager.exe")
ROUNDS, GAP = 3, 8


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


def fetch_release(tag: str, tok: str) -> dict | None:
    req = urllib.request.Request(
        f"https://api.github.com/repos/{OWNER_REPO}/releases/tags/{tag}")
    req.add_header("Authorization", "token " + tok)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        print("  GET release 失败: HTTP %d %s" % (e.code, e.read()[:200]))
        return None


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bk = Path(sys.argv[1]).resolve()
    if not (bk / "manifest.json").is_file():
        sys.exit("不是备份目录（无 manifest.json）: %s" % bk)
    m = re.search(r"(\d{4}-\d{2}-\d{2})", bk.name)
    if not m:
        sys.exit("备份目录名须含日期，实际: %s" % bk.name)
    tag = "backup-%s" % m.group(1)

    tok = token()
    bad = 1
    for attempt in range(1, ROUNDS + 1):
        rel = fetch_release(tag, tok)
        if not rel:
            time.sleep(GAP)
            continue
        lines, bad, missing = [], 0, []
        for a in sorted(rel.get("assets", []), key=lambda x: x["name"]):
            lp = bk / LOCAL_NAME.get(a["name"], a["name"])
            if not lp.is_file():
                missing.append(a["name"])
                lines.append("  %-22s %-10d (无本地对照)" % (a["name"], a["size"]))
                continue
            ok = (a.get("digest", "").replace("sha256:", "") == sha256(lp)
                  and a["size"] == lp.stat().st_size)
            if not ok:
                bad += 1
            lines.append("  %-22s %-10d %s" % (a["name"], a["size"], "OK" if ok else "MISMATCH"))
        print("--- 第 %d 轮（tag=%s）---" % (attempt, tag))
        print("\n".join(lines))
        if missing:
            print("  参考：本地缺少对应文件 %s（若为有意保留的旧附件可忽略）" % missing)
        if bad == 0:
            break
        if attempt < ROUNDS:
            print("  存在差异，%.0fs 后重查（刚上传时 API 可能返回缓存旧 digest）…" % GAP)
            time.sleep(GAP)

    n = len(rel.get("assets", [])) if rel else 0
    print("RESULT: %s（%d/%d 全等）" % ("ALL MATCH" if bad == 0 else "%d MISMATCH" % bad,
                                     n - bad, n))
    sys.exit(0 if bad == 0 else 1)


if __name__ == "__main__":
    main()
