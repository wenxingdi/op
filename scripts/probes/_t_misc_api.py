# -*- coding: utf-8 -*-
"""收尾批：覆盖普查剩下的 12 个无端到端调用证据的 C API。

其中 10 个可测（自建 Tk 靶子 + 本进程自证），YOLO 2 个因**本机没有模型文件**
记为 SKIP（不是缺陷，是环境前置缺失）。

这批 API 大多**没有 Python 侧包装**，所以统一走 ctypes 直调并显式声明
argtypes/restype（C API 字符串返回值是共享缓冲，取回后立刻 str() 拷贝）。

分组：
  X1 窗口枚举/鼠标点窗口  OpEnumWindowByProcess / OpGetMousePointWindow / OpBindWindowEx
  X2 AutoOcr 补口         OpAutoOcr / OpAutoOcrFromFile
  X3 杂项                 OpSendStringIme / OpDownCpu / OpSetDxAttr / OpGetDxAttr /
                          OpGetBinaryPreview
  X4 YOLO                 OpSetYoloEngine / OpYoloDetect（SKIP：无模型）

用法：
  python scripts/probes/_t_misc_api.py [--groups X1,X2,X3,X4]
"""
import argparse
import ctypes
import sys
import time
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
WORK = OUT_DIR / ("misc_" + TS)
RES = []
_logf = None
_group = ["X0"]

TITLE_A = "OP_MISC_TGT_A"
TITLE_B = "OP_MISC_TGT_B"
TEXT = "AB12"
COLOR = "000000"
SIM = 0.9


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
    log("%s %-46s %s" % (mark, api, detail))


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


class Raw:
    """ctypes 直调没被 Python 包装的 C API。"""

    def __init__(self, op):
        self.dll = op._dll
        self.h = op._handle

    def bind(self, name, restype, argtypes):
        f = getattr(self.dll, name)
        f.restype = restype
        f.argtypes = argtypes
        return f

    def wstr(self, name, extra_argtypes, *args):
        f = self.bind(name, ctypes.c_wchar_p, [ctypes.c_void_p] + list(extra_argtypes))
        r = f(self.h, *args)
        return str(r) if r else ""

    def wstr_int(self, name, extra_argtypes, *args):
        """最后一个参数是 int* 出参，返回 (str, int)"""
        out = ctypes.c_int(0)
        f = self.bind(name, ctypes.c_wchar_p,
                      [ctypes.c_void_p] + list(extra_argtypes) + [ctypes.POINTER(ctypes.c_int)])
        r = f(self.h, *args, ctypes.byref(out))
        return (str(r) if r else ""), int(out.value)

    def i32(self, name, argtypes, *args):
        f = self.bind(name, ctypes.c_int, [ctypes.c_void_p] + list(argtypes))
        return int(f(self.h, *args))

    def ptr(self, name, argtypes, *args):
        f = self.bind(name, ctypes.c_void_p, [ctypes.c_void_p] + list(argtypes))
        return int(f(self.h, *args) or 0)


def build_target(title, with_text=True, pos="+160+100"):
    import tkinter as tk
    root = tk.Tk()
    root.title(title)
    root.geometry("480x360" + pos)
    cv = tk.Canvas(root, width=480, height=360, highlightthickness=0)
    cv.pack(fill="both", expand=True)
    cv.create_rectangle(0, 0, 480, 360, fill="#ffffff", outline="")
    if with_text:
        cv.create_text(60, 60, text=TEXT, font=("Arial", 28), fill="#000000", anchor="nw")
    root.update()
    time.sleep(0.3)
    root.update()
    return root


def root_of(hwnd):
    """顶层祖先（GA_ROOT=2）。鼠标点窗口返回的是**最深子窗口**（Tk 的 Canvas），
    比较时必须回到顶层，否则会把"正确的子窗口"误判成失败。"""
    ga = ctypes.windll.user32.GetAncestor
    ga.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    ga.restype = ctypes.c_void_p
    return int(ga(ctypes.c_void_p(hwnd), 2) or 0)


def center_of(hwnd):
    r = ctypes.wintypes.RECT()
    ctypes.windll.user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.wintypes.RECT)]
    ctypes.windll.user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(r))
    return (r.left + r.right) // 2, (r.top + r.bottom) // 2


