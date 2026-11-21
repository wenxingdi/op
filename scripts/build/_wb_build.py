import os
import subprocess
import sys

MSVC = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"
BUILD = r"D:\AutoPro\op-master\op\build\nmake-x64-Release"

# 头文件依赖跟踪失效的补偿（2026-11-21，详见
# docs/2026-11/构建缺陷_改头文件不触发重编译_20261121.md）：
# cl 的 /showIncludes 输出是 UTF-8，而 CMake 写进 build.make 的 --filter-prefix
# 是 GBK（"注意:  包含文件:  "），字节都不同 -> cmake_cl_compile_depends 过滤不到任何行
# -> .obj.d 恒为 0 字节 -> 头文件不在依赖图里 -> **改 .h 不触发重编译**。
#
# 因此这里做两件事：
#   1) 扫描 .obj.d：凡是 0 字节的，删掉它对应的 .obj，强制重编（一次生效，之后正常）；
#   2) 额外保险：任何 .h 比某 .obj 新，就删该 .obj（补偿已失效的依赖图）。
# 设 OP_NO_DEP_GUARD=1 可关闭（调试构建系统本身时用）。
#
# ⚠ 2026-11-21 18:20 修正：第 2 条**只在依赖图确实坏了**（存在 0 字节 .obj.d）时才跑。
# 实测（17:20 那次全量重建之后）244 个 .obj.d 全部有内容（单个 40KB+），说明
# `cmake_cl_compile_depends` 在管道场景下是能匹配上 GBK 前缀的 —— 依赖图正常，
# nmake 自己就会因为头文件变化重编依赖它的 TU。这时再无条件按"最新头文件"删 obj，
# 等于把 233 个 TU 全重编一遍（10~25 分钟），纯浪费。
# 判据：只要有一个 .obj.d 是空的，就说明这套依赖跟踪不可信 -> 退回全量补偿。
def _guard_stale_objects(build_dir):
    if os.environ.get("OP_NO_DEP_GUARD") == "1":
        return []
    depfiles = []
    for dirpath, _dirnames, filenames in os.walk(build_dir):
        for fn in filenames:
            if fn.endswith(".obj.d"):
                depfiles.append(os.path.join(dirpath, fn))

    empty = [p for p in depfiles if os.path.getsize(p) == 0]
    # 依赖图健康 -> 只清空依赖的 obj（通常 0 个），不做按头文件时间的全量补偿
    graph_broken = bool(empty)
    newest_hdr = 0.0
    if graph_broken:
        hdr_roots = [os.path.join(os.path.dirname(build_dir), "libop"),
                     os.path.join(os.path.dirname(build_dir), "include"),
                     os.path.join(os.path.dirname(build_dir), "tests")]
        for root in hdr_roots:
            for dirpath, _dirnames, filenames in os.walk(root):
                if os.sep + "build" + os.sep in dirpath + os.sep:
                    continue
                for fn in filenames:
                    if fn.endswith((".h", ".hpp", ".idl")):
                        try:
                            newest_hdr = max(newest_hdr, os.path.getmtime(os.path.join(dirpath, fn)))
                        except OSError:
                            pass

    removed_zero, removed_hdr = [], []
    for dp in depfiles:
        obj = dp[:-2]  # "x.obj.d" -> "x.obj"
        try:
            if os.path.getsize(dp) == 0:
                if os.path.exists(obj):
                    os.remove(obj)
                    removed_zero.append(os.path.basename(dp))
            elif graph_broken and newest_hdr and os.path.exists(obj) and os.path.getmtime(obj) < newest_hdr:
                os.remove(obj)
                removed_hdr.append(os.path.basename(dp))
        except OSError:
            pass
    return removed_zero, removed_hdr


env = dict(os.environ)
env["INCLUDE"] = ";".join(
    [
        MSVC + r"\include",
        MSVC + r"\ATLMFC\include",
        SDK + r"\Include" + "\\" + SDK_VER + r"\um",
        SDK + r"\Include" + "\\" + SDK_VER + r"\ucrt",
        SDK + r"\Include" + "\\" + SDK_VER + r"\shared",
        SDK + r"\Include" + "\\" + SDK_VER + r"\winrt",
        SDK + r"\Include" + "\\" + SDK_VER + r"\cppwinrt",
    ]
)
env["LIB"] = ";".join(
    [
        MSVC + r"\lib\x64",
        MSVC + r"\ATLMFC\lib\x64",
        SDK + r"\Lib" + "\\" + SDK_VER + r"\um\x64",
        SDK + r"\Lib" + "\\" + SDK_VER + r"\ucrt\x64",
    ]
)
env["PATH"] = (
    MSVC + r"\bin\Hostx64\x64"
    + ";" + SDK + r"\bin" + "\\" + SDK_VER + r"\x64"
    + ";" + env.get("PATH", "")
)

targets = sys.argv[1:] or ["op_x64", "op_test"]

guard_zero, guard_hdr = _guard_stale_objects(BUILD)
if guard_zero or guard_hdr:
    print(
        "dep-guard: forced rebuild of %d obj with empty .obj.d, %d obj older than newest header"
        % (len(guard_zero), len(guard_hdr))
    )

r = subprocess.run(
    [r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\nmake.exe"]
    + targets,
    cwd=BUILD,
    env=env,
    capture_output=True,
    text=True,
    encoding="utf-8",
    errors="replace",
)
out = (r.stdout or "") + "\n=== STDERR ===\n" + (r.stderr or "")
open(os.path.join(BUILD, "_wb_build_out.txt"), "w", encoding="utf-8").write(out)
print("exit:", r.returncode)
print(out[-3000:])
