# -*- coding: utf-8 -*-
"""键鼠域 **C API 层** 验证 —— 覆盖缺口里未做端到端的 18 个输入 API。

判据方法（2026-09-29 真机定性，勿再走弯路）：
  op 的 `windows` 鼠标模式是**消息式**（`WinMouse.cpp:300-303`
  `SendTimeout(_hwnd, WM_MOUSEMOVE, ...)`）⇒ **物理光标不动** ⇒
    · `GetCursorPos` 回读    —— 无判别力（读到的是操作者自己的鼠标）
    · Tk `<Motion>` 绑定     —— 无判别力（Tk 依据真实光标生成事件，实测 0 条）
    · **原生 Win32 窗口 + 自定义 WndProc** —— 唯一有效判据（同线程 SendMessage
      直调 WndProc，消息与 lParam 可逐条核对）。
靶子是自建窗口 ⇒ 点击/按键只落在它上面 ⇒ **对游戏零副作用**。

分组：
  W1 鼠标按键族（10）：left/middle/right/xbutton1/xbutton2 × 单击/双击
  W2 滚轮（4）：wheel / hwheel / wheel_down / wheel_up
  W3 轨迹与延时（7）：set_mouse_trajectory / move_to_smooth / move_to_ex_smooth /
                     move_path / drag_path / set_mouse_delay / set_keypad_delay
  W4 键盘（4）：key_down_char / key_up_char / key_press_char / key_press_str + wait_key
  W5 lock_input 反向（1）

用法：
  python scripts/probes/_t_input_full.py [--groups W1,W2,W3,W4,W5]
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

# DPI 感知必须在**任何窗口 API 之前**声明，否则拿到虚拟化逻辑值（150% 缩放下丢画面）
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
WORK = OUT_DIR / ("input_full_" + TS)
RES = []
_logf = None
_group = ["W0"]


def log(msg=""):
    line = str(msg)
    print(line, flush=True)
    if _logf:
        _logf.write(line + "\n")
        _logf.flush()


def sec(title):
    log("")
    log("=" * 78)
    log("== " + title)
    log("=" * 78)


def rec(api, detail, status):
    RES.append((_group[0], api, detail, status))
    mark = {"PASS": "[ OK ]", "FAIL": "[FAIL]", "INFO": "[INFO]", "SKIP": "[SKIP]"}.get(status, "[ ?? ]")
    log("%s %-50s %s" % (mark, api, detail))


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


# ------------------------------------------------------------------ 原生靶子
WM_NAMES = {
    0x0200: "WM_MOUSEMOVE",
    0x0201: "WM_LBUTTONDOWN", 0x0202: "WM_LBUTTONUP", 0x0203: "WM_LBUTTONDBLCLK",
    0x0204: "WM_RBUTTONDOWN", 0x0205: "WM_RBUTTONUP", 0x0206: "WM_RBUTTONDBLCLK",
    0x0207: "WM_MBUTTONDOWN", 0x0208: "WM_MBUTTONUP", 0x0209: "WM_MBUTTONDBLCLK",
    0x020A: "WM_MOUSEWHEEL", 0x020E: "WM_MOUSEHWHEEL",
    0x020B: "WM_XBUTTONDOWN", 0x020C: "WM_XBUTTONUP", 0x020D: "WM_XBUTTONDBLCLK",
    0x0100: "WM_KEYDOWN", 0x0101: "WM_KEYUP", 0x0102: "WM_CHAR",
    0x0104: "WM_SYSKEYDOWN", 0x0105: "WM_SYSKEYUP",
}


class NativeTarget(object):
    """自建原生 Win32 窗口，WndProc 记录所有输入消息（含 lParam/wParam）。"""

    def __init__(self, title="OP_INPUT_PROBE_TGT", w=300, h=220):
        self.title = title
        self.w, self.h = w, h
        self.records = []
        self.hwnd = None
        self._proc = None
        u32 = ctypes.WinDLL("user32", use_last_error=True)
        g32 = ctypes.WinDLL("gdi32", use_last_error=True)
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.u32, self.g32, self.k32 = u32, g32, k32

        u32.CreateWindowExW.restype = wintypes.HWND
        u32.DefWindowProcW.restype = ctypes.c_longlong
        u32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT,
                                       wintypes.WPARAM, wintypes.LPARAM]
        k32.GetModuleHandleW.restype = ctypes.c_void_p
        u32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                        wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                        ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                        wintypes.HINSTANCE, wintypes.LPVOID]
        u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
        u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        u32.UpdateWindow.argtypes = [wintypes.HWND]
        u32.DestroyWindow.argtypes = [wintypes.HWND]

        WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wintypes.HWND, wintypes.UINT,
                                     wintypes.WPARAM, wintypes.LPARAM)

        def _proc(hwnd, msg, wp, lp):
            name = WM_NAMES.get(msg)
            if name:
                # lParam 低 16 = x，高 16 = y（有符号）；wParam 对滚轮是 delta<<16
                self.records.append((name,
                                     ctypes.c_short(lp & 0xFFFF).value,
                                     ctypes.c_short((lp >> 16) & 0xFFFF).value,
                                     wp))
            return u32.DefWindowProcW(hwnd, msg, wp, lp)

        self._proc = WNDPROC(_proc)          # 必须保引用，否则回调被 GC 后进程崩溃

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
        cls.style = 0x0008          # CS_DBLCLKS：否则系统不会合成 *DBLCLK 消息
        cls.lpfnWndProc = self._proc
        cls.hInstance = hinst
        cls.hbrBackground = g32.CreateSolidBrush(0xFFFFFF)
        cls.lpszClassName = title
        if not u32.RegisterClassExW(ctypes.byref(cls)) and ctypes.get_last_error() != 1410:
            raise RuntimeError("RegisterClassExW 失败 err=%d" % ctypes.get_last_error())
        self.hwnd = u32.CreateWindowExW(0, title, title, 0x00CF0000,
                                        80, 80, w, h, None, None, hinst, None)
        if self.hwnd:
            u32.ShowWindow(self.hwnd, 8)     # SW_SHOWNA：不抢焦点
            u32.UpdateWindow(self.hwnd)
            time.sleep(0.25)

    def reset(self):
        self.records = []

    def count(self, *names):
        return sum(1 for r in self.records if r[0] in names)

    def last(self, *names, n=3):
        xs = [r for r in self.records if r[0] in names]
        return xs[-n:]

    def close(self):
        if self.hwnd:
            self.u32.DestroyWindow(self.hwnd)
            self.hwnd = None
            time.sleep(0.15)


def bind_native(op, tgt):
    if not tgt.hwnd:
        rec("创建原生靶子窗口", "hwnd=0", "FAIL")
        return False
    rec("创建原生靶子窗口", "hwnd=%s" % hex(tgt.hwnd), "PASS")
    if not op.bind_window(tgt.hwnd, "gdi", "windows", "windows", 0):
        rec("bind_window(gdi/windows/windows)", "失败", "FAIL")
        return False
    cw, ch = op.get_client_size(tgt.hwnd)
    log("  靶子客户区 %dx%d" % (cw, ch))
    return True


# ------------------------------------------------------------------ W1 鼠标按键族
def w1(op, tgt):
    _group[0] = "W1"
    sec("W1 鼠标按键族（10）—— 每键都验「down 条数 + 坐标落在靶子内」")
    cw, ch = op.get_client_size(tgt.hwnd)
    tx, ty = cw // 2, ch // 2
    op.move_to(tx, ty)
    time.sleep(0.15)

    # (api, 调用, down 消息名, dblclk 消息名)
    # Windows 双击的真实序列是 DOWN → UP → **DBLCLK** → UP（第二次 down 由系统合成为
    # DBLCLK）。op 的 button_double_click 正是这个序列 ⇒ 断言「DOWN×1 + DBLCLK×1」，
    # 而不是想当然的「DOWN×2」（第一次按这个错判据跑出 5 条 FAIL，实为判据错）。
    cases = [
        ("left_click", lambda: op.left_click(), "WM_LBUTTONDOWN", None, 1),
        ("left_double_click", lambda: op.left_double_click(), "WM_LBUTTONDOWN", "WM_LBUTTONDBLCLK", 1),
        ("middle_click", lambda: op.middle_click(), "WM_MBUTTONDOWN", None, 1),
        ("middle_double_click", lambda: op.middle_double_click(), "WM_MBUTTONDOWN", "WM_MBUTTONDBLCLK", 1),
        ("right_click", lambda: op.right_click(), "WM_RBUTTONDOWN", None, 1),
        ("right_double_click", lambda: op.right_double_click(), "WM_RBUTTONDOWN", "WM_RBUTTONDBLCLK", 1),
        ("xbutton1_click", lambda: op.xbutton1_click(), "WM_XBUTTONDOWN", None, 1),
        ("xbutton1_double_click", lambda: op.xbutton1_double_click(), "WM_XBUTTONDOWN", "WM_XBUTTONDBLCLK", 1),
        ("xbutton2_click", lambda: op.xbutton2_click(), "WM_XBUTTONDOWN", None, 1),
        ("xbutton2_double_click", lambda: op.xbutton2_double_click(), "WM_XBUTTONDOWN", "WM_XBUTTONDBLCLK", 1),
    ]
    for name, fn, down_msg, dbl_msg, n in cases:
        tgt.reset()
        T(name + "()", fn, ok=lambda v: bool(v), show=lambda v: "ret=%s" % v, soft=True)
        time.sleep(0.2)
        got = tgt.count(down_msg)
        sample = tgt.last(down_msg)
        good = got >= n
        detail = "%s=%d" % (down_msg, got)
        if dbl_msg:
            gd = tgt.count(dbl_msg)
            good = good and gd >= 1
            detail += " + %s=%d" % (dbl_msg, gd)
        rec("  ↳ WndProc 收到 %s%s" % (down_msg, (" + " + dbl_msg) if dbl_msg else ""),
            "%s，样本=%s" % (detail, sample), "PASS" if good else "FAIL")

    # XBUTTON 语义：wParam 高 16 位标识是 XBUTTON1(1) 还是 XBUTTON2(2)
    tgt.reset()
    op.xbutton1_click()
    time.sleep(0.2)
    x1 = [r for r in tgt.records if r[0] == "WM_XBUTTONDOWN"]
    tgt.reset()
    op.xbutton2_click()
    time.sleep(0.2)
    x2 = [r for r in tgt.records if r[0] == "WM_XBUTTONDOWN"]
    b1 = (x1[0][3] >> 16) & 0xFFFF if x1 else -1
    b2 = (x2[0][3] >> 16) & 0xFFFF if x2 else -1
    rec("  ↳ XBUTTON1/2 区分（wParam 高 16 位应为 1 / 2）",
        "xbutton1→%d  xbutton2→%d" % (b1, b2), "PASS" if (b1 == 1 and b2 == 2) else "FAIL")

    # 坐标语义：点击坐标必须落在靶子客户区内（不是屏幕绝对、也不是 0,0）
    tgt.reset()
    op.move_to(tx, ty)
    op.left_click()
    time.sleep(0.2)
    dn = [r for r in tgt.records if r[0] == "WM_LBUTTONDOWN"]
    inside = dn and all(0 <= x < cw and 0 <= y < ch for _, x, y, _ in dn)
    rec("  ↳ 点击坐标落在客户区内（客户区口径）",
        "样本=%s 客户区=%dx%d" % (dn[-2:], cw, ch), "PASS" if inside else "FAIL")


# ------------------------------------------------------------------ W2 滚轮
def w2(op, tgt):
    _group[0] = "W2"
    sec("W2 滚轮（4）—— WM_MOUSEWHEEL / WM_MOUSEHWHEEL + delta 符号")
    cw, ch = op.get_client_size(tgt.hwnd)
    op.move_to(cw // 2, ch // 2)
    time.sleep(0.15)

    def delta_of(name, n=1):
        rs = [r for r in tgt.records if r[0] == name]
        if not rs:
            return None, 0
        # wParam 高 16 位是有符号 delta
        d = ctypes.c_short((rs[-1][3] >> 16) & 0xFFFF).value
        return d, len(rs)

    tgt.reset()
    T("wheel(+120)", lambda: op.wheel(120), ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    d, n = delta_of("WM_MOUSEWHEEL")
    rec("  ↳ 收到 WM_MOUSEWHEEL 且 delta=+120", "delta=%s 条数=%d" % (d, n),
        "PASS" if (n >= 1 and d == 120) else "FAIL")

    tgt.reset()
    T("wheel(-120)", lambda: op.wheel(-120), ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    d, n = delta_of("WM_MOUSEWHEEL")
    rec("  ↳ delta 符号随参数取反（应为 -120）", "delta=%s 条数=%d" % (d, n),
        "PASS" if (n >= 1 and d == -120) else "FAIL")

    tgt.reset()
    T("hwheel(+120)", lambda: op.hwheel(120), ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    d, n = delta_of("WM_MOUSEHWHEEL")
    rec("  ↳ 收到 WM_MOUSEHWHEEL（横向滚轮）", "delta=%s 条数=%d 样本=%s" % (d, n, tgt.last("WM_MOUSEHWHEEL")),
        "PASS" if n >= 1 else "FAIL")

    for name, fn, sign in (("wheel_down", lambda: op.wheel_down(), -1),
                           ("wheel_up", lambda: op.wheel_up(), 1)):
        tgt.reset()
        T(name + "()", fn, ok=lambda v: bool(v), soft=True)
        time.sleep(0.2)
        d, n = delta_of("WM_MOUSEWHEEL")
        okdir = (n >= 1) and ((d < 0) if sign < 0 else (d > 0))
        rec("  ↳ %s 方向正确（delta%s0）" % (name, "<" if sign < 0 else ">"),
            "delta=%s 条数=%d" % (d, n), "PASS" if okdir else "FAIL")


# ------------------------------------------------------------------ W3 轨迹与延时
def w3(op, tgt):
    _group[0] = "W3"
    sec("W3 轨迹与延时（7）—— 轨迹必须真的走中间点；延时必须真的产生耗时")
    cw, ch = op.get_client_size(tgt.hwnd)

    # ① move_to_smooth：应产生**多个**中间 MOUSEMOVE（不是一步跳过去）
    tgt.reset()
    T("move_to_smooth(20,20→%d,%d, 300ms)" % (cw - 20, ch - 20),
      lambda: op.move_to_smooth(cw - 20, ch - 20, 300), ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    mv = tgt.count("WM_MOUSEMOVE")
    rec("  ↳ 产生多步中间点（>3 条 MOUSEMOVE）", "收到 %d 条" % mv,
        "PASS" if mv > 3 else "FAIL")

    # ② move_to_ex_smooth：返回字符串（大漠语义是 "x,y" 或空）
    tgt.reset()
    r = T("move_to_ex_smooth(...,0)", lambda: op.move_to_ex_smooth(30, 30, 8, 8, 200),
          ok=lambda v: isinstance(v, str), show=lambda v: "ret=%r" % v, soft=True)
    time.sleep(0.2)
    rec("  ↳ 返回字符串且产生移动消息",
        "ret=%r MOUSEMOVE=%d" % (r, tgt.count("WM_MOUSEMOVE")),
        "PASS" if (isinstance(r, str) and tgt.count("WM_MOUSEMOVE") > 0) else "FAIL")
    # 反向边界：随机范围 0x0 ⇒ 退化为**确定性**落点（就是传入点本身），不报错
    r0 = T("  ↳ 边界：随机范围 0x0 ⇒ 退化为确定性落点 '30,30'",
           lambda: op.move_to_ex_smooth(30, 30, 0, 0, 100),
           ok=lambda v: v == "30,30", show=lambda v: "ret=%r" % v, soft=True)

    # ③ move_path / drag_path："x,y|x,y|..." 关键点串
    path = "20,20|%d,20|%d,%d|20,%d" % (cw - 20, cw - 20, ch - 20, ch - 20)
    tgt.reset()
    T("move_path(%r, 300ms)" % path, lambda: op.move_path(path, 300),
      ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    mvn = tgt.count("WM_MOUSEMOVE")
    rec("  ↳ move_path 走完全程（多点 MOUSEMOVE）", "收到 %d 条" % mvn,
        "PASS" if mvn > 3 else "FAIL")

    tgt.reset()
    T("drag_path(%r, 300ms)" % path, lambda: op.drag_path(path, 300),
      ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    drag_dn = tgt.count("WM_LBUTTONDOWN")
    drag_mv = tgt.count("WM_MOUSEMOVE")
    rec("  ↳ drag_path 全程按住（先 DOWN 再移动）",
        "LBUTTONDOWN=%d MOUSEMOVE=%d LBUTTONUP=%d" % (drag_dn, drag_mv, tgt.count("WM_LBUTTONUP")),
        "PASS" if (drag_dn >= 1 and drag_mv > 3) else "FAIL")
    # 反向：只有 1 个点 ⇒ 不成路径
    T("  ↳ 反向：单点路径 '10,10' 应失败", lambda: op.move_path("10,10", 100),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)

    # ④ set_mouse_trajectory：开关型 API，断言合法性 + 之后仍正常工作
    T("set_mouse_trajectory(0,...)", lambda: op.set_mouse_trajectory(0, 10, 30, 0, 0, 0),
      ok=lambda v: bool(v), soft=True)
    tgt.reset()
    op.move_to(40, 40)
    time.sleep(0.2)
    rec("  ↳ 设置轨迹后 move_to 仍可达", "MOUSEMOVE=%d" % tgt.count("WM_MOUSEMOVE"),
        "PASS" if tgt.count("WM_MOUSEMOVE") >= 1 else "FAIL")
    T("  ↳ 反向：非法 mode(99) 应失败", lambda: op.set_mouse_trajectory(99, 10, 30, 0, 0, 0),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)

    # ⑤ 延时：A/B 计时（不能只看绝对值 —— DelayJitter(base, 40%) 会把延时抖动到
    #    [0.6×base, 1.4×base]，用「≥300ms」当判据必挂，实测 255ms/282ms 就是这么来的）。
    #    正确判据：delay=0 时点击几乎瞬时；delay=300 时耗时明显变长（差值 ≥ 0.5×base）。
    def timeit(action, n=1):
        t0 = time.time()
        for _ in range(n):
            action()
        return (time.time() - t0) * 1000.0 / n

    for label, setter, action in (
            ("set_mouse_delay('windows',%d)", lambda v: op.set_mouse_delay("windows", v),
             lambda: op.left_click()),
            ("set_keypad_delay('windows',%d)", lambda v: op.set_keypad_delay("windows", v),
             lambda: op.key_press(0x41))):
        ok0 = T(label % 0, lambda: setter(0), ok=lambda v: bool(v), soft=True)
        base_ms = timeit(action, 3)
        ok3 = T(label % 300, lambda: setter(300), ok=lambda v: bool(v), soft=True)
        slow_ms = timeit(action, 3)
        delta = slow_ms - base_ms
        good = bool(ok0 and ok3 and delta >= 150)
        rec("  ↳ 延时 A/B：0ms→%.0fms，300ms→%.0fms（差 %.0fms ≥ 150）" %
            (base_ms, slow_ms, delta),
            "%s" % ("延时真实生效" if good else "未见延时效果"),
            "PASS" if good else "FAIL")
        setter(0)


# ------------------------------------------------------------------ W4 键盘
def w4(op, tgt):
    _group[0] = "W4"
    sec("W4 键盘（5）—— 字符键走 vkmap；wait_key 语义")
    cw, ch = op.get_client_size(tgt.hwnd)
    op.move_to(cw // 2, ch // 2)
    time.sleep(0.15)

    tgt.reset()
    T("key_down_char('a')", lambda: op.key_down_char("a"), ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    dn = tgt.count("WM_KEYDOWN", "WM_CHAR")
    rec("  ↳ WndProc 收到按键消息", "收到 %d 条，样本=%s" % (dn, tgt.last("WM_KEYDOWN", "WM_CHAR")),
        "PASS" if dn >= 1 else "FAIL")

    tgt.reset()
    T("key_up_char('a')", lambda: op.key_up_char("a"), ok=lambda v: bool(v), soft=True)
    time.sleep(0.2)
    up = tgt.count("WM_KEYUP")
    rec("  ↳ WndProc 收到 WM_KEYUP", "收到 %d 条" % up, "PASS" if up >= 1 else "FAIL")

    tgt.reset()
    T("key_press_char('b')", lambda: op.key_press_char("b"), ok=lambda v: bool(v), soft=True)
    time.sleep(0.25)
    both = tgt.count("WM_KEYDOWN", "WM_KEYUP", "WM_CHAR")
    rec("  ↳ key_press_char 产生下+上（≥2 条）", "收到 %d 条，样本=%s" % (both, tgt.last("WM_KEYDOWN", "WM_KEYUP", "WM_CHAR", n=4)),
        "PASS" if both >= 2 else "FAIL")

    tgt.reset()
    T("key_press_str('ab', 0)", lambda: op.key_press_str("ab", 0), ok=lambda v: bool(v), soft=True)
    time.sleep(0.3)
    nmsg = tgt.count("WM_KEYDOWN", "WM_KEYUP", "WM_CHAR")
    rec("  ↳ 两字符 ⇒ 至少 2 条按键消息", "收到 %d 条，样本=%s" % (nmsg, tgt.last("WM_KEYDOWN", "WM_CHAR", n=4)),
        "PASS" if nmsg >= 2 else "FAIL")

    # 反向：不支持的字符 ⇒ 应失败（而不是返回 True 空转）
    T("  ↳ 反向：非法字符 '\\u4e2d' 应失败" , lambda: op.key_press_char("中"),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)

    # wait_key 语义（WinKeyboard.cpp:190）：
    #   vk=0   → 扫 GetAsyncKeyState(1..254)，有按下就返回该 vk
    #   vk!=0  → GetKeyState(vk)，非 0 返回 vk；timeout=0 立即返回
    r0 = T("wait_key(vk=0x7B[F12], timeout=0)（未按下）", lambda: op.wait_key(0x7B, 0),
           ok=lambda v: v == 0, show=lambda v: "ret=%s" % v, soft=True)
    t0 = time.time()
    r1 = op.wait_key(0x7B, 400)
    el = (time.time() - t0) * 1000.0
    rec("wait_key(未按下, timeout=400) 应等满并返回 0",
        "ret=%s elapsed=%.0fms" % (r1, el),
        "PASS" if (r1 == 0 and 350 <= el <= 1500) else "FAIL")
    r2 = T("wait_key(vk=0, timeout=0)（扫任意键，本机幽灵键 0x85）",
           lambda: op.wait_key(0, 0), ok=lambda v: isinstance(v, int),
           show=lambda v: "ret=%s%s" % (v, "（幽灵键 0x85，环境已知）" if v == 0x85 else ""),
           soft=True)


# ------------------------------------------------------------------ W5 lock_input
def w5(op, tgt):
    _group[0] = "W5"
    sec("W5 lock_input 语义与反向验证")
    # 设计口径（BindingSession.cpp:405）：
    #   lock=0 → 直接解锁，恒成功；
    #   lock=1/2 需**鼠标通道为 dx**、lock=1/3 需**键盘通道为 dx**，
    #   否则返回 0 ⇒ 在 gdi/windows/windows 绑定下 lock_input(1) 恒为 **False**（不是 bug）。
    # 因此本组先用 windows 模式验「设计口径」，再尝试 dx 绑定验「锁定真的生效」。
    cw, ch = op.get_client_size(tgt.hwnd)
    op.move_to(cw // 2, ch // 2)
    time.sleep(0.15)

    tgt.reset()
    op.left_click()
    time.sleep(0.2)
    base = tgt.count("WM_LBUTTONDOWN")

    T("lock_input(1)（windows 模式，设计上应返回 False）",
      lambda: op.lock_input(1), ok=lambda v: not bool(v),
      show=lambda v: "ret=%s（设计：只锁 dx 通道）" % v, soft=True)
    tgt.reset()
    op.left_click()
    time.sleep(0.2)
    rec("  ↳ 未锁定时输入照常送达（对照组）", "DOWN=%d" % tgt.count("WM_LBUTTONDOWN"),
        "PASS" if tgt.count("WM_LBUTTONDOWN") >= 1 else "FAIL")

    T("lock_input(0)", lambda: op.lock_input(0), ok=lambda v: bool(v), soft=True)
    tgt.reset()
    op.left_click()
    time.sleep(0.2)
    rec("  ↳ lock_input(0) 恒成功且不影响送达", "DOWN=%d" % tgt.count("WM_LBUTTONDOWN"),
        "PASS" if tgt.count("WM_LBUTTONDOWN") >= 1 else "FAIL")

    # 反向：越界 lock 值必须失败
    T("  ↳ 反向：lock=99 越界应失败", lambda: op.lock_input(99),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)

    # dx 通道：改绑 dx/dx/dx 后 lock 应生效（绑定失败则记 SKIP，不臆造结论）
    op.unbind_window()
    dx_ok = False
    try:
        dx_ok = bool(op.bind_window(tgt.hwnd, "gdi", "dx", "dx", 0))
    except Exception:
        dx_ok = False
    if not dx_ok:
        rec("dx 绑定自建窗口（用于验 lock 生效）", "bind 失败 ⇒ 跳过", "SKIP")
        return
    # 关键语义：**LockInput 拦的是「用户的物理输入」，不是 op 自己合成的输入**
    # （大漠同口径 —— 锁输入的用途是防止人工干扰，插件自身的模拟输入必须继续生效）。
    # 所以「锁定后 op 的 left_click 仍送达」是**正确行为**，不是 bug；
    # 要验锁定生效，必须注入**物理层**输入（keybd_event）看它是否被拦下。
    # 用 VK_F15（0x7E）：没有应用绑定它 ⇒ 万一没拦住也不会在别处打出字符。
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    u32.SetForegroundWindow.argtypes = [wintypes.HWND]
    u32.SetForegroundWindow.restype = ctypes.c_bool
    u32.GetForegroundWindow.restype = wintypes.HWND

    def phys_key(vk=0x7E):
        ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
        ctypes.windll.user32.keybd_event(vk, 0, 0x0002, 0)

    def pump(seconds=0.5):
        """物理输入是**投递到消息队列**的（不像 SendMessage 同步直调 WndProc），
        没有消息泵 WndProc 永远收不到 ⇒ 必须显式 Peek/Dispatch。
        （第一版漏了泵，导致对照组也是 0，"锁定生效"成了空洞结论。）"""
        msg = wintypes.MSG()
        u32.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                                     wintypes.UINT, wintypes.UINT, wintypes.UINT]
        u32.PeekMessageW.restype = ctypes.c_bool
        u32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
        u32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
        end = time.time() + seconds
        while time.time() < end:
            while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 0x0001):  # PM_REMOVE
                u32.TranslateMessage(ctypes.byref(msg))
                u32.DispatchMessageW(ctypes.byref(msg))
            time.sleep(0.01)

    fg = False
    try:
        fg = bool(u32.SetForegroundWindow(tgt.hwnd))
    except Exception:
        fg = False
    time.sleep(0.4)
    fg = fg and (u32.GetForegroundWindow() == tgt.hwnd)
    if not fg:
        rec("物理输入验证前置：把靶子置前", "SetForegroundWindow 未生效 ⇒ 物理输入到不了靶子，无法判定",
            "SKIP")
        T("lock_input(0)（解锁）", lambda: op.lock_input(0), ok=lambda v: bool(v), soft=True)
        return

    tgt.reset()
    op.lock_input(0)
    phys_key()
    pump(0.6)
    unlocked_hits = tgt.count("WM_KEYDOWN", "WM_SYSKEYDOWN")
    rec("  ↳ 未锁定时物理按键到达靶子（对照组）", "KEYDOWN=%d" % unlocked_hits,
        "PASS" if unlocked_hits >= 1 else "FAIL")

    oklock = T("lock_input(1)（dx 模式，拦物理输入）", lambda: op.lock_input(1),
               ok=lambda v: bool(v), soft=True)
    tgt.reset()
    phys_key()
    pump(0.6)
    locked_hits = tgt.count("WM_KEYDOWN", "WM_SYSKEYDOWN")
    rec("  ↳ 锁定后物理按键被拦下", "KEYDOWN=%d（对照组 %d）" % (locked_hits, unlocked_hits),
        "PASS" if (oklock and locked_hits == 0) else "FAIL")

    T("lock_input(0)（解锁）", lambda: op.lock_input(0), ok=lambda v: bool(v), soft=True)
    tgt.reset()
    phys_key()
    pump(0.6)
    rec("  ↳ 解锁后物理按键恢复", "KEYDOWN=%d" % tgt.count("WM_KEYDOWN", "WM_SYSKEYDOWN"),
        "PASS" if tgt.count("WM_KEYDOWN", "WM_SYSKEYDOWN") >= 1 else "FAIL")


# ------------------------------------------------------------------ main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="W1,W2,W3,W4,W5")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    _logf = open(str(OUT_DIR / ("_t_input_full_%s.txt" % TS)), "w", encoding="utf-8")
    log("键鼠域 C API 验证  %s" % TS)
    log("产物目录 %s" % WORK)
    WORK.mkdir(parents=True, exist_ok=True)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("op.dll = %s" % op.dll_path)

    tgt = NativeTarget()
    try:
        if not bind_native(op, tgt):
            return 1
        if "W1" in groups:
            w1(op, tgt)
        if "W2" in groups:
            w2(op, tgt)
        if "W3" in groups:
            w3(op, tgt)
        if "W4" in groups:
            w4(op, tgt)
        if "W5" in groups:
            w5(op, tgt)
    finally:
        try:
            op.unbind_window()
        except Exception:
            pass
        tgt.close()

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
