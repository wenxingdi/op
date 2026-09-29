# -*- coding: utf-8 -*-
"""判别实验: 32 位记事本 + dx 绑定 → 必然 locate D3D9 failed → 观察目标进程是否崩溃
崩溃 = 注入/远程调用路径问题(与 D3D9 无关) | 不崩 = 蜀门环境特有
"""
import ctypes, os, sys, time, subprocess
from ctypes import wintypes

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
LOG = r"D:\AutoPro\op-master\op\workbench\_t_dx32_notepad_out.txt"

class _L:
    def __init__(s, p): s.f = open(p, "w", encoding="utf-8", buffering=1)
    def __call__(s, m): print(m); s.f.write(m + "\n")
log = _L(LOG)

try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()

os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)
dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]
dll.OpSetPath.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
dll.OpSetPath.restype = ctypes.c_int
dll.OpSetShowErrorMsg.argtypes = [ctypes.c_void_p, ctypes.c_int]
dll.OpBindWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                             ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
dll.OpBindWindow.restype = ctypes.c_int
dll.OpUnBindWindow.argtypes = [ctypes.c_void_p]
dll.OpUnBindWindow.restype = ctypes.c_int
dll.OpIsBind.argtypes = [ctypes.c_void_p]
dll.OpIsBind.restype = ctypes.c_int

# 启动 32 位记事本
NOTEPAD32 = r"C:\Windows\SysWOW64\notepad.exe"
proc = subprocess.Popen([NOTEPAD32])
time.sleep(2.5)
pid = proc.pid
log(f"[start] notepad32 pid={pid} alive={proc.poll() is None}")

user32 = ctypes.windll.user32
hwnd = 0
CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def cb(h, lp):
    global hwnd
    w = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(h, ctypes.byref(w))
    if w.value == pid and user32.IsWindowVisible(h):
        hwnd = h
        return False
    return True
user32.EnumWindows(CB(cb), 0)
log(f"[find] hwnd={hex(hwnd)}")
if not hwnd:
    log("[FAIL] no window"); sys.exit(2)

def alive():
    hf = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
    if hf:
        ctypes.windll.kernel32.CloseHandle(hf); return True
    return False

h = dll.OpCreate()
dll.OpSetPath(h, DLL_DIR)
dll.OpSetShowErrorMsg(h, 2)
SKIP_UNBIND = "--skip-unbind" in sys.argv
r = dll.OpBindWindow(h, ctypes.c_void_p(hwnd), "dx", "windows", "windows", 0)
log(f"[bind dx] ret={r} (期望 0: 记事本无 D3D9, locate 必失败)")
time.sleep(2)
log(f"[after 2s] target exit_code={proc.poll()}")
if SKIP_UNBIND:
    log("[skip-unbind] 直接 OpDestroy, 不调 UnBindWindow")
    dll.OpDestroy(h)
    time.sleep(2)
    log(f"=== VERDICT: exit={proc.poll()} → {'崩在bind返回路径' if proc.poll() == 3221225477 else '崩在unbind/卸载路径' if proc.poll() is None else 'exit=' + str(proc.poll())}")
    sys.exit(0)
ur = dll.OpUnBindWindow(h)
log(f"[unbind] ret={ur}")
time.sleep(2)
log(f"[after unbind+2s] target exit_code={proc.poll()}")
dll.OpDestroy(h)
final = proc.poll()
log(f"=== VERDICT: exit={final} → {'CRASH(0xC0000005)' if final == 3221225477 else 'NO-CRASH'}")
