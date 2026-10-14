# -*- coding: utf-8 -*-
"""绑定诊断域 **C API 层** 端到端探针（自建靶子窗口，独立可跑、无第三方依赖）。

对应能力：5.2.0「绑定失败可诊断」——失败原因码 + 实际生效图色链路透出。

覆盖导出：
    OpGetDisplayMethod / OpGetLastError / OpBindWindow / OpIsBind / OpUnBindWindow

断言（全部可反向证伪 —— 改坏实现必 FAIL）：
    A. 未绑定        → OpGetDisplayMethod 返回**空串**（返回固定串即 FAIL）
    B. 绑 normal     → ret=1, OpIsBind=1, 模式名 == "normal"（返回请求原串/空串即 FAIL）
    C. 解绑后        → 模式名回到空串（状态未清理即 FAIL）
    D. 绑 normal.auto→ ret=1，模式名**非空**（选择器没生效 / 没透出即 FAIL）
    E. 已销毁句柄    → ret=0 且 **紧接** 读取 OpGetLastError == 0x2001(INVALID_HWND)
                      （必须在失败后立刻读：中间任何 C API 调用都会覆盖 last-error）

用法：
    python scripts/probes/_t_bind_diagnose.py
退出码：0 全通过；1 有失败（可直接进回归核对）
"""
import ctypes
import os
import sys
import time
from ctypes import wintypes
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
WORKDIR = REPO / "workbench" / "probes"
WORKDIR.mkdir(parents=True, exist_ok=True)

# DPI 感知必须在任何窗口 API 之前声明（否则拿到虚拟化逻辑值，绑定矩形容器错）
try:
    _u = ctypes.windll.user32
    _u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    _u.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
    _u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
g32 = ctypes.windll.gdi32

FAILS = []


def check(ok, name, detail=""):
    print(("  [OK]   " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""), flush=True)
    if not ok:
        FAILS.append(name + (" | " + detail if detail else ""))


# ------------------------------------------------------------------ 自建靶子窗口
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wintypes.HWND, wintypes.UINT,
                             wintypes.WPARAM, wintypes.LPARAM)
_keepalive = []


def _wndproc(hwnd, msg, wp, lp):
    return u32.DefWindowProcW(hwnd, msg, wp, lp)


class WNDCLASSEX(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("style", wintypes.UINT),
                ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE),
                ("hIcon", wintypes.HICON), ("hCursor", wintypes.HANDLE),
                ("hbrBackground", wintypes.HBRUSH), ("lpszMenuName", wintypes.LPCWSTR),
                ("lpszClassName", wintypes.LPCWSTR), ("hIconSm", wintypes.HICON)]


def _setup_protos():
    u32.DefWindowProcW.restype = ctypes.c_longlong
    u32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    u32.RegisterClassExW.restype = wintypes.WORD
    u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
    u32.CreateWindowExW.restype = wintypes.HWND
    u32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                    wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                    wintypes.HINSTANCE, wintypes.LPVOID]
    u32.ShowWindow.restype = wintypes.BOOL
    u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    u32.DestroyWindow.restype = wintypes.BOOL
    u32.DestroyWindow.argtypes = [wintypes.HWND]
    u32.IsWindow.restype = wintypes.BOOL
    u32.IsWindow.argtypes = [wintypes.HWND]
    k32.GetModuleHandleW.restype = ctypes.c_void_p
    k32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    g32.CreateSolidBrush.restype = wintypes.HBRUSH
    g32.CreateSolidBrush.argtypes = [wintypes.COLORREF]


def make_window(title, x, y, w, h):
    proc = WNDPROC(_wndproc)
    _keepalive.append(proc)
    hinst = k32.GetModuleHandleW(None)
    cls = WNDCLASSEX()
    cls.cbSize = ctypes.sizeof(WNDCLASSEX)
    cls.lpfnWndProc = proc
    cls.hInstance = hinst
    cls.hbrBackground = g32.CreateSolidBrush(0x00FFFFFF)
    cls.lpszClassName = title
    u32.RegisterClassExW(ctypes.byref(cls))   # 重复注册(1410)忽略
    hwnd = u32.CreateWindowExW(0, title, title, 0x00CF0000, x, y, w, h, None, None, hinst, None)
    if hwnd:
        u32.ShowWindow(hwnd, 8)               # SW_SHOWNA：显示不抢焦点
        time.sleep(0.3)
    return hwnd


