"""带 /VERBOSE:LIB 复跑 op_c_api_x86 链接，定位 MSVCRT.lib 引用来源。"""
import os
import sys
import subprocess

# 仓库根：脚本自 <repo>/build/ 归档到 <repo>/scripts/build/ 后，原先的
# dirname(dirname(__file__)) 会指向 scripts/。改为向上找 .git，放哪层都不失效。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _root import repo_root  # noqa: E402

ROOT = repo_root()
MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"
NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")

env = dict(os.environ)
env["LIB"] = ";".join([
    MSVC_ROOT + r"\lib\x86",
    MSVC_ROOT + r"\ATLMFC\lib\x86",
    os.path.join(SDK_ROOT, "Lib", SDK_VER, "um", "x86"),
    os.path.join(SDK_ROOT, "Lib", SDK_VER, "ucrt", "x86"),
])
env["PATH"] = ";".join([
    MSVC_ROOT + r"\bin\Hostx64\x86",
    os.path.join(SDK_ROOT, "bin", SDK_VER, "x86"),
    NINJA_DIR,
    env.get("PATH", ""),
])

p = subprocess.run(
    ["cmake", "--build", os.path.join(ROOT, "build", "ninja-x86-Release"),
     "--config", "Release", "--", "op_c_api_x86"],
    cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    text=True, encoding="utf-8", errors="replace")
out = p.stdout or ""
open(os.path.join(ROOT, "build", "x86_verbose_link.log"), "w",
     encoding="utf-8", errors="replace").write(out)
print("exit", p.returncode)
for line in out.splitlines():
    low = line.lower()
    if "msvcrt" in low or "except_handler" in low or "searching" in low and "libcmt" in low:
        print(line[:200])
