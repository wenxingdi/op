"""配置并构建 op 的 32 位版本（op_x86.dll / op_c_api_x86.dll）。

依赖（均由同目录脚本先行产出）：
  build/_wb_build_opencv_x86.py     -> OpenCV 5.0.0 Win32 静态库
  build/_wb_build_minhook_x86.py    -> MinHook x86 静态库（vcpkg 布局）
  build/_wb_build_blackbone_x86.py  -> BlackBone x86 静态库

注意：本机 vcpkg 不可用（pipe busy），gtest 没有 x86 版本，故 OP_BUILD_TESTS=OFF ——
x86 只产 DLL，验证走导出符号核对 + 32 位宿主探针，不进 gtest 回归网。
"""
from __future__ import annotations

import os
import subprocess
import sys

# 仓库根：脚本自 <repo>/build/ 归档到 <repo>/scripts/build/ 后，原先的
# dirname(dirname(__file__)) 会指向 scripts/。改为向上找 .git，放哪层都不失效。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _root import repo_root  # noqa: E402

ROOT = repo_root()
BUILD = os.path.join(ROOT, "build", "ninja-x86-Release")
DEPS = os.path.join(ROOT, "build", "_deps")
NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"

OPENCV_ROOT = os.path.join(DEPS, "opencv", "install", "nmake-x86")
BLACKBONE_LIB = os.path.join(DEPS, "BlackBone", "build", "nmake-x86", "BlackBone", "BlackBone.lib")
BLACKBONE_INC = os.path.join(DEPS, "BlackBone", "src")


def x86_env() -> dict:
    env = dict(os.environ)
    env["INCLUDE"] = ";".join([
        MSVC_ROOT + r"\include",
        MSVC_ROOT + r"\ATLMFC\include",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\um",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\ucrt",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\shared",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\winrt",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\cppwinrt",
    ])
    env["LIB"] = ";".join([
        MSVC_ROOT + r"\lib\x86",
        MSVC_ROOT + r"\ATLMFC\lib\x86",
        SDK_ROOT + r"\Lib" + "\\" + SDK_VER + r"\um\x86",
        SDK_ROOT + r"\Lib" + "\\" + SDK_VER + r"\ucrt\x86",
    ])
    env["PATH"] = ";".join([
        MSVC_ROOT + r"\bin\Hostx64\x86",
        SDK_ROOT + r"\bin" + "\\" + SDK_VER + r"\x86",
        NINJA_DIR,
        env.get("PATH", ""),
    ])
    return env


CONFIGURE = [
    "cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    # 与 x64 主构建一致（x64 未设置此项 → 默认 /MD，且其 CMAKE_CXX_FLAGS_RELEASE 带 /MD）。
    # 此前误设 MultiThreaded(/MT) 与 /MD 对象混链 → MSVCRT chandler4gs 断链（LNK2019）。
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>",
    f"-DOPENCV_ROOT={OPENCV_ROOT}",
    "-DOPENCV_LIB_SUFFIX=500",
    f"-DBLACKBONE_LIBRARY={BLACKBONE_LIB}",
    f"-DBLACKBONE_INCLUDE_DIR={BLACKBONE_INC}",
    "-DOP_BUILD_TESTS=OFF",
    "-Dbuild_swig_py=OFF",
    # x86 静态 CRT（与 x64 相同均为 /MT）。全库无一处 /DEFAULTLIB:MSVCRT，但链接器仍会
    # 拉 MSVCRT.lib 的 chandler4gs.obj（x86 SEH 专属路径，x64 无此符号链）→ __except_handler4_common
    # 断链（LNK2019/LNK4098）。静态 CRT 完全自洽，直接忽略 MSVCRT 即通过；
    # 产物因此不依赖 VC 运行时 DLL，随 32 位宿主分发更简单。
    "-DCMAKE_SHARED_LINKER_FLAGS=/NODEFAULTLIB:MSVCRT",
    "-DCMAKE_EXE_LINKER_FLAGS=/NODEFAULTLIB:MSVCRT",
]


def run(label: str, args: list[str]) -> int:
    print(f"===== {label} =====\n$ {' '.join(args)}\n", flush=True)
    p = subprocess.run(args, cwd=ROOT, env=x86_env(),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    print((p.stdout or "")[-4000:], flush=True)
    print(f"[{label}] exit={p.returncode}", flush=True)
    return p.returncode


def main() -> int:
    for path in (BLACKBONE_LIB, os.path.join(OPENCV_ROOT, "x86", "vc17", "staticlib")):
        if not os.path.exists(path):
            print(f"missing dependency: {path}")
            return 2

    # 每次都重新配置（幂等，~10s）：改 CMake 参数后陈旧 build.ninja 是今天踩过的坑
    rc = run("configure", CONFIGURE)
    if rc != 0:
        return rc

    targets = sys.argv[1:] or ["op_x86", "op_c_api_x86"]
    rc = run("build", ["cmake", "--build", BUILD, "--config", "Release", "--"] + targets)
    if rc != 0:
        return rc

    for t in targets:
        dll = os.path.join(BUILD, "libop", f"{t}.dll")
        print(("OK  " if os.path.exists(dll) else "MISS"), dll)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
