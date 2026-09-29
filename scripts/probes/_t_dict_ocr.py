# -*- coding: utf-8 -*-
"""字库 / OCR 域 **C API 层** 验证（覆盖缺口里的 22 个 API）。

流程完全自洽（不依赖现成字库文件）：
  自建 Tk 靶子画已知文本 → extract_word_rects 取字框 → fetch_words 造字 →
  use_dict → ocr / find_str 回读 → save_dict 落盘 → clear_dict 后必须识别不出（反向）。

判据原则：字库 OCR 的"伪实现"最容易蒙混的地方是**忽略字库直接返回空串/恒返回同一串**，
所以每条正向断言都配反向：清空字库后同一次调用必须失败/为空。

分组：
  D1 字框与造字     extract_word_rects / fetch_word / fetch_words / get_dict_count
  D2 字库识别       ocr / ocr_ex / find_str / find_str_ex / get_words_no_dict /
                    get_word_result_count|pos|str
  D3 字库文件       save_dict / set_dict / get_dict / set_mem_dict / add_dict /
                    use_dict / get_now_dict / clear_dict
  D4 字库串工具     get_word_preview / check_word_dict / normalize_word_dict / rename_word_dict
  D5 免字库路径     autoocr_line / ocr_from_file / autoocr_from_file（AutoOcr 系列补齐）

用法：
  python scripts/probes/_t_dict_ocr.py [--groups D1,D2,D3,D4,D5]
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
WORK = OUT_DIR / ("dict_ocr_" + TS)
RES = []
_logf = None
_group = ["D0"]

TITLE = "OP_DICT_PROBE_TGT"
TEXT = "AB12"          # 4 个字形：A B 1 2（ASCII，字库造字最稳）
TEXT_X, TEXT_Y = 60, 60
FONT_PT = 28
COLOR = "000000"       # 黑字白底；op 颜色口径 = RRGGBB
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


def build_target():
    import tkinter as tk
    root = tk.Tk()
    root.title(TITLE)
    root.geometry("480x360+160+100")
    cv = tk.Canvas(root, width=480, height=360, highlightthickness=0)
    cv.pack(fill="both", expand=True)
    cv.create_rectangle(0, 0, 480, 360, fill="#ffffff", outline="")
    cv.create_text(TEXT_X, TEXT_Y, text=TEXT, font=("Arial", FONT_PT), fill="#000000", anchor="nw")
    root.update()
    time.sleep(0.4)
    root.update()
    return root


def bind_target(op):
    root = build_target()
    hwnd = op.find_window("", TITLE) or ctypes.windll.user32.FindWindowW(None, TITLE)
    if not hwnd:
        rec("自建靶子窗口", "未找到", "FAIL")
        root.destroy()
        return None, None
    rec("自建靶子窗口", "hwnd=%s" % hex(hwnd), "PASS")
    if not op.bind_window(hwnd, "gdi", "windows", "windows", 0):
        rec("bind_window", "失败", "FAIL")
        root.destroy()
        return None, None
    cw, ch = op.get_client_size(hwnd)
    log("  客户区 %dx%d，文本 '%s' @(%d,%d)" % (cw, ch, TEXT, TEXT_X, TEXT_Y))
    return root, (cw, ch)


# ------------------------------------------------------------------ D1 字框与造字
def d1(op, cw, ch):
    """造字 + 入库。

    口径（踩过坑，别改）：
      * `fetch_words` 的 words 是**连续字符串**，第 i 个字符对应第 i 个字框 —— 内部判
        `rects.size() != words.size()`（ImageSearchService.cpp:610）。传 "A|B|1|2"（7 字符）
        对 4 个字框 ⇒ 返回空串。要传 "AB12"。
      * `fetch_words` / `fetch_word` 只**返回**字库串（大漠"单独提取"语义），**不入库**；
        入库要 `add_dict(idx, 单条)` 或 `set_mem_dict(idx, 全文本)`。
    """
    _group[0] = "D1"
    sec("D1 字框提取与造字（文本 '%s'）" % TEXT)
    # 文本区域留足边界
    x1, y1, x2, y2 = 40, 40, 400, 140
    op.clear_dict(0)
    op.use_dict(0)

    rects = T("extract_word_rects(文本区)",
              lambda: op.extract_word_rects(x1, y1, x2, y2, COLOR, SIM, 8),
              ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:90])
    n_rects = 0
    if isinstance(rects, str) and rects:
        n_rects = len([r for r in rects.split("|") if r.count(",") >= 3])
    rec("  ↳ 字框数 == 字形数（%d）" % len(TEXT), "字框数=%d 原串=%s" % (n_rects, rects[:80]),
        "PASS" if n_rects == len(TEXT) else "FAIL")

    # 造字：整串批量（words 传连续串 TEXT，不是 '|' 分隔）
    entries = T("fetch_words(造字 %r)" % TEXT,
                lambda: op.fetch_words(x1, y1, x2, y2, COLOR, SIM, TEXT, 8),
                ok=lambda v: isinstance(v, str) and v.count("\n") >= len(TEXT) - 1,
                show=lambda v: "行数=%d 首行=%r" % (v.count("\n") + 1 if v else 0, v[:40]))
    lines = [s for s in (entries or "").split("\n") if s.strip()]
    rec("  ↳ 造字条数 == 字形数（%d）" % len(TEXT), "条数=%d" % len(lines),
        "PASS" if len(lines) == len(TEXT) else "FAIL")

    # 入库：逐条 add_dict
    added = 0
    for s in lines:
        if op.add_dict(0, s):
            added += 1
    rec("add_dict × %d 条入库" % len(lines), "成功=%d" % added, "PASS" if added == len(TEXT) else "FAIL")

    T("get_dict_count(0) == 4", lambda: op.get_dict_count(0),
      ok=lambda v: v == len(TEXT), show=lambda v: "count=%s" % v)
    T("get_now_dict() == 0", lambda: op.get_now_dict(),
      ok=lambda v: v == 0, show=lambda v: "now=%s" % v)

    # 反向：清空后 count 必须为 0
    T("  ↳ 反向：clear_dict(0) 后 count=0",
      lambda: (op.clear_dict(0), op.get_dict_count(0))[1],
      ok=lambda v: v == 0, show=lambda v: "count=%s" % v)
    # 复原
    for s in lines:
        op.add_dict(0, s)
    T("  ↳ 复原：重新入库后 count=4", lambda: op.get_dict_count(0),
      ok=lambda v: v == len(TEXT), show=lambda v: "count=%s" % v)
    return lines


# ------------------------------------------------------------------ D2 字库识别
def d2(op, cw, ch, lines):
    """字库识别。

    决定性判据（替换掉"清空字库后必须识别不出"这条**错误**判据）：
      op 的 `ocr` 在字库为空时**按设计走免字库 ONNX 兜底**（ImageSearchService.cpp:782-793
      有注释），所以"清空字库"反而仍能读出真值 —— 它证伪不了"忽略字库"的伪实现。
      真正能区分的判据是**把同样的点阵故意命名成别的字**：若 ocr 走字库 ⇒ 返回我们起的
      假名；若忽略字库走 ONNX ⇒ 仍返回真值 'AB12'。
    """
    _group[0] = "D2"
    sec("D2 字库识别（ocr / find_str / 结果解析）")
    x1, y1, x2, y2 = 40, 40, 400, 140
    T("ocr(文本区)", lambda: op.ocr(x1, y1, x2, y2, COLOR, SIM),
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r 真值=%r" % (v, TEXT))
    T("ocr_ex(文本区)", lambda: op.ocr_ex(x1, y1, x2, y2, COLOR, SIM),
      ok=lambda v: isinstance(v, str) and TEXT in v, show=lambda v: "识别=%r" % v, soft=True)

    fs = T("find_str(%r)" % TEXT, lambda: op.find_str(x1, y1, x2, y2, TEXT, COLOR, SIM),
           ok=lambda v: isinstance(v, tuple) and v[0] >= 0,
           show=lambda v: "(idx,x,y)=%s" % (v,), soft=True)
    if isinstance(fs, tuple) and fs[0] >= 0:
        rec("  ↳ 命中坐标落在文本区内", "(%d,%d) 文本区=(%d,%d)-(%d,%d)" % (fs[1], fs[2], x1, y1, x2, y2),
            "PASS" if (x1 <= fs[1] <= x2 and y1 <= fs[2] <= y2) else "FAIL")
    T("find_str_ex(%r)" % TEXT, lambda: op.find_str_ex(x1, y1, x2, y2, TEXT, COLOR, SIM),
      ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:80], soft=True)
    # 反向：找一个字库里没有的串
    T("  ↳ 反向：find_str('ZZZZ') 必须失败",
      lambda: op.find_str(x1, y1, x2, y2, "ZZZZ", COLOR, SIM),
      ok=lambda v: isinstance(v, tuple) and v[0] < 0, show=lambda v: "(idx,x,y)=%s" % (v,))

    nw = T("get_words_no_dict(文本区)（不依赖字库的字框）",
           lambda: op.get_words_no_dict(x1, y1, x2, y2, COLOR),
           ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:80], soft=True)
    if isinstance(nw, str) and nw:
        c = T("get_word_result_count(结果串)", lambda: op.get_word_result_count(nw),
              ok=lambda v: isinstance(v, int) and v >= 0, show=lambda v: "count=%s" % v, soft=True)
        if isinstance(c, int) and c > 0:
            T("get_word_result_pos(0)", lambda: op.get_word_result_pos(nw, 0),
              ok=lambda v: isinstance(v, tuple), show=lambda v: str(v), soft=True)
            T("get_word_result_str(0)", lambda: op.get_word_result_str(nw, 0),
              ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:40], soft=True)

    # 决定性反向：同一批点阵改名成 FAKE ⇒ ocr 必须返回 FAKE（证明真的在查字库）
    FAKE = "WXYZ"
    if lines and len(lines) == len(FAKE):
        renamed, nren = op.rename_word_dict("\n".join(lines), FAKE)
        rec("rename_word_dict(点阵→%r)" % FAKE, "条数=%s 首行=%r" % (nren, (renamed or "")[:40]),
            "PASS" if nren == len(FAKE) and renamed else "FAIL")
        fake_lines = [s for s in (renamed or "").split("\n") if s.strip()]
        op.clear_dict(0)
        for s in fake_lines:
            op.add_dict(0, s)
        T("  ↳ 决定性：改名字库后 ocr 必须返回假名 %r" % FAKE,
          lambda: op.ocr(x1, y1, x2, y2, COLOR, SIM),
          ok=lambda v: v == FAKE,
          show=lambda v: "识别=%r（真值 %r，假名 %r）" % (v, TEXT, FAKE))
        # 复原真名字库，确认回到真值
        op.clear_dict(0)
        for s in lines:
            op.add_dict(0, s)
        T("  ↳ 复原：真名字库后 ocr 回到真值", lambda: op.ocr(x1, y1, x2, y2, COLOR, SIM),
          ok=lambda v: v == TEXT, show=lambda v: "识别=%r" % v)
    else:
        rec("rename_word_dict 前置", "造字条数不足 ⇒ 跳过决定性判据", "SKIP")

    # 记录事实：字库为空时 ocr 走免字库兜底（设计如此，不是"忽略字库"）
    op.clear_dict(0)
    T("[事实] 清空字库后 ocr（免字库 ONNX 兜底）",
      lambda: op.ocr(x1, y1, x2, y2, COLOR, SIM),
      ok=lambda v: isinstance(v, str), show=lambda v: "识别=%r（兜底路径，非缺陷）" % v, soft=True)
    # 复原
    for s in (lines or []):
        op.add_dict(0, s)


# ------------------------------------------------------------------ D3 字库文件
def d3(op):
    _group[0] = "D3"
    sec("D3 字库文件（save / set / mem / add / use）")
    f = str(WORK / "dict0.txt")
    T("save_dict(0, 文件)", lambda: op.save_dict(0, f), ok=lambda v: bool(v))
    rec("  ↳ 文件真的落盘且非空", "size=%d" % (Path(f).stat().st_size if Path(f).exists() else -1),
        "PASS" if Path(f).exists() and Path(f).stat().st_size > 0 else "FAIL")

    T("clear_dict(0)", lambda: op.clear_dict(0), ok=lambda v: bool(v), soft=True)
    T("set_dict(0, 文件)", lambda: op.set_dict(0, f), ok=lambda v: bool(v))
    T("  ↳ 载入后 count == 4", lambda: op.get_dict_count(0),
      ok=lambda v: v == len(TEXT), show=lambda v: "count=%s" % v)
    info = T("get_dict(0, 0)（字库信息串）", lambda: op.get_dict(0, 0),
             ok=lambda v: isinstance(v, str) and v != "", show=lambda v: repr(v)[:70], soft=True)

    if not Path(f).exists():
        rec("字库文件（前序依赖 save_dict）", "文件不存在 ⇒ 跳过 D3 后半", "SKIP")
        return info
    data = Path(f).read_bytes()
    T("set_mem_dict(1, 字节内容)", lambda: op.set_mem_dict(1, data),
      ok=lambda v: bool(v), soft=True)
    T("  ↳ 内存字库 count == 4", lambda: op.get_dict_count(1),
      ok=lambda v: v == len(TEXT), show=lambda v: "count=%s" % v, soft=True)

    T("use_dict(1)", lambda: op.use_dict(1), ok=lambda v: bool(v), soft=True)
    T("  ↳ get_now_dict() == 1", lambda: op.get_now_dict(),
      ok=lambda v: v == 1, show=lambda v: "now=%s" % v, soft=True)
    T("use_dict(0)", lambda: op.use_dict(0), ok=lambda v: bool(v), soft=True)
    # 槽位合法范围 0..99（ImageSearchService.h:22 `_max_dict = 100`）⇒ 越界要用 100
    T("  ↳ 反向：use_dict(100) 越界应失败", lambda: op.use_dict(100),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)
    T("  ↳ 反向：use_dict(-1) 越界应失败", lambda: op.use_dict(-1),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)
    return info


# ------------------------------------------------------------------ D4 字库串工具
def d4(op, info):
    _group[0] = "D4"
    sec("D4 字库串工具（preview / check / normalize / rename）")
    if not info:
        rec("字库信息串（前序依赖）", "D3 未拿到 info ⇒ 跳过", "SKIP")
        return
    T("check_word_dict(info)", lambda: op.check_word_dict(info),
      ok=lambda v: isinstance(v, tuple), show=lambda v: "解析=%r" % (v,), soft=True)
    T("normalize_word_dict(info)", lambda: op.normalize_word_dict(info),
      ok=lambda v: isinstance(v, tuple), show=lambda v: "解析=%r" % (v,), soft=True)
    # info 是**单条**字库串 ⇒ rename 的 words 长度必须 == 1（否则 entries.size()!=words.size() 返回 0）
    rn = T("rename_word_dict(info, 'Q')（单条，words 长度须匹配）",
           lambda: op.rename_word_dict(info, "Q"),
           ok=lambda v: isinstance(v, tuple) and v[1] == 1 and v[0].startswith("Q$"),
           show=lambda v: "条数=%s 首行=%r" % (v[1], (v[0] or "")[:30]), soft=True)
    T("  ↳ 反向：words 长度不匹配应失败", lambda: op.rename_word_dict(info, "AB12"),
      ok=lambda v: isinstance(v, tuple) and v[1] == 0 and v[0] == "",
      show=lambda v: "解析=(%r, %s)" % (v[0][:20], v[1]), soft=True)
    T("get_word_preview(info)", lambda: op.get_word_preview(info),
      ok=lambda v: isinstance(v, tuple), show=lambda v: "解析=%r" % (v,), soft=True)
    T("  ↳ 反向：非法字库串应失败", lambda: op.check_word_dict("__not_a_dict__"),
      ok=lambda v: isinstance(v, tuple) and (v[1] == 0 or v[0] == ""),
      show=lambda v: "解析=%r" % (v,), soft=True)


# ------------------------------------------------------------------ D5 免字库路径
def d5(op, shot):
    _group[0] = "D5"
    sec("D5 免字库路径（autoocr_line / ocr_from_file / autoocr_from_file）")
    x1, y1, x2, y2 = 40, 40, 400, 140
    T("autoocr_line(文本区)", lambda: op.autoocr_line(x1, y1, x2, y2, COLOR, SIM),
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r（真值 %r）" % (v, TEXT))
    T("ocr_from_file(截图)", lambda: op.ocr_from_file(shot, COLOR, SIM),
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r（真值 %r）" % (v, TEXT))

    # Auto 系（无 color 参数）语义 = 免字库：**加载了字库也必须照常出结果**。
    # 这是 2026-09-30 修掉的缺陷的回归判据：旧实现 OcrAuto 复用 OCR(L"")，
    # 空串颜色二值化产出全背景 _binary ⇒ 有字库时恒返回空串。
    T("ocr_auto(文本区)（Auto 系=免字库，有字库也须出结果）",
      lambda: op.ocr_auto(x1, y1, x2, y2, SIM),
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r（真值 %r）" % (v, TEXT))
    T("ocr_auto_from_file(整图)", lambda: op.ocr_auto_from_file(shot, SIM),
      ok=lambda v: v == TEXT, show=lambda v: "识别=%r（真值 %r）" % (v, TEXT))
    crop = str(WORK / "crop_txt.bmp")
    if op.capture(x1, y1, x2, y2, crop):
        T("ocr_auto_from_file(裁剪文本区)", lambda: op.ocr_auto_from_file(crop, SIM),
          ok=lambda v: v == TEXT, show=lambda v: "识别=%r（真值 %r）" % (v, TEXT))
    # 事实记录：Ocr 走字库路径**必须给颜色**，空串色+字库 = 无前景可匹配（输入口径问题）
    T("[事实] ocr(空串色)（字库路径需显式颜色）", lambda: op.ocr(x1, y1, x2, y2, "", SIM),
      ok=lambda v: isinstance(v, str), show=lambda v: "识别=%r（空串色无前景，非缺陷）" % v, soft=True)
    T("  ↳ 反向：不存在的文件应失败",
      lambda: op.ocr_from_file(str(WORK / "__no_such__.bmp"), COLOR, SIM),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)


# ------------------------------------------------------------------ main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="D1,D2,D3,D4,D5")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    _logf = open(str(OUT_DIR / ("_t_dict_ocr_%s.txt" % TS)), "w", encoding="utf-8")
    log("字库 / OCR 域 C API 验证  %s" % TS)
    log("产物目录 %s" % WORK)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("op.dll = %s" % op.dll_path)

    try:
        import tkinter  # noqa: F401
    except Exception as e:
        rec("tkinter 可用", repr(e), "SKIP")
        return 1

    root, size = bind_target(op)
    if not root:
        return 1
    cw, ch = size
    shot = str(WORK / "shot.bmp")
    if not op.capture(0, 0, cw, ch, shot):
        rec("capture 靶子", "失败", "FAIL")
        root.destroy()
        return 1

    info = None
    lines = []
    try:
        if "D1" in groups:
            lines = d1(op, cw, ch)
        if "D2" in groups:
            d2(op, cw, ch, lines)
        if "D3" in groups:
            info = d3(op)
        if "D4" in groups:
            d4(op, info)
        if "D5" in groups:
            d5(op, shot)
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
