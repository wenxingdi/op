# -*- coding: utf-8 -*-
"""通用真机绑定探针：启动任意 exe → 枚举窗口/已加载图形模块 → 按指定模式绑定 → 截图统计。

用途：快速判定"某个第三方程序能不能被 op 绑、走哪条图形通道"，无需为每个靶子写脚本。

用法：
    python workbench/_t_bind_any.py "<exe路径>" [--wait 6] [--modes "dx|dx|dx,dx|windows|windows"]
    python workbench/_t_bind_any.py "<exe路径>" --keep     # 不杀进程（留着手动看画面）
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
os.chdir(WORKDIR)
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

LOG = os.path.join(WORKDIR, "_t_bind_any_out.txt")
open(LOG, "w").close()


def log(s):
    print(s, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(s + "\n")


def argval(name, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        return sys.argv[i + 1] if i + 1 < len(sys.argv) else default
    return default


exe = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else None
if not exe or not os.path.exists(exe):
    log(f"[FAIL] exe 不存在: {exe}")
    sys.exit(1)
WAIT = float(argval("--wait", "6"))
KEEP = "--keep" in sys.argv
raw_modes = argval("--modes", "dx|windows|windows,dx.d3d11|windows|windows")
MODES = [tuple(m.split("|")) for m in raw_modes.split(",")]

try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpSetPath.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
dll.OpSetShowErrorMsg.argtypes = [ctypes.c_void_p, ctypes.c_int]
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
dll.OpGetWindowRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
dll.OpDestroy.argtypes = [ctypes.c_void_p]

user32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32

log(f"[launch] {exe}")
# cwd 用可写的 probes 目录：目标进程的 __op.log 落在自己 cwd，落在 SysWOW64 之类只读目录
# 会让注入侧诊断整段丢失（排查"绑不上"时最关键的信息就没了）。
EXE_ARGS = (argval("--exe-args", "") or "").split()
proc = subprocess.Popen([exe] + EXE_ARGS, cwd=os.path.join(ROOT, "workbench", "probes"))
tpid = proc.pid
time.sleep(WAIT)
log(f"[launch] pid={tpid} alive={proc.poll() is None}")

# ---- 窗口枚举（该 pid 的可见顶层窗口，按面积降序）----
CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
wins = []


def cb(h, lp):
    w = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(h, ctypes.byref(w))
    if w.value == tpid and user32.IsWindowVisible(h):
        r = wintypes.RECT()
        user32.GetWindowRect(h, ctypes.byref(r))
        b = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, b, 256)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, cls, 256)
        wins.append((h, b.value, cls.value, (r.right - r.left) * (r.bottom - r.top),
                     (r.left, r.top, r.right - r.left, r.bottom - r.top)))
    return True


user32.EnumWindows(CB(cb), 0)
wins.sort(key=lambda x: -x[3])
for h, t, c, a, r in wins[:6]:
    log(f"[win] hwnd={hex(h)} size={r[2]}x{r[3]}@{r[0]},{r[1]} class={c!r} title={t!r}")
if not wins:
    log("[FAIL] 无可绑定窗口")
    if not KEEP:
        proc.terminate()
    sys.exit(1)
hwnd = wins[0][0]

# ---- 模块枚举（绑定前后各来一次）----
# 按需加载会把 dinput8/d3d9 塞进**目标进程**，宿主侧 GetModuleHandle 看不到；
# 只有从外部枚举 Toolhelp 快照才作数 —— "绑定后多出来了"才是它真的加载了的硬证据。
class ME(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD), ("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


def list_interest(pid):
    snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, pid)  # SNAPMODULE | SNAPMODULE32
    mods = []
    me = ME()
    me.dwSize = ctypes.sizeof(me)
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            mods.append(me.szModule.decode("ascii", "replace"))
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    keys = ("d3d9", "d3d10", "d3d11", "d3d12", "dxgi", "dinput", "opengl", "op_c_api", "minhook")
    return len(mods), sorted({m for m in mods if any(k in m.lower() for k in keys)})


n0, i0 = list_interest(tpid)
log(f"[mods] 绑定前 共{n0}个; 相关: {i0}")


def caps(op, tag, tries=4, delay=1.0):
    """多帧重试：D3D 通道首次 Present 未必在绑定瞬间发生，单次失败不足以下结论。"""
    ret = ctypes.c_int(0)
    w, h = 200, 150
    for k in range(tries):
        ret.value = 0
        ptr = dll.OpGetScreenData(op, 0, 0, w, h, ctypes.byref(ret))
        if ptr and ret.value == 1:
            n = w * h * 4
            data = bytes((ctypes.c_char * n).from_address(ptr))
            px = {data[i:i + 4] for i in range(0, n, 4)}
            corner = " ".join(f"{data[i+2]:02X}{data[i+1]:02X}{data[i]:02X}"
                              for i in [(h // 4 * w + w // 4) * 4, (h // 4 * w + 3 * w // 4) * 4,
                                        (3 * h // 4 * w + w // 4) * 4, (3 * h // 4 * w + 3 * w // 4) * 4])
            log(f"  [{tag}] capture ret=1 (第{k + 1}次) uniq={len(px)} 采样点(RGB)={corner}")
            return True
        time.sleep(delay)
    log(f"  [{tag}] capture FAIL 连续{tries}次 ret=0 → 目标未产生可捕获的帧")
    return False


res = []
for spec in MODES:
    disp, m, k = spec
    hh = dll.OpCreate()
    dll.OpSetPath(hh, DLL_DIR)
    dll.OpSetShowErrorMsg(hh, 2)
    r = dll.OpBindWindow(hh, ctypes.c_void_p(hwnd), disp, m, k, 0)
    ib = dll.OpIsBind(hh)
    log(f"[{disp}/{m}/{k}] bind ret={r} is_bind={ib} alive={proc.poll() is None}")
    n1, i1 = list_interest(tpid)
    log(f"  [{disp}/{m}/{k}] 绑定后 共{n1}个; 相关: {i1}")
    if ib == 1:
        caps(hh, f"{disp}/{m}/{k}")
    ur = dll.OpUnBindWindow(hh)
    log(f"[{disp}/{m}/{k}] unbind={ur} alive_after={proc.poll() is None}")
    dll.OpDestroy(hh)
    res.append((disp, m, k, r, ib, proc.poll() is None))
    time.sleep(0.6)

log("=== 汇总 ===")
for disp, m, k, r, ib, alive in res:
    log(f"  {disp}/{m}/{k:8s} bind={r} is_bind={ib} alive={alive}")

if not KEEP:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
log("done")
