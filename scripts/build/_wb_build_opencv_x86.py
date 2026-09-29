"""编译 OpenCV 5.0.0 Win32(32 位) 静态库 —— 供 op_c_api_x86.dll 链接。

对齐 x64 的既有配置（build/_deps/opencv/build/nmake-x64）：
  BUILD_LIST=core,imgcodecs,flann,imgproc,features,objdetect,stereo,calib
  BUILD_SHARED_LIBS=OFF / BUILD_TESTS=OFF / BUILD_PERF_TESTS=OFF
  CPU_BASELINE=SSE3 / Release
差异：生成器改用 Ninja（NMake 单线程，16 核浪费；产物与生成器无关）。
本脚本属工作区脚本，不入库（与 _wb_build.py 同）。
"""
from __future__ import annotations

import os
import subprocess
import sys
import time

# 仓库根：脚本自 <repo>/build/ 归档到 <repo>/scripts/build/ 后，原先的
# dirname(dirname(__file__)) 会指向 scripts/。改为向上找 .git，放哪层都不失效。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _root import repo_root  # noqa: E402

ROOT = repo_root()
DEPS = os.path.join(ROOT, "build", "_deps", "opencv")
SRC = os.path.join(DEPS, "opencv-5.0.0")
BUILD = os.path.join(DEPS, "build", "nmake-x86")
INSTALL = os.path.join(DEPS, "install", "nmake-x86")

VCVARS = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")

CMAKE_ARGS = [
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DBUILD_LIST=core,imgcodecs,flann,imgproc,features,objdetect,stereo,calib",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_TESTS=OFF",
    "-DBUILD_PERF_TESTS=OFF",
    "-DBUILD_EXAMPLES=OFF",
    "-DBUILD_DOCS=OFF",
    "-DBUILD_JAVA=OFF",
    "-DBUILD_opencv_python3=OFF",
    "-DBUILD_opencv_apps=OFF",
    "-DCPU_BASELINE=SSE3",
    "-DWITH_IPP=OFF",
    "-DWITH_ITT=OFF",   # 与 x64 保持一致：x64 缓存 WITH_ITT:BOOL=OFF，开了会引入 ittnotify 链接依赖
    # 以下三项对齐 x64（x64 staticlib 无 libwebp/libtiff/libopenjp2）：
    # WebP mux 符号（WebPMuxSetChunk 等）在 OpenCV 内置 libwebp 移植里不编 → LNK2019
    "-DWITH_WEBP=OFF",
    "-DWITH_TIFF=OFF",
    "-DWITH_OPENJPEG=OFF",
    "-DWITH_JASPER=OFF",   # x64 同为 OFF； JasPer 静态库未编入 → _jas_* LNK2019
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>",  # x64 同值 → /MD
    "-DWITH_OPENCL=OFF",
    "-DWITH_CUDA=OFF",
    "-DWITH_PROTOBUF=OFF",
    "-DWITH_FFMPEG=OFF",
    "-DWITH_MSMF=OFF",
    "-DWITH_DSHOW=OFF",
    "-DWITH_GTK=OFF",
    "-DWITH_QT=OFF",
    "-DWITH_WIN32UI=OFF",
    f"-DCMAKE_INSTALL_PREFIX={INSTALL}",
]


MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"


def x86_env() -> dict:
    """手工拼 32 位构建环境（与 build/_wb_build.py 同一手法，不用 vcvarsall：
    vcvarsall x86 不把 Windows SDK 的 rc/mt 加进 PATH，CMake 的编译器探测会失败）。"""
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
        MSVC_ROOT + r"\bin\Hostx64\x86",           # 交叉编译器（比 Hostx86 快）
        SDK_ROOT + r"\bin" + "\\" + SDK_VER + r"\x86",  # rc.exe / mt.exe
        NINJA_DIR,
        env.get("PATH", ""),
    ])
    return env


def run(step: str, args: list[str]) -> int:
    print(f"===== {step} =====", flush=True)
    print(f"$ {' '.join(args)}\n", flush=True)
    t0 = time.time()
    proc = subprocess.run(args, cwd=ROOT, env=x86_env(),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace")
    tail = (proc.stdout or "")[-4000:]
    print(tail, flush=True)
    print(f"\n[{step}] exit={proc.returncode} elapsed={time.time() - t0:.0f}s", flush=True)
    return proc.returncode


def main() -> int:
    if not os.path.exists(os.path.join(SRC, "CMakeLists.txt")):
        print(f"OpenCV source missing: {SRC}")
        return 2

    # 坏 cache（例如编译器探测失败）也会留下 CMAKE_GENERATOR 行，只按生成器判定会复用它。
    # 这里以「是否 configure 成功产出 build.ninja」为唯一判据。
    reuse = os.path.exists(os.path.join(BUILD, "build.ninja"))
    if not reuse:
        # 复用失败/异生成器的 cache 会让 CMake 直接报错，清干净重来
        subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", BUILD], cwd=ROOT)
        rc = run("configure", ["cmake", "-S", SRC, "-B", BUILD] + CMAKE_ARGS)
        if rc != 0:
            return rc
    else:
        # 每次都重新配置（幂等，~15s）：改 CMake 参数后陈旧 cache 是踩过的坑
        rc = run("reconfigure", ["cmake", "-S", SRC, "-B", BUILD] + CMAKE_ARGS)
        if rc != 0:
            return rc

    rc = run("build", ["cmake", "--build", BUILD, "--config", "Release"])
    if rc != 0:
        return rc

    rc = run("install", ["cmake", "--install", BUILD, "--config", "Release"])
    if rc != 0:
        return rc

    libs = sorted(f for f in os.listdir(INSTALL) for _ in [0]) if os.path.isdir(INSTALL) else []
    found = []
    for dp, _dn, fns in os.walk(INSTALL):
        for fn in fns:
            if fn.endswith(".lib"):
                found.append(os.path.join(dp, fn))
    print(f"\ninstalled .lib count = {len(found)}")
    for p in sorted(found)[:20]:
        print("  ", os.path.relpath(p, INSTALL))
    return 0


if __name__ == "__main__":
    sys.exit(main())
