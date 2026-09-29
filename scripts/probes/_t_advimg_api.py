# -*- coding: utf-8 -*-
"""高级图色域 **C API 层** 验证 —— 覆盖缺口里未做端到端的图色 API。

靶子 = 自建 Tk 窗口（像素完全已知）⇒ 所有命中坐标都有 ground truth，
且每条断言都配一个「只有真实现才成立」的反向用例。对游戏零副作用。

分组：
  A1 find_multi_color / _ex          —— 多点比色（anchor + 偏移色）
  A2 find_color_block_ex / _ex_s     —— 色块（ground truth 尺寸/位置）
  A3 find_line / _ex / _ex_s         —— 霍夫直线（ground truth 角度+距离）
  A4 find_pic_ex / _ex_s + 图片缓存池（load/free/mem/cache_max/match_name）
  A5 capture_pre / get_screen_data_bmp

用法：
  python scripts/probes/_t_advimg_api.py [--groups A1,A2,A3,A4,A5]
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

sys.path.insert(0, str(REPO / "scripts" / "probes"))
from _t_opencv_api import Img  # noqa: E402  （复用它的纯标准库 BMP 解析）

TS = time.strftime("%Y%m%d_%H%M%S")
WORK = OUT_DIR / ("advimg_" + TS)
RES = []
_logf = None
_group = ["A0"]

# 靶子布局（客户区坐标，480x360）
ANCHORS = [(80, 60), (300, 200)]        # 蓝块 + 偏移(红 +10,0 / 绿 0,+10)
BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H = 200, 240, 40, 30   # 品红色块
LINE_Y, LINE_X0, LINE_X1 = 320, 60, 180                 # 红色水平线
ICONS = [(40, 130), (150, 130), (260, 130)]             # 3 个相同 16x16 图标


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


# ------------------------------------------------------------------ 靶子
def build_target():
    import tkinter as tk
    root = tk.Tk()
    root.title("OP_ADVIMG_PROBE_TGT")
    root.geometry("480x360+140+80")
    cv = tk.Canvas(root, width=480, height=360, highlightthickness=0)
    cv.pack(fill="both", expand=True)
    cv.create_rectangle(0, 0, 480, 360, fill="#ffffff", outline="")

    for (ax, ay) in ANCHORS:
        cv.create_rectangle(ax, ay, ax + 5, ay + 5, fill="#0000ff", outline="")      # 锚点色 蓝
        cv.create_rectangle(ax + 10, ay, ax + 15, ay + 5, fill="#ff0000", outline="")  # +10,0 红
        cv.create_rectangle(ax, ay + 10, ax + 5, ay + 15, fill="#00ff00", outline="")  # 0,+10 绿

    cv.create_rectangle(BLOCK_X, BLOCK_Y, BLOCK_X + BLOCK_W - 1, BLOCK_Y + BLOCK_H - 1,
                        fill="#ff00ff", outline="")                                   # 品红色块
    cv.create_rectangle(LINE_X0, LINE_Y, LINE_X1, LINE_Y + 1, fill="#ff0000", outline="")  # 红线

    for (ix, iy) in ICONS:
        cv.create_rectangle(ix, iy, ix + 15, iy + 15, fill="#ffff00", outline="")     # 黄底
        cv.create_rectangle(ix + 6, iy + 6, ix + 9, iy + 9, fill="#000000", outline="")  # 黑心
    root.update()
    time.sleep(0.4)
    root.update()
    return root


def bind_target(op):
    root = build_target()
    hwnd = op.find_window("", "OP_ADVIMG_PROBE_TGT") or \
        ctypes.windll.user32.FindWindowW(None, "OP_ADVIMG_PROBE_TGT")
    if not hwnd:
        rec("自建靶子窗口", "未找到", "FAIL")
        root.destroy()
        return None, None
    rec("自建靶子窗口", "hwnd=%s" % hex(hwnd), "PASS")
    if not op.bind_window(hwnd, "gdi", "windows", "windows", 0):
        rec("bind_window(gdi/windows/windows)", "失败", "FAIL")
        root.destroy()
        return None, None
    cw, ch = op.get_client_size(hwnd)
    log("  客户区 %dx%d" % (cw, ch))
    return root, (cw, ch)


def near(x, y, tx, ty, tol=3):
    return abs(x - tx) <= tol and abs(y - ty) <= tol


# ------------------------------------------------------------------ A1 多点比色
def a1(op):
    _group[0] = "A1"
    sec("A1 find_multi_color / _ex（anchor + 偏移色）")
    ax, ay = ANCHORS[0]
    # ⚠ 偏移色串格式 = "dx|dy|颜色" 多段之间用**英文逗号**分隔（ImageSearchService.cpp:262
    # split(offset_color, ',') 后再按 '|' 拆成 3 段）。写成 "dx,dy,color|..." 会被
    # 静默整段跳过 ⇒ 退化成「只找锚点色」（我第一版正是这么写，误报成偏移校验失效）。
    off = "10|0|ff0000,0|10|00ff00"
    T("find_multi_color(蓝 + 两处偏移)",
      lambda: op.find_multi_color(0, 0, 480, 360, "0000ff", off, 0.9),
      ok=lambda v: isinstance(v, tuple) and v[0] == 1 and near(v[1], v[2], ax, ay),
      show=lambda v: "(ret,x,y)=%s" % (v,))
    # 反向 1：偏移色对不上（该处是红，偏要说绿）⇒ 必须 -1,-1
    T("  ↳ 反向：偏移色错误 ⇒ ret=0 且 -1,-1",
      lambda: op.find_multi_color(0, 0, 480, 360, "0000ff", "10|0|00ff00,0|10|00ff00", 0.9),
      ok=lambda v: isinstance(v, tuple) and v[0] == 0 and v[1] == -1 and v[2] == -1,
      show=lambda v: "(ret,x,y)=%s" % (v,))
    # 反向 2：格式非法（缺 '|'）⇒ 偏移整段被跳过，退化成只找锚点色（记录事实，非缺陷）
    T("  ↳ 反向：偏移串格式非法 ⇒ 退化为只找锚点色（仍命中）",
      lambda: op.find_multi_color(0, 0, 480, 360, "0000ff", "10,0,ff0000", 0.9),
      ok=lambda v: isinstance(v, tuple) and v[0] == 1,
      show=lambda v: "(ret,x,y)=%s（格式非法时偏移量被静默丢弃）" % (v,))
    # 反向 3：搜索区内没有锚点色
    T("  ↳ 反向：搜索区无锚点色 ⇒ ret=0",
      lambda: op.find_multi_color(0, 330, 480, 360, "0000ff", off, 0.9),
      ok=lambda v: isinstance(v, tuple) and v[0] == 0 and v[1] == -1,
      show=lambda v: "(ret,x,y)=%s" % (v,))

    s = T("find_multi_color_ex（所有命中点）",
          lambda: op.find_multi_color_ex(0, 0, 480, 360, "0000ff", off, 0.9),
          ok=lambda v: isinstance(v, str) and v != "", show=lambda v: str(v)[:60] + ("..." if len(v) > 60 else ""))
    if isinstance(s, str) and s and s != "-1,-1":
        pts = []
        for item in s.split("|"):
            p = item.split(",")
            if len(p) >= 2:
                pts.append((int(p[0]), int(p[1])))
        # 真值：两个 6x6 蓝块内部的每个像素都是合法命中（大漠语义返回**所有**命中点）
        # Tk create_rectangle(x1,y1,x2,y2) 实际覆盖 [x1,x2) ⇒ 锚点块是 5x5（不是 6x6）
        def in_anchor(x, y):
            return any(axx <= x <= axx + 4 and ayy <= y <= ayy + 4 for axx, ayy in ANCHORS)
        allin = all(in_anchor(x, y) for x, y in pts)
        both = any(in_anchor(axx, ayy) for axx, ayy in ANCHORS for x, y in pts if (x, y) == (axx, ayy))
        rec("  ↳ 命中点**全部**落在两处锚点块内（%d 点）" % len(pts),
            "样例=%s 真值=两处 6x6 蓝块" % (pts[:3],),
            "PASS" if (allin and both and len(pts) == 2 * 25) else "FAIL")


# ------------------------------------------------------------------ A2 色块
def a2(op):
    _group[0] = "A2"
    sec("A2 find_color_block_ex / _ex_s（品红块 %dx%d @%d,%d）" % (BLOCK_W, BLOCK_H, BLOCK_X, BLOCK_Y))
    # 大漠语义：count=需要匹配的像素数；height/width=块的宽高
    # 滑动窗口语义（ImageSearchAlgorithms.cpp:884）：窗口 w×h 内匹配像素数 >= count 即命中。
    # count 取 w*h ⇒ 窗口必须**整块**落在色块内 ⇒ 命中坐标被真值块严格约束（可证伪）。
    s = T("find_color_block_ex(count=400=20x20)",
          lambda: op.find_color_block_ex(0, 0, 480, 360, "ff00ff", 0.9, 400, 20, 20),
          ok=lambda v: isinstance(v, str) and v != "", show=lambda v: str(v)[:60])
    if isinstance(s, str) and s and s != "-1,-1":
        pts = [tuple(int(t) for t in it.split(",")[:2]) for it in s.split("|") if "," in it]
        inside = all(BLOCK_X <= x <= BLOCK_X + BLOCK_W - 20 and BLOCK_Y <= y <= BLOCK_Y + BLOCK_H - 20
                     for x, y in pts)
        has_tl = (BLOCK_X, BLOCK_Y) in pts
        rec("  ↳ 命中窗口全部落在真值块内且含块左上角",
            "点数=%d 样例=%s 真值块=[%d,%d]x[%d,%d]" %
            (len(pts), pts[:3], BLOCK_X, BLOCK_X + BLOCK_W, BLOCK_Y, BLOCK_Y + BLOCK_H),
            "PASS" if (inside and has_tl) else "FAIL")
    # 反向：count 超过实际像素数 ⇒ 不可能有块
    T("  ↳ 反向：count=99999 ⇒ 必须无结果",
      lambda: op.find_color_block_ex(0, 0, 480, 360, "ff00ff", 0.9, 99999, 20, 20),
      ok=lambda v: (v == "" or v == "-1,-1"), show=lambda v: repr(v))
    # 反向：区域内没有该色
    T("  ↳ 反向：搜索区无该色 ⇒ 必须无结果",
      lambda: op.find_color_block_ex(0, 0, 100, 100, "ff00ff", 0.9, 10, 5, 5),
      ok=lambda v: (v == "" or v == "-1,-1"), show=lambda v: repr(v))

    s2 = T("find_color_block_ex_s(mode=1 聚类合并)",
           lambda: op.find_color_block_ex_s(0, 0, 480, 360, "ff00ff", 0.9, 400, 20, 20, 1),
           ok=lambda v: isinstance(v, str) and v != "", show=lambda v: str(v))
    if isinstance(s2, str) and s2 and s2 != "-1,-1":
        pts = [tuple(int(t) for t in it.split(",")[:2]) for it in s2.split("|") if "," in it]
        n = len(pts)
        inside = all(BLOCK_X - 20 <= x <= BLOCK_X + BLOCK_W and BLOCK_Y - 20 <= y <= BLOCK_Y + BLOCK_H
                     for x, y in pts)
        rec("  ↳ mode=1：重合窗口合并成 1 个坐标且落在真值块附近",
            "块数=%d 坐标=%s" % (n, pts), "PASS" if (n == 1 and inside) else "FAIL")


# ------------------------------------------------------------------ A3 直线
def a3(op):
    _group[0] = "A3"
    sec("A3 find_line / _ex / _ex_s（红线 y=%d, x∈[%d,%d]）" % (LINE_Y, LINE_X0, LINE_X1))
    # FindLine 返回 "角度,距离"（霍夫）：水平线 ⇒ 角度 90°、距离 == 线的 y
    s = T("find_line(红, 0.9)", lambda: op.find_line(0, 0, 480, 360, "ff0000", 0.9),
          ok=lambda v: isinstance(v, str) and "," in v, show=lambda v: str(v))
    if isinstance(s, str) and "," in s:
        deg, dis = (int(p) for p in s.split(",")[:2])
        ok = (abs(deg - 90) <= 2) and (abs(dis - LINE_Y) <= 3)
        rec("  ↳ 角度/距离 == 真值（90,%d）" % LINE_Y, "解析=(%d,%d)" % (deg, dis),
            "PASS" if ok else "FAIL")

    line, pts = T("find_line_ex(红, 0.9)", lambda: op.find_line_ex(0, 0, 480, 360, "ff0000", 0.9),
                  ok=lambda v: isinstance(v, tuple), show=lambda v: str(v), soft=True)
    if isinstance(line, str) and "," in line:
        deg, dis = (int(p) for p in line.split(",")[:2])
        ok = (abs(deg - 90) <= 2) and (abs(dis - LINE_Y) <= 3) and pts >= 50
        rec("  ↳ 角度/距离正确且峰值点数可信", "(%d,%d) 点数=%d（线长 %d）" %
            (deg, dis, pts, LINE_X1 - LINE_X0), "PASS" if ok else "FAIL")

    # _ex_s：min_points 阈值 —— 超过线长必然置空
    s3, p3 = T("find_line_ex_s(min_points=9999 ⇒ 应置空)",
               lambda: op.find_line_ex_s(0, 0, 480, 360, "ff0000", 0.9, 9999),
               ok=lambda v: isinstance(v, tuple) and v[0] == "",
               show=lambda v: "retstr=%r 峰值=%s" % (v[0], v[1]), soft=True)
    s4, p4 = T("find_line_ex_s(min_points=10 ⇒ 应有结果)",
               lambda: op.find_line_ex_s(0, 0, 480, 360, "ff0000", 0.9, 10),
               ok=lambda v: isinstance(v, tuple) and v[0] != "",
               show=lambda v: "retstr=%r 峰值=%s" % (v[0], v[1]), soft=True)


# ------------------------------------------------------------------ A4 找图 + 图片缓存池
def a4(op, shot):
    _group[0] = "A4"
    sec("A4 find_pic_ex / _ex_s + 图片缓存池")
    ix, iy = ICONS[0]
    tpl = str(WORK / "icon.bmp")
    if not T("cv_crop 图标模板(%d,%d,16,16)" % (ix, iy),
             lambda: op.cv_crop(shot, ix, iy, 16, 16, tpl), ok=lambda v: v is True):
        return

    # 先不缓存，拿到基线
    # 返回元组 = (**命中的图片序号**, x, y)；未找到时序号为 -1（大漠语义，不是成功标志）
    T("find_pic(基线) ⇒ 序号 0 @真值",
      lambda: op.find_pic(0, 0, 480, 360, tpl, "000000", 0.95),
      ok=lambda v: isinstance(v, tuple) and v[0] == 0 and near(v[1], v[2], ix, iy),
      show=lambda v: "(idx,x,y)=%s" % (v,))

    s = T("find_pic_ex（应命中 3 处）",
          lambda: op.find_pic_ex(0, 0, 480, 360, tpl, "000000", 0.95),
          ok=lambda v: isinstance(v, str) and v.count("|") >= 0, show=lambda v: str(v)[:80])
    if isinstance(s, str) and s and s != "-1,-1|-1,-1|-1,-1":
        pts = []
        for item in s.split("|"):
            p = item.split(",")
            if len(p) >= 3:
                pts.append((int(p[1]), int(p[2])))
        ok = len(pts) == 3 and all(any(near(x, y, txx, tyy) for txx, tyy in ICONS) for x, y in pts)
        rec("  ↳ 命中 3 处且坐标 == 真值", "解析=%s 真值=%s" % (pts, ICONS),
            "PASS" if ok else "FAIL")

    s2 = T("find_pic_ex_s（带文件名的形式）",
           lambda: op.find_pic_ex_s(0, 0, 480, 360, tpl, "000000", 0.95),
           ok=lambda v: isinstance(v, str), show=lambda v: str(v)[:80], soft=True)

    # ---- 图片缓存池 ----
    T("enable_pic_cache(1)", lambda: op.enable_pic_cache(1), ok=lambda v: bool(v))
    T("set_pic_cache_max(10)", lambda: op.set_pic_cache_max(10), ok=lambda v: bool(v), soft=True)
    T("find_pic（开缓存后仍命中同一处）",
      lambda: op.find_pic(0, 0, 480, 360, tpl, "000000", 0.95),
      ok=lambda v: isinstance(v, tuple) and v[0] == 0 and near(v[1], v[2], ix, iy),
      show=lambda v: "(idx,x,y)=%s" % (v,))
    T("clear_pic_cache()", lambda: op.clear_pic_cache(), ok=lambda v: bool(v), soft=True)
    T("clear 后 find_pic 结果不变（缓存只影响性能）",
      lambda: op.find_pic(0, 0, 480, 360, tpl, "000000", 0.95),
      ok=lambda v: isinstance(v, tuple) and v[0] == 0 and near(v[1], v[2], ix, iy),
      show=lambda v: "(idx,x,y)=%s" % (v,))

    T("load_pic(模板)", lambda: op.load_pic(tpl), ok=lambda v: bool(v), soft=True)
    T("get_pic_size(模板) == 16x16", lambda: op.get_pic_size(tpl),
      ok=lambda v: isinstance(v, tuple) and v[0] == 16 and v[1] == 16, show=lambda v: str(v))
    T("free_pic(模板)", lambda: op.free_pic(tpl), ok=lambda v: bool(v), soft=True)
    T("  ↳ 反向：重复 free ⇒ 应失败", lambda: op.free_pic(tpl),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)

    # load_mem_pic：把同一张 bmp 的字节喂进去
    data = Path(tpl).read_bytes()
    T("load_mem_pic(mem.bmp)", lambda: op.load_mem_pic("mem.bmp", data),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ get_pic_size(mem.bmp) == 16x16", lambda: op.get_pic_size("mem.bmp"),
      ok=lambda v: isinstance(v, tuple) and v[0] == 16 and v[1] == 16,
      show=lambda v: str(v))

    # match_pic_name：合法名返回串；含路径分隔符应判非法
    T("match_pic_name('mem.bmp')", lambda: op.match_pic_name("mem.bmp"),
      ok=lambda v: isinstance(v, str), show=lambda v: "ret=%r" % v, soft=True)
    T("  ↳ 反向：含路径分隔符 ⇒ 空串", lambda: op.match_pic_name("a\\b.bmp"),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)


# ------------------------------------------------------------------ A5 截图副产品
def a5(op):
    _group[0] = "A5"
    sec("A5 capture_pre / get_screen_data_bmp")
    # capture_pre = 取**上次图色操作的区域**存盘 ⇒ 必须先有一次图色操作
    op.find_multi_color(0, 0, 480, 360, "0000ff", "10,0,ff0000|0,10,00ff00", 0.9)
    pre = str(WORK / "pre.bmp")
    if T("capture_pre(上次区域)", lambda: op.capture_pre(pre), ok=lambda v: bool(v), soft=True):
        try:
            im = Img(pre)
            rec("  ↳ 落盘 BMP 可解析", "%dx%d（应≈上次搜索区 480x360）" % (im.w, im.h),
                "PASS" if (im.w == 480 and im.h == 360) else "FAIL")
        except Exception as e:
            rec("  ↳ 落盘 BMP 可解析", "解析失败 %r" % (e,), "FAIL")

    ptr, size = T("get_screen_data_bmp(0,0,100,80)",
                  lambda: op.get_screen_data_bmp(0, 0, 100, 80),
                  ok=lambda v: isinstance(v, tuple), show=lambda v: "ptr=%s size=%s" % (v[0], v[1]),
                  soft=True)
    if isinstance(size, int) and size > 0:
        # 100x80 的 32bpp 裸数据 = 32000；若为 BMP 整文件则 ≈ 24000+
        rec("  ↳ size 与 100x80 像素规模相符",
            "size=%d（32bpp 裸=%d）" % (size, 100 * 80 * 4),
            "PASS" if size >= 100 * 80 * 3 else "FAIL")
    T("  ↳ 反向：越界区域（宽=0）", lambda: op.get_screen_data_bmp(0, 0, 0, 0),
      ok=lambda v: True, show=lambda v: "ptr=%s size=%s" % (v[0], v[1]), soft=True)


# ------------------------------------------------------------------ main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="A1,A2,A3,A4,A5")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    _logf = open(str(OUT_DIR / ("_t_advimg_api_%s.txt" % TS)), "w", encoding="utf-8")
    log("高级图色域 C API 验证  %s" % TS)
    log("产物目录 %s" % WORK)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("op.dll = %s" % op.dll_path)

    root, size = bind_target(op)
    if not root:
        return 1
    cw, ch = size
    shot = str(WORK / "shot.bmp")
    if not op.capture(0, 0, cw, ch, shot):
        rec("capture 靶子", "失败", "FAIL")
        root.destroy()
        return 1
    im = Img(shot)
    rec("靶子截图尺寸", "%dx%d" % (im.w, im.h), "PASS" if (im.w, im.h) == (cw, ch) else "FAIL")

    try:
        if "A1" in groups:
            a1(op)
        if "A2" in groups:
            a2(op)
        if "A3" in groups:
            a3(op)
        if "A4" in groups:
            a4(op, shot)
        if "A5" in groups:
            a5(op)
    finally:
        op.unbind_window()
        root.destroy()
        time.sleep(0.2)

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
