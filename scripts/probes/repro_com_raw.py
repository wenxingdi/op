# -*- coding: utf-8 -*-
"""绕开 comtypes 便捷层，用原始 __com_ 方法 + byref 直调 COM 的 GetClientSize/GetWindowRect。
验证 COM 实现层出参写入是否正确。"""
import ctypes
from ctypes import wintypes
import comtypes.client
from comtypes.automation import VARIANT, VT_I4, VT_EMPTY

u32 = ctypes.windll.user32
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

r = wintypes.RECT()
u32.GetClientRect(target, ctypes.byref(r))
print(f"[native] GetClientRect = {r.right}x{r.bottom}")
wr = wintypes.RECT()
u32.GetWindowRect(target, ctypes.byref(wr))
print(f"[native] GetWindowRect = ({wr.left},{wr.top})-({wr.right},{wr.bottom})")

mod = comtypes.client.GetModule(r"D:\AutoPro\op-master\op\bin\x64\op_x64.dll")
iface = mod.IOpAutomation
raw = iface._IOpAutomation__com_GetClientSize
raw_rect = iface._IOpAutomation__com_GetWindowRect

op = comtypes.client.CreateObject("op.opsoft", interface=iface)
print("Ver:", op.Ver())

# 原始调用：最后 nret 是 LP_c_long（typelib 标了 retval，comtypes 友好层吞掉了它）
this = op._IOpAutomation__com_GetClientSize.__self__ if hasattr(op._IOpAutomation__com_GetClientSize, "__self__") else op
nret = wintypes.LONG(-9)
w = VARIANT(); h = VARIANT()
hr = this._IOpAutomation__com_GetClientSize(target, ctypes.byref(w), ctypes.byref(h), ctypes.byref(nret))
print(f"[raw COM] GetClientSize HRESULT=0x{hr & 0xFFFFFFFF:08X} nret={nret.value} w.vt={w.vt} w.value={w.value} h.vt={h.vt} h.value={h.value}")

x1, y1, x2, y2 = VARIANT(), VARIANT(), VARIANT(), VARIANT()
nret2 = wintypes.LONG(-9)
hr2 = this._IOpAutomation__com_GetWindowRect(target, ctypes.byref(x1), ctypes.byref(y1), ctypes.byref(x2), ctypes.byref(y2), ctypes.byref(nret2))
print(f"[raw COM] GetWindowRect HRESULT=0x{hr2 & 0xFFFFFFFF:08X} nret={nret2.value} ({x1.value},{y1.value})-({x2.value},{y2.value})")

# 对照：空 VARIANT（VT_EMPTY）传入是否也正常
w2 = VARIANT(); h2 = VARIANT()
nret3 = wintypes.LONG(-9)
hr3 = this._IOpAutomation__com_GetClientSize(target, ctypes.byref(w2), ctypes.byref(h2), ctypes.byref(nret3))
print(f"[raw COM #2 VT_EMPTY in] nret={nret3.value} w={w2.value} h={h2.value}")

try:
    op.UnBindWindow()
except Exception:
    pass
print("DONE")
