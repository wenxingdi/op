# -*- coding: utf-8 -*-
"""跨进程 DPI 口径探针（批 62 校验用）。

场景：宿主进程 DPI aware（PMv2），目标窗口在**另一个 DPI 不感知进程**里创建。
要确认三件事：
  1. aware 宿主跨进程 GetClientRect 拿到的是物理尺寸（= 逻辑 × 缩放）
  2. aware 宿主跨进程 ClientToScreen(0,0) 拿到物理屏幕原点
  3. 宿主线程 SetThreadDpiAwarenessContext(UNAWARE) 后，跨进程 GetClientRect
     拿到的是逻辑尺寸 —— 这是 op 的 window_pixel_scale 的基础
"""
import ctypes
import ctypes.wintypes as wt
import multiprocessing
import os
import sys
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

u32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
u32.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
u32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
u32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
u32.GetThreadDpiAwarenessContext.restype = ctypes.c_void_p
u32.GetWindowDpiAwarenessContext.argtypes = [wt.HWND]
u32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
u32.AreDpiAwarenessContextsEqual.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
u32.AreDpiAwarenessContextsEqual.restype = ctypes.c_bool
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetClientRect.restype = ctypes.c_bool
u32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
u32.ClientToScreen.restype = ctypes.c_bool
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowRect.restype = ctypes.c_bool
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = wt.UINT
u32.GetDpiForSystem.restype = wt.UINT
u32.CreateWindowExW.restype = wt.HWND
u32.CreateWindowExW.argtypes = [
    wt.DWORD, ctypes.c_wchar_p, ctypes.c_wchar_p, wt.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p,
]
u32.DefWindowProcW.restype = ctypes.c_longlong
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.RegisterClassW.argtypes = [ctypes.c_void_p]
u32.RegisterClassW.restype = wt.ATOM

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


def _wndproc(hwnd, msg, wp, lp):
    if msg == 0x0002:  # WM_DESTROY
        u32.PostQuitMessage(0)
        return 0
    return u32.DefWindowProcW(hwnd, msg, wp, lp)


def target_proc(path):
    """DPI 不感知子进程：创建一个 200x150 客户区的窗口，把 hwnd 写文件。"""
    # 直接用系统预注册的 STATIC 类，省掉自建 WNDCLASS（ctypes 没有 WNDCLASSW）
    hwnd = u32.CreateWindowExW(
        0, "STATIC", "probe", 0x00CF0000,  # WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME
        260, 200, 200 + 16, 150 + 39, None, None, k32.GetModuleHandleW(None), None)
    if not hwnd:
        with open(path, "w") as f:
            f.write("error=1\n")
        return
    u32.ShowWindow(hwnd, 4)  # SW_SHOWNOACTIVATE
    u32.UpdateWindow(hwnd)
    with open(path, "w") as f:
        f.write("hwnd=%d\npid=%d\n" % (hwnd, os.getpid()))
    msg = wt.MSG()
    while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        u32.TranslateMessage(ctypes.byref(msg))
        u32.DispatchMessageW(ctypes.byref(msg))
    _wndproc(None, 0x0002, 0, 0)  # 收尾，避免句柄泄漏


def main():
    tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_probe_dpi_xproc.txt")
    if os.path.exists(tmp):
        os.remove(tmp)
    p = multiprocessing.Process(target=target_proc, args=(tmp,))
    p.daemon = True
    p.start()

    for _ in range(100):
        if os.path.exists(tmp):
            break
        time.sleep(0.05)
    if not os.path.exists(tmp):
        print("FAIL: 子进程没写出 hwnd")
        return
    info = dict(line.split("=", 1) for line in open(tmp).read().splitlines() if "=" in line)
    if "error" in info:
        print("FAIL: 子进程建窗失败")
        return
    hwnd = int(info["hwnd"])
    print("target hwnd=%s pid=%s" % (hwnd, info["pid"]))

    # 宿主切到 PerMonitorV2
    ok = u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    print("host SetProcessDpiAwarenessContext(PMv2) ->", bool(ok))

    rc = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(rc))
    host_client = (rc.right - rc.left, rc.bottom - rc.top)
    wr = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(wr))
    pt = wt.POINT(0, 0)
    u32.ClientToScreen(hwnd, ctypes.byref(pt))
    print("[aware 宿主] GetClientRect =", host_client, " GetWindowRect =", (wr.left, wr.top, wr.right, wr.bottom))
    print("[aware 宿主] ClientToScreen(0,0) =", (pt.x, pt.y), " GetDpiForWindow =", u32.GetDpiForWindow(hwnd))

    # 切线程到 UNAWARE，模拟 op 的 dpi_context_scope
    prev = u32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-1))
    print("SetThreadDpiAwarenessContext(UNAWARE) ->", "NULL(失败)" if not prev else "ok")
    rc2 = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(rc2))
    own_client = (rc2.right - rc2.left, rc2.bottom - rc2.top)
    print("[线程 UNAWARE] GetClientRect =", own_client)
    if prev:
        u32.SetThreadDpiAwarenessContext(prev)

    print("=> 宿主/窗口像素比 =", (host_client[0] / own_client[0]) if own_client[0] else "n/a")
    print("system dpi =", u32.GetDpiForSystem())


if __name__ == "__main__":
    main()
