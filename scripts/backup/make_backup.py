# -*- coding: utf-8 -*-
"""
make_backup.py — OP Master / OPTool 稳定版本全量备份

每次升级前运行一次，生成按日期隔离的备份包：
  backup/<YYYY-MM-DD_stable>/
    op-repo.bundle          op 仓库全量历史（含全部分支/tag，git bundle 格式）
    optool-repo.bundle      OPTool 仓库全量历史（无 remote，备份尤其重要）
    op-bin-x64/             op 发布件（op_x64.dll / op_c_api_x64.dll / ORT / tools / op_test.exe）
    OPTestTool-Release/     OPTestTool 运行目录（exe+dll+config+Dll/，排除 captures 截图产物）
    WordDictTool-Release/   WordDictTool 运行目录（同上）
    manifest.json           全量文件清单（sha1/大小/来源路径/仓库 commit+tag）
    恢复说明.md

用法:
    python make_backup.py                 # 备份到脚本所在目录
    python make_backup.py --root D:\\xxx  # 指定备份根目录
    python make_backup.py --verify <备份目录>   # 校验备份完整性（重算 sha1 对 manifest）
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

OP_REPO = Path(r"D:\AutoPro\op-master\op")
OPTOOL_REPO = Path(r"D:\AutoPro\OPTool")
OP_BIN_X64 = OP_REPO / "bin" / "x64"
OPTESTTOOL_REL = OPTOOL_REPO / "OPTestTool" / "bin" / "Release" / "net10.0-windows7.0"
WORDDICTTOOL_REL = OPTOOL_REPO / "WordDictTool" / "bin" / "Release" / "net10.0-windows7.0"
TAG = "v2026.09.23-stable"

# 运行目录快照：顶层白名单后缀 + 必带子目录；其余（captures/logs/临时产物）排除
RUN_KEEP_SUFFIX = {".exe", ".dll", ".json", ".config", ".pdb"}
RUN_KEEP_DIRS = {"Dll"}


def sha1(p: Path) -> str:
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(repo: Path, *args) -> str:
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} @ {repo} 失败: {r.stderr.strip()}")
    return r.stdout.strip()


def repo_info(repo: Path) -> dict:
    return {
        "path": str(repo),
        "commit": git(repo, "rev-parse", "HEAD"),
        "branch": git(repo, "rev-parse", "--abbrev-ref", "HEAD"),
        "tag": TAG if TAG in git(repo, "tag", "--points-at", "HEAD").split() else None,
        "describe": git(repo, "describe", "--tags", "--always"),
        "subject": git(repo, "log", "-1", "--format=%s"),
    }


def copy_snapshot(src: Path, dst: Path, keep_dirs=None) -> list:
    """拷贝运行目录快照（白名单后缀顶层文件 + 指定子目录整拷），返回文件清单条目。"""
    entries = []
    keep_dirs = keep_dirs or set()
    if not src.is_dir():
        print(f"  SKIP（目录不存在）: {src}")
        return entries
    for item in sorted(src.iterdir()):
        if item.is_file() and item.suffix.lower() in RUN_KEEP_SUFFIX:
            target = dst / item.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target)
            entries.append(target)
        elif item.is_dir() and item.name in keep_dirs:
            for f in sorted(item.rglob("*")):
                if f.is_file():
                    target = dst / item.name / f.relative_to(item)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(f, target)
                    entries.append(target)
    return entries


def manifest_entries(base: Path, files: list) -> list:
    out = []
    for f in sorted(files):
        rel = str(f.relative_to(base)).replace("\\", "/")
        out.append({"path": rel, "size": f.stat().st_size, "sha1": sha1(f)})
    return out


def make_backup(root: Path) -> Path:
    day = time.strftime("%Y-%m-%d")
    dst = root / f"{day}_stable"
    n = 2
    while dst.exists():
        dst = root / f"{day}_stable_{n}"
        n += 1
    dst.mkdir(parents=True)

    print(f"== 备份目录: {dst}")
    manifest = {
        "created_at": time.strftime("%Y-%m-%d %H:%M:%S %z"),
        "tag": TAG,
        "repos": {"op": repo_info(OP_REPO), "optool": repo_info(OPTOOL_REPO)},
        "sources": {
            "op_bin_x64": str(OP_BIN_X64),
            "optesttool_release": str(OPTESTTOOL_REL),
            "worddicttool_release": str(WORDDICTTOOL_REL),
        },
        "files": [],
    }

    # 1) git bundle（全历史+全分支+全 tag）
    for name, repo in (("op-repo", OP_REPO), ("optool-repo", OPTOOL_REPO)):
        bundle = dst / f"{name}.bundle"
        r = subprocess.run(["git", "-C", str(repo), "bundle", "create", str(bundle), "--all"],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode != 0:
            raise RuntimeError(f"bundle {name} 失败: {r.stderr.strip()}")
        print(f"  {bundle.name}: {bundle.stat().st_size:,} bytes")
        manifest["files"].append({"path": bundle.name, "size": bundle.stat().st_size, "sha1": sha1(bundle)})

    # 2) 发布件/运行目录快照
    snapshots = [
        ("op-bin-x64", OP_BIN_X64, None),
        ("OPTestTool-Release", OPTESTTOOL_REL, RUN_KEEP_DIRS),
        ("WordDictTool-Release", WORDDICTTOOL_REL, RUN_KEEP_DIRS),
    ]
    for sub, src, kd in snapshots:
        files = copy_snapshot(src, dst / sub, kd)
        manifest["files"] += [{"path": f"{sub}/{e.relative_to(dst / sub)}".replace("\\", "/"),
                               "size": e.stat().st_size, "sha1": sha1(e)} for e in files]
        print(f"  {sub}/: {len(files)} 个文件")

    (dst / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    write_readme(dst, manifest)
    print(f"== 完成，共 {len(manifest['files'])} 项，manifest.json 已生成")
    return dst


def write_readme(dst: Path, manifest: dict) -> None:
    op, ot = manifest["repos"]["op"], manifest["repos"]["optool"]
    (dst / "恢复说明.md").write_text(f"""# 稳定版本备份（{manifest['created_at']}）

