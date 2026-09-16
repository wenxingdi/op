"""编译 DX 验证目标窗口 scripts/dx_target.cpp。

默认输出到一个**不含 onnxruntime.dll** 的目录，用于验证：
  - DELAYLOAD 修复前：注入 op_c_api_x64.dll 会因找不到 onnxruntime.dll 失败（0xC0000135）
  - DELAYLOAD 修复后：应能成功注入并完成 dx 绑定

用法：
    python scripts/build_dx_target.py                 # 输出到 %TEMP%\\dxprobe_lab
    python scripts/build_dx_target.py <输出目录>        # 自定义
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "scripts", "dx_target.cpp")

VS_BASES = [
    r"C:\Program Files\Microsoft Visual Studio",
    r"C:\Program Files (x86)\Microsoft Visual Studio",
    r"D:\Program Files\Microsoft Visual Studio",
    r"D:\Program Files (x86)\Microsoft Visual Studio",
]
SDK_BASES = [
    r"C:\Program Files (x86)\Windows Kits\10",
    r"D:\Program Files (x86)\Windows Kits\10",
    r"C:\Program Files\Windows Kits\10",
]


def find_cl() -> str | None:
    for base in VS_BASES:
        if not os.path.isdir(base):
            continue
        hits = glob.glob(os.path.join(base, "**", "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "cl.exe"),
                         recursive=True)
        if hits:
            return sorted(hits)[-1]
    return None


def find_sdk() -> tuple[str | None, str | None]:
    for base in SDK_BASES:
        inc_root = os.path.join(base, "Include")
        lib_root = os.path.join(base, "Lib")
        if not os.path.isdir(inc_root):
            continue
        incs = sorted(os.listdir(inc_root))
        libs = sorted(os.listdir(lib_root)) if os.path.isdir(lib_root) else []
        if incs:
            return os.path.join(inc_root, incs[-1]), (os.path.join(lib_root, libs[-1]) if libs else None)
    return None, None


def main() -> int:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TEMP", "."), "dxprobe_lab")
    os.makedirs(out_dir, exist_ok=True)

    cl = find_cl()
    if not cl:
        print("ERROR: 未找到 cl.exe，请确认已安装 Visual Studio C++ 工具链")
        return 2
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(cl))))
    sdk_inc, sdk_lib = find_sdk()

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

    print("cl      =", cl)
    print("MSVC    =", msvc)
    print("SDK inc =", sdk_inc)
    print("SDK lib =", sdk_lib)
    print("输出目录 =", out_dir)
    print()

    obj = os.path.join(out_dir, "dx_target.obj")
    exe = os.path.join(out_dir, "dx_target.exe")
    cmd = [cl, "/nologo", "/O2", "/MD", "/EHsc", "/utf-8",
           "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WINDOWS",
           "/Fo" + obj, "/Fe" + exe, SRC,
           "/link", "/SUBSYSTEM:WINDOWS", "/MACHINE:X64",
           "user32.lib", "gdi32.lib"]
    r = subprocess.run(cmd, env=env, capture_output=True)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    for line in out.split("\n"):
        if line.strip():
            print("  ", line.rstrip()[:170])
    print()
    print("rc =", r.returncode)
    if os.path.exists(exe):
        print("exe =", exe, round(os.path.getsize(exe) / 1024, 1), "KB")
    print("目录内 onnxruntime.dll 存在 =", os.path.exists(os.path.join(out_dir, "onnxruntime.dll")))
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
