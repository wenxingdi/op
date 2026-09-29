# -*- coding: utf-8 -*-
"""refresh_bundle.py — 重打某备份目录内的 git bundle 至仓库当前 master。

背景：`make_backup.py` 产出的 bundle 是**备份时刻**的快照。若备份之后又提交了
新内容（例如脚本/记录归档），冷备份就会缺最后几个提交。本脚本把 bundle 重打到
最新 master，并同步刷新 manifest.json 的 commit/sha1 与恢复说明的补录节。

用法:
    python refresh_bundle.py <备份目录> [--repo op|optool|all]

只动 bundle 与文档元数据，**不动运行时快照**（二进制通常未变）。若二进制也变了，
应重跑 `make_backup.py` 生成新备份，而不是用本脚本打补丁。
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPOS = {
    "op": Path(r"D:\AutoPro\op-master\op"),
    "optool": Path(r"D:\AutoPro\OPTool"),
}
BUNDLE_NAME = {"op": "op-repo.bundle", "optool": "optool-repo.bundle"}


def git(*args, cwd):
    return subprocess.run(["git", *args], cwd=str(cwd), capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def sha1(p: Path) -> str:
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def bundle_head(path: Path, repo: Path) -> str:
    """读 bundle 内 refs/heads/master 指向的 sha；文件不存在/不可解析返回 ""。"""
    if not path.is_file():
        return ""
    lh = git("bundle", "list-heads", str(path), cwd=repo)
    return next((l.split()[0] for l in lh.stdout.splitlines()
                 if l.endswith("refs/heads/master")), "")


def refresh_one(bk: Path, key: str) -> dict | None:
    repo = REPOS[key]
    name = BUNDLE_NAME[key]
    # `bk` 可能是相对路径；而下面所有 git 子进程的 cwd 是 **repo**（不是备份目录所在仓库），
    # 相对路径会被解析到 repo 下 → shutil/bundle 找不到目录。统一转绝对路径。
    bk = bk.resolve()
    print(f"== {key}: {repo}")
    if not (repo / ".git").exists():
        print("  仓库不存在，跳过")
        return None
    local = git("rev-parse", "master", cwd=repo).stdout.strip()
    target = bk / name

    # `git bundle create` 的**字节不确定**（pack 头含时间戳）：同 refs 两次生成 sha1 不同。
    # 若已是最新则**不重打**，否则每次跑都会让 manifest 里的 sha1 与线上附件漂移。
    if bundle_head(target, repo) == local:
        size, sha = target.stat().st_size, sha1(target)
        print(f"  已是最新（master={local[:12]}），跳过重打  {size/1048576:.1f} MB  sha1={sha[:12]}")
        return {**repo_meta(repo, local), "size": size, "sha1": sha, "name": name}

    tmp = bk / (name + ".new")
    r = git("bundle", "create", str(tmp), "--all", cwd=repo)
    if r.returncode != 0:
        print("  bundle create 失败:", (r.stderr or "").strip()[:200])
        tmp.unlink(missing_ok=True)
        return None
    v = git("bundle", "verify", str(tmp), cwd=repo)
    head = bundle_head(tmp, repo)
    if v.returncode != 0 or head != local:
        print(f"  校验失败 verify={v.returncode} head={head[:12]} local={local[:12]}")
        tmp.unlink(missing_ok=True)
        return None
    target.write_bytes(tmp.read_bytes())
    tmp.unlink(missing_ok=True)
    size, sha = target.stat().st_size, sha1(target)
    print(f"  OK {name}  master={local[:12]}  {size/1048576:.1f} MB  sha1={sha[:12]}")
    return {**repo_meta(repo, local), "size": size, "sha1": sha, "name": name}


def repo_meta(repo: Path, commit: str) -> dict:
    return {
        "commit": commit,
        "subject": git("log", "-1", "--format=%s", cwd=repo).stdout.strip(),
        "describe": git("describe", "--tags", "--always", cwd=repo).stdout.strip(),
        "tag": next((t for t in git("tag", "--points-at", "HEAD", cwd=repo).stdout.split()), None),
    }



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("backup_dir")
    ap.add_argument("--repo", default="all", choices=["op", "optool", "all"])
    a = ap.parse_args()
    bk = Path(a.backup_dir).resolve()
    if not (bk / "manifest.json").is_file():
        sys.exit(f"不是备份目录（无 manifest.json）: {bk}")

    keys = ["op", "optool"] if a.repo == "all" else [a.repo]
    results = {k: refresh_one(bk, k) for k in keys}

    mf = json.loads((bk / "manifest.json").read_text(encoding="utf-8"))
    for k, res in results.items():
        if not res:
            continue
        mf["repos"][k].update({x: res[x] for x in ("commit", "subject", "describe", "tag")})
        for item in mf["files"]:
            if item["path"] == res["name"]:
                item["size"], item["sha1"] = res["size"], res["sha1"]
    done = [k for k, v in results.items() if v]
    if done:
        mf["post_backup_refresh"] = {
            "repos": done,
            "note": "bundle 已重打至备份之后的最新 master；运行时快照未重打，"
                    "内嵌 manifest 仍为备份时刻。",
        }
        (bk / "manifest.json").write_text(json.dumps(mf, ensure_ascii=False, indent=2),
                                          encoding="utf-8")
        print("manifest.json 已刷新")

    rd = (bk / "恢复说明.md")
    if rd.is_file() and done:
        txt = rd.read_text(encoding="utf-8")
        # 补录节恒为文末最后一节：命中则整段重写（否则 bundle 重打后 sha1 会留旧值）
        mark = "## 补录（备份后）"
        if mark in txt:
            txt = txt[:txt.index(mark)].rstrip() + "\n"
        rows = "\n".join(
            f"- `{results[k]['name']}` → `{results[k]['commit'][:7]}`（`{results[k]['describe']}`），"
            f"sha1 `{results[k]['sha1'][:12]}`" for k in done)
        txt += f"""

## 补录（备份后）

{rows}

- `runtime-snapshot.zip` 未重打，内嵌 manifest 仍记录备份时刻（运行件二进制未变，
  恢复使用不受影响）
- Tag 锚点不变；其后 master 上仅文档/脚本类提交
- 注：`git bundle` 字节不确定（pack 头含时间戳），故本脚本对"已是最新"的 bundle
  跳过重打，避免 manifest 里 sha1 与线上附件漂移
"""
        rd.write_text(txt, encoding="utf-8")
        print("恢复说明.md 补录节已刷新")
    print("完成")


if __name__ == "__main__":
    main()
