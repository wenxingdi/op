# -*- coding: utf-8 -*-
"""窗口/进程域 + 杂项域 **C API 层** 验证（覆盖缺口里的 ~70 个 API）。

安全约定（真机在跑，勿污染用户环境）：
  · 所有**写操作**只作用于自建靶子窗口（move/size/text/transparent/state/send_string…）；
  · 剪贴板：先存原值，测完**恢复**；
  · 进程启动：用 `cmd /c exit` + SW_HIDE，不弹窗、不留残留；
  · 声音：`beep` 用 1ms（几乎听不见）；
  · 不做任何会动到游戏窗口/其它窗口的操作。

分组：
  P1 进程与枚举      get_path / get_base_path / enum_process / enum_window /
                     find_window_by_process(_id) / get_process_info / get_window_process_*
  P2 窗口查询        get_client_rect / get_window_title/class / get_window_state /
                     get_window / get_special_window / get_foreground_window /
                     get_point_window / screen_to_client ↔ client_to_screen 往返
  P3 窗口写操作      只作用于自建靶子（move/size/set_client_size/text/transparent/state/
                     lock_position/lock_size/disable_min_max/layout_windows）
  P4 杂项与字符串    get_id / get_dpi / get_time / get_machine_code / get_random_* /
                     gai_lu / delay(s) / clipboard / win_exec / get_cmd_str /
                     send_string / send_paste / set_ime / get_foreground_focus

用法：
  python scripts/probes/_t_window_api.py [--groups P1,P2,P3,P4]
"""
import argparse
import ctypes
import os
import sys
import time
from ctypes import wintypes
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
OUT_DIR = REPO / "workbench" / "probes"
sys.path.insert(0, str(REPO / "bindings" / "python"))

try:
    _u = ctypes.windll.user32
    _u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    _u.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
    _u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

from op import Op  # noqa: E402

TS = time.strftime("%Y%m%d_%H%M%S")
WORK = OUT_DIR / ("window_api_" + TS)
RES = []
_logf = None
_group = ["P0"]

TITLE = "OP_WINDOW_PROBE_TGT"


def log(msg=""):
    line = str(msg)
    print(line, flush=True)
    if _logf:
        _logf.write(line + "\n")
        _logf.flush()


def sec(t):
    log("")
    log("=" * 78)
    log("== " + t)
    log("=" * 78)


def rec(api, detail, status):
    RES.append((_group[0], api, detail, status))
    mark = {"PASS": "[ OK ]", "FAIL": "[FAIL]", "INFO": "[INFO]", "SKIP": "[SKIP]"}.get(status, "[ ?? ]")
    log("%s %-48s %s" % (mark, api, detail))


def T(api, fn, ok=lambda v: bool(v), show=lambda v: repr(v)[:70], soft=False):
    try:
        v = fn()
    except Exception as e:
        rec(api, "EXC %r" % (e,), "INFO" if soft else "FAIL")
        return None
    try:
        good = bool(ok(v))
    except Exception as e:
        rec(api, "断言异常 %r（值=%s）" % (e, show(v)), "INFO" if soft else "FAIL")
        return v
    rec(api, show(v), "PASS" if good else ("INFO" if soft else "FAIL"))
    return v


