# -*- coding: utf-8 -*-
"""用户色串 BGR↔RGB 口径对照探针。

背景（已实测铁证）：
  op 对外颜色 = **RRGGBB（RGB 顺序）**，见 `_t_color_order.py`：
  受控目标白底红字 RED / 蓝字 BLUE —— 传 'ff0000' 只出 RED、'0000ff' 只出 BLUE。
  依据：libop/image/Color.h:38-47 `str2color` = swscanf("%02X%02X%02X", &r,&g,&b)。

  而**用户侧取色器/游戏日志常给 BGR 串（BBGGRR）** → 直接传给 op 会**红蓝错位**，
  典型症状：某个色段"过滤不到任何东西"（恒返空），因为它在被当作另一种颜色匹配。
  对称色（如 33ee33，R==B）无法暴露错位，只有非对称色（如 1111e0）才行。

本探针：同一区域/同一 sim，把用户串按不同口径变体逐一实测 + 放大截图供目视核对，
        判定**该不该做 R/B 换位**，并给出该区域的元素构成。

用法：
  python scripts/probes/_t_color_bgr_rgb.py
  python scripts/probes/_t_color_bgr_rgb.py --rect 1174,1,1356,17 --color "33ee33-000000|1111e0-000000"
  python scripts/probes/_t_color_bgr_rgb.py --scale 6 --sim 0.8 --channel gdi
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
SHOT = OUT / ("bgr_rgb_" + TS)
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
    log("%s %-50s %s" % (mark, api, detail))


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


# ---------------- BMP 读写（标准库，无第三方依赖） ----------------

def read_bmp(path):
    """→ (px, w, h)，px[row][col] = (r,g,b)，已按 top-down 归一。"""
    data = open(path, "rb").read()
    off = int.from_bytes(data[10:14], "little")
    w = int.from_bytes(data[18:22], "little", signed=True)
    h = int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    top_down = h < 0
    h = abs(h)
    step = bpp // 8
    row_bytes = ((w * step + 3) // 4) * 4
    px = []
    for row in range(h):
        src = row if top_down else h - 1 - row
        base = off + src * row_bytes
        line = []
        for col in range(w):
            i = base + col * step
            line.append((data[i + 2], data[i + 1], data[i]))  # BMP 存 BGR → RGB
        px.append(line)
    return px, w, h


def write_bmp24(path, px, scale):
    """最近邻放大后写 24bpp BMP（bottom-up）。"""
    h, w = len(px), len(px[0])
    W, H = w * scale, h * scale
    row_bytes = ((W * 3 + 3) // 4) * 4
    pad = row_bytes - W * 3
    body = bytearray()
    for row in range(H - 1, -1, -1):
        r0 = row // scale
        line = bytearray()
        for col in range(W):
            r, g, b = px[r0][col // scale]
            line += bytes((b, g, r))
        line += b"\x00" * pad
        body += line
    size = 54 + len(body)
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    hdr[2:6] = size.to_bytes(4, "little")
    hdr[10:14] = (54).to_bytes(4, "little")
    hdr[14:18] = (40).to_bytes(4, "little")
    hdr[18:22] = W.to_bytes(4, "little")
    hdr[22:26] = H.to_bytes(4, "little")
    hdr[26:28] = (1).to_bytes(2, "little")
    hdr[28:30] = (24).to_bytes(2, "little")
    hdr[34:38] = len(body).to_bytes(4, "little")
    open(path, "wb").write(bytes(hdr) + bytes(body))


# ---------------- 色串口径变换 ----------------

def swap_rb_seg(seg):
    """单段（可能带 -偏色）做 R/B 换位。"""
    parts = seg.split("-")
    h = parts[0]
    if len(h) == 6:
        h = h[4:6] + h[2:4] + h[0:2]
    return "-".join([h] + parts[1:])


def swap_rb(s):
    """整串逐段 R/B 换位（BGR↔RGB 互转）。"""
    return "|".join(swap_rb_seg(x) for x in s.split("|") if x)


def segs_of(s):
    return [x for x in s.split("|") if x]


def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--rect", default="1174,1,1356,17")
    ap.add_argument("--color", default="33ee33-000000|1111e0-000000")
    ap.add_argument("--sim", type=float, default=0.8)
    ap.add_argument("--channel", default="gdi")
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--scale", type=int, default=6)
    a = ap.parse_args()

    x1, y1, x2, y2 = [int(t) for t in a.rect.replace(" ", "").split(",")]
    OUT.mkdir(parents=True, exist_ok=True)
    SHOT.mkdir(parents=True, exist_ok=True)
    _logf = open(OUT / ("_t_color_bgr_rgb_%s.txt" % TS), "w", encoding="utf-8", buffering=1)

    mode = dpi_aware()
    sec("环境")
    log("DPI=%s  区域=(%d,%d,%d,%d) %dx%d  sim=%s  通道=%s"
        % (mode, x1, y1, x2, y2, x2 - x1, y2 - y1, a.sim, a.channel))
    log("用户原串 = %s" % a.color)
    log("整体 R/B 换位 = %s" % swap_rb(a.color))

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
    rec("区域在客户区内", "(%d,%d,%d,%d) ⊂ %dx%d" % (x1, y1, x2, y2, cw, ch),
        "PASS" if (0 <= x1 and 0 <= y1 and x2 <= cw and y2 <= ch) else "FAIL")
    r = op.bind_window(hwnd, a.channel, "windows", "windows", 0)
    rec("bind_window(%s)" % a.channel, "ret=%s" % r, "PASS" if r else "FAIL")
    if not r:
        return
    op.set_window_state(hwnd, 1)
    time.sleep(0.6)

    bmp = str(SHOT / "rect.bmp")
    png = str(SHOT / "rect.png")
    big_bmp = str(SHOT / ("rect_x%d.bmp" % a.scale))
    big_png = str(SHOT / ("rect_x%d.png" % a.scale))
    try:
        op.capture(x1, y1, x2, y2, bmp)
        if os.path.exists(bmp):
            subprocess.run([sys.executable, str(CONV), bmp, png], capture_output=True)
            px, w, h = read_bmp(bmp)
            write_bmp24(big_bmp, px, a.scale)
            subprocess.run([sys.executable, str(CONV), big_bmp, big_png], capture_output=True)
    except Exception as e:
        rec("区域截图", "EXC %r" % (e,), "FAIL")
    rec("截图落盘（原图）", "→ %s" % png, "PASS" if os.path.exists(png) else "FAIL")
    rec("截图落盘（%dx 放大，供目视）" % a.scale, "→ %s" % big_png,
        "PASS" if os.path.exists(big_png) else "FAIL")

    # ---- 像素构成：区域里到底有哪些主色（按出现频次） ----
    sec("区域像素构成 Top-12（按 RGB 频次；用于判断\"该色到底存不存在\"）")
    if os.path.exists(bmp):
        from collections import Counter
        px, w, h = read_bmp(bmp)
        c = Counter(p for line in px for p in line)
        for rgb, n in c.most_common(12):
            log("  #%02X%02X%02X  %6d px  (%.1f%%)" % (rgb[0], rgb[1], rgb[2], n, 100.0 * n / (w * h)))

    # ---- 口径对照矩阵：同区域同 sim，只改 color ----
    sec("口径对照矩阵（只改 color）")
    user = a.color
    segs = segs_of(user)
    cases = [("不过滤（空串 → 自动反白路径）", "")]
    for i, sg in enumerate(segs, 1):
        head = sg.split("-")[0]
        cases.append(("仅第%d色·原样 %s" % (i, head), head))
        cases.append(("仅第%d色·R/B换位 %s" % (i, swap_rb_seg(sg)), swap_rb_seg(sg)))
    cases.append(("★用户原串", user))
    cases.append(("★用户原串·整体R/B换位", swap_rb(user)))

    rows = []
    for nm, col in cases:
        star = nm.startswith("★")
        try:
            ln = op.autoocr_line(x1, y1, x2, y2, col, a.sim)
        except Exception as e:
            ln = "EXC %r" % (e,)
        try:
            ex = op.autoocr_ex(x1, y1, x2, y2, col, a.sim)
            exs = "count=%s items=%r" % (ex[1], ex[0][:70]) if isinstance(ex, tuple) else repr(ex)
        except Exception as e:
            exs = "EXC %r" % (e,)
        rows.append((nm, col, ln, exs))
        rec("autoocr_line | %-30s" % nm[:30], repr(ln)[:60],
            "PASS" if ln else ("PASS" if star and ln else "INFO"))
        log("      ↳ autoocr_ex | %s" % exs[:110])

    # ---- 结论 ----
    sec("判定")
    base = next((r_[2] for r_ in rows if r_[0] == "★用户原串"), "")
    swapped = next((r_[2] for r_ in rows if r_[0] == "★用户原串·整体R/B换位"), "")
    log("  用户原串        → %r" % base)
    log("  用户原串·整体换位 → %r" % swapped)
    log("  不过滤基准      → %r" % (rows[0][2],))

    # 单色段的可命中性 —— 只有「原样空、换位非空」才证明用户串是 BGR
    bgr_evidence = []
    for i, sg in enumerate(segs, 1):
        head = sg.split("-")[0]
        sw = swap_rb_seg(sg)
        orig_on = next((r_[2] for r_ in rows if r_[0] == "仅第%d色·原样 %s" % (i, head)), "")
        swap_on = next((r_[2] for r_ in rows if r_[0] == "仅第%d色·R/B换位 %s" % (i, sw)), "")
        if orig_on == "" and swap_on != "":
            bgr_evidence.append("第%d色 %s：原样空、换位(%s)出 %r" % (i, head, sw, swap_on))
        elif orig_on != "" and swap_on == "":
            bgr_evidence.append("第%d色 %s：原样出 %r、换位(%s)空 → 原样已正确" % (i, head, orig_on, sw))

    if bgr_evidence:
        for e in bgr_evidence:
            log("  证据：%s" % e)
    if any("原样空、换位" in e for e in bgr_evidence):
        rec("口径判定", "**用户串含 BGR 段，传入 op 前需 R/B 换位**", "FAIL")
    elif any("原样已正确" in e for e in bgr_evidence):
        rec("口径判定", "用户串与 op 同为 RGB，**无需换位**", "PASS")
    else:
        rec("口径判定", "两序均无效或均有效 → 该色段在本区域不存在/对称色，无区分度", "INFO")

    try:
        op.unbind_window()
        op.close()
    except Exception:
        pass

    sec("汇总")
    n = {k: sum(1 for x in RES if x[2] == k) for k in ("PASS", "FAIL", "INFO")}
    log("PASS=%d FAIL=%d INFO=%d   截图=%s" % (n["PASS"], n["FAIL"], n["INFO"], SHOT))
    log("日志: %s" % (OUT / ("_t_color_bgr_rgb_%s.txt" % TS)))
    sys.stdout.flush()
    if _logf:
        _logf.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
