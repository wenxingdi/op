# -*- coding: utf-8 -*-
"""蜀门(client.exe, 32位) 绑定域真机回归 — 当前 DLL(bd2817d)
矩阵: 非注入模式(gdi/gdi2/dx2/normal) + dx hook 注入(=自建 op_c_api_x86.dll 真机验证)
每组合: bind -> is_bind -> get_bind_window -> capture 唯一色统计 -> unbind -> is_bind
"""
import ctypes, os, sys, time
from ctypes import wintypes

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
LOG = r"D:\AutoPro\op-master\op\workbench\_t_bind_shumen_out.txt"
TARGET_PID = 0          # 0 = 自动探测 client.exe（游戏重启后 pid 会变）
TARGET_TITLE = "我的客户端"


def _find_pid(name=b"client.exe"):
    """按进程名自动探测 pid。"""
    k32 = ctypes.windll.kernel32

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_char * 260)]

    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    if snap == -1:
        return []
    pe = PE()
    pe.dwSize = ctypes.sizeof(pe)
    out = []
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == name:
            out.append(pe.th32ProcessID)
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return out


if TARGET_PID == 0:
    _c = _find_pid()
    TARGET_PID = _c[0] if _c else 0

class _Logger:
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8", buffering=1)
    def __call__(self, msg):
        print(msg)
        self.f.write(msg + "\n")

log = _Logger(LOG)

# DPI 感知必须先行（150% 缩放）
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
dll.OpSetShowErrorMsg.restype = ctypes.c_int
dll.OpFindWindow.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p]
dll.OpFindWindow.restype = ctypes.c_void_p
dll.OpBindWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                             ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
dll.OpBindWindow.restype = ctypes.c_int
dll.OpUnBindWindow.argtypes = [ctypes.c_void_p]
dll.OpUnBindWindow.restype = ctypes.c_int
dll.OpIsBind.argtypes = [ctypes.c_void_p]
dll.OpIsBind.restype = ctypes.c_int
dll.OpGetBindWindow.argtypes = [ctypes.c_void_p]
dll.OpGetBindWindow.restype = ctypes.c_void_p
dll.OpGetScreenData.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
dll.OpGetScreenData.restype = ctypes.c_void_p
dll.OpCapture.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_wchar_p]
dll.OpCapture.restype = ctypes.c_int

# ---- 找窗口 ----
user32 = ctypes.windll.user32
found = []
CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def cb(hwnd, lp):
    w = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(w))
    if w.value == TARGET_PID:
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        if user32.IsWindowVisible(hwnd) and buf.value:
            found.append((hwnd, buf.value))
    return True
user32.EnumWindows(CB(cb), 0)
log(f"[find] visible windows of pid {TARGET_PID}: {[(hex(h), t) for h, t in found]}")
game_hwnd = None
for h, t in found:
    if TARGET_TITLE in t:
        game_hwnd = h
        break
if game_hwnd is None and found:
    game_hwnd = found[0][0]
if game_hwnd is None:
    log("[FAIL] target window not found"); sys.exit(2)
log(f"[find] use hwnd={hex(game_hwnd)}")

# ---- 唯一色统计（gdi 系判真实画面: 唯一色 >1万; normal 桌面覆盖层 ~千级也算通）----
def capture_stats(op, tag):
    iconic = user32.IsIconic(game_hwnd)
    rc = wintypes.RECT()
    user32.GetWindowRect(game_hwnd, ctypes.byref(rc))
    log(f"  [winstate] iconic={iconic} rect=({rc.left},{rc.top},{rc.right},{rc.bottom})")
    size = ctypes.c_int(0)
    ptr = dll.OpGetScreenData(op, 0, 0, 200, 150, ctypes.byref(size))
    if not ptr or size.value < 1000:
        log(f"  [{tag}] capture FAIL ptr={ptr} size={size.value}")
        return None
    buf = (ctypes.c_char * size.value).from_address(ptr)
    data = bytes(buf)
    # BGRA 32bpp 自上而下，跳 54 字节头（BITMAPINFOHEADER+尺寸行,与 T3 同口径粗排）
    colors = set()
    n = len(data)
    off = 54 if n % 4 else 0
    body = data[off:] if off else data
    for i in range(0, len(body) - 3, 4):
        colors.add(body[i:i+4])
    uniq = len(colors)
    # 亮度粗均值
    if colors:
        sample = list(colors)[:20000]
        lum = sum(0.299*c[2] + 0.587*c[1] + 0.114*c[0] for c in sample) / len(sample)
    else:
        lum = 0
    log(f"  [{tag}] capture OK size={size.value} uniq_colors={uniq} lum~{lum:.0f}")
    return uniq

# ---- 单组合测试 ----
def test_combo(display, mouse, keyboard):
    tag = f"{display}/{mouse}/{keyboard}"
    log(f"[{tag}] ---")
    h = dll.OpCreate()
    dll.OpSetPath(h, DLL_DIR)
    dll.OpSetShowErrorMsg(h, 2)
    try:
        r = dll.OpBindWindow(h, ctypes.c_void_p(game_hwnd), display, mouse, keyboard, 0)
        ib = dll.OpIsBind(h)
        gw = dll.OpGetBindWindow(h)
        log(f"  bind ret={r} is_bind={ib} get_bind_window={'OK' if gw == game_hwnd else hex(gw or 0)}")
        if r != 1:
            log(f"  [{tag}] RESULT: BIND-FAIL")
            return False
        uniq = capture_stats(h, tag)
        # 重复绑定切换（绑 gdi 状态下直接再绑 dx 是同模式内部行为，这里只测同参重绑）
        r2 = dll.OpBindWindow(h, ctypes.c_void_p(game_hwnd), display, mouse, keyboard, 0)
        ib2 = dll.OpIsBind(h)
        log(f"  rebind ret={r2} is_bind={ib2}")
        ur = dll.OpUnBindWindow(h)
        ib3 = dll.OpIsBind(h)
        log(f"  unbind ret={ur} is_bind_after={ib3}")
        ok = (ib == 1 and gw == game_hwnd and ur == 1 and ib3 == 0)
        log(f"  [{tag}] RESULT: {'PASS' if ok else 'FAIL'} (capture_uniq={uniq})")
        return ok
    finally:
        if dll.OpIsBind(h):
            dll.OpUnBindWindow(h)
        dll.OpDestroy(h)

results = {}
# 保底对照 + dx 注入重点（自建 op_c_api_x86.dll 已拷入 x64 运行时目录）
for d, m, k in [("gdi", "windows", "windows"),
                ("dx", "windows", "windows"), ("dx", "dx", "dx")]:
    results[f"{d}/{m}/{k}"] = test_combo(d, m, k)

log("\n==== SUMMARY ====")
for k, v in results.items():
    log(f"  {k:28} {'PASS' if v else 'FAIL'}")
log(f"TOTAL: {sum(results.values())}/{len(results)} PASS")
log("game alive check + exit")
