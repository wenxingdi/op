# -*- coding: utf-8 -*-
"""颜色字节序（RGB vs BGR）**决定性判定**探针。

问题：op 的 color 参数是 RRGGBB 还是 BBGGRR？
方法：建**受控 Tk 目标**（fg 用标准 #RRGGBB，ground truth 无歧义）——
      画 R 行（纯红 #ff0000）与 B 行（纯蓝 #0000ff），
      分别传「原序」与「互换序」，看哪个命中哪一行。
      → 不依赖游戏画面内容，一次定死，且双向可证伪。

代码侧旁证：libop/image/Color.h:38-47
      color_t::str2color 用 swscanf("%02X%02X%02X", &r, &g, &b)：
        前两位 → r、中两位 → g、后两位 → b
      → 对外口径 = RRGGBB（RGB 顺序）；内部存 color_t{b,g,r,alpha}（BGRA 布局）。
      本探针的作用是**实测验证**这个旁证，而不是只信注释。

用法：
  python scripts/probes/_t_color_order.py
  python scripts/probes/_t_color_order.py --sim 0.9 --channel gdi
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
SHOT = OUT / ("color_order_" + TS)
TITLE = "OP_COLOR_ORDER_TARGET"
RED_RGB = (255, 0, 0)
BLUE_RGB = (0, 0, 255)
TEXT_RED = "RED"
TEXT_BLUE = "BLUE"
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
    log("%s %-56s %s" % (mark, api, detail))


def dpi_aware():
    """必须在任何窗口 API（含 Tk）之前调用，否则拿到虚拟化逻辑坐标。"""
    u = ctypes.windll.user32
    try:
        u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return "PER_MONITOR_AWARE_V2"
    except Exception:
        u.SetProcessDPIAware()
        return "SYSTEM_AWARE"


def find_own_window(title):
    """枚举本进程可见顶层窗口，返回标题匹配的 HWND（Tk 顶层可直接被 EnumWindows 看到）。"""
    u = ctypes.windll.user32
    me = os.getpid()
    hit = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(h, _l):
        h = int(h)
        if not u.IsWindowVisible(h):
            return True
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != me:
            return True
        n = u.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(h, buf, n + 1)
        if buf.value == title:
            hit.append(h)
        return True

    u.EnumWindows(cb, None)
    return hit[0] if hit else None


def read_bmp_color_counts(path, targets):
    """统计 BMP 中目标 RGB 的像素数。BMP 像素按 BGR 存；兼容 24/32bpp、上下行序。"""
    data = open(path, "rb").read()
    off = int.from_bytes(data[10:14], "little")
    w = int.from_bytes(data[18:22], "little", signed=True)
    h = int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    h = abs(h)
    step = bpp // 8
    row_bytes = ((w * step + 3) // 4) * 4
    counts = {t: 0 for t in targets}
    for row in range(h):
        base = off + row * row_bytes
        for col in range(w):
            i = base + col * step
            b, g, r = data[i], data[i + 1], data[i + 2]
            key = (r, g, b)
            if key in counts:
                counts[key] += 1
    return counts, (w, h, bpp)


def run(op, name, fn, ok=None, soft=False):
    try:
        v = fn()
    except Exception as e:
        rec(name, "EXC %r" % (e,), "FAIL")
        return None
    try:
        good = bool(ok(v)) if ok else (v not in (None, "", 0, False))
    except Exception:
        good = False
    rec(name, repr(v)[:80], "PASS" if good else ("INFO" if soft else "FAIL"))
    return v


def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", type=float, default=0.8)
    ap.add_argument("--channel", default="gdi")
    a = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    SHOT.mkdir(parents=True, exist_ok=True)
    _logf = open(OUT / ("_t_color_order_%s.txt" % TS), "w", encoding="utf-8", buffering=1)

    mode = dpi_aware()
    sec("环境")
    log("DPI=%s   通道=%s   sim=%s" % (mode, a.channel, a.sim))
    log("受控目标：白底两行 —— %r 纯红 #ff0000 / %r 纯蓝 #0000ff（Tk fg = 标准 RGB，ground truth）"
        % (TEXT_RED, TEXT_BLUE))

    # ---- 建受控目标（必须在 DPI aware 之后） ----
    import tkinter as tk
    root = tk.Tk()
    root.title(TITLE)
    root.configure(bg="white")
    fnt = ("Arial", 44, "bold")
    tk.Label(root, text=TEXT_RED, fg="#ff0000", bg="white", font=fnt).pack(padx=24, pady=10, expand=True)
    tk.Label(root, text=TEXT_BLUE, fg="#0000ff", bg="white", font=fnt).pack(padx=24, pady=10, expand=True)
    root.geometry("+300+240")          # 只定位，尺寸自适应内容
    root.update_idletasks()
    root.update()
    time.sleep(0.4)

    hwnd = find_own_window(TITLE)
    rec("定位受控窗口", "hwnd=%s" % hex(hwnd or 0), "PASS" if hwnd else "FAIL")
    if not hwnd:
        root.destroy()
        os._exit(1)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    cw, ch = op.get_client_size(hwnd)
    log("  客户区=%dx%d" % (cw, ch))
    r = op.bind_window(hwnd, a.channel, "windows", "windows", 0)
    rec("bind_window(%s)" % a.channel, "ret=%s" % r, "PASS" if r else "FAIL")
    if not r:
        root.destroy()
        os._exit(1)
    op.set_window_state(hwnd, 1)   # 前台化，避免被遮挡
    time.sleep(0.5)

    bmp = str(SHOT / "target.bmp")
    png = str(SHOT / "target.png")
    run(op, "整窗截图", lambda: op.capture(0, 0, cw, ch, bmp), ok=lambda v: os.path.exists(bmp))
    subprocess.run([sys.executable, str(CONV), bmp, png], capture_output=True)
    rec("截图落盘(供目视)", "→ %s" % png, "PASS" if os.path.exists(png) else "FAIL")

    # ---- 像素层确认：截图里确实存在纯红/纯蓝（区分「渲染问题」vs「OCR 问题」） ----
    sec("像素层确认（截图里是否真有纯红/纯蓝像素）")
    cnt, wh = read_bmp_color_counts(bmp, [RED_RGB, BLUE_RGB])
    log("  BMP=%dx%d/%dbpp   纯红#ff0000=%d px   纯蓝#0000ff=%d px"
        % (wh[0], wh[1], wh[2], cnt[RED_RGB], cnt[BLUE_RGB]))
    rec("纯红像素存在（ground truth 成立）", "%d px" % cnt[RED_RGB], "PASS" if cnt[RED_RGB] > 0 else "FAIL")
    rec("纯蓝像素存在（ground truth 成立）", "%d px" % cnt[BLUE_RGB], "PASS" if cnt[BLUE_RGB] > 0 else "FAIL")

    # ---- 决定性判定 ----
    sec("决定性判定：color='ff0000' 命中哪一行？")
    log("  若 op 是 RRGGBB(RGB) → 'ff0000' 是红 → 应只出 %r" % TEXT_RED)
    log("  若 op 是 BBGGRR(BGR) → 'ff0000' 是蓝 → 应只出 %r" % TEXT_BLUE)

    r1 = run(op, "autoocr_line color='ff0000'（原序）",
             lambda: op.autoocr_line(0, 0, cw, ch, "ff0000", a.sim), soft=True)
    r2 = run(op, "autoocr_line color='0000ff'（原序）",
             lambda: op.autoocr_line(0, 0, cw, ch, "0000ff", a.sim), soft=True)
    r3 = run(op, "autoocr_line color='ff0000|0000ff'（双色）",
             lambda: op.autoocr_line(0, 0, cw, ch, "ff0000|0000ff", a.sim), soft=True)
    r4 = run(op, "autoocr_line color=''（不传色，走自动反白）",
             lambda: op.autoocr_line(0, 0, cw, ch, "", a.sim), soft=True)
    r5 = run(op, "autoocr_ex  color='ff0000'（结构化，看 bbox 分布）",
             lambda: op.autoocr_ex(0, 0, cw, ch, "ff0000", a.sim), soft=True)

    sec("判定")
    s1 = (r1 or "")
    s2 = (r2 or "")
    verdict, level = "未判定（结果不足以区分）", "INFO"
    if TEXT_RED in s1 and TEXT_BLUE in s2 and TEXT_BLUE not in s1 and TEXT_RED not in s2:
        verdict = "**RGB 口径（RRGGBB）** —— 'ff0000'→红行、'0000ff'→蓝行，与 Color.h:38-47 一致"
        level = "PASS"
    elif TEXT_BLUE in s1 and TEXT_RED in s2 and TEXT_RED not in s1 and TEXT_BLUE not in s2:
        verdict = "**BGR 口径（BBGGRR）** —— 'ff0000'→蓝行，与代码注释相反，需修正实现"
        level = "FAIL"
    rec("字节序判定", verdict, level)
    log("  'ff0000' → %r" % s1)
    log("  '0000ff' → %r" % s2)
    log("  双色串   → %r" % (r3 or ""))
    log("  不传色   → %r" % (r4 or ""))
    if isinstance(r5, tuple):
        log("  autoocr_ex bbox → %r" % (r5[0][:120],))

    try:
        op.unbind_window()
        op.close()
    except Exception:
        pass
    root.destroy()

    sec("汇总")
    n = {k: sum(1 for x in RES if x[2] == k) for k in ("PASS", "FAIL", "INFO")}
    log("PASS=%d FAIL=%d INFO=%d   截图=%s" % (n["PASS"], n["FAIL"], n["INFO"], SHOT))
    log("字节序结论：%s" % verdict)
    log("日志: %s" % (OUT / ("_t_color_order_%s.txt" % TS)))
    sys.stdout.flush()
    if _logf:
        _logf.flush()
    os._exit(0 if n["FAIL"] == 0 else 1)


if __name__ == "__main__":
    main()
