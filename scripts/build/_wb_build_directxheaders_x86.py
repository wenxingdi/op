"""手工编译 directx-headers x86 并铺成 vcpkg triplet 布局（同 minhook：vcpkg 不可用）。

targets 文件用 ${_IMPORT_PREFIX} 相对定位，x64 的 share/directx-headers/*.cmake
可直接复制到 x86（无架构硬编码）。
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
SRC = os.path.join(VCPKG, "buildtrees", "directx-headers", "src", "v1.619.4-e3a2e5eef8.clean")
BUILD = os.path.join(VCPKG, "buildtrees", "directx-headers", "x86-windows-static-rel")
BUILD_DBG = os.path.join(VCPKG, "buildtrees", "directx-headers", "x86-windows-static-dbg")
PREFIX = os.path.join(VCPKG, "installed", "x86-windows-static")
X64_PREFIX = os.path.join(VCPKG, "installed", "x64-windows-static")
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


def main() -> int:
    if not os.path.exists(os.path.join(SRC, "CMakeLists.txt")):
        print(f"directx-headers source missing: {SRC}")
        return 2

    if not os.path.exists(os.path.join(BUILD, "build.ninja")):
        rc = run(["cmake", "-S", SRC, "-B", BUILD, "-G", "Ninja",
                  "-DCMAKE_BUILD_TYPE=Release",
                  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                  "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG",
                  "-DBUILD_TESTING=OFF"])
        if rc != 0:
            return rc
    rc = run(["cmake", "--build", BUILD, "--config", "Release"])
    if rc != 0:
        return rc

    # vcpkg triplet 布局还有 debug/lib：targets.cmake 会校验 debug 路径存在（哪怕本次
    # 只用 Release），所以连同 Debug 版（/MTd）一起产出，与 x64 安装目录结构一致。
    if not os.path.exists(os.path.join(BUILD_DBG, "build.ninja")):
        rc = run(["cmake", "-S", SRC, "-B", BUILD_DBG, "-G", "Ninja",
                  "-DCMAKE_BUILD_TYPE=Debug",
                  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug",
                  "-DCMAKE_CXX_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1",
                  "-DBUILD_TESTING=OFF"])
        if rc != 0:
            return rc
    rc = run(["cmake", "--build", BUILD_DBG, "--config", "Debug"])
    if rc != 0:
        return rc

    # include：header-only，直接从 x64 已安装目录复制（架构无关）
    os.makedirs(os.path.join(PREFIX, "include"), exist_ok=True)
    for sub in ("directx", "dxguids", "wsl"):
        src_dir = os.path.join(X64_PREFIX, "include", sub)
        dst_dir = os.path.join(PREFIX, "include", sub)
        if os.path.isdir(src_dir) and not os.path.exists(dst_dir):
            shutil.copytree(src_dir, dst_dir)
            print("include:", sub)

    os.makedirs(os.path.join(PREFIX, "lib"), exist_ok=True)
    copied = []
    for dp, _dn, fns in os.walk(BUILD):
        for fn in fns:
            if fn.lower().endswith(".lib"):
                shutil.copy2(os.path.join(dp, fn), os.path.join(PREFIX, "lib", fn))
                copied.append(fn)
    print("libs:", copied)

    os.makedirs(os.path.join(PREFIX, "debug", "lib"), exist_ok=True)
    for dp, _dn, fns in os.walk(BUILD_DBG):
        for fn in fns:
            if fn.lower().endswith(".lib"):
                shutil.copy2(os.path.join(dp, fn), os.path.join(PREFIX, "debug", "lib", fn))
                copied.append("debug/" + fn)
    print("libs:", copied)

    # share/directx-headers：targets 用 ${_IMPORT_PREFIX} 相对定位，x64 的可以直接搬
    src_share = os.path.join(X64_PREFIX, "share", "directx-headers")
    dst_share = os.path.join(PREFIX, "share", "directx-headers")
    if os.path.isdir(src_share) and not os.path.exists(dst_share):
        shutil.copytree(src_share, dst_share)
        print("share copied:", sorted(os.listdir(dst_share)))
    # vcpkg 生成的 config-version.cmake 带 32/64 位匹配检查（x64 包写死 "8"），
    # 32 位构建会被判 UNSUITABLE。库与头都是 32 位的，去掉该段才是正确语义。
    ver = os.path.join(dst_share, "directx-headers-config-version.cmake")
    if os.path.exists(ver):
        with open(ver, encoding="utf-8") as fh:
            text = fh.read()
        marker = '# if the installed or the using project don\'t have CMAKE_SIZEOF_VOID_P set'
        if marker in text:
            text = text.split(marker)[0]
            text += ("# 位数检查已移除：本目录为手工铺的 x86 包，share 从 x64 复制而来，\n"
                     "# 原检查会让 32 位构建永远找不到该包。\n")
            with open(ver, "w", encoding="utf-8") as fh:
                fh.write(text)
            print("patched config-version.cmake (dropped 64bit check)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