Tag：`{TAG}`（两仓库同名 tag 均指向本备份点）

## 版本锚点

| 仓库 | 路径 | commit | 分支 | 描述 |
|---|---|---|---|---|
| op | `{op['path']}` | `{op['commit'][:12]}` | {op['branch']} | {op['describe']} |
| OPTool | `{ot['path']}` | `{ot['commit'][:12]}` | {ot['branch']} | {ot['describe']} |

op HEAD 提交：{op['subject']}
OPTool HEAD 提交：{ot['subject']}

## 内容物

- `op-repo.bundle` / `optool-repo.bundle` — git bundle（含全部历史/分支/tag）。OPTool 仓库无 remote，此文件是**唯一**的完整历史备份。
- `op-bin-x64/` — op 发布件（构建产物，不入 git）。
- `OPTestTool-Release/` / `WordDictTool-Release/` — 工具运行目录快照（含 Dll/ 内 op_c_api_x64.dll）。

## 恢复方法

**1. 回滚源码**（在仓库原位置或新目录）：
```
git clone <本目录>\\op-repo.bundle op-restored
cd op-restored && git checkout {TAG}
```
或在原仓库直接：`git fetch <本目录>\\op-repo.bundle --all && git reset --hard {TAG}`

**2. 回滚运行目录**：用备份的 `OPTestTool-Release/` 等整目录覆盖现网对应目录
（先确认 OPTestTool.exe / WordDictTool.exe 未运行，否则 DLL 被锁）。

**3. 校验完整性**：
```
python make_backup.py --verify <本备份目录>
```
（重算每个文件 sha1 与 manifest.json 比对）

## 升级前建议流程

1. 运行 `python make_backup.py` 生成新备份
2. 确认 manifest 校验通过、bundle 可 `git bundle verify`
3. 再开始升级改动
""", encoding="utf-8")


def verify(backup: Path) -> int:
    manifest = json.loads((backup / "manifest.json").read_text(encoding="utf-8"))
    bad = 0
    for e in manifest["files"]:
        p = backup / e["path"]
        if not p.is_file():
            print(f"MISSING  {e['path']}")
            bad += 1
            continue
        actual = sha1(p)
        if actual != e["sha1"] or p.stat().st_size != e["size"]:
            print(f"CORRUPT  {e['path']}  (期望 {e['sha1'][:12]}/{e['size']}, 实际 {actual[:12]}/{p.stat().st_size})")
            bad += 1
    print(f"校验 {'通过' if bad == 0 else '失败'}：{len(manifest['files'])} 项，{bad} 异常")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent))
    ap.add_argument("--verify", metavar="备份目录")
    a = ap.parse_args()
    if a.verify:
        sys.exit(verify(Path(a.verify)))
    make_backup(Path(a.root))


if __name__ == "__main__":
    main()
