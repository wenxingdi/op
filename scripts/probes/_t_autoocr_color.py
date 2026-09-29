# -*- coding: utf-8 -*-
"""免字库 OCR（autoocr_line / autoocr_ex）**颜色过滤必要性**对照探针。

背景（2026-09-29 真机实测修正）：
  免字库 OCR 的 color 是**精度工具，不是必需输入**：
    · 空串  → str2colordfs 返回 1 → bgr2binarybk 的空 vector 分支
             → 「灰度 + 自动取背景色」反白 → **仍能识别**（可能混入全角符/杂字）
    · 对色  → 按色域二值化（命中该色的像素成前景）→ 去噪提纯，结果最干净
    · 错色  → 区域内不存在该色 → 前景为空 → 返回空串
             （表现为"识别不了"，实为**输入口径错**，不是 OCR 路径不通）

  另一个坑：op 对外颜色 = **RRGGBB（RGB 顺序）**（见 Color.h:38-47 str2color），
    而用户侧取色器/游戏日志常给 **BGR 串**（BBGGRR）→ 非对称色会红蓝错位、静默失效。
    铁证与对照见：`_t_color_order.py`（字节序决定性判定）、`_t_color_bgr_rgb.py`（用户串口径对照）。

用法：
  python scripts/probes/_t_autoocr_color.py
  python scripts/probes/_t_autoocr_color.py --rect 1174,1,1356,17 --color "33ee33-000000|1111e0-000000"
  python scripts/probes/_t_autoocr_color.py --sim 0.9 --channel gdi --pid 23692

颜色串语法（见 docs/2026-08/FINDCOLOR_DEEPDIVE.md 第二节）：
  "RRGGBB"                   单色
  "RRGGBB|RRGGBB"            多色（或）
  "RRGGBB-dfRRGGBB"          带显式偏色；df 全 0（如 -000000）→ 回落用 sim 推容差（大漠惯例写法）
  ""                         空串 → colors 为空 → 永远不匹配（返回 0，不报错）
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
OUT = REPO / "workbench" / "probes"
CONV = REPO / "scripts" / "probes" / "_bmp2png.py"
sys.path.insert(0, str(REPO / "bindings" / "python"))

from op import Op  # noqa: E402

TS = time.strftime("%Y%m%d_%H%M%S")
SHOT = OUT / ("autoocr_color_" + TS)
RES = []
_logf = None


def log(m=""):
    print(m)
    if _logf:
        _logf.write(str(m) + "\n")
        _logf.flush()


def sec(name):
    log()
    log("=" * 96)
    log(name)
    log("=" * 96)


def rec(api, detail, st):
    RES.append((api, detail, st))
    mark = {"PASS": "  [PASS]", "FAIL": "[FAIL] ", "INFO": "  [INFO]"}.get(st, "  [?]")
    log("%s %-52s %s" % (mark, api, detail))


def brief(v):
    if isinstance(v, tuple) and len(v) == 2 and isinstance(v[0], str):
        return "count=%s items=%r" % (v[1], v[0][:70])
    return repr(v)[:76]


def dpi_aware():
    u = ctypes.windll.user32
    try:
        u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return "PER_MONITOR_AWARE_V2"
    except Exception:
        u.SetProcessDPIAware()
        return "SYSTEM_AWARE"


def find_pid(name=b"client.exe"):
    k32 = ctypes.windll.kernel32

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                    ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                    ("szExeFile", ctypes.c_char * 260)]

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


def enum_windows(pid):
    u = ctypes.windll.user32
    out = []
    u.IsWindowVisible.argtypes = [ctypes.c_void_p]
    u.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    u.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    u.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    u.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.DWORD)]

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(h, _l):
        h = int(h)
        if not u.IsWindowVisible(h):
            return True
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid:
            return True
        n = u.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(h, buf, n + 1)
        cls = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(h, cls, 256)
        out.append((h, buf.value, cls.value))
        return True

    u.EnumWindows(cb, None)
    return out


def call(op, api, fn, ok=None, soft=False):
    """跑一个 API 并记录。ok(v) 为真 → PASS，否则 INFO(soft) / FAIL。"""
    try:
        v = fn()
    except Exception as e:
        rec(api, "EXC %r" % (e,), "FAIL")
        return None
    try:
        good = bool(ok(v)) if ok else (v not in (None, "", 0, False))
    except Exception:
        good = False
    rec(api, brief(v), "PASS" if good else ("INFO" if soft else "FAIL"))
    return v


def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--rect", default="1174,1,1356,17", help="识别区域 x1,y1,x2,y2")
    ap.add_argument("--color", default="33ee33-000000|1111e0-000000", help="颜色串")
    ap.add_argument("--sim", type=float, default=0.8)
    ap.add_argument("--channel", default="gdi")
    ap.add_argument("--pid", type=int, default=0)
    a = ap.parse_args()

    x1, y1, x2, y2 = [int(t) for t in a.rect.replace(" ", "").split(",")]
    OUT.mkdir(parents=True, exist_ok=True)
    SHOT.mkdir(parents=True, exist_ok=True)
    _logf = open(OUT / ("_t_autoocr_color_%s.txt" % TS), "w", encoding="utf-8", buffering=1)

    mode = dpi_aware()
    sec("环境")
    log("DPI=%s   区域=(%d,%d,%d,%d) %dx%d   颜色=%r   sim=%s   通道=%s"
        % (mode, x1, y1, x2, y2, x2 - x1, y2 - y1, a.color, a.sim, a.channel))

    pid = a.pid or (find_pid() or [0])[0]
    rec("定位 client.exe", "pid=%d" % pid, "PASS" if pid else "FAIL")
    if not pid:
        return
    wins = enum_windows(pid)
    log("  可见窗口: %s" % [(hex(h), t[:34], c) for h, t, c in wins])
    hwnd = next((h for h, t, c in wins if "蜀门" in t), None) or (wins[0][0] if wins else None)
    rec("定位游戏窗口", "hwnd=%s" % hex(hwnd or 0), "PASS" if hwnd else "FAIL")
    if not hwnd:
        return

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    cw, ch = op.get_client_size(hwnd)
    log("  客户区=%dx%d   区域在界内=%s" % (cw, ch, "OK" if (0 <= x1 and 0 <= y1 and x2 <= cw and y2 <= ch) else "⚠ 越界"))
    rec("区域在客户区内", "(%d,%d,%d,%d) ⊂ %dx%d" % (x1, y1, x2, y2, cw, ch),
        "PASS" if (0 <= x1 and 0 <= y1 and x2 <= cw and y2 <= ch) else "FAIL")

    r = op.bind_window(hwnd, a.channel, "windows", "windows", 0)
    rec("bind_window(%s)" % a.channel, "ret=%s" % r, "PASS" if r else "FAIL")
    if not r:
        return
    op.set_window_state(hwnd, 1)  # 前台化，避免 normal 通道截到遮挡者
    time.sleep(0.6)

    bmp = str(SHOT / "rect.bmp")
    png = str(SHOT / "rect.png")
    try:
        op.capture(x1, y1, x2, y2, bmp)
        if os.path.exists(bmp):
            subprocess.run([sys.executable, str(CONV), bmp, png], capture_output=True)
    except Exception as e:
        rec("区域截图", "EXC %r" % (e,), "FAIL")
    rec("区域截图落盘", "→ %s" % png, "PASS" if os.path.exists(png) else "FAIL")

    # ---- ① 单变量对照：只改 color ----
    # 语义（代码 + 实测）：
    #   空串 → str2colordfs 返回 1 → bgr2binarybk 空 vector 分支 → 自动背景色反白（能识别，稍杂）
    #   有色 → str2colordfs 返回 0 → bgr2binary 按色域二值化
    #   色域内无该色（近黑/纯白打在绿字上）→ 前景为空 → 空串
    sec("① 免字库 OCR —— 单变量对照（同一区域/同一 sim，只改 color）")
    cases = [
        ("空串 \"\"（自动反白路径）", ""),
        ("近黑 000000-303030（上轮口径）", "000000-303030"),
        ("纯白 FFFFFF", "FFFFFF"),
        ("★用户色 %s" % a.color, a.color),
    ]
    for nm, col in cases:
        star = nm.startswith("★")
        call(op, "autoocr_line | %s" % nm,
             lambda c=col: op.autoocr_line(x1, y1, x2, y2, c, a.sim),
             ok=(lambda v: bool(v)) if star else None, soft=not star)
        call(op, "autoocr_ex   | %s" % nm,
             lambda c=col: op.autoocr_ex(x1, y1, x2, y2, c, a.sim),
             ok=(lambda v: v[1] >= 1) if star else None, soft=not star)

    # ---- ② 其它免字库入口 + 交叉一致性 ----
    sec("② 其它入口对照（color=★用户色）")
    # 注：C API 的 AutoOcr（无后缀，整图直 rec）**未在 Python 绑定暴露**（只有 autoocr_line/autoocr_ex）。
    la = call(op, "autoocr_line（免字库，用户色）",
              lambda: op.autoocr_line(x1, y1, x2, y2, a.color, a.sim), soft=True)
    ca = call(op, "ocr_auto（**无 color 参数**，自动取区域主色）",
              lambda: op.ocr_auto(x1, y1, x2, y2, a.sim), soft=True)
    if la or ca:
        rec("  ↳ 免字库 vs 自动主色 一致性",
            "%r vs %r -> %s" % (la, ca, "一致" if la == ca else "**不一致**（免字库可能误识单字）"),
            "PASS" if la == ca else "INFO")
    call(op, "ocr（字库路径，需要字库）",
         lambda: op.ocr(x1, y1, x2, y2, a.color, a.sim), soft=True)
    if os.path.exists(bmp):
        call(op, "ocr_from_file（同图不同入口，应与 autoocr_line 一致）",
             lambda: op.ocr_from_file(bmp, a.color, a.sim), soft=True)
        call(op, "autoocr_ex_from_file" if hasattr(op, "autoocr_ex_from_file") else "ocr_auto_from_file",
             lambda: op.ocr_auto_from_file(bmp, a.sim), soft=True)

    # ---- ③ sim 扫描（找最优相似度） ----
    sec("③ sim 扫描（color=★用户色）—— 找最优 sim")
    for s in (0.7, 0.8, 0.85, 0.9, 0.95, 1.0):
        call(op, "autoocr_line sim=%.2f" % s,
             lambda ss=s: op.autoocr_line(x1, y1, x2, y2, a.color, ss), soft=True)

    # ---- ④ 颜色必要性 & 字节序矩阵 ----
    # 口径铁证（libop/image/Color.h:38-47）：str2color 用 swscanf("%02X%02X%02X", &r,&g,&b)
    #   → 对外是 RRGGBB（RGB 顺序），内部存 color_t{b,g,r,alpha}（BGRA 内存布局）。
    #   本组做「红蓝互换」变异，定量证明**格式搞错会失效**，并给出各单色的贡献。
    sec("④ 颜色必要性 & 字节序矩阵（同一区域/同一 sim，只改 color）")

    def swap_rb(s):
        """RRGGBB -> BBGGRR（模拟把 RGB 串当 BGR 解读）。多色/带偏色串逐段处理。"""
        out = []
        for seg in s.split("|"):
            parts = seg.split("-")
            h = parts[0]
            if len(h) == 6:
                h = h[4:6] + h[2:4] + h[0:2]
            out.append("-".join([h] + parts[1:]))
        return "|".join(out)

    user, swapped = a.color, swap_rb(a.color)
    log("  口径：op 对外 = RRGGBB（RGB），内部 color_t{b,g,r,alpha}")
    log("  用户原串 = %s" % user)
    log("  红蓝互换 = %s（若 op 真是 BGR 口径，这串才该正确）" % swapped)
    # 修正（2026-09-29 实测）：空串**不是**"恒不匹配"。
    #   str2colordfs 对空串 return 1 → 走 bgr2binarybk(colors)，
    #   其 bk_colors 为空时进「灰度 + 自动取背景色」反白分支 → **仍能识别**
    #   （实测得 '成都［258,-507]'，注意"成都"后混入全角［，精度略差）。
    matrix = [
        ("不传色（空串 → 自动背景色反白，能识别但可能混杂字）", ""),
        ("仅绿 %s" % user.split("|")[0].split("-")[0], user.split("|")[0].split("-")[0]),
        ("仅蓝 %s" % user.split("|")[1].split("-")[0], user.split("|")[1].split("-")[0]),
        ("★用户原串", user),
        ("★用户原串·整体红蓝互换（模拟 BGR 误用）", swapped),
    ]
    base = None
    for nm, col in matrix:
        star = nm.startswith("★")
        v = call(op, "autoocr_line | %-34s" % nm,
                 lambda c=col: op.autoocr_line(x1, y1, x2, y2, c, a.sim),
                 ok=(lambda t: bool(t)) if star else None, soft=not star)
        if nm == "★用户原串":
            base = v
    if base:
        for nm, col in matrix:
            v = op.autoocr_line(x1, y1, x2, y2, col, a.sim)
            rec("  ↳ 与基准比对 %-28s" % nm[:28],
                "%r -> %s" % (v, "一致" if v == base else "**不一致**"),
                "PASS" if v == base else "INFO")

    op.unbind_window()

    sec("汇总")
    n = {k: sum(1 for r_ in RES if r_[2] == k) for k in ("PASS", "FAIL", "INFO")}
    log("PASS=%d FAIL=%d INFO=%d   截图=%s" % (n["PASS"], n["FAIL"], n["INFO"], SHOT))
    log("结论要点（2026-09-29 实测修正）：免字库 OCR 的 color 是**精度工具，不是必需输入**——")
    log("  · 空串 → 自动背景色反白，也能识别（可能混入全角符/杂字）")
    log("  · 给对颜色 → 去噪提纯，结果最干净")
    log("  · 给错颜色（区域内无该色）→ 二值图无前景 → 空串，**看起来像识别不了，实为输入口径错**")
    log("日志: %s" % (OUT / ("_t_autoocr_color_%s.txt" % TS)))
    sys.stdout.flush()
    if _logf:
        _logf.flush()
    os._exit(0 if n["FAIL"] == 0 else 1)


if __name__ == "__main__":
    main()
