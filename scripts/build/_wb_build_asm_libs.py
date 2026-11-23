"""本地构建汇编/反汇编第三方库（keystone / capstone）静态库，x64 + x86。

## 为什么不用 vcpkg

vcpkg 引导自身工具链时用 `CreateFileW` 重定向 stdin 解压 7z，在沙箱里返回
`Access is denied`（`calling CreateFileW stdin failed with 5`），两个 port 都装不上。
手工把 7z 解包到 `downloads/tools/` 也无效 —— vcpkg 仍坚持走自己的解压分支。
所以改为**直接下源码 + Ninja/MSVC 本地构建**，源码包按 sha256 校验后缓存。

## 产物

    3rd_party/capstone/include/capstone/*.h
    3rd_party/capstone/lib/x64/capstone.lib
    3rd_party/capstone/lib/x86/capstone.lib
    3rd_party/keystone/include/keystone/*.h
    3rd_party/keystone/lib/x64/keystone.lib
    3rd_party/keystone/lib/x86/keystone.lib

两个库都按 **/MT 静态运行时** 构建，与 op 全库的 `/MT /O2` 口径一致
（混 /MD 会在宿主进程里引入第二份 CRT 堆，跨 DLL 传内存必崩）。

## 关键坑

* **CMake 4.x 拒绝 `cmake_minimum_required(VERSION 2.8.7)`**（keystone 0.9.2 写的就是这个），
  必须传 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`，否则 configure 阶段直接报错退出。
* **keystone 内嵌 LLVM 3.9 默认编全部 8 个后端**（ARM/AArch64/Hexagon/Mips/PPC/Sparc/
  SystemZ/X86）；我们只用 x86，传 `-DLLVM_TARGETS_TO_BUILD=X86`，编译量降一个数量级。
* **LLVM 3.9 是 C++11 时代代码**，用 C++17 编会撞 `std::binary_function` 之类已移除的东西，
  所以第三方源码一律用 `/std:c++14` 单独编，**不影响** op 自己的 C++17 口径。
* **x86 靠 PATH 顺序切换工具链**：`Hostx64\\x86` 的 cl.exe 产出 32 位目标文件，
  同时 `LIB` 必须指向 `lib\\x86`；build 目录不能复用（CMake 缓存了编译器路径）。

用法：
    python scripts/build/_wb_build_asm_libs.py                 # 全部（2 库 x 2 架构）
    python scripts/build/_wb_build_asm_libs.py --lib capstone  # 只做 capstone
    python scripts/build/_wb_build_asm_libs.py --arch x86      # 只做 x86
    python scripts/build/_wb_build_asm_libs.py --clean         # 清 build 目录重来
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _root import repo_root  # noqa: E402

ROOT = repo_root()
DEPS = os.path.join(ROOT, "build", "_deps", "asm_libs")
THIRD = os.path.join(ROOT, "3rd_party")

NINJA_DIR = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
             r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
# ⚠ 必须用 VS 自带的 cmake（3.31.x），**不能用** pip 装的 cmake 4.x：
# keystone 0.9.2 的 CMakeLists 里有 `cmake_policy(SET CMP0051 OLD)`，
# CMake 4 已彻底移除该策略的 OLD 行为 → configure 直接 "Policy CMP0051 may not be
# set to OLD behavior"。3.31 只警告不报错，配合 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
# 可以连 <3.5 的兼容警告一起压掉。
VS_CMAKE = (r"D:\Program Files\Microsoft Visual Studio\2022\Professional"
            r"\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
CMAKE = VS_CMAKE if os.path.isfile(VS_CMAKE) else "cmake"
MSVC_ROOT = r"D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"
SDK_VER = "10.0.26100.0"

# 源码包：name -> (version, url, sha256)
PKGS = {
    "capstone": (
        "5.0.3",
        "https://codeload.github.com/capstone-engine/capstone/tar.gz/refs/tags/5.0.3",
        "3970c63ca1f8755f2c8e69b41432b710ff634f1b45ee4e5351defec4ec8e1753",
    ),
    "keystone": (
        "0.9.2",
        "https://codeload.github.com/keystone-engine/keystone/tar.gz/refs/tags/0.9.2",
        "c9b3a343ed3e05ee168d29daf89820aff9effb2c74c6803c2d9e21d55b5b7c24",
    ),
}


def tool_env(arch: str) -> dict:
    """按架构拼 MSVC 环境（Hostx64\\<arch> 的 cl 决定目标位数）。"""
    env = dict(os.environ)
    bits = "x64" if arch == "x64" else "x86"
    env["INCLUDE"] = ";".join([
        MSVC_ROOT + r"\include",
        MSVC_ROOT + r"\ATLMFC\include",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\um",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\ucrt",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\shared",
        SDK_ROOT + r"\Include" + "\\" + SDK_VER + r"\winrt",
    ])
    env["LIB"] = ";".join([
        MSVC_ROOT + "\\lib\\" + bits,
        MSVC_ROOT + r"\ATLMFC\lib" + "\\" + bits,
        SDK_ROOT + r"\Lib" + "\\" + SDK_VER + r"\um\\" + bits,
        SDK_ROOT + r"\Lib" + "\\" + SDK_VER + r"\ucrt\\" + bits,
    ])
    # PATH 里**不能**留别的 cl：CMake 按 PATH 第一个 cl 判定目标位数
    env["PATH"] = ";".join([
        MSVC_ROOT + "\\bin\\Hostx64\\" + bits,
        os.path.dirname(CMAKE) if os.path.isfile(VS_CMAKE) else "",
        SDK_ROOT + r"\bin" + "\\" + SDK_VER + "\\" + bits,
        NINJA_DIR,
        env.get("PATH", ""),  # cmake 由 pip 安装（Python312\Scripts），走继承 PATH 即可
    ])
    return env


def run(args: list[str], env: dict, tail: int = 4000) -> int:
    print("$ " + " ".join(args), flush=True)
    p = subprocess.run(args, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace")
    out = p.stdout or ""
    print(out[-tail:], flush=True)
    return p.returncode


def fetch(name: str) -> str:
    """下载并解包源码，返回源码根目录。已存在则跳过（幂等）。"""
    ver, url, digest = PKGS[name]
    src = os.path.join(DEPS, f"{name}-{ver}")
    if os.path.isdir(os.path.join(src, "CMakeLists.txt")) or \
       os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        print(f"[skip] 源码已在: {src}")
        return src
    # 有的包解压后是 <name>-<ver>/ 的子目录结构，用统一循环探测
    if os.path.isdir(src) and os.listdir(src):
        return src
    os.makedirs(DEPS, exist_ok=True)
    tgz = os.path.join(DEPS, f"{name}-{ver}.tar.gz")
    if not os.path.isfile(tgz):
        print(f"[get ] {url}")
        urllib.request.urlretrieve(url, tgz)
    got = hashlib.sha256(open(tgz, "rb").read()).hexdigest()
    if got != digest:
        raise SystemExit(f"sha256 不匹配 {name}: 期望 {digest} 实际 {got}")
    print(f"[ok  ] sha256 {name} {got[:16]}…")
    tmp = os.path.join(DEPS, f"_x_{name}")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    with tarfile.open(tgz, "r:gz") as tf:
        # tarfile 的 'data' 过滤：挡掉绝对路径/.. 越权（Python 3.12+ 默认行为，这里显式声明）
        try:
            tf.extractall(tmp, filter="data")
        except TypeError:
            tf.extractall(tmp)
    inner = [d for d in os.listdir(tmp) if os.path.isdir(os.path.join(tmp, d))]
    if len(inner) != 1:
        raise SystemExit(f"解包后发现多个顶层目录: {inner}")
    shutil.move(os.path.join(tmp, inner[0]), src)
    shutil.rmtree(tmp, ignore_errors=True)
    return src


def cmake_cfg(src: str, build: str, extra: list[str], env: dict) -> int:
    os.makedirs(build, exist_ok=True)
    args = [CMAKE, "-S", src, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release"] + extra
    return run(args, env)


def build_capstone(arch: str) -> str:
    src = fetch("capstone")
    build = os.path.join(DEPS, f"build-capstone-{arch}")
    env = tool_env(arch)
    if not os.path.isfile(os.path.join(build, "build.ninja")):
        # 只编 x86 后端：我们的 Disassemble 没有 arch 参数，永远按 x86/x64 走
        rc = cmake_cfg(src, build, [
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCAPSTONE_BUILD_STATIC_RUNTIME=ON",
            "-DCAPSTONE_BUILD_TESTS=OFF",
            "-DCAPSTONE_BUILD_CSTOOL=OFF",
            "-DCAPSTONE_INSTALL=OFF",
            "-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF",
            "-DCAPSTONE_X86_SUPPORT=ON",
            "-DCMAKE_C_FLAGS=/MT /O2 /D_CRT_SECURE_NO_WARNINGS",
            "-DCMAKE_CXX_FLAGS=/MT /O2 /D_CRT_SECURE_NO_WARNINGS",
        ], env)
        if rc != 0:
            raise SystemExit(f"capstone {arch} configure 失败")
    # capstone 5.x 的静态库目标名就是 capstone；老版本叫 capstone-static，兜底试一次
    rc = run([CMAKE, "--build", build, "--target", "capstone"], env)
    if rc != 0:
        rc = run([CMAKE, "--build", build, "--target", "capstone-static"], env)
    if rc != 0:
        raise SystemExit(f"capstone {arch} 编译失败")
    for cand in ("libcapstone.a", "capstone.lib", "libcapstone.lib"):
        p = os.path.join(build, cand)
        if os.path.isfile(p):
            return p
    raise SystemExit(f"capstone {arch} 找不到产物: {os.listdir(build)}")


def patch_keystone_static_header(src: str) -> None:
    """把 keystone.h 的 `KEYSTONE_EXPORT` 改成可被 KEYSTONE_STATIC 关掉。

    ## 为什么必须改源码而不是改使用侧

    keystone.h 在 MSVC 下**无条件** `#define KEYSTONE_EXPORT __declspec(dllexport)`。
    dllexport 会写进**定义该符号的那个 .obj 的 .drectve 段**（`/EXPORT:ks_open`），
    链接器无条件照办 —— 也就是说 `keystone.lib` 一旦进 op.dll，`ks_open/ks_asm/...`
    十来个符号就会**自动出现在 op.dll 的导出表**里，改使用侧的头文件也拦不住。
    这不是洁癖问题：op.dll 的导出表是**对外契约**（易语言/宿主按名取函数），
    多出的 `ks_*` 与外部 keystone.dll 撞名会让 GetProcAddress 命中错模块。

    改法：加 `KEYSTONE_STATIC` 分支（与本目录 capstone 的 `CAPSTONE_STATIC` 口径一致），
    并在编译/使用两侧都定义它。幂等：已改过就跳过。
    """
    h = os.path.join(src, "include", "keystone", "keystone.h")
    with open(h, "r", encoding="utf-8", errors="replace") as f:
        s = f.read()
    if "KEYSTONE_STATIC" in s:
        return
    old = "#define KEYSTONE_EXPORT __declspec(dllexport)"
    new = ("#if defined(KEYSTONE_STATIC)\n"
           "#define KEYSTONE_EXPORT\n"
           "#else\n"
           "#define KEYSTONE_EXPORT __declspec(dllexport)\n"
           "#endif")
    if old not in s:
        raise SystemExit("keystone.h 结构变了：找不到 KEYSTONE_EXPORT 定义，需人工确认")
    with open(h, "w", encoding="utf-8") as f:
        f.write(s.replace(old, new, 1))
    print("[patch] keystone.h: KEYSTONE_EXPORT 支持 KEYSTONE_STATIC（避免 ks_* 污染 op.dll 导出表）")


def patch_keystone_radix(src: str) -> None:
    """修 keystone 0.9.2 的 radix bug：`ks_option` 里无条件 `setRadix(16)`。

    ## 现象

    `mov eax, 100` 编成 `B8 00 01 00 00` —— 裸数字被当**十六进制** 0x100 = 256。
    更糟的是**没有任何语法能写十进制**：`42d` / `0d42` / `42h` 全部
    `KS_ERR_ASM_INVALIDOPERAND`，因为 radix 一旦是 16，`0x` 前缀和裸数字就没区别了。

    ## 根因

    `ks.cpp` 的 `ks_option()` 第一行就是 `ks->MAI->setRadix(16);`，而后面
    `KS_OPT_SYNTAX_INTEL` / `_NASM` / `_ATT` 这些**不带 RADIX16 位**的分支
    只是设 dialect 就 break，**从不把 radix 改回去**。于是官方选项
    `KS_OPT_SYNTAX_RADIX16` 形同虚设（默认已是 16），而官方设计意图
    （裸数字=十进制，`0x`=十六进制）永远达不到。

    ## 我们的改法

    入口改成 `setRadix(10)`；带 `RADIX16` 位的分支里原本就有 `setRadix(16)`，
    于是 **默认十进制、显式带 RADIX16 才是十六进制**，与上游文档一致。
    十六进制仍可用 `0x` 前缀表达 ⇒ 这是**严格更宽**的字面量语法，不存在"改成十进制
    就写不出十六进制"的退化。项目内固定走十进制口径（见 OpAsm.cpp 的说明与用例）。

    幂等：已改过就跳过。
    """
    p = os.path.join(src, "llvm", "keystone", "ks.cpp")
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        s = f.read()
    if "op patch 2026-11-23" in s:
        return
    old = "ks_err ks_option(ks_engine *ks, ks_opt_type type, size_t value)\n{\n    ks->MAI->setRadix(16);"
    if old not in s:
        raise SystemExit("ks.cpp 结构变了：找不到 ks_option 里的 setRadix(16)，需人工确认")
    new = ("ks_err ks_option(ks_engine *ks, ks_opt_type type, size_t value)\n{\n"
           "    // [op patch 2026-11-23] 原为无条件 setRadix(16)：裸数字全按十六进制，\n"
           "    // 且不存在任何十进制写法。改为默认 10，仅显式带 RADIX16 位时切 16。\n"
           "    ks->MAI->setRadix(10);")
    with open(p, "w", encoding="utf-8") as f:
        f.write(s.replace(old, new, 1))
    print("[patch] ks.cpp: 裸数字默认十进制（原实现无条件 radix=16，写不出十进制）")


def build_keystone(arch: str) -> str:
    src = fetch("keystone")
    patch_keystone_static_header(src)
    patch_keystone_radix(src)
    build = os.path.join(DEPS, f"build-keystone-{arch}")
    env = tool_env(arch)
    # 每次都 reconfigure：静态/动态口径变了必须重写 build.ninja
    rc = cmake_cfg(src, build, [
        # CMake 4.x 不再兼容 <3.5 的 cmake_minimum_required，这个变量放行
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DLLVM_TARGETS_TO_BUILD=X86",
        "-DBUILD_LIBS_ONLY=ON",
        "-DKEYSTONE_BUILD_STATIC_RUNTIME=ON",
        # LLVM 3.9 是 C++11 时代代码，/std:c++14 是它还能编的最高档
        "-DCMAKE_C_FLAGS=/MT /O2 /std:c11 /DKEYSTONE_STATIC /D_CRT_SECURE_NO_WARNINGS /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS",
        "-DCMAKE_CXX_FLAGS=/MT /O2 /std:c++14 /DKEYSTONE_STATIC /D_CRT_SECURE_NO_WARNINGS /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS",
    ], env)
    if rc != 0:
        raise SystemExit(f"keystone {arch} configure 失败")
    rc = run([CMAKE, "--build", build, "--target", "keystone", "--parallel"], env)
    if rc != 0:
        raise SystemExit(f"keystone {arch} 编译失败")
    # keystone 的静态库落在 <build>/llvm/lib/keystone.lib（顶层没有）
    for cand in ("llvm/lib/keystone.lib", "keystone.lib", "libkeystone.a", "libkeystone.lib"):
        p = os.path.join(build, cand.replace("/", os.sep))
        if os.path.isfile(p):
            return p
    raise SystemExit(f"keystone {arch} 找不到产物: {os.listdir(build)}")


def stage(name: str, arch: str, lib_path: str, src: str) -> None:
    """把头文件与 lib 落到 3rd_party/<name>/。"""
    dst = os.path.join(THIRD, name)
    inc_dst = os.path.join(dst, "include", name)
    os.makedirs(inc_dst, exist_ok=True)
    lib_dst = os.path.join(dst, "lib", arch)
    os.makedirs(lib_dst, exist_ok=True)
    for base in (os.path.join(src, "include", name), os.path.join(src, name)):
        if os.path.isdir(base):
            for fn in os.listdir(base):
                if fn.endswith(".h"):
                    shutil.copy2(os.path.join(base, fn), os.path.join(inc_dst, fn))
            break
    else:
        raise SystemExit(f"{name} 找不到头文件目录")
    shutil.copy2(lib_path, os.path.join(lib_dst, f"{name}.lib"))
    size = os.path.getsize(os.path.join(lib_dst, f"{name}.lib"))
    print(f"[stage] 3rd_party/{name}/lib/{arch}/{name}.lib  {size/1024:.0f} KB")
    for lic in ("LICENSE", "LICENSE.TXT", "LICENSE-COM.TXT", "COPYING"):
        p = os.path.join(src, lic)
        if os.path.isfile(p) and not os.path.isfile(os.path.join(dst, "LICENSE")):
            shutil.copy2(p, os.path.join(dst, "LICENSE"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", choices=["capstone", "keystone", "all"], default="all")
    ap.add_argument("--arch", choices=["x64", "x86", "both"], default="both")
    ap.add_argument("--clean", action="store_true", help="删除 build 目录后重编")
    a = ap.parse_args()

    libs = ["capstone", "keystone"] if a.lib == "all" else [a.lib]
    archs = ["x64", "x86"] if a.arch == "both" else [a.arch]
    os.makedirs(DEPS, exist_ok=True)
    if a.clean:
        for lib in libs:
            for arch in archs:
                d = os.path.join(DEPS, f"build-{lib}-{arch}")
                if os.path.isdir(d):
                    shutil.rmtree(d)
                    print(f"[clean] {d}")

    for lib in libs:
        src = fetch(lib)
        for arch in archs:
            print(f"\n=== {lib} {arch} ===", flush=True)
            lib_path = build_capstone(arch) if lib == "capstone" else build_keystone(arch)
            stage(lib, arch, lib_path, src)
    print("\n全部完成", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
