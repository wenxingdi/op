"""编译图形捕获真机载体 scripts/dx_carrier.cpp（x64 / x86 双架构）。

复用 build_dx_target.py 的 MSVC / SDK 定位逻辑，避免重复维护工具链探测代码。

为什么需要 x86 版：
    hook 注入要求 DLL 位数与**目标进程**位数一致；32 位 D3D9 目标此前只能拿真实
    游戏（蜀门 client.exe）当靶子。x86 载体让 32 位 dx 注入链路有一个尺寸/颜色
    完全可控、且不依赖游戏状态的受控靶子。

用法：
    python scripts/build_dx_carrier.py                      # x64 -> workbench/probes/dx_carrier.exe
    python scripts/build_dx_carrier.py --arch x86           # x86 -> workbench/probes/dx_carrier_x86.exe
    python scripts/build_dx_carrier.py --arch x86 <输出目录>
"""
from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "scripts", "dx_carrier.cpp")

sys.path.insert(0, os.path.join(ROOT, "scripts"))
import build_dx_target as bdt  # noqa: E402

VALID_ARCHS = ("x64", "x86")


def parse_args(argv: list[str]) -> tuple[str, str]:
    """返回 (arch, out_dir)。位置参数 = 输出目录，--arch 选择目标架构。"""
    arch = "x64"
    positional: list[str] = []
    i = 0
    while i < len(argv):
        if argv[i] == "--arch" and i + 1 < len(argv):
            arch = argv[i + 1].lower()
            i += 2
            continue
        positional.append(argv[i])
        i += 1
    if arch not in VALID_ARCHS:
        raise SystemExit(f"ERROR: --arch 只支持 {VALID_ARCHS}，收到 {arch!r}")
    out_dir = positional[0] if positional else os.path.join(ROOT, "workbench", "probes")
    return arch, out_dir


def main() -> int:
    arch, out_dir = parse_args(sys.argv[1:])
    os.makedirs(out_dir, exist_ok=True)

    cl = bdt.find_cl(arch)
    if not cl:
        print(f"ERROR: 未找到 {arch} 的 cl.exe，请确认已安装 Visual Studio C++ 工具链")
        return 2
    # bin/Hostx64/<arch>/cl.exe -> 上溯 4 级得到 MSVC 版本根
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(cl))))
    sdk_inc, sdk_lib = bdt.find_sdk()

    env = dict(os.environ)
    incs = [os.path.join(msvc, "include")]
    libs = [os.path.join(msvc, "lib", arch)]
    if sdk_inc:
        for sub in ("ucrt", "um", "shared", "winrt", "cppwinrt"):
            p = os.path.join(sdk_inc, sub)
            if os.path.isdir(p):
                incs.append(p)
    if sdk_lib:
        for sub in ("ucrt", "um"):
            p = os.path.join(sdk_lib, sub, arch)
            if os.path.isdir(p):
                libs.append(p)
    env["INCLUDE"] = ";".join(incs)
    env["LIB"] = ";".join(libs)
    # cl.exe 与 link.exe 同在 bin/Hostx64/<arch>/，把该目录前置即可（无需 vcvars）
    env["PATH"] = os.path.dirname(cl) + os.pathsep + env.get("PATH", "")

    suffix = "" if arch == "x64" else "_x86"
    obj = os.path.join(out_dir, f"dx_carrier{suffix}.obj")
    exe = os.path.join(out_dir, f"dx_carrier{suffix}.exe")
    # x86 走静态 CRT：免 VC 运行库依赖，避免目标机上缺 x86 vcruntime 导致载体起不来
    # （载体本身是探针，自包含比省体积重要；x64 保持 /MD 与既有产物一致）。
    crt = "/MD" if arch == "x64" else "/MT"
    cmd = [
        cl, "/nologo", "/O2", crt, "/EHsc", "/utf-8",
        "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WINDOWS",
        f"/Fo{obj}", f"/Fe{exe}", SRC,
        "/link", "/SUBSYSTEM:CONSOLE", f"/MACHINE:{arch.upper()}",
        "d3d9.lib", "d3d10.lib", "d3d11.lib", "dxgi.lib", "opengl32.lib", "user32.lib", "gdi32.lib",
    ]
    r = subprocess.run(cmd, env=env, capture_output=True)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    for line in out.split("\n"):
        if line.strip():
            print("  ", line.rstrip()[:180])
    print("arch=", arch)
    print("rc  =", r.returncode)
    if os.path.exists(exe):
        print("exe =", exe, round(os.path.getsize(exe) / 1024, 1), "KB")
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
