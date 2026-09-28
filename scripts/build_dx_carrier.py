"""编译图形捕获真机载体 scripts/dx_carrier.cpp。

复用 build_dx_target.py 的 MSVC / SDK 定位逻辑，避免重复维护工具链探测代码。

用法：
    python scripts/build_dx_carrier.py                    # 输出到 workbench/probes/
    python scripts/build_dx_carrier.py <输出目录>
"""
from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "scripts", "dx_carrier.cpp")

sys.path.insert(0, os.path.join(ROOT, "scripts"))
import build_dx_target as bdt  # noqa: E402


def main() -> int:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "workbench", "probes")
    os.makedirs(out_dir, exist_ok=True)

    cl = bdt.find_cl()
    if not cl:
        print("ERROR: 未找到 cl.exe，请确认已安装 Visual Studio C++ 工具链")
        return 2
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(cl))))
    sdk_inc, sdk_lib = bdt.find_sdk()

    env = dict(os.environ)
    incs = [os.path.join(msvc, "include")]
    libs = [os.path.join(msvc, "lib", "x64")]
    if sdk_inc:
        for sub in ("ucrt", "um", "shared", "winrt", "cppwinrt"):
            p = os.path.join(sdk_inc, sub)
            if os.path.isdir(p):
                incs.append(p)
    if sdk_lib:
        for sub in ("ucrt", "um"):
            p = os.path.join(sdk_lib, sub, "x64")
            if os.path.isdir(p):
                libs.append(p)
    env["INCLUDE"] = ";".join(incs)
    env["LIB"] = ";".join(libs)
    env["PATH"] = os.path.dirname(cl) + os.pathsep + env.get("PATH", "")

    obj = os.path.join(out_dir, "dx_carrier.obj")
    exe = os.path.join(out_dir, "dx_carrier.exe")
    cmd = [
        cl, "/nologo", "/O2", "/MD", "/EHsc", "/utf-8",
        "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WINDOWS",
        "/Fo" + obj, "/Fe" + exe, SRC,
        "/link", "/SUBSYSTEM:CONSOLE", "/MACHINE:X64",
        "d3d9.lib", "d3d10.lib", "d3d11.lib", "dxgi.lib", "opengl32.lib", "user32.lib", "gdi32.lib",
    ]
    r = subprocess.run(cmd, env=env, capture_output=True)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    for line in out.split("\n"):
        if line.strip():
            print("  ", line.rstrip()[:180])
    print("rc  =", r.returncode)
    if os.path.exists(exe):
        print("exe =", exe, round(os.path.getsize(exe) / 1024, 1), "KB")
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
