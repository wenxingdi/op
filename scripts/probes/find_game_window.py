#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""枚举可见顶层窗口，找游戏窗口（工作区脚本，不入库）。

坑：① 必须显式声明 DPI 感知，否则拿到虚拟化逻辑坐标/尺寸；
    ② ctypes 回调与 Win32 API 必须显式 argtypes，否则 64 位句柄 OverflowError。
"""
import ctypes
from ctypes import wintypes

try:
    ctypes.windll.shcore.SetProcessDpiAwarenessContext(-4)
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()

u = ctypes.windll.user32
u.IsWindowVisible.argtypes = [wintypes.HWND]
u.IsWindowVisible.restype = wintypes.BOOL
u.GetWindowTextLengthW.argtypes = [wintypes.HWND]
u.GetWindowTextLengthW.restype = ctypes.c_int
u.GetWindowTextW.argtypes = [wintypes.HWND, ctypes.c_wchar_p, ctypes.c_int]
u.GetWindowTextW.restype = ctypes.c_int
u.GetClassNameW.argtypes = [wintypes.HWND, ctypes.c_wchar_p, ctypes.c_int]
u.GetClassNameW.restype = ctypes.c_int
u.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u.GetWindowRect.restype = wintypes.BOOL
u.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
u.EnumWindows.restype = wintypes.BOOL
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

rows = []


def cb(hwnd, _lp):
    if not u.IsWindowVisible(hwnd):
        return True
    n = u.GetWindowTextLengthW(hwnd)
    if n == 0:
        return True
    buf = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(hwnd, buf, n + 1)
    cbuf = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(hwnd, cbuf, 256)
    r = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    rows.append((int(hwnd) & 0xFFFFFFFF, buf.value, cbuf.value, r.right - r.left, r.bottom - r.top))
    return True


u.EnumWindows(EnumWindowsProc(cb), 0)
print(f"{'hwnd':>12}  {'class':<26} {'size':<11} title")
for hwnd, title, cls, w, h in rows:
    mark = "   <<< GAME?" if ("蜀门" in title or "shumeng" in title.lower()) else ""
    print(f"{hwnd:>12}  {cls:<26} {w}x{h:<11} {title[:48]}{mark}")
