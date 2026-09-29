# -*- coding: utf-8 -*-
"""枚举目标进程已加载模块（跨位数安全：SNAPMODULE|SNAPMODULE32）。

用途：判断 32 位游戏 dx/dx/dx 绑定时 SetInputHook 返回 0 是否因目标尚未加载
dinput8.dll（版本检查界面未创建 DirectInput 设备）→ 输入 hook 无目标可挂。
"""
import ctypes
import sys
from ctypes import wintypes

k32 = ctypes.windll.kernel32
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10


class PE(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_char * 260)]


class ME(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD), ("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    p = PE()
    p.dwSize = ctypes.sizeof(p)
    ok = k32.Process32First(snap, ctypes.byref(p))
    pid = 0
    while ok:
        if p.szExeFile.lower() == name.encode():
            pid = p.th32ProcessID
            break
        ok = k32.Process32Next(snap, ctypes.byref(p))
    k32.CloseHandle(snap)
    return pid


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == -1 or snap == 0xFFFFFFFF:
        return None
    m = ME()
    m.dwSize = ctypes.sizeof(m)
    out = []
    ok = k32.Module32First(snap, ctypes.byref(m))
    while ok:
        out.append((m.szModule.decode("mbcs", "replace"), m.modBaseSize))
        ok = k32.Module32Next(snap, ctypes.byref(m))
    k32.CloseHandle(snap)
    return out


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "client.exe"
    pid = int(sys.argv[2]) if len(sys.argv) > 2 else find_pid(target)
    print(f"[pid] {target}={pid}")
    mods = modules(pid)
    if mods is None:
        print("[ERR] snapshot failed (need admin / process gone)")
        raise SystemExit(1)
    print(f"[mods] total={len(mods)}")
    for name, size in sorted(mods, key=lambda x: x[0].lower()):
        print(f"  {name:32s} size=0x{size:X}")
    low = {n.lower() for n, _ in mods}
    for probe in ("dinput8.dll", "d3d9.dll", "op_c_api_x86.dll", "op_x86.dll"):
        print(f"[check] {probe:20s} loaded={probe in low}")
