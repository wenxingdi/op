# -*- coding: utf-8 -*-
"""批 2 发布件同步（一次性，不入库）。

源 = build/nmake-x64-Release/libop/{op_x64.dll, op_c_api_x64.dll}
      build/ninja-x86-Release/libop/{op_x86.dll, op_c_api_x86.dll}   ← x86（跨位数 dx 注入必需）
目标 = bin/x64、bindings/python/op/bin/x64、OPTool 三处 Dll、optool_verify Dll
校验 = 逐目标 sha1 与源一致（不只看拷贝返回值 —— 历史上出现过"cp 报成功但时间戳未变"）。

⚠ 2026-09-29：x86 hook dll 必须与 x64 同目录分发。x64 宿主绑 32 位窗口走 dx 注入时，
   blackbone 按 SetPath 目录找 `op_c_api_x86.dll`（代码注释：只在**已加载 dll 所在目录**找）；
   缺失时症状是 `op_c_api_x86.dll not exists` → dx 绑定干净失败（第一版踩过，非崩溃）。
"""
import hashlib
import os
import shutil
import sys

SRC_X64 = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
SRC_X86 = r"D:\AutoPro\op-master\op\build\ninja-x86-Release\libop"

# 每个文件名 -> 源目录（x64 与 x86 分属不同构建树）
SOURCES = {
    "op_x64.dll": SRC_X64,
    "op_c_api_x64.dll": SRC_X64,
    "op_x86.dll": SRC_X86,
    "op_c_api_x86.dll": SRC_X86,
}
X64 = ["op_x64.dll", "op_c_api_x64.dll"]
X86 = ["op_x86.dll", "op_c_api_x86.dll"]

# COM DLL 只进 op 仓库与 OPTool；optool_verify 只用 C API。
TARGETS = [
    (r"D:\AutoPro\op-master\op\bin\x64", X64 + X86),
    (r"D:\AutoPro\op-master\op\bindings\python\op\bin\x64", X64 + X86),
    (r"D:\AutoPro\OPTool\Common\Dll", X64 + X86),
    (r"D:\AutoPro\OPTool\OPTestTool\bin\Release\net10.0-windows7.0\Dll", X64 + X86),
    (r"D:\AutoPro\OPTool\WordDictTool\bin\Release\net10.0-windows7.0\Dll", X64 + X86),
    (r"D:\AgentWork\WorkBuddy\2026-08-04-11-41-10\optool_verify\bin\Release\net10.0-windows7.0\Dll",
     ["op_c_api_x64.dll", "op_c_api_x86.dll"]),
    # 探针/测试用的 x64 构建树：blackbone 按 `SetPath` 目录找 `op_c_api_x86.dll`，
    # 而 `workbench/_t_dx32_*.py`、`_t_bind_any.py` 等 32 位真机探针都以这里为 DLL_DIR。
    # **只补 x86**（x64 件本来就由 `_wb_build.py` 写在这个目录）。
    # 不纳入的后果（2026-09-29 实测踩到）：x86 件停在旧版 → 蜀门 `dx/dx/dx` 一直 bind=0，
    # 看着像"按需加载没生效"，其实是注入的根本不是新 dll。
    (r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop", X86),
]


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    srcs = {}
    missing = set()
    for name, sdir in SOURCES.items():
        p = os.path.join(sdir, name)
        if not os.path.isfile(p):
            print("缺源文件（将跳过该文件）:", p)
            missing.add(name)
            continue
        srcs[name] = (p, sha1(p))
        print(f"源 {name}: {os.path.getsize(p)} bytes sha1={srcs[name][1][:12]}")

    failed = []
    for target, names in TARGETS:
        if not os.path.isdir(target):
            print("SKIP(目录不存在):", target)
            continue
        print("=== ", target)
        for name in names:
            if name in missing:
                print(f"  {name}: 源缺失，跳过")
                continue
            sp, expect = srcs[name]
            dp = os.path.join(target, name)
            if os.path.isfile(dp) and sha1(dp) == expect:
                print(f"  {name}: 已一致，跳过")
                continue
            shutil.copy2(sp, dp)
            actual = sha1(dp) if os.path.isfile(dp) else "<缺失>"
            ok = actual == expect
            print(f"  {name}: {'OK' if ok else 'MISMATCH'} sha1={actual[:12]}")
            if not ok:
                failed.append(dp)

    print("\n结论:", "全部一致" if not failed else f"{len(failed)} 处不一致 -> {failed}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
