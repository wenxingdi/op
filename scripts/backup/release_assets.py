# -*- coding: utf-8 -*-
"""release_assets.py — 把稳定版本备份包上传为 wenxingdi/op 的 Release 附件。

用法: python release_assets.py <备份目录>   （如 D:\\AutoPro\\op-master\\backup\\2026-09-28_stable）
tag 自动取目录名日期部分：2026-09-28_stable → backup-2026-09-28（幂等，已存在的附件跳过）。

附件：op-repo.bundle / optool-repo.bundle / runtime-snapshot.zip / manifest.json / RESTORE.md
      （本地名「恢复说明.md」→ 上传名 RESTORE.md：Release 附件名不支持非 ASCII）
token 来自 GCM，不落盘、不打印。单文件大（123MB+132MB），超时 600s，失败重试 2 次。
"""
import json, os, re, subprocess, sys, time, urllib.parse, urllib.request, urllib.error

OWNER_REPO = "wenxingdi/op"

BACKUP_DIR = sys.argv[1] if len(sys.argv) > 1 else r"D:\AutoPro\op-master\backup"
BACKUP = __import__("pathlib").Path(BACKUP_DIR)
_date = re.search(r"(\d{4}-\d{2}-\d{2})", BACKUP.name)
if not _date:
    sys.exit(f"备份目录名须含日期（如 2026-09-28_stable），实际: {BACKUP.name}")
BACKUP_DATE = _date.group(1)
ASSETS = ["op-repo.bundle", "optool-repo.bundle", "runtime-snapshot.zip",
          "manifest.json", "恢复说明.md"]
# GitHub Release 附件名**不支持非 ASCII**：中文会被整体剥离 —— 实测 09-23 / 09-28 / 09-29
# 三次都把「恢复说明.md」落成 default.md（`RESTORE-恢复说明.md` 更被吃成 `RESTORE-.md`）。
# 故上传时改用 ASCII 名，文件内容不变。传完仍会校验，名不符则按 asset id 纠正。
UPLOAD_NAME = {"恢复说明.md": "RESTORE.md"}
RELEASE_TAG = f"backup-{BACKUP_DATE}"


def token():
    """优先直调 GCM（本机实证稳定）；git credential fill 在沙箱 shell 会 segfault(139) 故不作主路径。"""
    gcm = r"C:\Users\lc\.workbuddy\binaries\PortableGit\versions\1.2.0\mingw64\bin\git-credential-manager.exe"
    if os.path.isfile(gcm):
        r = subprocess.run([gcm, "get"], input="protocol=https\nhost=github.com\n\n",
                           capture_output=True, text=True)
        kv = dict(l.split("=", 1) for l in r.stdout.strip().splitlines() if "=" in l)
        if "password" in kv:
            return kv["password"]
    r = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                       capture_output=True, text=True)
    kv = dict(l.split("=", 1) for l in r.stdout.strip().splitlines() if "=" in l)
    return kv["password"]


def api(method, url, tok, body=None, raw=None, ctype="application/json", timeout=600):
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


def get_release(tok):
    code, resp = api("GET", f"https://api.github.com/repos/{OWNER_REPO}/releases/tags/{RELEASE_TAG}", tok)
    return resp if code == 200 else None


def create_release(tok, commit):
    body = {
        "tag_name": RELEASE_TAG,
        "target_commitish": commit,
        "name": f"稳定版本备份 backup-{BACKUP_DATE}（升级前基线）",
        "body": (
            f"OP Master + OPTool 备份点 {BACKUP_DATE}。\n\n"
            "| 仓库 | commit |\n|---|---|\n"
            f"| op | `{commit[:12]}` |\n"
            f"| OPTool | 见 manifest.json |\n\n"
            "**附件说明**：\n"
            "- `op-repo.bundle` / `optool-repo.bundle` — git bundle 全量历史（含 tag），clone 即可恢复\n"
            "- `runtime-snapshot.zip` — 运行件快照（op-bin-x64 + OPTestTool/WordDictTool Release + manifest + 恢复说明）\n"
            "- `RESTORE.md` — 中文恢复说明（GitHub 附件名不支持非 ASCII，故不叫「恢复说明.md」）\n"
            "- 源码本身已在各分支/master，此 release 提供单文件冷备份\n\n"
            "恢复方法见压缩包内 `恢复说明.md`。"
        ),
    }
    code, resp = api("POST", f"https://api.github.com/repos/{OWNER_REPO}/releases", tok, body=body)
    if code not in (200, 201):
        print(f"创建 release 失败 HTTP {code}: {resp}")
        sys.exit(1)
    return resp


def rename_asset(aid, name, tok) -> bool:
    code, _ = api("PATCH", f"https://api.github.com/repos/{OWNER_REPO}/releases/assets/{aid}",
                  tok, body={"name": name})
    return code in (200, 201)


def upload(release, name, path, tok):
    """上传单个附件；name 为本地文件名，实际用 UPLOAD_NAME 映射后的 ASCII 名。"""
    up_name = UPLOAD_NAME.get(name, name)
    base = release["upload_url"].split("{")[0]
    url = f"{base}?name={urllib.parse.quote(up_name)}"
    size = os.path.getsize(path)
    for attempt in (1, 2, 3):
        t0 = time.time()
        with open(path, "rb") as f:
            code, resp = api("POST", url, tok, raw=f.read(), ctype="application/octet-stream", timeout=900)
        dt = time.time() - t0
        if code in (200, 201):
            got = resp.get("name", "") if isinstance(resp, dict) else ""
            if isinstance(resp, dict) and got != up_name and rename_asset(resp["id"], up_name, tok):
                got = up_name
            url2 = resp.get("browser_download_url", "") if isinstance(resp, dict) else ""
            print(f"  OK {up_name}  {size/1048576:.1f}MB  {dt:.0f}s"
                  + (f"  -> {url2[:90]}" if url2 else "")
                  + ("" if got == up_name else f"  !! 名不符: {got}"))
            return True
        print(f"  重试{attempt} {up_name}: HTTP {code} {resp}")
        time.sleep(5)
    return False


def main():
    tok = token()
    release = get_release(tok)
    if release:
        print(f"release {RELEASE_TAG} 已存在(id={release['id']})，追加缺失附件")
    else:
        commit = json.loads((BACKUP / "manifest.json").read_text(encoding="utf-8"))["repos"]["op"]["commit"]
        release = create_release(tok, commit)
        print(f"release 已创建 id={release['id']} tag={release['tag_name']} -> {commit[:12]}")

    existing = {a["name"] for a in release.get("assets", [])}
    ok = True
    for name in ASSETS:
        up_name = UPLOAD_NAME.get(name, name)
        if up_name in existing:
            print(f"  跳过（已存在）: {up_name}")
            continue
        ok = upload(release, name, BACKUP / name, tok) and ok
    print("全部完成" if ok else "存在失败附件，请重跑本脚本（已存在的会跳过）")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