def native_window(title=TITLE, w=320, h=240, x=120, y=120):
    """原生 Win32 靶子（不依赖 tkinter）。返回 (hwnd, u32)。"""
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    g32 = ctypes.WinDLL("gdi32", use_last_error=True)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32.CreateWindowExW.restype = wintypes.HWND
    u32.DefWindowProcW.restype = ctypes.c_longlong
    u32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    k32.GetModuleHandleW.restype = ctypes.c_void_p
    u32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                    wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                    wintypes.HINSTANCE, wintypes.LPVOID]
    u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
    u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    u32.DestroyWindow.argtypes = [wintypes.HWND]

    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wintypes.HWND, wintypes.UINT,
                                 wintypes.WPARAM, wintypes.LPARAM)

    def _proc(hwnd, msg, wp, lp):
        return u32.DefWindowProcW(hwnd, msg, wp, lp)

    proc = WNDPROC(_proc)
    native_window._keepalive.append(proc)

    class WNDCLASSEX(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.UINT), ("style", wintypes.UINT),
                    ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                    ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE),
                    ("hIcon", wintypes.HICON), ("hCursor", wintypes.HANDLE),
                    ("hbrBackground", wintypes.HBRUSH), ("lpszMenuName", wintypes.LPCWSTR),
                    ("lpszClassName", wintypes.LPCWSTR), ("hIconSm", wintypes.HICON)]

    hinst = k32.GetModuleHandleW(None)
    cls = WNDCLASSEX()
    cls.cbSize = ctypes.sizeof(WNDCLASSEX)
    cls.lpfnWndProc = proc
    cls.hInstance = hinst
    cls.hbrBackground = g32.CreateSolidBrush(0xFFFFFF)
    cls.lpszClassName = title
    if not u32.RegisterClassExW(ctypes.byref(cls)) and ctypes.get_last_error() != 1410:
        return None, u32
    hwnd = u32.CreateWindowExW(0, title, title, 0x00CF0000, x, y, w, h, None, None, hinst, None)
    if hwnd:
        u32.ShowWindow(hwnd, 8)      # SW_SHOWNA：显示但不抢焦点
        time.sleep(0.3)
    return hwnd, u32


native_window._keepalive = []


# ------------------------------------------------------------------ P1 进程与枚举
def p1(op, hwnd):
    _group[0] = "P1"
    sec("P1 进程与枚举")
    T("get_path()（插件所在目录）", lambda: op.get_path(),
      ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: v[:70])
    T("get_base_path()", lambda: op.get_base_path(),
      ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: v[:70])

    # 大漠语义：EnumProcess(name) 按**进程名**枚举 pid；空名返回空串（不是"全部进程"）。
    self_pid = os.getpid()
    me = Path(sys.executable).name
    s = T("enum_process(%r)（按进程名枚举 pid）" % me, lambda: op.enum_process(me),
          ok=lambda v: isinstance(v, str) and v != "", show=lambda v: "pids=%s" % v[:60])
    if isinstance(s, str) and s:
        rec("  ↳ 本进程 pid 在枚举结果中", "self_pid=%d" % self_pid,
            "PASS" if str(self_pid) in s.split(",") else "FAIL")
    T("  ↳ 反向：不存在的进程名 ⇒ 空串", lambda: op.enum_process("__no_such_proc__.exe"),
      ok=lambda v: v == "", show=lambda v: repr(v), soft=True)
    T("  ↳ 空进程名 ⇒ 空串（不是\"全部进程\"）", lambda: op.enum_process(),
      ok=lambda v: v == "", show=lambda v: repr(v), soft=True)

    s2 = T("enum_window(0,'','' )（顶层窗口）", lambda: op.enum_window(0, "", "", 0),
           ok=lambda v: isinstance(v, str), show=lambda v: "%d 个" % len(v.split(",")) if v else repr(v), soft=True)
    if isinstance(s2, str) and s2:
        rec("  ↳ 自建靶子在枚举结果中", "hwnd=%s" % hex(hwnd),
            "PASS" if str(hwnd) in s2.split(",") else "FAIL")

    me = Path(sys.executable).name
    T("find_window_by_process(%r)" % me, lambda: op.find_window_by_process(me, "", ""),
      ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)
    T("find_window_by_process_id(self)", lambda: op.find_window_by_process_id(self_pid, "", ""),
      ok=lambda v: isinstance(v, int) and v != 0, show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)
    T("  ↳ 反向：pid=0 ⇒ 0", lambda: op.find_window_by_process_id(0, "", ""),
      ok=lambda v: not v, show=lambda v: "ret=%s" % v, soft=True)
    T("find_window_ex(0, 类名/标题)", lambda: op.find_window_ex(0, "", TITLE),
      ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s（0 表示未搜到，标题匹配规则见实现）" % hex(v or 0),
      soft=True)

    T("get_window_process_id(靶子)", lambda: op.get_window_process_id(hwnd),
      ok=lambda v: isinstance(v, int) and v > 0, show=lambda v: "pid=%s（self=%d）" % (v, self_pid))
    T("get_window_process_path(靶子)", lambda: op.get_window_process_path(hwnd),
      ok=lambda v: isinstance(v, str) and v.endswith(".exe"), show=lambda v: str(v)[:60])
    T("get_process_info(self_pid)", lambda: op.get_process_info(self_pid),
      ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:70], soft=True)
    T("  ↳ 反向：不存在的 pid ⇒ 空串", lambda: op.get_process_info(999999),
      ok=lambda v: v == "", show=lambda v: repr(v), soft=True)


