# -*- coding: utf-8 -*-
"""复现验证：OpGetClientSize / OpGetWindowRect 是否返回全 0。
只走 C-API（ctypes 直接 LoadLibrary，不经过 COM/PowerShell 封送），
不调用 OCR，避开 P1 退出挂起。"""
import ctypes
from ctypes import wintypes
import os, sys

BUILD = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
os.environ["PATH"] = BUILD + ";" + os.environ["PATH"]
os.add_dll_directory(BUILD)

dll = ctypes.CDLL(os.path.join(BUILD, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpVer.restype = ctypes.c_wchar_p
dll.OpVer.argtypes = []
dll.OpGetClientSize.restype = ctypes.c_int
dll.OpGetClientSize.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
dll.OpGetWindowRect.restype = ctypes.c_int
dll.OpGetWindowRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
dll.OpBindWindow.restype = ctypes.c_int
dll.OpBindWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                             ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
dll.OpUnBindWindow.restype = ctypes.c_int
dll.OpUnBindWindow.argtypes = [ctypes.c_void_p]
dll.OpGetBindWindow.restype = ctypes.c_void_p
dll.OpGetBindWindow.argtypes = [ctypes.c_void_p]

h = dll.OpCreate()
print("OpCreate ok, Ver =", dll.OpVer())

# ---- 用 user32 找一个真实可见窗口（按类名 Notepad，找不到退回工作区任意可见顶层窗口）----
u32 = ctypes.windll.user32
target = None

def _enum_cb(hwnd, lparam):
    global target
    if target:
        return True
    if not u32.IsWindowVisible(hwnd):
        return True
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, buf, 256)
    cls = buf.value
    u32.GetWindowTextW(hwnd, buf, 256)
    title = buf.value
    if cls == "Notepad" and title:
        target = (hwnd, cls, title)
    return True

ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows(ENUMPROC(_enum_cb), 0)

if target is None:
    print("未找到 Notepad 窗口，改用任务栏任意可见窗口（找 Notepad 失败）")
    # 兜底：找前台窗口
    hwnd = u32.GetForegroundWindow()
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, buf, 256)
    target = (hwnd, buf.value, "")
hwnd, cls, title = target
print(f"target hwnd={hwnd} (0x{hwnd:X}) class='{cls}' title='{title}'")

# ---- Win32 原生 ground truth ----
rect = wintypes.RECT()
u32.GetClientRect(hwnd, ctypes.byref(rect))
print(f"[native] GetClientRect = {rect.right}x{rect.bottom}")
wr = wintypes.RECT()
u32.GetWindowRect(hwnd, ctypes.byref(wr))
print(f"[native] GetWindowRect = ({wr.left},{wr.top})-({wr.right},{wr.bottom})")

hx = ctypes.c_void_p(hwnd)
w, ht = ctypes.c_int(-777), ctypes.c_int(-777)
x1, y1, x2, y2 = (ctypes.c_int(v) for v in (-1, -2, -3, -4))

# ---- 未绑定时 ----
r1 = dll.OpGetClientSize(h, hx, ctypes.byref(w), ctypes.byref(ht))
print(f"[op unbind] OpGetClientSize ret={r1} -> {w.value}x{ht.value}")
a1, b1, c1, d1 = (ctypes.c_int(v) for v in (-1, -2, -3, -4))
r2 = dll.OpGetWindowRect(h, hx, ctypes.byref(a1), ctypes.byref(b1), ctypes.byref(c1), ctypes.byref(d1))
print(f"[op unbind] OpGetWindowRect ret={r2} -> ({a1.value},{b1.value})-({c1.value},{d1.value})")

# ---- 绑定 normal 后 ----
rb = dll.OpBindWindow(h, hx, "normal", "normal", "normal", 0)
print(f"[op] BindWindow(normal) ret={rb}  GetBindWindow={dll.OpGetBindWindow(h)}")
w, ht = ctypes.c_int(-777), ctypes.c_int(-777)
r3 = dll.OpGetClientSize(h, hx, ctypes.byref(w), ctypes.byref(ht))
print(f"[op bind] OpGetClientSize ret={r3} -> {w.value}x{ht.value}")
a2, b2, c2, d2 = (ctypes.c_int(v) for v in (-1, -2, -3, -4))
r4 = dll.OpGetWindowRect(h, hx, ctypes.byref(a2), ctypes.byref(b2), ctypes.byref(c2), ctypes.byref(d2))
print(f"[op bind] OpGetWindowRect ret={r4} -> ({a2.value},{b2.value})-({c2.value},{d2.value})")

# ---- 换一个已知假句柄做对照 ----
fake = ctypes.c_void_p(0x590F)
w2, h2 = ctypes.c_int(-555), ctypes.c_int(-555)
r5 = dll.OpGetClientSize(h, fake, ctypes.byref(w2), ctypes.byref(h2))
print(f"[op fake hwnd] OpGetClientSize ret={r5} -> {w2.value}x{h2.value}")

dll.OpUnBindWindow(h)
print("DONE (未调 OCR，无 P1 风险)")
