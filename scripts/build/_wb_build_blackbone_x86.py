"""编译 BlackBone x86（32 位）静态库 —— op_x86.dll / op_c_api_x86.dll 必需依赖。

op 的根 CMakeLists 在找不到 BLACKBONE_LIBRARY 时直接 FATAL_ERROR，所以 x86 构建
必须先产出 32 位 BlackBone.lib。源码在 build/_deps/BlackBone/src（自带 CMakeLists）。
"""
from __future__ import annotations

import os
import sys
import subprocess

# 仓库根：脚本自 <repo>/build/ 归档到 <repo>/scripts/build/ 后，原先的
# dirname(dirname(__file__)) 会指向 scripts/。改为向上找 .git，放哪层都不失效。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _root import repo_root  # noqa: E402

ROOT = repo_root()
SRC = os.path.join(ROOT, "build", "_deps", "BlackBone", "src")
BUILD = os.path.join(ROOT, "build", "_deps", "BlackBone", "build", "nmake-x86")
NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"


def x86_env() -> dict:
    env = dict(os.environ)
    env["INCLUDE"] = ";".join([
        MSVC_ROOT + r"\include",
        MSVC_ROOT + r"\ATLMFC\include",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\um",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\ucrt",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\shared",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\winrt",
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


def run(args: list[str]) -> int:
    print(f"$ {' '.join(args)}", flush=True)
    p = subprocess.run(args, cwd=ROOT, env=x86_env(),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    print((p.stdout or "")[-3000:], flush=True)
    return p.returncode


def main() -> int:
    if not os.path.exists(os.path.join(BUILD, "build.ninja")):
        # cor.h / corhdr.h 等 .NET 元数据头在 src/3rd_party 下，而 BlackBone 自带
        # CMake 只 include 了 src/（include_directories(..)）→ 必须补 /I。
        # 另：op 全库按 /MT 构建，这里显式指定 Release 的运行时，避免 /MD 混入。
        rc = run(["cmake", "-S", SRC, "-B", BUILD, "-G", "Ninja",
                  "-DCMAKE_BUILD_TYPE=Release",
                  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                  "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
                  "-DCMAKE_CXX_FLAGS=/I " + os.path.join(SRC, "3rd_party")])
        if rc != 0:
            return rc
    rc = run(["cmake", "--build", BUILD, "--config", "Release", "--target", "BlackBone"])
    if rc != 0:
        return rc
    lib = os.path.join(BUILD, "BlackBone", "BlackBone.lib")
    print("exists:", os.path.exists(lib), lib)
    return 0 if os.path.exists(lib) else 3


if __name__ == "__main__":
    raise SystemExit(main())
