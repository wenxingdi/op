import os
import subprocess
import sys

# x64 (NMake) 配置脚本：改 CMakeLists.txt 后必须重新 configure，
# _wb_build.py 只跑 nmake，不会重新生成 build.make（新增源文件会漏编）。
#
# 用法：python scripts/build/_wb_configure_x64.py

MSVC = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"
ROOT = r"D:\AutoPro\op-master\op"
BUILD = os.path.join(ROOT, "build", "nmake-x64-Release")
CMAKE = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

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

cmd = [
    CMAKE,
    "-S", ROOT,
    "-B", BUILD,
    "-G", "NMake Makefiles",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_CXX_COMPILER=" + MSVC + r"\bin\Hostx64\x64\cl.exe",
    "-DCMAKE_MAKE_PROGRAM=" + MSVC + r"\bin\Hostx64\x64\nmake.exe",
    "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ROOT, "build", "_deps", "vcpkg", "scripts", "buildsystems", "vcpkg.cmake"),
    "-DOP_BUILD_TESTS=ON",
    "-DOP_PYTHON_WHEEL=OFF",
]

r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
out = (r.stdout or "") + "\n=== STDERR ===\n" + (r.stderr or "")
open(os.path.join(BUILD, "_wb_configure_out.txt"), "w", encoding="utf-8").write(out)
print("exit:", r.returncode)
print(out[-4000:])
sys.exit(r.returncode)
