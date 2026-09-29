# -*- coding: utf-8 -*-
"""内省 comtypes 生成的 IOpAutomation 签名，再用正确形态调 GetClientSize/GetWindowRect。"""
import ctypes
from ctypes import wintypes
import comtypes.client

u32 = ctypes.windll.user32
target = None
notepad = None

def _enum_cb(hwnd, lparam):
    global notepad
    if not u32.IsWindowVisible(hwnd):
        return True
    cbuf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, cbuf, 256)
    tbuf = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, tbuf, 256)
    if cbuf.value == "Notepad" and tbuf.value and notepad is None:
        notepad = hwnd
    return True

ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows(ENUMPROC(_enum_cb), 0)
target = notepad
print(f"target hwnd={target}")

mod = comtypes.client.GetModule(r"D:\AutoPro\op-master\op\bin\x64\op_x64.dll")
iface = mod.IOpAutomation
meth = iface.GetClientSize
print("GetClientSize comtypes info:")
print("  argtypes:", getattr(mth := meth, "argtypes", None))
print("  outparams:", getattr(mth, "outparams", None))
print("  idlflags:", getattr(mth, "idlflags", None))

op = comtypes.client.CreateObject("op.opsoft", interface=iface)
print("Ver:", op.Ver())

from comtypes.automation import VARIANT
w, h = VARIANT(), VARIANT()
try:
    res = op.GetClientSize(target, w, h)
    print(f"[typed] GetClientSize -> ret={res}")
except Exception as e:
    print("call attempt 1 failed:", e)
    # 打印签名帮助诊断
    import inspect
    for name in dir(iface):
        if "Client" in name or "Rect" in name:
            m = getattr(iface, name)
            print(name, "outparams=", getattr(m, "outparams", None), "argtypes=", getattr(m, "argtypes", None))
