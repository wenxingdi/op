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
def _guard_stale_objects(build_dir):
    if os.environ.get("OP_NO_DEP_GUARD") == "1":
        return []
    removed_zero, removed_hdr = [], []
    newest_hdr = 0.0
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
    for dirpath, _dirnames, filenames in os.walk(build_dir):
        for fn in filenames:
            if not fn.endswith(".obj.d"):
                continue
            dp = os.path.join(dirpath, fn)
            obj = dp[:-2]  # "x.obj.d" -> "x.obj"
            try:
                if os.path.getsize(dp) == 0:
                    if os.path.exists(obj):
                        os.remove(obj)
                        removed_zero.append(fn)
                elif newest_hdr and os.path.exists(obj) and os.path.getmtime(obj) < newest_hdr:
                    os.remove(obj)
                    removed_hdr.append(fn)
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
