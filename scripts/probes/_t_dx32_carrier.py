# -*- coding: utf-8 -*-
"""32 位 D3D9 受控靶子探针：起 dx_carrier_x86.exe，用 64 位宿主绑定并校验四象限像素。

为什么需要它：hook 注入的 dll 位数必须匹配**目标进程**位数。此前 32 位 dx 通道只能拿
蜀门 client.exe 当靶子（状态不可控、且 dinput8 未加载时 input 侧必然失败）。本探针让
32 位 dx 注入有尺寸/颜色完全可控的靶子。

用法：
    python workbench/_t_dx32_carrier.py                 # x86 载体 + d3d9
    python workbench/_t_dx32_carrier.py --arch x64      # 对照：x64 载体
    python workbench/_t_dx32_carrier.py --backend d3d9 --w 400 --h 300
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

ROOT = r"D:\AutoPro\op-master\op"
DLL_DIR = os.path.join(ROOT, "build", "nmake-x64-Release", "libop")  # 宿主是 64 位
WORKDIR = os.path.join(ROOT, "workbench")
PROBES = os.path.join(WORKDIR, "probes")
os.chdir(WORKDIR)
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

# 载体窗口四象限期望色（RGB hex）：左上红 / 右上绿 / 左下蓝 / 右下白
QUADRANTS = [("TL", 0.25, 0.25, "FF0000"), ("TR", 0.75, 0.25, "00FF00"),
             ("BL", 0.25, 0.75, "0000FF"), ("BR", 0.75, 0.75, "FFFFFF")]

LOG = os.path.join(WORKDIR, "_t_dx32_carrier_out.txt")
open(LOG, "w").close()


def log(s):
    print(s, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(s + "\n")


def argval(name, default):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


ARCH = argval("--arch", "x86")
BACKEND = argval("--backend", "d3d9")
W = int(argval("--w", "400"))
H = int(argval("--h", "300"))
# 模式可覆盖：--modes "dx|dx|dx,dx.d3d11|windows|windows" 格式同 _t_bind_any.py
_default = "dx|dx|dx,dx|windows|windows"
_raw = argval("--modes", None)
if _raw is None:
    _raw = _default
MODES = []
for _s in _raw.split(","):
    _p = _s.split("|")
    MODES.append(("/".join(_p), _p[0], _p[1], _p[2]))

# 宿主进程显式 DPI 感知（与目标无关，但避免宿主侧拿到虚拟化坐标）
try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception as e:
    log(f"[dpi] SetProcessDpiAwarenessContext failed: {e}")

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]
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

exe = os.path.join(PROBES, "dx_carrier_x86.exe" if ARCH == "x86" else "dx_carrier.exe")
report = os.path.join(WORKDIR, "_t_dx32_carrier_report.txt")
if os.path.exists(report):
    os.remove(report)

log(f"[carrier] arch={ARCH} backend={BACKEND} exe={exe}")
cmd = [exe, "--backend", BACKEND, "--seconds", "90",
       "--w", str(W), "--h", str(H), "--report", report]
if "--dinput" in sys.argv:
    cmd.append("--dinput")
proc = subprocess.Popen(cmd, cwd=PROBES, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

info = {}
for _ in range(100):
    if os.path.exists(report):
        txt = open(report, encoding="utf-8", errors="replace").read()
        if "READY" in txt:
            for line in txt.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    info[k.strip()] = v.strip()
            break
    if proc.poll() is not None:
        break
    time.sleep(0.1)

if "hwnd" not in info:
    log(f"[FAIL] carrier 未就绪 rc={proc.poll()}")
    out = proc.stdout.read(2000).decode("utf-8", "replace") if proc.stdout else ""
    log(out)
    proc.terminate()
    sys.exit(1)

hwnd = int(info["hwnd"], 16)
tpid = int(info["pid"])
w, h = (int(v) for v in info["size"].split("x"))
log(f"[carrier] hwnd={hex(hwnd)} pid={tpid} size={w}x{h} dinput={info.get('dinput', 'n/a')}")


def sample(data, x, y):
    i = (y * w + x) * 4
    b, g, r, _a = data[i:i + 4]
    return f"{r:02X}{g:02X}{b:02X}"


def caps(op, tag):
    # OpGetScreenData 第 6 个出参是 ret（1=成功），不是 size；缓冲按请求宽高解读（BGRA）
    ret = ctypes.c_int(0)
    ptr = dll.OpGetScreenData(op, 0, 0, w, h, ctypes.byref(ret))
    if not ptr or ret.value != 1:
        log(f"  [{tag}] capture FAIL ptr={ptr} ret={ret.value}")
        return None
    n = w * h * 4
    data = bytes((ctypes.c_char * n).from_address(ptr))
    uniq = len({data[i:i + 4] for i in range(0, n, 4)})
    got = []
    ok = True
    for name, fx, fy, exp in QUADRANTS:
        got_c = sample(data, int(w * fx), int(h * fy))
        ok = ok and got_c == exp
        got.append(f"{name}={got_c}{'' if got_c == exp else '(exp %s)' % exp}")
    log(f"  [{tag}] capture ret=1 uniq={uniq} quad_ok={ok} {' '.join(got)}")
    return ok


def once(disp, m, k, tag):
    hh = dll.OpCreate()
    dll.OpSetPath(hh, DLL_DIR)
    dll.OpSetShowErrorMsg(hh, 2)
    r = dll.OpBindWindow(hh, ctypes.c_void_p(hwnd), disp, m, k, 0)
    ib = dll.OpIsBind(hh)
    gw = dll.OpGetBindWindow(hh)
    log(f"[{tag}] bind ret={r} is_bind={ib} gw={hex(gw or 0)} alive={proc.poll() is None}")
    ok = caps(hh, tag) if ib == 1 else None
    ur = dll.OpUnBindWindow(hh)
    log(f"[{tag}] unbind={ur} alive_after={proc.poll() is None}")
    dll.OpDestroy(hh)
    return r, ok


results = []
for tag, disp, m, k in MODES:
    r, ok = once(disp, m, k, tag)
    results.append((tag, r, ok, proc.poll() is None))
    time.sleep(0.5)

log("=== 汇总 ===")
for tag, r, ok, alive in results:
    log(f"  {tag:14s} bind={r} pixels={'OK' if ok else ('N/A' if ok is None else 'MISMATCH')} target_alive={alive}")

proc.terminate()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
log("done")
