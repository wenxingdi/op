# -*- coding: utf-8 -*-
"""屏保当 D3D9 靶子的可行性探针：把系统屏保以 /p <hwnd> 预览模式渲染进载体窗口，
再用 op 绑该子窗口取帧。

为什么试这条路：Bubbles.scr 等自带屏保直接导入 d3d9.dll，是"非自建的第三方 D3D9 程序"。
若不成立，则 32/64 位 D3D9 靶子统一走 dx_carrier（已有 x86 版）即可。

用法：python workbench/_t_scr_probe.py [Bubbles.scr]
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

ROOT = r"D:\AutoPro\op-master\op"
DLL_DIR = os.path.join(ROOT, "build", "nmake-x64-Release", "libop")
WORKDIR = os.path.join(ROOT, "workbench")
PROBES = os.path.join(WORKDIR, "probes")
os.chdir(WORKDIR)
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

SCR = sys.argv[1] if len(sys.argv) > 1 else r"C:\Windows\System32\Bubbles.scr"
LOG = os.path.join(WORKDIR, "_t_scr_probe_out.txt")
open(LOG, "w").close()


def log(s):
    print(s, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(s + "\n")


try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

user32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
# 64 位下不显式声明 argtypes/restype 会把 HWND 当 c_int 截断成 32 位（经典 ctypes 坑）
user32.GetParent.argtypes = [wintypes.HWND]
user32.GetParent.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
# EnumWindows/EnumChildWindows 不设 argtypes（否则回调实例传不进去），改为把 hwnd 包成 c_void_p
dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpSetPath.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
dll.OpSetShowErrorMsg.argtypes = [ctypes.c_void_p, ctypes.c_int]
dll.OpBindWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                             ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
dll.OpBindWindow.restype = ctypes.c_int
dll.OpIsBind.argtypes = [ctypes.c_void_p]
dll.OpIsBind.restype = ctypes.c_int
dll.OpUnBindWindow.argtypes = [ctypes.c_void_p]
dll.OpGetScreenData.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
dll.OpGetScreenData.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]

# 1) 起载体当父窗口
report = os.path.join(WORKDIR, "_t_scr_probe_report.txt")
if os.path.exists(report):
    os.remove(report)
carrier = subprocess.Popen([os.path.join(PROBES, "dx_carrier.exe"), "--backend", "d3d9",
                            "--seconds", "45", "--w", "500", "--h", "400", "--report", report],
                           cwd=PROBES, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
info = {}
for _ in range(80):
    if os.path.exists(report):
        txt = open(report, encoding="utf-8", errors="replace").read()
        if "READY" in txt:
            for line in txt.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    info[k.strip()] = v.strip()
            break
    time.sleep(0.1)
if "hwnd" not in info:
    log("[FAIL] 载体未就绪")
    carrier.terminate()
    sys.exit(1)
parent = int(info["hwnd"], 16)
log(f"[carrier] hwnd={hex(parent)} pid={carrier.pid} size={info.get('size')}")

# 2) 屏保以预览模式渲染到该窗口
log(f"[scr] launch {SCR} /p {parent}")
try:
    scr = subprocess.Popen([SCR, "/p", str(parent)], cwd=os.path.dirname(SCR))
except Exception as e:
    log(f"[FAIL] 启动屏保异常: {e}")
    carrier.terminate()
    sys.exit(1)
time.sleep(4)
log(f"[scr] pid={scr.pid} alive={scr.poll() is None}")

# 3) 枚举屏保进程的窗口 + 父窗口为载体的子窗口
CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
found = []


def cb(h, lp):
    w = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(h, ctypes.byref(w))
    par = user32.GetParent(h)
    b = ctypes.create_unicode_buffer(128)
    user32.GetWindowTextW(h, b, 128)
    cls = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(h, cls, 128)
    r = wintypes.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    if w.value == scr.pid or par == parent:
        found.append((h, w.value, par, b.value, cls.value, (r.right - r.left, r.bottom - r.top)))
        log(f"[win] hwnd={hex(h)} pid={w.value} parent={hex(par)} size={r.right - r.left}x{r.bottom - r.top} "
            f"class={cls.value!r} title={b.value!r}")
    return True


user32.EnumWindows(CB(cb), 0)
user32.EnumChildWindows(ctypes.c_void_p(parent), CB(cb), 0)
if not found:
    log("[结论] 屏保未创建任何窗口 → 该屏保的 /p 预览模式对本机不适用")

# 4) 屏保进程模块：是否真的加载 d3d9
class ME(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD), ("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, scr.pid)
mods = []
me = ME()
me.dwSize = ctypes.sizeof(me)
if k32.Module32First(snap, ctypes.byref(me)):
    while True:
        mods.append(me.szModule.decode("ascii", "replace"))
        if not k32.Module32Next(snap, ctypes.byref(me)):
            break
k32.CloseHandle(snap)
graphics = sorted({m for m in mods if any(k in m.lower() for k in ("d3d", "dxgi", "opengl"))})
log(f"[scr mods] 共{len(mods)}个; 图形相关={graphics}")

# 5) op 绑定屏保窗口取帧
targets = [f for f in found if f[0] != parent and f[5][0] > 50 and f[5][1] > 50]
if not targets:
    log("[结论] 无可用屏保渲染窗口，跳过绑定")
else:
    h = targets[0][0]
    hh = dll.OpCreate()
    dll.OpSetPath(hh, DLL_DIR)
    dll.OpSetShowErrorMsg(hh, 2)
    r = dll.OpBindWindow(hh, ctypes.c_void_p(h), "dx", "windows", "windows", 0)
    log(f"[bind] hwnd={hex(h)} display=dx ret={r} is_bind={dll.OpIsBind(hh)}")
    if dll.OpIsBind(hh) == 1:
        ret = ctypes.c_int(0)
        ptr = dll.OpGetScreenData(hh, 0, 0, 200, 150, ctypes.byref(ret))
        if ptr and ret.value == 1:
            n = 200 * 150 * 4
            data = bytes((ctypes.c_char * n).from_address(ptr))
            px = {data[i:i + 4] for i in range(0, n, 4)}
            log(f"[capture] ret=1 uniq={len(px)}  ← >1 说明真取到屏保画面")
        else:
            log(f"[capture] FAIL ret={ret.value}（屏保未产生 Present 帧）")
    dll.OpUnBindWindow(hh)
    dll.OpDestroy(hh)

scr.terminate()
carrier.terminate()
for p in (scr, carrier):
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
log("done")
