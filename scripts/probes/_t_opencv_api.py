# -*- coding: utf-8 -*-
"""OpenCV 域 **C API 层** 验证（`OpCv*` 36 个，Python 绑定的实际调用面）。

为什么单独做这一层：
  `tests/opencv_test.cpp` 有 30 个用例，但实测只有 **3 个** 走 C API
  （`OpCvConnectedComponents` / `OpCvMatchAllTemplates` / `OpCvMatchTemplate`，
   且其中 2 个是 `nullptr` handle 的失败路径），其余 33 个是直接调 C++ 内部
  `op::Op::CvXxx`，**绕过 C API 导出层**。而 C API 正是 Python / OPlug 的入口，
  参数顺序、宽字符转换、返回值构建都在这一层 —— 故必须单独验。

判据原则（避免「返 True 但没干活」的伪实现蒙混）：
  * 输入用**自生成合成图**（像素完全已知）→ 输出可逐像素核对；
  * 每条断言都对应一个只有真实现才具备的特征（灰度图 R==G==B、二值图只含 0/255、
    crop 的内容等于原图对应区域、连通域数 == ground truth 白块数）；
  * 关键项带**反向验证**（不存在的文件/非法枚举/越界区域必须 False）。

分组：
  V1 版本 + 模板库 CRUD（9）      —— 状态机闭环 + 反向
  V2 文件型预处理（17）           —— 像素级 ground-truth 断言
  V3 JSON 返回型（2）             —— 结构 + ground truth 计数 + 共享缓冲
  V4 捕获型匹配（8）              —— 自建 Tk 窗口，自洽闭环（截图裁模板 → 找回去）
  V5 边界与错误路径                —— 反向验证

用法：
  python scripts/probes/_t_opencv_api.py
  python scripts/probes/_t_opencv_api.py --groups V1,V2
无需游戏窗口、无副作用（V4 只绑自建 Tk 窗口）。
"""
import argparse
import ctypes
import hashlib
import json
import os
import struct
import sys
import time
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
OUT_DIR = REPO / "workbench" / "probes"
sys.path.insert(0, str(REPO / "bindings" / "python"))

from op import Op  # noqa: E402
from op import constants  # noqa: E402

TS = time.strftime("%Y%m%d_%H%M%S")
WORK = OUT_DIR / ("opencv_api_" + TS)
RES = []
GRP = ["V0"]
_logf = None


# ------------------------------------------------------------------ 日志/断言
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
    GRP[-1] = status if status in ("FAIL", "SKIP") else GRP[-1]
    RES.append((current_group(), api, detail, status))
    mark = {"PASS": "[ OK ]", "FAIL": "[FAIL]", "INFO": "[INFO]", "SKIP": "[SKIP]"}.get(status, "[ ?? ]")
    log("%s %-52s %s" % (mark, api, detail))


def current_group():
    return _group[0]


_group = ["V0"]


def T(api, fn, ok=lambda v: bool(v), show=lambda v: repr(v)[:80], soft=False):
    """软断言：soft=True 时失败记为 INFO（用于可证伪预测/环境项）。"""
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


# ------------------------------------------------------------------ BMP 读写（纯标准库）
def write_bmp32(path, w, h, put):
    """put(x, y) -> (r, g, b)；写出 32bpp BGRA bottom-up BMP。"""
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b = put(x, y)
            rows += bytes((b, g, r, 255))
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(rows), 0, 0, 14 + 40)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(rows), 2835, 2835, 0, 0)
    Path(path).write_bytes(hdr + info + bytes(rows))


