import os
import subprocess
import sys

MSVC = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"
BUILD = r"D:\AutoPro\op-master\op\build\nmake-x64-Release"

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

targets = sys.argv[1:] or ["op_x64", "op_test"]
r = subprocess.run(
    [r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\nmake.exe"]
    + targets,
    cwd=BUILD,
    env=env,
    capture_output=True,
    text=True,
    encoding="utf-8",
    errors="replace",
)
out = (r.stdout or "") + "\n=== STDERR ===\n" + (r.stderr or "")
open(os.path.join(BUILD, "_wb_build_out.txt"), "w", encoding="utf-8").write(out)
print("exit:", r.returncode)
print(out[-3000:])
