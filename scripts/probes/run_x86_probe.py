"""编译并运行 32 位宿主探针（复用 op x86 工具链环境）。"""
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WB = os.path.join(ROOT, "workbench")
PROBE_SRC = os.path.join(WB, "x86_host_probe.c")
PROBE_EXE = os.path.join(WB, "x86_host_probe.exe")
DLL_DIR = os.path.join(ROOT, "build", "ninja-x86-Release", "libop")

MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"
NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")


def x86_env() -> dict:
    env = dict(os.environ)
    env["INCLUDE"] = ";".join([
        MSVC_ROOT + r"\include",
        os.path.join(SDK_ROOT, "Include", SDK_VER, "um"),
        os.path.join(SDK_ROOT, "Include", SDK_VER, "ucrt"),
        os.path.join(SDK_ROOT, "Include", SDK_VER, "shared"),
    ])
    env["LIB"] = ";".join([
        MSVC_ROOT + r"\lib\x86",
        os.path.join(SDK_ROOT, "Lib", SDK_VER, "um", "x86"),
        os.path.join(SDK_ROOT, "Lib", SDK_VER, "ucrt", "x86"),
    ])
    env["PATH"] = ";".join([
        MSVC_ROOT + r"\bin\Hostx64\x86",
        os.path.join(SDK_ROOT, "bin", SDK_VER, "x86"),
        NINJA_DIR,
        env.get("PATH", ""),
    ])
    return env


def run(args, **kw):
    print("$", " ".join(args), flush=True)
    return subprocess.run(args, env=x86_env(), cwd=WB, **kw)


def main() -> int:
    rc = run(["cl", "/nologo", "/W3", "/O2", "/DWIN32",
              "x86_host_probe.c", "/Fe:x86_host_probe.exe"]).returncode
    if rc != 0:
        return rc

    # 运行目录放一份 dll（探针 LoadLibrary 相对工作目录）
    sandbox = os.path.join(WB, "x86_probe_run")
    os.makedirs(sandbox, exist_ok=True)
    for fn in ("op_c_api_x86.dll", "op_x86.dll"):
        shutil.copy2(os.path.join(DLL_DIR, fn), os.path.join(sandbox, fn))
    for exe in ("x86_host_probe.exe",):
        dst = os.path.join(sandbox, exe)
        if os.path.exists(dst):
            os.remove(dst)
        shutil.copy2(PROBE_EXE, dst)

    print("=== selftest ===", flush=True)
    p = subprocess.run([os.path.join(sandbox, "x86_host_probe.exe"), "selftest"],
                       cwd=sandbox, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    print(p.stdout)
    return p.returncode


if __name__ == "__main__":
    raise SystemExit(main())