class Img(object):
    """BMP 读取器。**必须支持 8bpp 调色板** —— op 对单通道结果（灰度/二值/掩膜/边缘）
    就是用 8bpp+1024 字节调色板写的（实测 `off=1078 = 14+40+1024`）。"""

    def __init__(self, path):
        b = Path(path).read_bytes()
        if b[:2] != b"BM":
            raise ValueError("not a BMP: %s" % path)
        self.raw = b
        self.off = struct.unpack_from("<I", b, 10)[0]
        self.bih = struct.unpack_from("<I", b, 14)[0]
        w, h = struct.unpack_from("<ii", b, 18)
        self.bpp = struct.unpack_from("<H", b, 28)[0]
        self.comp = struct.unpack_from("<I", b, 30)[0]
        self.top_down = h < 0
        self.w, self.h = w, abs(h)
        self.stride = ((self.w * self.bpp // 8) + 3) // 4 * 4
        self.nch = self.bpp // 8
        self.pal = None
        if self.bpp == 8:
            base = self.off - 256 * 4
            pal = []
            for i in range(256):
                bb, gg, rr = b[base + i * 4], b[base + i * 4 + 1], b[base + i * 4 + 2]
                pal.append((rr, gg, bb))
            self.pal = pal

    def at(self, x, y):
        sy = y if self.top_down else (self.h - 1 - y)
        p = self.off + sy * self.stride
        if self.bpp == 8:
            return self.pal[self.raw[p + x]]
        p += x * self.nch
        return (self.raw[p + 2], self.raw[p + 1], self.raw[p])   # BGRA/BGR -> RGB

    def sha1(self):
        return hashlib.sha1(self.raw).hexdigest()

    def is_gray(self, tol=0):
        # 8bpp：检查**调色板**本身是否灰度（比抽样像素更强 —— 排除「索引映射到彩色」）
        if self.pal is not None:
            return all(abs(r - g) <= tol and abs(g - b) <= tol for r, g, b in self.pal)
        for y in range(0, self.h, 2):
            for x in range(0, self.w, 2):
                r, g, b = self.at(x, y)
                if abs(r - g) > tol or abs(g - b) > tol or abs(r - b) > tol:
                    return False
        return True

    def is_binary(self):
        if self.pal is not None:
            used = set()
            for y in range(0, self.h, 4):
                for x in range(0, self.w, 4):
                    used.add(self.at(x, y))
            return used <= {(0, 0, 0), (255, 255, 255)}
        for y in range(0, self.h, 2):
            for x in range(0, self.w, 2):
                r, g, b = self.at(x, y)
                if (r, g, b) not in ((0, 0, 0), (255, 255, 255)):
                    return False
        return True

    def black_ratio(self):
        n = blk = 0
        for y in range(0, self.h, 2):
            for x in range(0, self.w, 2):
                r, g, b = self.at(x, y)
                n += 1
                if r < 8 and g < 8 and b < 8:
                    blk += 1
        return blk / float(n or 1)

    def region_all(self, x0, y0, x1, y1, color, tol=6):
        """区域 [x0,x1) x [y0,y1) 内像素是否全等于 color（容差 tol）。"""
        inside = 0
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                r, g, b = self.at(x, y)
                if abs(r - color[0]) > tol or abs(g - color[1]) > tol or abs(b - color[2]) > tol:
                    return False, (x, y, (r, g, b))
                inside += 1
        return (inside > 0), None

    def unique_colors(self, cap=None):
        s = set()
        for y in range(0, self.h, 2):
            for x in range(0, self.w, 2):
                s.add(self.at(x, y))
                if cap and len(s) > cap:
                    return len(s)
        return len(s)


# ------------------------------------------------------------------ 合成 ground truth
GW, GH = 480, 360
RED, GREEN, BLUE, WHITE, BLACK = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0)
# 内容包围盒（四周留 40 白边）→ crop_valid 的预期尺寸
BB = (40, 40, 400, 320)


def synth(x, y):
    if 40 <= x < 200 and 40 <= y < 160:
        return RED
    if 240 <= x < 400 and 40 <= y < 160:
        return GREEN
    if 40 <= x < 200 and 200 <= y < 320:
        g = int(round((x - 40) * 255.0 / 159.0))          # 灰阶渐变
        return (g, g, g)
    if 240 <= x < 400 and 200 <= y < 320:
        return BLUE
    return WHITE


def blobs(x, y):
    """黑底 + 3 个分离白方块 —— 连通域/轮廓数的 ground truth。"""
    for cx in (60, 200, 340):
        if cx - 30 <= x < cx + 30 and 150 <= y < 210:
            return WHITE
    return BLACK


def paths():
    WORK.mkdir(parents=True, exist_ok=True)
    d = {
        "src": WORK / "src.bmp",
        "blobs": WORK / "blobs.bmp",
    }
    write_bmp32(d["src"], GW, GH, synth)
    write_bmp32(d["blobs"], GW, GH, blobs)
    return d


# ------------------------------------------------------------------ V1 模板库 + 版本
def v1(op, P):
    _group[0] = "V1"
    sec("V1 版本 + 模板库 CRUD（9 个 C API）")
    tpath = str(REPO / "assets" / "opencv_game_coin_template.png")
    tpath2 = str(REPO / "assets" / "opencv_game_gem_template.png")
    mask = str(WORK / "mask.bmp")
    write_bmp32(mask, 70, 70, lambda x, y: WHITE)

    ver = T("cv_get_open_cv_version", op.cv_get_open_cv_version,
            ok=lambda v: isinstance(v, str) and v.strip() != "" and
            any(c.isdigit() for c in v), show=lambda v: repr(v))
    log("     OpenCV 版本 = %s" % ver)

    op.cv_remove_all_templates()
    T("cv_remove_all_templates（起始清空）", lambda: (op.cv_remove_all_templates(), op.cv_get_template_count())[1],
      ok=lambda v: v == 0, show=lambda v: "count=%s" % v)

    T("cv_load_template(coin)", lambda: op.cv_load_template("coin", tpath), ok=lambda v: v is True)
    T("cv_has_template(coin) == True", lambda: op.cv_has_template("coin"), ok=lambda v: v is True)
    T("  ↳ 反向：cv_has_template(不存在) == False",
      lambda: op.cv_has_template("no_such_tpl"), ok=lambda v: v is False)
    T("cv_get_template_count == 1", op.cv_get_template_count, ok=lambda v: v == 1,
      show=lambda v: "count=%s" % v)
    T("cv_get_all_template_names 含 coin",
      op.cv_get_all_template_names,
      ok=lambda v: isinstance(v, str) and "coin" in v, show=lambda v: repr(v))

    T("cv_load_masked_template(coin_m, tpl, mask)",
      lambda: op.cv_load_masked_template("coin_m", tpath, mask), ok=lambda v: v is True)
    T("cv_get_template_count == 2", op.cv_get_template_count, ok=lambda v: v == 2,
      show=lambda v: "count=%s" % v)

    T("cv_load_template_list('a,<p1>|b,<p2>')",
      lambda: op.cv_load_template_list("a,%s|b,%s" % (tpath, tpath2)), ok=lambda v: v is True)
    T("cv_get_template_count == 4", op.cv_get_template_count, ok=lambda v: v == 4,
      show=lambda v: "count=%s" % v)
    T("  ↳ 反向：cv_load_template_list('坏格式')  必须 False",
      lambda: op.cv_load_template_list("no_comma_here"), ok=lambda v: v is False)
    T("  ↳ 反向：cv_load_template(不存在路径) 必须 False",
      lambda: op.cv_load_template("bad", str(WORK / "nope.png")), ok=lambda v: v is False)

    T("cv_remove_template(a)", lambda: op.cv_remove_template("a"), ok=lambda v: v is True)
    T("  ↳ 移除后 count == 3", op.cv_get_template_count, ok=lambda v: v == 3,
      show=lambda v: "count=%s" % v)
    T("  ↳ 移除后 has(a) == False", lambda: op.cv_has_template("a"), ok=lambda v: v is False)
    T("  ↳ 反向：cv_remove_template(不存在) 必须 False",
      lambda: op.cv_remove_template("no_such_tpl"), ok=lambda v: v is False)

    # 共享缓冲验证：C API 字符串返回是共享缓冲 → 先拷贝再比对
    s1 = op.cv_get_all_template_names()
    s2 = op.cv_get_template_count()
    s3 = op.cv_get_all_template_names()
    T("共享缓冲：跨调用后首次结果是否仍正确",
      lambda: (s1, s3),
      ok=lambda v: "coin_m" in v[0] and "coin_m" in v[1],
      show=lambda v: "第一次=%r 第二次=%r（中间插了一次 count 调用）" % (v[0][:40], v[1][:40]))

    T("cv_remove_all_templates（收尾清空）",
      lambda: (op.cv_remove_all_templates(), op.cv_get_template_count())[1],
      ok=lambda v: v == 0, show=lambda v: "count=%s" % v)


# ------------------------------------------------------------------ V2 文件型预处理
def v2(op, P):
    _group[0] = "V2"
    sec("V2 文件型预处理（17 个 C API）—— ground-truth 合成图（480x360）")
    src = str(P["src"])
    src_sha = Img(P["src"]).sha1()
    out = {}

    def dst(name):
        p = WORK / (name + ".bmp")
        out[name] = p
        return str(p)

    # --- 逐像素强判据组 ---
    p = dst("gray")
    if T("cv_to_gray", lambda: op.cv_to_gray(src, p), ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 尺寸不变", lambda: (im.w, im.h), ok=lambda v: v == (GW, GH), show=lambda v: str(v))
        T("  ↳ 真灰度（全图 R==G==B）", im.is_gray, ok=lambda v: v is True)
        r = im.at(100, 100)      # 红区中心
        g = im.at(300, 100)      # 绿区中心
        T("  ↳ 红/绿区灰度值不同（排除未处理的伪实现）",
          lambda: (r[0], g[0]), ok=lambda v: v[0] != v[1] and 0 < v[0] < 255 and 0 < v[1] < 255,
          show=lambda v: "红→灰%s 绿→灰%s" % (v[0], v[1]))

    p = dst("binary")
    if T("cv_to_binary", lambda: op.cv_to_binary(src, p), ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 真二值（只含 0/255）", im.is_binary, ok=lambda v: v is True)
        T("  ↳ 尺寸不变", lambda: (im.w, im.h), ok=lambda v: v == (GW, GH), show=lambda v: str(v))

    p = dst("crop")
    if T("cv_crop(240,40,160,120) 裁绿区", lambda: op.cv_crop(src, 240, 40, 160, 120, p),
         ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 输出尺寸 == 160x120", lambda: (im.w, im.h), ok=lambda v: v == (160, 120), show=lambda v: str(v))
        okc, bad = im.region_all(0, 0, 160, 120, GREEN, tol=4)
        T("  ↳ 像素级：内容 == 原图绿区（纯 0,255,0）", lambda: (okc, bad),
          ok=lambda v: v[0], show=lambda v: "区域全绿=%s%s" % (okc, "" if okc else " 反例像素=%s" % (bad,)))

    p = dst("resize")
    if T("cv_resize(240,180)", lambda: op.cv_resize(src, 240, 180, p), ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 输出尺寸 == 240x180", lambda: (im.w, im.h), ok=lambda v: v == (240, 180), show=lambda v: str(v))

    p = dst("thresh")
    if T("cv_threshold(128,255,'binary')", lambda: op.cv_threshold(src, p, 128, 255, constants.Threshold.BINARY),
         ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 真二值", im.is_binary, ok=lambda v: v is True)
        T("  ↳ 白底(255) 保白、红区(暗) 变黑", lambda: (im.at(10, 10), im.at(100, 100)),
          ok=lambda v: v[0] == (255, 255, 255) and v[1] == (0, 0, 0),
          show=lambda v: "白底=%s 红区=%s" % (v[0], v[1]))

    p = dst("inrange")
    if T("cv_in_range('bgr', lower='0,0,255', upper='0,0,255')",
         lambda: op.cv_in_range(src, p, constants.ColorSpace.BGR, "0,0,255", "0,0,255"),
         ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 真二值", im.is_binary, ok=lambda v: v is True)
        T("  ↳ BGR 语义：白落在**红**区（B=0,G=0,R=255）而蓝区为黑",
          lambda: (im.at(100, 100), im.at(300, 300)),
          ok=lambda v: v[0] == (255, 255, 255) and v[1] == (0, 0, 0),
          show=lambda v: "红区=%s 蓝区=%s" % (v[0], v[1]))

    p = dst("cropvalid")
    if T("cv_crop_valid（裁掉纯色边框）", lambda: op.cv_crop_valid(src, p), ok=lambda v: v is True):
        im = Img(p)
        exp = (BB[2] - BB[0], BB[3] - BB[1])
        T("  ↳ 输出尺寸 == 内容包围盒 %dx%d" % exp, lambda: (im.w, im.h),
          ok=lambda v: v == exp, show=lambda v: "%s" % (v,), soft=True)

    # --- 边缘/轮廓：黑像素占多数 ---
    p = dst("edge")
    if T("cv_to_edge", lambda: op.cv_to_edge(src, p), ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 边缘图稀疏（黑像素占比 > 0.8）", im.black_ratio,
          ok=lambda v: v > 0.8, show=lambda v: "黑比 %.3f  uniq=%d" % (v, im.unique_colors(cap=999)))

    p = dst("outline")
    if T("cv_to_outline", lambda: op.cv_to_outline(src, p), ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 轮廓图稀疏（黑像素占比 > 0.5）", im.black_ratio,
          ok=lambda v: v > 0.5, show=lambda v: "黑比 %.3f  uniq=%d" % (v, im.unique_colors(cap=999)))

    # --- 保尺寸型：尺寸不变 + 内容确实变了 ---
    keep = [
        ("cv_denoise", lambda p: op.cv_denoise(src, p), None),
        ("cv_equalize", lambda p: op.cv_equalize(src, p), None),
        ("cv_clahe", lambda p: op.cv_clahe(src, p, 2.0, 8), None),
        ("cv_blur(gaussian,k=3)", lambda p: op.cv_blur(src, p, constants.Blur.GAUSSIAN, 3), None),
        ("cv_sharpen(1.0)", lambda p: op.cv_sharpen(src, p, 1.0), None),
        ("cv_morphology(open,k=3,it=1)",
         lambda p: op.cv_morphology(src, p, constants.Morphology.OPEN, 3, 1), None),
    ]
    for name, fn, _ in keep:
        tag = name.split("(")[0][3:]
        p = dst(tag)
        if T(name, lambda f=fn, pp=p: f(pp), ok=lambda v: v is True):
            im = Img(p)
            same = im.sha1() == src_sha
            T("  ↳ 尺寸不变 %dx%d" % (GW, GH), lambda: (im.w, im.h),
              ok=lambda v: v == (GW, GH), show=lambda v: str(v))
            T("  ↳ 内容确实变了（与源图字节不同）", lambda: same,
              ok=lambda v: v is False, show=lambda v: "与源图相同=%s" % v)

    # cv_thin 通常要求二值输入 → 用 binary 输出作输入
    p = dst("thin")
    binp = str(out.get("binary", "")) if out.get("binary") else None
    if binp and Path(binp).exists():
        T("cv_thin(zhang_suen)  [输入=binary 图]",
          lambda: op.cv_thin(binp, p, constants.Thin.ZHANG_SUEN),
          ok=lambda v: v is True, soft=True)
        if Path(p).exists():
            im = Img(p)
            T("  ↳ 真二值 且尺寸不变", lambda: (im.is_binary(), im.w, im.h),
              ok=lambda v: v[0] and (v[1], v[2]) == (GW, GH), show=lambda v: str(v))
    else:
        rec("cv_thin", "前置 binary 图缺失，跳过", "SKIP")

    # --- pipeline：两步串联，验证每步生效 ---
    p = dst("pipe")
    if T("cv_preprocess_pipeline('gray|binary')",
         lambda: op.cv_preprocess_pipeline(src, p, "gray|binary"), ok=lambda v: v is True):
        im = Img(p)
        T("  ↳ 两步都生效 → 真二值", im.is_binary, ok=lambda v: v is True)
    p2 = dst("pipe2")
    T("  ↳ 对照：cv_preprocess_pipeline('gray') 应得灰度非二值",
      lambda: op.cv_preprocess_pipeline(src, p2, "gray"), ok=lambda v: v is True, soft=True)
    if Path(p2).exists():
        im2 = Img(p2)
        T("     ↳ 灰度成立（R==G==B）", im2.is_gray, ok=lambda v: v is True)

    # 反向验证：不存在的源文件
    T("  ↳ 反向：cv_to_gray(不存在文件) 必须 False",
      lambda: op.cv_to_gray(str(WORK / "nope.png"), dst("bad")), ok=lambda v: v is False)
    T("  ↳ 反向：cv_threshold(非法 mode) 必须 False",
      lambda: op.cv_threshold(src, dst("bad2"), 128, 255, "bogus_mode"), ok=lambda v: v is False)


# ------------------------------------------------------------------ V3 JSON 返回型
def v3(op, P):
    _group[0] = "V3"
    sec("V3 JSON 返回型（2 个 C API）—— ground truth：3 个分离白方块")
    bl = str(P["blobs"])

    raw_cc = T("cv_connected_components(blobs, min_area=100)", lambda: op.cv_connected_components(bl, 100.0),
               ok=lambda v: isinstance(v, str) and v.startswith("{"),
               show=lambda v: v[:110])
    if raw_cc:
        try:
            j = json.loads(raw_cc)
            T("  ↳ JSON 可解析且含 ok 字段", lambda: j, ok=lambda v: "ok" in v,
              show=lambda v: "keys=%s" % sorted(v.keys()))
            log("     JSON = %s" % raw_cc[:200])
        except Exception as e:
            rec("  ↳ JSON 解析", "EXC %r  原文=%r" % (e, raw_cc[:120]), "FAIL")

    raw_fc = T("cv_find_contours(blobs, min_area=100)", lambda: op.cv_find_contours(bl, 100.0),
               ok=lambda v: isinstance(v, str) and v.startswith("{"),
               show=lambda v: v[:110])
    if raw_fc:
        try:
            j = json.loads(raw_fc)
            T("  ↳ JSON 可解析且含 ok 字段", lambda: j, ok=lambda v: "ok" in v,
              show=lambda v: "keys=%s" % sorted(v.keys()))
        except Exception as e:
            rec("  ↳ JSON 解析", "EXC %r  原文=%r" % (e, raw_fc[:120]), "FAIL")

    # 共享缓冲：连续两次调用，把首次结果先拷贝再比对
    a = op.cv_connected_components(bl, 100.0)
    b = op.cv_find_contours(bl, 100.0)
    T("共享缓冲：交错调用后首次结果是否仍有效",
      lambda: (a, b),
      ok=lambda v: v[0].startswith("{") and v[1].startswith("{"),
      show=lambda v: "cc=%s... fc=%s..." % (v[0][:32], v[1][:32]))

    T("  ↳ 反向：不存在的文件仍返回合法失败 JSON",
      lambda: op.cv_connected_components(str(WORK / "nope.bmp"), 1.0),
      ok=lambda v: isinstance(v, str) and v.startswith("{"),
      show=lambda v: repr(v)[:80])


# ------------------------------------------------------------------ V4 捕获型匹配
def v4(op):
    _group[0] = "V4"
    sec("V4 捕获型匹配（8 个 C API）—— 自建 Tk 窗口，自洽闭环")
    try:
        import tkinter as tk
    except Exception as e:
        rec("tkinter 可用", repr(e), "SKIP")
        return

    root = tk.Tk()
    root.title("OP_OPENCV_PROBE_TGT")
    root.geometry("480x360+120+60")
    cv = tk.Canvas(root, width=480, height=360, highlightthickness=0)
    cv.pack(fill="both", expand=True)
    # 与合成图同布局（画布坐标即客户区坐标）
    cv.create_rectangle(0, 0, 480, 360, fill="#ffffff", outline="")
    cv.create_rectangle(40, 40, 200, 160, fill="#ff0000", outline="")
    cv.create_rectangle(240, 40, 400, 160, fill="#00ff00", outline="")
    for i in range(160):
        g = int(round(i * 255.0 / 159.0))
        cv.create_line(40 + i, 200, 40 + i, 320, fill="#%02x%02x%02x" % (g, g, g))
    cv.create_rectangle(240, 200, 400, 320, fill="#0000ff", outline="")
    # 全画面**唯一**的高对比特征（黑十字）—— 模板必须裁在它上面。
    # 实测教训 1：裁在「红块右下角（红/白交界）」时，白/绿交界处仍有 score=0.990039 的
    #   近似候选 ⇒ threshold=0.99 下依旧歧义，命中漂到 (235,138)。
    # 实测教训 2：**黑十字必须先铺白底**。十字原本直接画在蓝块(灰度 29)上，
    #   灰度差仅 29 < Canny(60,180) 的高阈值 180 ⇒ 模板边缘图**全零** ⇒
    #   CCOEFF_NORMED 退化成「处处 1.0」、SQDIFF_NORMED 退化成「处处不命中」，
    #   两种 method 都测不出真行为（我一度误判为函数缺陷）。铺白底后
    #   黑(0)↔白(255) 梯度 255 ⇒ Canny 出真实边缘，edge/shape 匹配才可判定。
    cv.create_rectangle(330 - 42, 260 - 42, 330 + 42, 260 + 42, fill="#ffffff", outline="")
    cv.create_line(330 - 24, 260, 330 + 24, 260, fill="#000000", width=9)
    cv.create_line(330, 260 - 24, 330, 260 + 24, fill="#000000", width=9)
    # 第二个唯一特征（黑圆环，落在灰阶渐变区）—— 供 t2 使用，使 any/all 双模板都有唯一解
    cv.create_oval(120 - 26, 260 - 26, 120 + 26, 260 + 26, outline="#000000", width=7)
    root.update()
    time.sleep(0.4)
    root.update()

    hwnd = op.find_window("", "OP_OPENCV_PROBE_TGT") or \
        ctypes.windll.user32.FindWindowW(None, "OP_OPENCV_PROBE_TGT")
    rec("自建靶子窗口", "hwnd=%s" % hex(hwnd or 0), "PASS" if hwnd else "FAIL")
    if not hwnd:
        root.destroy()
        return
    if not op.bind_window(hwnd, "gdi", "windows", "windows", 0):
        rec("bind_window(gdi/windows/windows)", "失败", "FAIL")
        root.destroy()
        return

    cw, ch = op.get_client_size(hwnd)
    log("  客户区（物理像素）= %dx%d" % (cw, ch))

    shot = str(WORK / "v4_shot.bmp")
    if not T("capture(0,0,cw,ch)", lambda: op.capture(0, 0, cw, ch, shot), ok=lambda v: bool(v)):
        op.unbind_window(); root.destroy(); return
    im = Img(shot)
    T("  ↳ 截图尺寸 == 客户区", lambda: (im.w, im.h), ok=lambda v: v == (cw, ch), show=lambda v: str(v))

    # 模板裁自画面：**必须含强特征**（角点/边界）。
    # 若裁在大面积同色区中心（如纯红块内部），任何落在该色块内的位置 score 都是满分
    # ⇒ 多解 ⇒ 实现取首个扫描位置 = 搜索窗左上角，「命中==原点」判据失效。
    # （上一轮 G3 的 find_pic 已踩过同一个坑：模板选在浅色重复区。这里加唯一性自检防再犯。）
    tw, th = max(24, cw // 8), max(24, ch // 8)
    tx = int(cw * 330 / 480.0) - tw // 2      # 黑十字中心 → 全画面唯一特征
    ty = int(ch * 260 / 360.0) - th // 2
    tpl = str(WORK / "v4_tpl.bmp")
    if not T("cv_crop 裁模板(%d,%d,%d,%d)" % (tx, ty, tw, th),
             lambda: op.cv_crop(shot, tx, ty, tw, th, tpl), ok=lambda v: v is True):
        op.unbind_window(); root.destroy(); return
    ti = Img(tpl)
    rec("  ↳ 模板唯一性自检（含 ≥2 色才可能唯一解）",
        "uniq=%d（红色纯块内部=1 会多解）" % ti.unique_colors(cap=64),
        "PASS" if ti.unique_colors(cap=64) >= 2 else "FAIL")
    tpl2 = str(WORK / "v4_tpl2.bmp")
    tx2 = int(cw * 120 / 480.0) - tw // 2     # 黑圆环中心 → 第二个唯一特征
    ty2 = ty
    op.cv_crop(shot, tx2, ty2, tw, th, tpl2)
    log("  t1 原点=(%d,%d)  t2 原点=(%d,%d)" % (tx, ty, tx2, ty2))

    T("cv_load_template(t1/t2)", lambda: (op.cv_load_template("t1", tpl) and
                                          op.cv_load_template("t2", tpl2)),
      ok=lambda v: v is True)

    def mj(api, fn, expect_hit=True, expect_origin=(tx, ty), tol=3, soft=False):
        # expect_origin 可传多点候选（多模板 any/all 命中任一即可）
        roots = tuple(expect_origin) if isinstance(expect_origin[0], (tuple, list)) else (expect_origin,)

        def _near(x, y):
            return any(abs(x - ox) <= tol and abs(y - oy) <= tol for ox, oy in roots)

        raw = T(api, fn, ok=lambda v: isinstance(v, str) and v.startswith("{"),
                show=lambda v: v[:110], soft=soft)
        if not raw or not raw.startswith("{"):
            return None
        try:
            j = json.loads(raw)
        except Exception as e:
            rec("  ↳ " + api + " JSON", "解析失败 %r" % (e,), "FAIL")
            return None
        if expect_hit:
            # any/all 形式返回 {"ok":1,"results":[...]}；单结果形式返回 {"ok":1,"x":..,"y":..}
            if "results" in j:
                res = j.get("results") or []
                if not res:
                    rec("  ↳ 结果数组非空", "ok=%s results=[] ⇒ 未命中" % j.get("ok"), "FAIL")
                    return j
                it = res[0]
                good = _near(it.get("x", -9999), it.get("y", -9999))
                rec("  ↳ 首项坐标 ∈ 候选原点 %s" % (roots,),
                    "results=%d 首项 name=%s x=%s y=%s score=%s" %
                    (len(res), it.get("name"), it.get("x"), it.get("y"), it.get("score")),
                    "PASS" if good else ("INFO" if soft else "FAIL"))
                return j
            good = j.get("ok") == 1 and _near(j.get("x", -9999), j.get("y", -9999))
            rec("  ↳ 命中且坐标 ∈ 候选原点 %s" % (roots,),
                "ok=%s name=%s x=%s y=%s w=%s h=%s score=%s" %
                (j.get("ok"), j.get("name"), j.get("x"), j.get("y"), j.get("width"),
                 j.get("height"), j.get("score")),
                "PASS" if good else ("INFO" if soft else "FAIL"))
        else:
            rec("  ↳ 期望未命中", "ok=%s" % j.get("ok"),
                "PASS" if j.get("ok") == 0 else "FAIL")
        return j

    # 搜索窗从模板左上角外扩一点，避免「只返回窗口左上角」的伪实现蒙对
    sx, sy = max(0, tx - 20), max(0, ty - 20)

    # ---- 阈值语义定性（本轮实测发现，值得单独记录）----
    # 实测：threshold=0.90 时 match_template 命中 (150,199) score=0.9028（**刚过线就返回**），
    # 而 template_scale / _rot / _edge 在 (170,138) score=1.0 精确命中。
    # 与 gtest 用例名 `MatchTemplateReturnsFirstThresholdHit` 一致 ⇒ 语义 = **首个超阈值命中**，
    # 不是「全局最优」。故「命中==原点」只在阈值足够高（仅真位置达标）时成立。
    # 若用户按「找最像」使用，会拿到非最优位置 —— 属 API 语义，需文档明示。
    j90 = mj("cv_match_template(th=0.90)", lambda: op.cv_match_template(sx, sy, cw - sx, ch - sy, "t1", 0.90),
             soft=True)
    j99 = mj("cv_match_template(th=0.99)", lambda: op.cv_match_template(sx, sy, cw - sx, ch - sy, "t1", 0.99))
    if j90 is not None and j99 is not None:
        moved = (j90.get("x"), j90.get("y")) != (j99.get("x"), j99.get("y"))
        rec("  ↳ 阈值语义：提高阈值使命中收敛到真位置",
            "th0.90→(%s,%s) score=%s ｜ th0.99→(%s,%s) score=%s  ⇒ %s" %
            (j90.get("x"), j90.get("y"), j90.get("score"), j99.get("x"), j99.get("y"), j99.get("score"),
             "**首个超阈值命中**（非全局最优）" if moved else "两次一致（该靶子无歧义位置）"),
            "PASS")
    mj("cv_match_template(th=0.999)", lambda: op.cv_match_template(sx, sy, cw - sx, ch - sy, "t1", 0.999))

    mj("cv_match_template_scale(scales='0.8|1.0|1.2')",
       lambda: op.cv_match_template_scale(sx, sy, cw - sx, ch - sy, "t1", "0.8|1.0|1.2", 0.9))
    mj("cv_match_template_rot(angles='0|15|-15')",
       lambda: op.cv_match_template_rot(sx, sy, cw - sx, ch - sy, "t1", "0|15|-15", 0.9))
    mj("cv_match_any_template(th=0.90)", lambda: op.cv_match_any_template(sx, sy, cw - sx, ch - sy, "t1|t2", 0.90),
       expect_origin=((tx, ty), (tx2, ty2)), soft=True)
    mj("cv_match_any_template(th=0.99)", lambda: op.cv_match_any_template(sx, sy, cw - sx, ch - sy, "t1|t2", 0.99),
       expect_origin=((tx, ty), (tx2, ty2)))
    mj("cv_match_all_templates(th=0.90)", lambda: op.cv_match_all_templates(sx, sy, cw - sx, ch - sy, "t1|t2", 0.90),
       expect_origin=((tx, ty), (tx2, ty2)), soft=True)
    mj("cv_match_all_templates(th=0.99)", lambda: op.cv_match_all_templates(sx, sy, cw - sx, ch - sy, "t1|t2", 0.99),
       expect_origin=((tx, ty), (tx2, ty2)))

    # 专用匹配：**算法强度由 `tests/opencv_test.cpp` 覆盖**（EdgeMatchTemplateFindsOutline /
    # ShapeMatchTemplateFindsRectangle / FeatureMatchTemplateFindsPattern），本层的目标是
    # 「C API 参数与返回结构正确」⇒ 命中位置判据记为 INFO，仅硬断言 JSON 结构。
    mj("cv_feature_match_template", lambda: op.cv_feature_match_template(0, 0, cw, ch, "t1", 0.2), soft=True)
    mj("cv_edge_match_template", lambda: op.cv_edge_match_template(0, 0, cw, ch, "t1", 0.5), soft=True)
    # **决定性反向验证**：cv_edge_match_template 在 (0,0) 起点时返回 (0,0) score=1.0 ——
    # 这既可能是「真最优命中」，也可能是「CCOEFF_NORMED 退化 → 恒返回搜索窗左上角」的伪命中。
    # 单变量 A/B：只平移搜索窗左上角（模板与画面都不变）。
    #   · 命中点跟着平移到窗左上 ⇒ **伪命中**（边缘图退化，与模板无关）
    #   · 命中点仍在 (300,238)      ⇒ 真匹配（(0,0) 只是恰好最优）
    ox, oy = 120, 90
    je = mj("cv_edge_match_template(搜索窗平移到 %d,%d)" % (ox, oy),
            lambda: op.cv_edge_match_template(ox, oy, cw - ox, ch - oy, "t1", 0.5), soft=True)
    if je:
        ex_, ey_ = je.get("x"), je.get("y")
        pseudo = (ex_ == ox and ey_ == oy)
        rec("  ↳ 边缘匹配坐标语义判定",
            "窗左上=(%d,%d) 命中=(%s,%s) score=%s ⇒ %s" %
            (ox, oy, ex_, ey_, je.get("score"),
             "**伪命中**：命中点恒=搜索窗左上角（边缘图退化，与模板无关）" if pseudo
             else "真匹配：命中点不随搜索窗平移"),
            "INFO")
    for th in (0.3, 0.5, 0.8):
        mj("cv_shape_match_template(th=%.1f)" % th,
           lambda t=th: op.cv_shape_match_template(0, 0, cw, ch, "t1", t), soft=True)

    # 反向：把匹配阈值设成不可能命中（模板来自画面但已被删）
    op.cv_remove_all_templates()
    T("  ↳ 反向：模板已删 → 命中必须失败",
      lambda: op.cv_match_template(sx, sy, cw - sx, ch - sy, "t1", 0.9),
      ok=lambda v: isinstance(v, str) and json.loads(v).get("ok") == 0,
      show=lambda v: v[:60])

    T("  ↳ 反向：越界搜索窗（宽=0）必须失败",
      lambda: op.cv_match_template(0, 0, 0, 0, "t1", 0.9),
      ok=lambda v: isinstance(v, str) and json.loads(v).get("ok") == 0,
      show=lambda v: v[:60])

    op.unbind_window()
    root.destroy()
    time.sleep(0.2)


# ------------------------------------------------------------------ V5 退化模板安全方向
# 单变量 A/B：靶子画面与搜索窗完全不变，只换「模板是否退化」。
#   · 模板裁自纯红块内部 ⇒ Canny 边缘图**全零**（退化）
#   · 模板裁自黑十字（含 0↔255 梯度）⇒ 边缘图真实（对照）
# 退化模板的正确行为 = **不命中**（安全方向）；
# 若实现用 TM_CCOEFF_NORMED，退化时 raw 恒为 1.0 且 minMaxLoc 取行优先首个
# ⇒ 伪命中：ok=1、score=1.0、命中点 == 搜索窗左上角（换窗就跟着动，与模板内容无关）。
def v5(op):
    _group[0] = "V5"
    sec("V5 退化模板安全方向（cv_edge_match_template 伪命中反向验证）")
    try:
        import tkinter as tk
    except Exception as e:
        rec("tkinter 可用", repr(e), "SKIP")
        return

    root = tk.Tk()
    root.title("OP_OPENCV_PROBE_TGT")
    root.geometry("480x360+120+60")
    cv = tk.Canvas(root, width=480, height=360, highlightthickness=0)
    cv.pack(fill="both", expand=True)
    cv.create_rectangle(0, 0, 480, 360, fill="#ffffff", outline="")
    cv.create_rectangle(40, 40, 200, 160, fill="#ff0000", outline="")
    cv.create_rectangle(240, 40, 400, 160, fill="#00ff00", outline="")
    for i in range(160):
        g = int(round(i * 255.0 / 159.0))
        cv.create_line(40 + i, 200, 40 + i, 320, fill="#%02x%02x%02x" % (g, g, g))
    cv.create_rectangle(240, 200, 400, 320, fill="#0000ff", outline="")
    cv.create_rectangle(330 - 42, 260 - 42, 330 + 42, 260 + 42, fill="#ffffff", outline="")
    cv.create_line(330 - 24, 260, 330 + 24, 260, fill="#000000", width=9)
    cv.create_line(330, 260 - 24, 330, 260 + 24, fill="#000000", width=9)
    root.update()
    time.sleep(0.4)
    root.update()

    hwnd = op.find_window("", "OP_OPENCV_PROBE_TGT") or \
        ctypes.windll.user32.FindWindowW(None, "OP_OPENCV_PROBE_TGT")
    if not hwnd or not op.bind_window(hwnd, "gdi", "windows", "windows", 0):
        rec("绑定自建靶子窗口", "hwnd=%s" % hex(hwnd or 0), "FAIL")
        root.destroy()
        return
    cw, ch = op.get_client_size(hwnd)
    shot = str(WORK / "v5_shot.bmp")
    if not op.capture(0, 0, cw, ch, shot):
        rec("capture", "失败", "FAIL")
        op.unbind_window(); root.destroy(); return

    op.cv_remove_all_templates()

    # ---- 退化模板：纯红块内部（(40,40)-(200,160)），裁 (60,70,60,45) 完全落在块内 ----
    flat = str(WORK / "v5_flat.bmp")
    fx, fy, fw, fh = 60, 70, 60, 45
    if not T("cv_crop 退化模板 纯红块内部(%d,%d,%d,%d)" % (fx, fy, fw, fh),
             lambda: op.cv_crop(shot, fx, fy, fw, fh, flat), ok=lambda v: v is True):
        op.unbind_window(); root.destroy(); return
    fi = Img(flat)
    uq = fi.unique_colors(cap=64)
    rec("  ↳ 退化模板自检（必须 1 色 ⇒ Canny 全零）", "uniq=%d" % uq,
        "PASS" if uq == 1 else "FAIL")
    T("cv_load_template(flat)", lambda: op.cv_load_template("flat", flat), ok=lambda v: v is True)

    def edge_call(ox, oy):
        raw = op.cv_edge_match_template(ox, oy, cw - ox, ch - oy, "flat", 0.5)
        try:
            return json.loads(raw) if isinstance(raw, str) and raw.startswith("{") else None
        except Exception:
            return None

    pseudo_seen = []
    for ox, oy in ((0, 0), (120, 90)):
        j = edge_call(ox, oy)
        if j is None:
            rec("退化模板 edge_match(窗左上=%d,%d)" % (ox, oy), "返回非 JSON", "FAIL")
            continue
        hit = j.get("ok") == 1
        at_origin = (j.get("x") == ox and j.get("y") == oy)
        if hit and at_origin:
            pseudo_seen.append((ox, oy, j.get("score")))
        rec("退化模板 edge_match 必须不命中(窗左上=%d,%d)" % (ox, oy),
            "ok=%s x=%s y=%s score=%s%s" % (j.get("ok"), j.get("x"), j.get("y"), j.get("score"),
                                             "  ⇒ **伪命中：命中点==窗左上**" if (hit and at_origin) else ""),
            "FAIL" if hit else "PASS")

    # ---- 对照：非退化模板（黑十字）在同一搜索窗下必须命中真位置 ----
    tw, th = max(24, cw // 8), max(24, ch // 8)
    tx = int(cw * 330 / 480.0) - tw // 2
    ty = int(ch * 260 / 360.0) - th // 2
    good_tpl = str(WORK / "v5_good.bmp")
    op.cv_crop(shot, tx, ty, tw, th, good_tpl)
    T("cv_load_template(good=黑十字)", lambda: op.cv_load_template("good", good_tpl), ok=lambda v: v is True)
    for ox, oy in ((0, 0), (120, 90)):
        raw = op.cv_edge_match_template(ox, oy, cw - ox, ch - oy, "good", 0.5)
        try:
            j = json.loads(raw)
        except Exception:
            j = None
        if j is None:
            rec("对照 非退化模板 edge_match(窗左上=%d,%d)" % (ox, oy), "返回非 JSON: %r" % raw, "FAIL")
            continue
        near = abs(j.get("x", -9999) - tx) <= 3 and abs(j.get("y", -9999) - ty) <= 3
        rec("对照 非退化模板 必须命中真位置(窗左上=%d,%d)" % (ox, oy),
            "ok=%s x=%s y=%s score=%s（真原点=(%d,%d)）" % (j.get("ok"), j.get("x"), j.get("y"),
                                                       j.get("score"), tx, ty),
            "PASS" if (j.get("ok") == 1 and near) else "FAIL")

    rec("  ↳ 判定",
        ("伪命中复现 %s ⇒ 实现仍在用退化会返回 1.0 的 method" % pseudo_seen) if pseudo_seen
        else "退化模板一律不命中、非退化模板命中真位置 ⇒ **安全方向，判定通过**",
        "FAIL" if pseudo_seen else "PASS")

    op.cv_remove_all_templates()
    op.unbind_window()
    root.destroy()
    time.sleep(0.2)


# ------------------------------------------------------------------ V6 大面积平坦区
# V4 的靶子每一处 60x45 窗口都含边缘（红块/绿块/渐变铺满），**碰巧没有退化窗口**，
# 因此 CCOEFF_NORMED 的伪命中在 V4 上测不出来（我一度据此误判"已修复"）。
# 真实游戏截图恰恰相反：天空/纯色 UI 背景 ⇒ 存在大量「局部方差=0」的窗口。
# 本组靶子：480x360 纯白 + 右下角一个黑十字 ⇒ 左上角存在大量纯白（方差=0）窗口。
#   · 无掩码的 CCOEFF_NORMED：退化窗口分母=0 ⇒ OpenCV 返回 1.0，与真位置并列，
#     minMaxLoc 并列取行优先首个 ⇒ 命中 (0,0) score=1.0（伪命中）
#   · 有掩码：退化位置被排除 ⇒ 命中真位置 (300,238)
def v6(op):
    _group[0] = "V6"
    sec("V6 大面积平坦区（真实截图退化窗口 ⇒ CCOEFF_NORMED 伪命中）")
    try:
        import tkinter as tk
    except Exception as e:
        rec("tkinter 可用", repr(e), "SKIP")
        return

    root = tk.Tk()
    root.title("OP_OPENCV_PROBE_FLAT")
    root.geometry("480x360+120+60")
    cv = tk.Canvas(root, width=480, height=360, highlightthickness=0)
    cv.pack(fill="both", expand=True)
    cv.create_rectangle(0, 0, 480, 360, fill="#ffffff", outline="")
    # 唯一特征：黑十字（黑 0 ↔ 白 255，梯度满量程 ⇒ Canny 出真实边缘）
    cv.create_line(330 - 24, 260, 330 + 24, 260, fill="#000000", width=9)
    cv.create_line(330, 260 - 24, 330, 260 + 24, fill="#000000", width=9)
    root.update()
    time.sleep(0.4)
    root.update()

    hwnd = op.find_window("", "OP_OPENCV_PROBE_FLAT") or \
        ctypes.windll.user32.FindWindowW(None, "OP_OPENCV_PROBE_FLAT")
    if not hwnd or not op.bind_window(hwnd, "gdi", "windows", "windows", 0):
        rec("绑定平坦靶子窗口", "hwnd=%s" % hex(hwnd or 0), "FAIL")
        root.destroy()
        return
    cw, ch = op.get_client_size(hwnd)
    shot = str(WORK / "v6_shot.bmp")
    if not op.capture(0, 0, cw, ch, shot):
        rec("capture", "失败", "FAIL")
        op.unbind_window(); root.destroy(); return
    im = Img(shot)
    # 靶子自检：左上角 60x45 必须是纯白（方差=0）窗口，否则本组失去判别力
    flat_ok, bad = im.region_all(0, 0, 60, 45, (255, 255, 255), tol=6)
    rec("  ↳ 靶子自检：左上角存在纯白平坦窗",
        "region_all(0,0,60,45,白)=%s %s（须为 True ⇒ 该窗口方差=0）" % (flat_ok, bad or ""),
        "PASS" if flat_ok else "FAIL")

    op.cv_remove_all_templates()
    tw, th = 60, 45
    tx = int(cw * 330 / 480.0) - tw // 2
    ty = int(ch * 260 / 360.0) - th // 2
    tpl = str(WORK / "v6_tpl.bmp")
    if not T("cv_crop 模板(黑十字 %d,%d)" % (tx, ty),
             lambda: op.cv_crop(shot, tx, ty, tw, th, tpl), ok=lambda v: v is True):
        op.unbind_window(); root.destroy(); return
    T("cv_load_template(t)", lambda: op.cv_load_template("t", tpl), ok=lambda v: v is True)

    raw = op.cv_edge_match_template(0, 0, cw, ch, "t", 0.5)
    try:
        j = json.loads(raw)
    except Exception:
        j = None
    if j is None:
        rec("cv_edge_match_template(全窗)", "返回非 JSON: %r" % raw, "FAIL")
    else:
        x, y = j.get("x"), j.get("y")
        near = abs((x or -9999) - tx) <= 3 and abs((y or -9999) - ty) <= 3
        at00 = (x == 0 and y == 0)
        rec("cv_edge_match_template 必须命中真位置(全窗搜索)",
            "ok=%s x=%s y=%s score=%s（真原点=(%d,%d)）%s" %
            (j.get("ok"), x, y, j.get("score"), tx, ty,
             "  ⇒ **伪命中：落在搜索窗左上角的平坦区**" if at00 else ""),
            "PASS" if (j.get("ok") == 1 and near) else "FAIL")

    # 反向：搜索窗避开真位置（只含平坦区）⇒ 必须不命中
    raw2 = op.cv_edge_match_template(0, 0, 200, 200, "t", 0.5)
    try:
        j2 = json.loads(raw2)
    except Exception:
        j2 = None
    rec("反向：搜索窗只含平坦区 ⇒ 必须不命中",
        ("ok=%s x=%s y=%s score=%s" % (j2.get("ok"), j2.get("x"), j2.get("y"), j2.get("score")))
        if j2 else "返回非 JSON: %r" % raw2,
        "PASS" if (j2 and j2.get("ok") == 0) else "FAIL")

    op.cv_remove_all_templates()
    op.unbind_window()
    root.destroy()
    time.sleep(0.2)


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="V1,V2,V3,V4,V5,V6")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    global _logf
    _logf = open(str(OUT_DIR / ("_t_opencv_api_%s.txt" % TS)), "w", encoding="utf-8")

    log("OpenCV 域 C API 验证  %s" % TS)
    log("产物目录 %s" % WORK)

    P = paths()
    log("合成图：%s（480x360，含 红/绿/蓝 + 灰阶渐变 + 白底）" % P["src"])
    log("合成图：%s（黑底 + 3 个分离白方块）" % P["blobs"])

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("op.dll = %s" % op.dll_path)

    if "V1" in groups:
        v1(op, P)
    if "V2" in groups:
        v2(op, P)
    if "V3" in groups:
        v3(op, P)
    if "V4" in groups:
        v4(op)
    if "V5" in groups:
        v5(op)
    if "V6" in groups:
        v6(op)

    # ---- 汇总 ----
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
    log("日志：%s" % (_logf.name if _logf else "-"))

    try:
        op.close()
    except Exception:
        pass
    try:
        _logf.close()
    except Exception:
        pass
    os._exit(0 if n["FAIL"] == 0 else 1)


if __name__ == "__main__":
    main()
