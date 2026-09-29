# -*- coding: utf-8 -*-
"""run_app 真机实测：蜀门 game.exe / game.lnk 两条路径（缺陷②回归现场）。"""
import ctypes
import os
import sys

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]
dll.OpRunApp.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_uint32)]
dll.OpRunApp.restype = ctypes.c_int

h = dll.OpCreate()
assert h, "OpCreate failed"

EXE = r"D:\Program Files (x86)\shumen\game.exe"
LNK = r"D:\Program Files (x86)\shumen\game.lnk"


def run_app(cmd, mode, label):
    pid = ctypes.c_uint32(0)
    ret = dll.OpRunApp(h, cmd, mode, ctypes.byref(pid))
    ok = "OK" if ret == 1 else "FAIL"
    print(f"[{ok}] {label}: ret={ret} pid={pid.value}")
    return ret, pid.value


cases = [
    (EXE, 0, "exe mode=0(默认目录)"),
    (EXE, 1, "exe mode=1(工作目录=exe 所在目录,缺陷②回归点)"),
    (LNK, 0, "lnk mode=0(ShellExecuteEx 分支)"),
    (LNK, 1, "lnk mode=1(应同样走 Shell 分支)"),
]

for cmd, mode, label in cases:
    if not os.path.exists(cmd):
        print(f"[SKIP] {label}: 路径不存在 {cmd}")
        continue
    run_app(cmd, mode, label)

dll.OpDestroy(h)
print("DONE")
