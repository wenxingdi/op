# -*- coding: utf-8 -*-
"""用 comtypes 强类型包装独立验证 COM 路径 GetClientSize/GetWindowRect 的 VARIANT* 出参。"""
import ctypes
from ctypes import wintypes
import sys

# 找记事本窗口
u32 = ctypes.windll.user32
target = None

notepad = None
anywin = None

def _enum_cb(hwnd, lparam):
    global notepad, anywin
    if not u32.IsWindowVisible(hwnd):
        return True
    cbuf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, cbuf, 256)
    tbuf = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, tbuf, 256)
    if cbuf.value == "Notepad" and tbuf.value and notepad is None:
        notepad = hwnd
    if tbuf.value and cbuf.value not in ("Shell_TrayWnd", "Progman", "WorkerW") and anywin is None:
        anywin = (hwnd, cbuf.value, tbuf.value)
    return True

ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows(ENUMPROC(_enum_cb), 0)
if notepad:
    target = notepad
    print(f"target: Notepad hwnd={target} (0x{target:X})")
elif anywin:
    target = anywin[0]
    print(f"no Notepad; fallback hwnd={target} (0x{target:X}) class='{anywin[1]}' title='{anywin[2]}'")
else:
    print("no window at all")
    sys.exit(1)

r = wintypes.RECT()
u32.GetClientRect(target, ctypes.byref(r))
print(f"[native] GetClientRect = {r.right}x{r.bottom}")
wr = wintypes.RECT()
u32.GetWindowRect(target, ctypes.byref(wr))
print(f"[native] GetWindowRect = ({wr.left},{wr.top})-({wr.right},{wr.bottom})")

import comtypes.client
try:
    mod = comtypes.client.GetModule(r"D:\AutoPro\op-master\op\bin\x64\op_x64.dll")
    print("typelib module loaded")
    op = comtypes.client.CreateObject("op.opsoft")
    print("created via typed wrapper:", type(op))
except Exception as e:
    print("typed wrapper failed:", e)
    op = comtypes.client.CreateObject("op.opsoft")
    print("fallback dynamic dispatch:", type(op))

print("Ver:", op.Ver())

try:
    # 强类型包装：out VARIANT* 参数由 comtypes 处理，返回 (ret, w, h) 或元组
    res = op.GetClientSize(target, 0, 0)
    print(f"[comtypes] GetClientSize -> {res}")
except Exception as e:
    print("GetClientSize FAILED:", e)

try:
    res2 = op.GetWindowRect(target, 0, 0, 0, 0)
    print(f"[comtypes] GetWindowRect -> {res2}")
except Exception as e:
    print("GetWindowRect FAILED:", e)

op.UnBindWindow()
print("DONE")
