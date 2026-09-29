"""手工编译 MinHook x86 静态库并伪装成 vcpkg triplet 布局。

原因：本机 vcpkg.exe 在本环境下一律报
  error: calling CreateFileW stdin failed with 231 (All pipe instances are busy.)
（Git Bash / PowerShell 都复现），无法 `vcpkg install minhook:x86-windows-static`。
MinHook 源码已随 x64 安装缓存在 buildtrees/minhook/src，直接复用同一份源码编译，
产物放到 vcpkg 的 installed/x86-windows-static 布局里，op 的
find_package(minhook CONFIG REQUIRED) 无需任何改动即可命中。
"""
from __future__ import annotations

import os
import sys
import shutil
import subprocess

# 仓库根：脚本自 <repo>/build/ 归档到 <repo>/scripts/build/ 后，原先的
# dirname(dirname(__file__)) 会指向 scripts/。改为向上找 .git，放哪层都不失效。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _root import repo_root  # noqa: E402

ROOT = repo_root()
VCPKG = os.path.join(ROOT, "build", "_deps", "vcpkg")
SRC = os.path.join(VCPKG, "buildtrees", "minhook", "src", "v1.3.4-e512d7508e.clean")
BUILD = os.path.join(VCPKG, "buildtrees", "minhook", "x86-windows-static-rel")
PREFIX = os.path.join(VCPKG, "installed", "x86-windows-static")
NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"


def x86_env() -> dict:
    env = dict(os.environ)
    env["INCLUDE"] = ";".join([
        MSVC_ROOT + r"\include",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\um",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\ucrt",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\shared",
    ])
    env["LIB"] = ";".join([
        MSVC_ROOT + r"\lib\x86",
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
    print((p.stdout or "")[-2500:], flush=True)
    return p.returncode


CONFIG_CMAKE = '''# MinHook x86-windows-static (手工构建，非 vcpkg 产出；布局与 vcpkg triplet 一致)
set(MINHOOK_VERSION "1.3.3")
set(MINHOOK_FOUND ON)

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)
set(MINHOOK_INCLUDE_DIRS "${PACKAGE_PREFIX_DIR}/include")
set(MINHOOK_LIBRARY_DIRS "${PACKAGE_PREFIX_DIR}/lib")

if(NOT TARGET minhook::minhook)
  add_library(minhook::minhook STATIC IMPORTED)
  set_target_properties(minhook::minhook PROPERTIES
    IMPORTED_LOCATION "${PACKAGE_PREFIX_DIR}/lib/minhook.x86.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include")
endif()
'''


def main() -> int:
    if not os.path.exists(os.path.join(SRC, "CMakeLists.txt")):
        print(f"minhook source missing: {SRC}")
        return 2

    if not os.path.exists(os.path.join(BUILD, "build.ninja")):
        rc = run(["cmake", "-S", SRC, "-B", BUILD, "-G", "Ninja",
                  "-DCMAKE_BUILD_TYPE=Release",
                  "-DBUILD_SHARED_LIBS=OFF",
                  # 注意：MinHook 1.3.4 的 CMakeLists 声明的 cmake_minimum_required 过老
                  # （CMP0091 OLD），-DCMAKE_MSVC_RUNTIME_LIBRARY 会被忽略 → 落回默认 /MD。
                  # /MD 与本工程其余 /MT 静态库混链 → MSVCRT chandler4gs 断链（LNK2019）。
                  # 故直接在 flags 里强指定 /MT。
                  "-DCMAKE_C_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
                  "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG"])
        if rc != 0:
            return rc
    rc = run(["cmake", "--build", BUILD, "--config", "Release"])
    if rc != 0:
        return rc

    built = None
    for dp, _dn, fns in os.walk(BUILD):
        for fn in fns:
            if fn.lower().startswith("minhook") and fn.lower().endswith(".lib"):
                built = os.path.join(dp, fn)
    if not built:
        print("minhook .lib not found after build")
        return 3
    print("built:", built)

    os.makedirs(os.path.join(PREFIX, "lib"), exist_ok=True)
    os.makedirs(os.path.join(PREFIX, "include"), exist_ok=True)
    os.makedirs(os.path.join(PREFIX, "share", "minhook"), exist_ok=True)
    shutil.copy2(built, os.path.join(PREFIX, "lib", "minhook.x86.lib"))
    for hdr in ("MinHook.h",):
        src_hdr = os.path.join(SRC, "include", hdr)
        if os.path.exists(src_hdr):
            shutil.copy2(src_hdr, os.path.join(PREFIX, "include", hdr))
    with open(os.path.join(PREFIX, "share", "minhook", "minhook-config.cmake"),
              "w", encoding="utf-8") as fh:
        fh.write(CONFIG_CMAKE)
    print("installed ->", PREFIX)
    for dp, _dn, fns in os.walk(os.path.join(PREFIX)):
        for fn in fns:
            print("  ", os.path.relpath(os.path.join(dp, fn), PREFIX))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