# ------------------------------------------------------------------ C API 装载
def load():
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(DLL_DIR))
    dll = ctypes.CDLL(str(DLL_DIR / "op_c_api_x64.dll"))
    dll.OpCreate.restype = ctypes.c_void_p
    dll.OpCreate.argtypes = []
    dll.OpDestroy.restype = None
    dll.OpDestroy.argtypes = [ctypes.c_void_p]
    dll.OpSetPath.restype = ctypes.c_int
    dll.OpSetPath.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    dll.OpSetShowErrorMsg.restype = ctypes.c_int
    dll.OpSetShowErrorMsg.argtypes = [ctypes.c_void_p, ctypes.c_int]
    dll.OpBindWindow.restype = ctypes.c_int
    dll.OpBindWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                                 ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
    dll.OpIsBind.restype = ctypes.c_int
    dll.OpIsBind.argtypes = [ctypes.c_void_p]
    dll.OpUnBindWindow.restype = ctypes.c_int
    dll.OpUnBindWindow.argtypes = [ctypes.c_void_p]
    # 共享缓冲：调用后立即转成 Python str（下次调用会被覆盖）
    dll.OpGetDisplayMethod.restype = ctypes.c_wchar_p
    dll.OpGetDisplayMethod.argtypes = [ctypes.c_void_p]
    dll.OpGetLastError.restype = ctypes.c_int
    dll.OpGetLastError.argtypes = [ctypes.c_void_p]
    return dll


def main():
    _setup_protos()
    os.chdir(WORKDIR)
    dll = load()

    A_BIN = 0x2001  # BIND_FAIL_INVALID_HWND

    hwnd = make_window("OP_BIND_DIAG_TGT", 120, 120, 480, 320)
    dead = make_window("OP_BIND_DIAG_DEAD", 700, 120, 300, 200)
    if not hwnd or not dead:
        print("[FAIL] 自建靶子窗口创建失败")
        return 1
    u32.DestroyWindow(dead)                    # 保留已失效句柄值 → 用作无效句柄分支
    time.sleep(0.2)
    print(f"[tgt] hwnd={hex(hwnd)}  dead_hwnd={hex(dead)} is_window(dead)={u32.IsWindow(dead)}")

    op = dll.OpCreate()
    dll.OpSetPath(op, str(DLL_DIR))
    dll.OpSetShowErrorMsg(op, 0)               # 0：不落 __op.log，避免污染 workbench

    print("\n-- A. 未绑定：模式名应为空串")
    m = dll.OpGetDisplayMethod(op)
    check(m == "", "未绑定返回空串", f"实得 {m!r}")

    print("\n-- B. BindWindow(normal)：成功且模式名==normal")
    r = dll.OpBindWindow(op, ctypes.c_void_p(hwnd), "normal", "windows", "windows", 0)
    ib = dll.OpIsBind(op)
    m = dll.OpGetDisplayMethod(op)
    check(r == 1, "bind normal ret=1", f"实得 {r}")
    check(ib == 1, "OpIsBind=1", f"实得 {ib}")
    check(m == "normal", "模式名==normal", f"实得 {m!r}")

    print("\n-- C. 解绑后：模式名应回到空串")
    dll.OpUnBindWindow(op)
    m = dll.OpGetDisplayMethod(op)
    check(m == "", "解绑后回到空串", f"实得 {m!r}")

    print("\n-- D. BindWindow(normal.auto)：选择器生效且透出实际后端名")
    r = dll.OpBindWindow(op, ctypes.c_void_p(hwnd), "normal.auto", "windows", "windows", 0)
    m = dll.OpGetDisplayMethod(op)
    check(r == 1, "bind normal.auto ret=1", f"实得 {r}")
    check(bool(m), "auto 模式名非空（选择器已透出）", f"实得 {m!r}")
    if m and m != "normal.auto":
        print(f"  [INFO] 请求 normal.auto → 实际生效 {m}（GetDisplayMethod 返回的是生效值而非请求值）")
    dll.OpUnBindWindow(op)

    print("\n-- E. 已销毁句柄：ret=0 且紧接读取原因码==0x2001")
    r = dll.OpBindWindow(op, ctypes.c_void_p(dead), "normal", "windows", "windows", 0)
    err = dll.OpGetLastError(op)               # 必须紧接，中间不可插任何 C API 调用
    check(r == 0, "无效句柄 bind ret=0", f"实得 {r}")
    check(err == A_BIN, f"原因码==0x{A_BIN:X}(INVALID_HWND)", f"实得 0x{err & 0xFFFFFFFF:X}")

    dll.OpDestroy(op)
    u32.DestroyWindow(hwnd)

    print("\n=== 汇总 ===")
    if FAILS:
        print(f"FAIL {len(FAILS)} 项：")
        for f in FAILS:
            print("   -", f)
        return 1
    print("PASS 全通过（0 FAIL）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