# ------------------------------------------------------------------ P2 窗口查询
def p2(op, hwnd, u32):
    _group[0] = "P2"
    sec("P2 窗口查询（含 screen↔client 往返）")
    T("get_client_rect(靶子)", lambda: op.get_client_rect(hwnd),
      ok=lambda v: isinstance(v, tuple) and len(v) == 4, show=lambda v: str(v))
    cw, ch = op.get_client_size(hwnd)
    T("get_client_size(靶子)", lambda: (cw, ch), ok=lambda v: v[0] > 0 and v[1] > 0,
      show=lambda v: "%dx%d" % v)
    T("get_window_title(靶子)", lambda: op.get_window_title(hwnd),
      ok=lambda v: v == TITLE, show=lambda v: repr(v))
    T("get_window_class(靶子)", lambda: op.get_window_class(hwnd),
      ok=lambda v: isinstance(v, str) and v != "", show=lambda v: repr(v))
    T("get_window_state(靶子, 0=是否存在)", lambda: op.get_window_state(hwnd, 0),
      ok=lambda v: isinstance(v, int), show=lambda v: "state=%s" % v, soft=True)
    T("get_window(靶子, 0=父)", lambda: op.get_window(hwnd, 0),
      ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)
    T("get_special_window(0)", lambda: op.get_special_window(0),
      ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)
    T("get_foreground_window()", lambda: op.get_foreground_window(),
      ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)
    T("get_foreground_focus()", lambda: op.get_foreground_focus(),
      ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)

    # get_point_window：用靶子**客户区中心**换算出的屏幕坐标，应取回靶子
    rect = wintypes.RECT()
    u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    u32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    u32.GetClientRect(hwnd, ctypes.byref(rect))
    pt = wintypes.POINT((rect.right - rect.left) // 2, (rect.bottom - rect.top) // 2)
    u32.ClientToScreen(hwnd, ctypes.byref(pt))
    got = T("get_point_window(靶子中心的屏幕坐标)", lambda: op.get_point_window(pt.x, pt.y),
            ok=lambda v: isinstance(v, int), show=lambda v: "hwnd=%s" % hex(v or 0), soft=True)
    rec("  ↳ 取回的 hwnd == 靶子", "期望=%s 实际=%s" % (hex(hwnd), hex(got or 0)),
        "PASS" if got == hwnd else "FAIL")

    # screen_to_client ↔ client_to_screen 往返
    sc = T("screen_to_client(靶子, 屏幕中心)", lambda: op.screen_to_client(hwnd, pt.x, pt.y),
           ok=lambda v: isinstance(v, tuple) and len(v) == 2, show=lambda v: str(v), soft=True)
    if isinstance(sc, tuple):
        back = op.client_to_screen(hwnd, sc[0], sc[1])
        rec("  ↳ client_to_screen 往返还原",
            "屏幕(%d,%d) → 客户(%s) → 屏幕%s" % (pt.x, pt.y, sc, back),
            "PASS" if (isinstance(back, tuple) and abs(back[0] - pt.x) <= 1 and abs(back[1] - pt.y) <= 1)
            else "FAIL")


# ------------------------------------------------------------------ P3 窗口写操作（只动自建靶子）
def p3(op, hwnd, u32):
    _group[0] = "P3"
    sec("P3 窗口写操作（只作用于自建靶子）")

    def rect_of():
        r = wintypes.RECT()
        u32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        return r

    r0 = rect_of()
    T("move_window(靶子, %d,%d)" % (r0.left + 40, r0.top + 30),
      lambda: op.move_window(hwnd, r0.left + 40, r0.top + 30), ok=lambda v: bool(v))
    time.sleep(0.35)
    r1 = rect_of()
    rec("  ↳ 窗口左上角真的移动了", "(%d,%d) → (%d,%d)" % (r0.left, r0.top, r1.left, r1.top),
        "PASS" if (r1.left != r0.left or r1.top != r0.top) else "FAIL")
    op.move_window(hwnd, r0.left, r0.top)
    time.sleep(0.3)

    T("set_window_size(靶子, 360,280)", lambda: op.set_window_size(hwnd, 360, 280),
      ok=lambda v: bool(v))
    time.sleep(0.35)
    r2 = rect_of()
    rec("  ↳ 外框尺寸真的变了", "宽 %d→%d 高 %d→%d" %
        (r0.right - r0.left, r2.right - r2.left, r0.bottom - r0.top, r2.bottom - r2.top),
        "PASS" if (r2.right - r2.left) != (r0.right - r0.left) else "FAIL")

    T("set_client_size(靶子, 200,150)", lambda: op.set_client_size(hwnd, 200, 150),
      ok=lambda v: bool(v))
    time.sleep(0.35)
    cw, ch = op.get_client_size(hwnd)
    rec("  ↳ 客户区真的变成 200x150", "实测 %dx%d" % (cw, ch),
        "PASS" if (cw, ch) == (200, 150) else "FAIL")

    new_title = TITLE + "_RENAMED"
    T("set_window_text(靶子)", lambda: op.set_window_text(hwnd, new_title), ok=lambda v: bool(v))
    time.sleep(0.2)
    rec("  ↳ 标题真的改了", "读回=%r" % op.get_window_title(hwnd),
        "PASS" if op.get_window_title(hwnd) == new_title else "FAIL")
    op.set_window_text(hwnd, TITLE)

    T("set_window_transparent(靶子, 128)", lambda: op.set_window_transparent(hwnd, 128),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ 复原 transparent=0", lambda: op.set_window_transparent(hwnd, 0),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ 边界：越界 999（实现按钳制处理，仍返 True）",
      lambda: op.set_window_transparent(hwnd, 999),
      ok=lambda v: bool(v), show=lambda v: "ret=%s（越界值被钳制而非报错）" % v, soft=True)

    T("set_window_state(靶子, 1=激活显示)", lambda: op.set_window_state(hwnd, 1),
      ok=lambda v: bool(v), soft=True)
    T("lock_window_position(靶子, True)", lambda: op.lock_window_position(hwnd, True),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ 解锁", lambda: op.lock_window_position(hwnd, False), ok=lambda v: bool(v), soft=True)
    T("lock_window_size(靶子, True)", lambda: op.lock_window_size(hwnd, True),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ 解锁", lambda: op.lock_window_size(hwnd, False), ok=lambda v: bool(v), soft=True)
    T("disable_min_max(靶子, True)", lambda: op.disable_min_max(hwnd, True),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ 恢复", lambda: op.disable_min_max(hwnd, False), ok=lambda v: bool(v), soft=True)


# ------------------------------------------------------------------ P4 杂项
def p4(op, hwnd):
    _group[0] = "P4"
    sec("P4 杂项 / 字符串 / 剪贴板（剪贴板会先存后还原）")
    # 大漠 GetID：注册版返回授权 id；未注册/未设置返回 0（本环境的实测值）
    T("get_id()（未注册环境返回 0）", lambda: op.get_id(),
      ok=lambda v: isinstance(v, int) and v >= 0, show=lambda v: "id=%s" % v, soft=True)
    T("get_dpi()", lambda: op.get_dpi(), ok=lambda v: isinstance(v, int) and v in (96, 120, 144, 192),
      show=lambda v: "dpi=%s（本机 150%% ⇒ 144）" % v)
    T("get_time()", lambda: op.get_time(), ok=lambda v: isinstance(v, str) and len(v) >= 8,
      show=lambda v: repr(v))
    T("get_machine_code()", lambda: op.get_machine_code(),
      ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: "%d 字符" % len(v))
    T("get_random_number(5,5) == 5", lambda: op.get_random_number(5, 5),
      ok=lambda v: v == 5, show=lambda v: str(v))
    vals = [op.get_random_number(1, 1000) for _ in range(30)]
    rec("get_random_number(1,1000) 落在区间且非恒定",
        "min=%d max=%d 不同值=%d" % (min(vals), max(vals), len(set(vals))),
        "PASS" if (1 <= min(vals) and max(vals) <= 1000 and len(set(vals)) > 1) else "FAIL")
    T("  ↳ 反向：区间反了(min>max) 仍返回值（记录实现行为）",
      lambda: op.get_random_number(10, 1), ok=lambda v: isinstance(v, int),
      show=lambda v: "ret=%s" % v, soft=True)
    d = T("get_random_double(2.5,2.5) == 2.5", lambda: op.get_random_double(2.5, 2.5),
          ok=lambda v: abs(v - 2.5) < 1e-9, show=lambda v: str(v))
    T("gai_lu(1) 恒 True", lambda: op.gai_lu(1), ok=lambda v: v is True, show=lambda v: str(v))
    T("gai_lu(0) 恒 False", lambda: op.gai_lu(0), ok=lambda v: v is False, show=lambda v: str(v))

    t0 = time.time()
    op.delay(120)
    el = (time.time() - t0) * 1000
    rec("delay(120) 真的等了", "elapsed=%.0fms" % el, "PASS" if el >= 110 else "FAIL")
    t0 = time.time()
    op.delays(80, 80)
    el = (time.time() - t0) * 1000
    rec("delays(80,80) 真的等了", "elapsed=%.0fms" % el, "PASS" if el >= 70 else "FAIL")

    # ---- 剪贴板：先存后还原 ----
    u32 = ctypes.windll.user32
    k32 = ctypes.windll.kernel32
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [ctypes.c_void_p]
    k32.GlobalUnlock.restype = ctypes.c_bool
    k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    u32.OpenClipboard.argtypes = [wintypes.HWND]
    u32.CloseClipboard.restype = ctypes.c_bool
    u32.GetClipboardData.restype = ctypes.c_void_p
    u32.GetClipboardData.argtypes = [wintypes.UINT]

    def read_clip():
        if not u32.OpenClipboard(None):
            return None
        try:
            h = u32.GetClipboardData(13)     # CF_UNICODETEXT
            if not h:
                return None
            p = k32.GlobalLock(h)
            if not p:
                return None
            try:
                return ctypes.wstring_at(p)
            finally:
                k32.GlobalUnlock(h)
        finally:
            u32.CloseClipboard()

    def clip_set(text, tries=6):
        """剪贴板是**全局共享资源**：外部进程（剪贴板历史/云同步/监视器）会间歇占用，
        此时 OpenClipboard 直接 ACCESS_DENIED(err=5) 且 op 不重试 ⇒ 探针必须重试，
        否则会把环境争用误判成插件缺陷（第一轮就踩了这个坑）。"""
        for i in range(tries):
            if op.set_clipboard(text):
                return True
            time.sleep(0.2)
        return False

    def clip_get(expect, tries=6):
        """读也要重试：写成功后外部进程可能立刻改写/清空剪贴板（同一类环境争用）。"""
        for i in range(tries):
            if op.get_clipboard() == expect:
                return True
            time.sleep(0.2)
        return False

    old = read_clip()
    marker = "OP_PROBE_CLIP_%s" % TS
    ok_set = clip_set(marker)
    rec("set_clipboard(带重试，规避外部占用)", "ret=%s" % ok_set, "PASS" if ok_set else "FAIL")
    rec("get_clipboard() 读回 marker", "读回=%r" % op.get_clipboard()[:60],
        "PASS" if clip_get(marker) else "FAIL")
    # CF_TEXT(ANSI) 口径下的中文往返（op 用 wide_to_ansi/ansi_to_wide，本机代码页 GBK）
    cn = "探针中文往返OK"
    ok_cn = clip_set(cn)
    rec("set_clipboard(中文) 带重试", "ret=%s" % ok_cn, "PASS" if ok_cn else "FAIL")
    rec("  ↳ 中文往返一致", "读回=%r" % op.get_clipboard(), "PASS" if clip_get(cn) else "FAIL")
    # 还原
    if old is not None:
        ok_restore = clip_set(old) and op.get_clipboard() == old
        rec("  ↳ 还原原剪贴板内容", "还原=%s 原=%r" % (ok_restore, old[:40]),
            "PASS" if ok_restore else "FAIL")
    else:
        rec("  ↳ 还原原剪贴板内容", "原剪贴板为空/非文本，跳过还原", "SKIP")

    # ---- 进程启动：SW_HIDE，不留窗口 ----
    T("win_exec('cmd /c exit', SW_HIDE=0)", lambda: op.win_exec("cmd /c exit", 0),
      ok=lambda v: bool(v), soft=True)
    out = T("get_cmd_str('cmd /c echo OP_PROBE_OK', 3000)",
            lambda: op.get_cmd_str("cmd /c echo OP_PROBE_OK", 3000),
            ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:60], soft=True)
    if isinstance(out, str):
        rec("  ↳ 命令行回显含标记", "out=%r" % out[:40],
            "PASS" if "OP_PROBE_OK" in out else "INFO")

    # ---- 字符串发送：只发到自建靶子 ----
    T("send_string(靶子, 'abc')", lambda: op.send_string(hwnd, "abc"),
      ok=lambda v: bool(v), soft=True)
    T("send_paste(靶子)", lambda: op.send_paste(hwnd), ok=lambda v: bool(v), soft=True)
    T("set_ime(靶子, False)", lambda: op.set_ime(hwnd, False), ok=lambda v: bool(v), soft=True)
    T("  ↳ set_ime(靶子, True) 复原", lambda: op.set_ime(hwnd, True),
      ok=lambda v: bool(v), soft=True)
    T("beep(1000, 1)（1ms，几乎无声）", lambda: op.beep(1000, 1),
      ok=lambda v: bool(v), soft=True)


# ------------------------------------------------------------------ main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="P1,P2,P3,P4")
    ap.add_argument("--with-beep", action="store_true", help="包含 beep（默认包含，1ms）")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    _logf = open(str(OUT_DIR / ("_t_window_api_%s.txt" % TS)), "w", encoding="utf-8")
    log("窗口/进程 + 杂项域 C API 验证  %s" % TS)
    log("产物目录 %s" % WORK)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("op.dll = %s" % op.dll_path)

    hwnd, u32 = native_window()
    if not hwnd:
        rec("创建原生靶子窗口", "失败", "FAIL")
        return 1
    rec("创建原生靶子窗口", "hwnd=%s" % hex(hwnd), "PASS")

    try:
        if "P1" in groups:
            p1(op, hwnd)
        if "P2" in groups:
            p2(op, hwnd, u32)
        if "P3" in groups:
            p3(op, hwnd, u32)
        if "P4" in groups:
            p4(op, hwnd)
    finally:
        try:
            u32.DestroyWindow(hwnd)
        except Exception:
            pass

    n = {"PASS": 0, "FAIL": 0, "INFO": 0, "SKIP": 0}
    for _, _, _, s in RES:
        n[s] = n.get(s, 0) + 1
    sec("汇总")
    log("PASS=%d  FAIL=%d  INFO=%d  SKIP=%d   （共 %d 条）" % (n["PASS"], n["FAIL"], n["INFO"], n["SKIP"], len(RES)))
    if n["FAIL"]:
        log("")
        log("失败明细：")
        for g, api, detail, s in RES:
            if s == "FAIL":
                log("  [%s] %s -- %s" % (g, api, detail))
    log("")
    log("日志：%s" % _logf.name)
    try:
        op.close()
    except Exception:
        pass
    return 0 if n["FAIL"] == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