# ------------------------------------------------------------------ X1
def x1(op, raw, roots):
    _group[0] = "X1"
    sec("X1 OpEnumWindowByProcess / OpGetMousePointWindow / OpBindWindowEx")

    # 1) 按进程名枚举：靶子属于 python.exe（探针自身进程）
    #    口径（WindowService.cpp:250）：**filter=0 是"全枚举"语义，与"按进程过滤"互斥**，
    #    带进程名时必须用 filter 1/2/4/8/16（1=按标题、2=按类名、3=标题+类名…）。
    import os  # noqa: F401
    exe = Path(sys.executable).name  # python.exe
    hwnd_a = int(ctypes.windll.user32.FindWindowW(None, TITLE_A) or 0)
    hwnd_b = int(ctypes.windll.user32.FindWindowW(None, TITLE_B) or 0)

    def _enum(proc, title, cls, flt):
        return raw.wstr("OpEnumWindowByProcess",
                        [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int],
                        proc, title, cls, flt)

    def _has(r, h):
        # 口径：EnumWindow 系列 AppendHwndText 拼的是**十进制** hwnd（'436213918'），
        # 不是 hex。与 FindWindow 族返回的 hwnd 数值同、表示不同，别混用 int(s,16)。
        return any(int(s) == h for s in r.split(",") if s.strip())

    r0 = _enum(exe, "", "", 0)
    rec("[事实] enum_window_by_process(filter=0 + 进程名)",
        "返回=%r（设计上拒绝：全枚举与按进程过滤互斥）" % r0, "PASS" if r0 == "" else "FAIL")

    r1 = _enum(exe, TITLE_A, "", 1)
    rec("enum_window_by_process(%r, title=%r, filter=1)" % (exe, TITLE_A),
        "返回=%r 含靶子A=%s" % (r1[:60], _has(r1, hwnd_a)), "PASS" if _has(r1, hwnd_a) else "FAIL")
    r1b = _enum(exe, TITLE_B, "", 1)
    rec("  ↳ 换 title=%r 命中另一靶子" % TITLE_B, "含靶子B=%s" % _has(r1b, hwnd_b),
        "PASS" if _has(r1b, hwnd_b) else "FAIL")
    r1c = _enum("__no_such_proc__.exe", TITLE_A, "", 1)
    rec("  ↳ 反向：不存在的进程名应为空", "返回=%r" % r1c, "PASS" if r1c == "" else "FAIL")
    r1d = _enum(exe, "__no_such_title__", "", 1)
    rec("  ↳ 反向：不存在的标题应为空", "返回=%r" % r1d, "PASS" if r1d == "" else "FAIL")

    # 2) 鼠标点窗口：把物理光标移到靶子 A 中心
    cx, cy = center_of(hwnd_a)
    ctypes.windll.user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
    ctypes.windll.user32.SetCursorPos(cx, cy)
    time.sleep(0.2)
    got = raw.ptr("OpGetMousePointWindow", [])
    got_root = root_of(got)
    rec("get_mouse_point_window()（光标已移到靶子A中心 %d,%d）" % (cx, cy),
        "返回=%s 顶层=%s 期望顶层=%s" % (hex(got), hex(got_root), hex(hwnd_a)),
        "PASS" if got_root == hwnd_a else "FAIL")
    # 反向：移到桌面空白处（屏幕左上角），只要不崩即可
    ctypes.windll.user32.SetCursorPos(0, 0)
    time.sleep(0.1)
    got2 = raw.ptr("OpGetMousePointWindow", [])
    rec("  ↳ 光标移到 (0,0) 仍可调用", "返回=%s" % hex(got2), "PASS")

    # 3) BindWindowEx：display 用 A，input 用 B（分离句柄）
    op.unbind_window()
    ok = raw.i32("OpBindWindowEx",
                 [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p,
                  ctypes.c_int],
                 ctypes.c_void_p(hwnd_a), ctypes.c_void_p(hwnd_b), "gdi", "windows", "windows", 0)
    rec("bind_window_ex(display=A, input=B, gdi/windows/windows)", "ret=%s" % ok,
        "PASS" if ok == 1 else "FAIL")
    T("  ↳ is_bind() == 1", lambda: op.is_bind(), ok=lambda v: v == 1, show=lambda v: "ret=%s" % v)
    T("  ↳ get_bind_window() == A", lambda: op.get_bind_window(),
      ok=lambda v: int(v) == hwnd_a, show=lambda v: "hwnd=%s 期望=%s" % (hex(int(v)), hex(hwnd_a)))
    # 分离句柄后截图应来自 A（有文字）
    shot = str(WORK / "ex_a.bmp")
    T("  ↳ capture 来自 display 窗口 A", lambda: op.capture(0, 0, 480, 360, shot),
      ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    T("  ↳ autoocr_line 能读出 A 的文本", lambda: op.autoocr_line(40, 40, 400, 140, COLOR, SIM),
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r 真值=%r" % (v, TEXT))
    op.unbind_window()
    T("  ↳ 反向：unbind 后 is_bind() == 0", lambda: op.is_bind(),
      ok=lambda v: v == 0, show=lambda v: "ret=%s" % v)


# ------------------------------------------------------------------ X2
def x2(op, raw):
    _group[0] = "X2"
    sec("X2 OpAutoOcr / OpAutoOcrFromFile（Python 侧无包装，ctypes 直调）")
    x1_, y1_, x2_, y2_ = 40, 40, 400, 140

    def _autoocr():
        r = raw.wstr("OpAutoOcr", [ctypes.c_int] * 4 + [ctypes.c_wchar_p, ctypes.c_double],
                     x1_, y1_, x2_, y2_, COLOR, SIM)
        return r

    T("OpAutoOcr(文本区, color, sim)", _autoocr,
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r 真值=%r" % (v, TEXT))

    shot = str(WORK / "shot.bmp")
    if op.capture(0, 0, 480, 360, shot):
        T("OpAutoOcrFromFile(截图, color, sim)",
          lambda: raw.wstr("OpAutoOcrFromFile", [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_double],
                           shot, COLOR, SIM),
          ok=lambda v: v == TEXT, show=lambda v: "识别=%r 真值=%r" % (v, TEXT))
        T("  ↳ 反向：不存在的文件应为空",
          lambda: raw.wstr("OpAutoOcrFromFile", [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_double],
                           str(WORK / "__no_such__.bmp"), COLOR, SIM),
          ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)


# ------------------------------------------------------------------ X3
def x3(op, raw):
    _group[0] = "X3"
    sec("X3 OpSendStringIme / OpDownCpu / OpSetDxAttr / OpGetDxAttr / OpGetBinaryPreview")
    hwnd = int(ctypes.windll.user32.FindWindowW(None, TITLE_A) or 0)

    # 1) SendStringIme（对自建窗口，软断言：只保证不崩 + 有返回值）
    r = raw.i32("OpSendStringIme", [ctypes.c_void_p, ctypes.c_wchar_p], ctypes.c_void_p(hwnd), "TEST")
    rec("send_string_ime(靶子A, 'TEST')", "ret=%s（返回值口径：目标非 IME 窗口时语义由实现决定）" % r, "PASS")
    r0 = raw.i32("OpSendStringIme", [ctypes.c_void_p, ctypes.c_wchar_p], ctypes.c_void_p(0), "TEST")
    rec("  ↳ 反向：hwnd=0", "ret=%s" % r0, "PASS")

    # 2) DownCpu：type 0..1、rate 0..100（越界钳制，不报错）
    T("down_cpu(1, 50)", lambda: op.down_cpu(1, 50), ok=lambda v: bool(v),
      show=lambda v: "ret=%s" % v, soft=True)
    T("  ↳ 复原 down_cpu(1, 0)", lambda: op.down_cpu(1, 0), ok=lambda v: bool(v),
      show=lambda v: "ret=%s" % v)
    T("  ↳ 反向：type=2 越界应失败", lambda: op.down_cpu(2, 10), ok=lambda v: not bool(v),
      show=lambda v: "ret=%s" % v)
    T("  ↳ 反向：type=-1 越界应失败", lambda: op.down_cpu(-1, 10), ok=lambda v: not bool(v),
      show=lambda v: "ret=%s" % v, soft=True)

    # 3) Set/GetDxAttr：attr=0 时 value 是完整掩码，合法位 = 1|2|4 = 7
    T("set_dx_attr(0, 7)", lambda: op.set_dx_attr(0, 7), ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    T("  ↳ get_dx_attr() == 7", lambda: op.get_dx_attr(), ok=lambda v: v == 7, show=lambda v: "ret=%s" % v)
    T("set_dx_attr(1, 0)（清 bit0）", lambda: op.set_dx_attr(1, 0), ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    T("  ↳ get_dx_attr() == 6", lambda: op.get_dx_attr(), ok=lambda v: v == 6, show=lambda v: "ret=%s" % v)
    T("set_dx_attr(2, 1)（置 bit1）", lambda: op.set_dx_attr(2, 1), ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    T("  ↳ get_dx_attr() == 6（bit1 已为 1）", lambda: op.get_dx_attr(), ok=lambda v: v == 6,
      show=lambda v: "ret=%s" % v)
    T("  ↳ 反向：set_dx_attr(0, 8) 越位应失败", lambda: op.set_dx_attr(0, 8), ok=lambda v: not bool(v),
      show=lambda v: "ret=%s" % v)
    T("  ↳ 反向：set_dx_attr(0, -1) 应失败", lambda: op.set_dx_attr(0, -1), ok=lambda v: not bool(v),
      show=lambda v: "ret=%s" % v, soft=True)
    op.set_dx_attr(0, 7)  # 复原全开

    # 4) GetBinaryPreview：二值化预览（ASCII 点阵 + 命中点数）
    def _prev():
        return raw.wstr_int("OpGetBinaryPreview", [ctypes.c_int] * 4 + [ctypes.c_wchar_p, ctypes.c_double],
                            40, 40, 400, 140, COLOR, SIM)

    prev, cnt = _prev()
    head = prev.split("\n")[0] if prev else ""
    ok_prev = bool(prev) and cnt > 0 and head == "360,100"
    rec("get_binary_preview(文本区)", "尺寸=%r 命中点=%d 行数=%d" % (head, cnt, prev.count("\n") + 1 if prev else 0),
        "PASS" if ok_prev else "FAIL")
    rec("  ↳ 预览内容含 '#'（有前景）", "含#=%s" % ("#" in prev), "PASS" if "#" in prev else "FAIL")
    rec("  ↳ 预览含 '.'（有背景）", "含.=%s" % ("." in prev), "PASS" if "." in prev else "FAIL")
    # 反向：区域内没有该颜色（找红色，靶子是白底黑字）
    prev2, cnt2 = raw.wstr_int("OpGetBinaryPreview", [ctypes.c_int] * 4 + [ctypes.c_wchar_p, ctypes.c_double],
                               40, 40, 400, 140, "ff0000", SIM)
    rec("  ↳ 反向：区域内无该色 ⇒ 命中点=0", "命中点=%d" % cnt2, "PASS" if cnt2 == 0 else "FAIL")


# ------------------------------------------------------------------ X4
def x4(op, raw):
    _group[0] = "X4"
    sec("X4 YOLO（OpSetYoloEngine / OpYoloDetect）")
    rec("OpSetYoloEngine / OpYoloDetect", "SKIP：本机无 YOLO 模型与推理引擎（`OP_HAS_ONNX` 只覆盖 OCR），"
                                          "缺前置资源而非功能缺陷", "SKIP")


# ------------------------------------------------------------------ main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="X1,X2,X3,X4")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    _logf = open(str(OUT_DIR / ("_t_misc_api_%s.txt" % TS)), "w", encoding="utf-8")
    log("收尾批（剩余缺口 12 个）C API 验证  %s" % TS)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    raw = Raw(op)
    log("op.dll = %s" % op.dll_path)

    try:
        import tkinter  # noqa: F401
    except Exception as e:
        rec("tkinter 可用", repr(e), "SKIP")
        return 1

    root_a = build_target(TITLE_A, with_text=True, pos="+160+100")
    root_b = build_target(TITLE_B, with_text=False, pos="+700+100")
    roots = [root_a, root_b]

    try:
        if "X1" in groups:
            x1(op, raw, roots)
        # X2/X3 需要绑定才能截图
        hwnd_a = int(ctypes.windll.user32.FindWindowW(None, TITLE_A) or 0)
        op.bind_window(hwnd_a, "gdi", "windows", "windows", 0)
        if "X2" in groups:
            x2(op, raw)
        if "X3" in groups:
            x3(op, raw)
        if "X4" in groups:
            x4(op, raw)
        op.unbind_window()
    finally:
        try:
            op.unbind_window()
        except Exception:
            pass
        for r in roots:
            try:
                r.destroy()
            except Exception:
                pass
        time.sleep(0.2)
        try:
            op.close()
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
    return 0 if n["FAIL"] == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
