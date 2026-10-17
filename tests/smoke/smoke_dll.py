#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OP C API DLL 冒烟测试（无头，无需游戏窗口）。

用途：每次构建/替换 op_c_api DLL 后，5 秒内确认核心能力没坏。
覆盖：导出完整性、实例生命周期、路径/JSON 无头接口、OCR 引擎加载、
      文件识别两条路径（免字库直识 / 颜色二值化）、非法偏色段报错落地。

用法：
    python smoke_dll.py --dll D:\\AutoPro\\op-master\\op\\build\\nmake-x64-Release\\libop\\op_c_api_x64.dll

注意：必须用与 DLL 同架构的 Python（x64 DLL 配 64 位 Python）。
退出码：0 = 全部通过；1 = 有失败项。
"""
import argparse
import ctypes
import os
import sys
import tempfile
import time
from pathlib import Path

FAILURES = []
CHECKS = [0]


def check(name, cond, detail=""):
    CHECKS[0] += 1
    mark = "PASS" if cond else "FAIL"
    print(f"[{mark}] {name}" + (f"  -- {detail}" if detail else ""))
    if not cond:
        FAILURES.append(name)
    return cond


# 各模块代表导出：任一个丢失都说明构建/导出环节出了问题。
EXPORT_CHECKLIST = [
    # 生命周期/基础
    "OpVer", "OpCreate", "OpDestroy", "OpSetPath", "OpGetPath", "OpGetBasePath", "OpGetID",
    # 窗口/绑定/输入
    "OpBindWindow", "OpUnBindWindow", "OpFindWindow", "OpMoveTo", "OpLeftClick", "OpKeyPressStr", "OpSendString",
    # 图色
    "OpGetColor", "OpCmpColor", "OpFindColor", "OpFindColorEx", "OpFindMultiColor",
    "OpFindPic", "OpFindPicEx", "OpCapture",
    # OCR（含 2026-10-16 空颜色门面别名）
    "OpOcr", "OpOcrEx", "OpOcrFromFile", "OpOcrAutoColor", "OpOcrAutoColorEx",
    "OpOcrAutoColorFromFile", "OpOcrAuto", "OpOcrAutoFromFile", "OpOcrAutoColorLine",
    "OpSetOcrEngine", "OpOcrWarmup",
    # OpenCV
    "OpCvMatchTemplate", "OpCvInRange", "OpCvRotate", "OpCvFlip",
    # 内存/进程
    "OpFindData", "OpReadInt", "OpWriteData", "OpRunApp",
    # JSON / HTTP（大模型通道）
    "OpJsonIsValid", "OpJsonGetString", "OpJsonSetString", "OpHttpPost", "OpHttpGet",
]


def bind(dll):
    h = ctypes.c_void_p
    w = ctypes.c_wchar_p
    i = ctypes.c_int
    d = ctypes.c_double
    P = ctypes.POINTER

    f = {}
    f["OpVer"] = dll.OpVer
    f["OpVer"].restype = w
    f["OpCreate"] = dll.OpCreate
    f["OpCreate"].restype = h
    f["OpDestroy"] = dll.OpDestroy
    f["OpDestroy"].argtypes = [h]
    for name, res, args in [
        ("OpGetID", i, [h]),
        ("OpSetPath", i, [h, w]),
        ("OpGetPath", w, [h]),
        ("OpGetBasePath", w, [h]),
        ("OpSetShowErrorMsg", i, [h, i]),
        ("OpJsonIsValid", i, [h, w]),
        ("OpJsonGetString", w, [h, w, w]),
        ("OpJsonGetNumber", i, [h, w, w, P(d)]),
        ("OpJsonSetString", w, [h, w, w, w]),
        ("OpSetOcrEngine", i, [h, w, w, w]),
        ("OpOcrWarmup", i, [h]),
        ("OpOcrAutoFromFile", w, [h, w, d]),
        ("OpOcrAutoColorFromFile", w, [h, w, w, d]),
        ("OpOcrFromFile", w, [h, w, w, d]),
    ]:
        fn = getattr(dll, name)
        fn.restype = res
        fn.argtypes = args
        f[name] = fn
    return f


def make_synthetic_images(tmpdir):
    """生成两张合成图：白底深字（走默认 0x60）、深底红字（走显式小偏色）。"""
    from PIL import Image, ImageDraw, ImageFont

    font = None
    for cand in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\arial.ttf"):
        if os.path.exists(cand):
            try:
                font = ImageFont.truetype(cand, 24)
                break
            except OSError:
                pass

    def draw(path, bg, fg, text):
        img = Image.new("RGB", (340, 60), bg)
        dr = ImageDraw.Draw(img)
        dr.text((10, 12), text, font=font, fill=fg)
        img.save(path)
        return path

    white_bg = draw(os.path.join(tmpdir, "smoke_white.png"), (255, 255, 255), (32, 32, 32), "HP 12345 MP 678")
    dark_bg = draw(os.path.join(tmpdir, "smoke_dark.png"), (20, 20, 20), (224, 17, 17), "HP 12345 MP 678")
    return white_bg, dark_bg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True, help="op_c_api DLL 路径（x64/x86 均可，需同架构 Python）")
    ap.add_argument("--skip-ocr", action="store_true", help="跳过 OCR 引擎加载（最快 1 秒，只看导出与无头接口）")
    args = ap.parse_args()

    dll_path = Path(args.dll)
    check("DLL 文件存在", dll_path.is_file(), str(dll_path))
    if not dll_path.is_file():
        return 1

    py_bits = ctypes.sizeof(ctypes.c_void_p) * 8
    dll_is_x64 = "x64" in dll_path.name.lower()
    check("Python/DLL 架构一致", (py_bits == 64) == dll_is_x64,
          f"python={py_bits}bit, dll 名暗示 {'x64' if dll_is_x64 else 'x86'}")

    try:
        dll = ctypes.WinDLL(str(dll_path))
        check("DLL 加载", True)
    except OSError as e:
        check("DLL 加载", False, str(e))
        return 1

    missing = [n for n in EXPORT_CHECKLIST if not hasattr(dll, n)]
    check(f"核心导出完整（{len(EXPORT_CHECKLIST) - len(missing)}/{len(EXPORT_CHECKLIST)}）",
          not missing, "缺失: " + ",".join(missing) if missing else "")

    f = bind(dll)

    ver = f["OpVer"]()
    check("OpVer 返回非空", bool(ver), ver or "")

    op = f["OpCreate"]()
    check("OpCreate 返回句柄", bool(op))
    if not op:
        return 1

    try:
        check("OpGetID >= 0", f["OpGetID"](op) >= 0)
        check("SetPath/GetPath 往返", f["OpSetPath"](op, tempfile.gettempdir()) == 1
              and f["OpGetPath"](op) == tempfile.gettempdir())

        # JSON 无头往返
        j = '{"a":{"b":"hello"},"n":42}'
        check("OpJsonIsValid", f["OpJsonIsValid"](op, j) == 1)
        check("OpJsonGetString", f["OpJsonGetString"](op, j, "a.b") == "hello")
        nv = ctypes.c_double(-1)
        ok_num = f["OpJsonGetNumber"](op, j, "n", ctypes.byref(nv)) == 1 and nv.value == 42
        check("OpJsonGetNumber", ok_num, f"n={nv.value}")
        j2 = f["OpJsonSetString"](op, j, "a.b", "world")
        check("OpJsonSetString 往返", f["OpJsonGetString"](op, j2, "a.b") == "world")

        if args.skip_ocr:
            print("--skip-ocr：跳过引擎加载与识别检查")
        else:
            check("SetOcrEngine(默认引擎)", f["OpSetOcrEngine"](op, "", "", "--timeout=3000") == 1)
            t0 = time.time()
            warm = f["OpOcrWarmup"](op)
            check("OpOcrWarmup", warm == 1, f"耗时 {time.time() - t0:.1f}s")

            tmpdir = tempfile.mkdtemp(prefix="op_smoke_")
            white_bg, dark_bg = make_synthetic_images(tmpdir)

            # 路径一：免字库直识（白底深字，空颜色）
            r1 = f["OpOcrAutoFromFile"](op, white_bg, 0.8) or ""
            check("OcrAutoFromFile 非空", bool(r1.strip()), r1.strip()[:40])

            # 路径二：颜色二值化（深底红字，显式小偏色——游戏 UI 场景口径）
            r2 = f["OpOcrAutoColorFromFile"](op, dark_bg, "E01111-202020", 0.8) or ""
            check("OcrAutoColorFromFile(小偏色) 非空", bool(r2.strip()), r2.strip()[:40])

            # 路径三：非法偏色段必须报错落地（2026-10-17 加固），不再静默错解析
            f["OpSetShowErrorMsg"](op, 2)  # 2 = 写 __op.log
            log_path = Path(os.getcwd()) / "__op.log"
            size_before = log_path.stat().st_size if log_path.exists() else 0
            r3 = f["OpOcrAutoColorFromFile"](op, dark_bg, "E01111-20", 0.8) or ""
            _ = r3  # 返回值不做要求，只验证报错落地
            time.sleep(0.2)
            grew = log_path.exists() and log_path.stat().st_size > size_before
            # __op.log 是 ANSI（GBK）编码，按 UTF-8 读会丢字节导致误判
            content = log_path.read_bytes().decode("gbk", errors="ignore") if grew else ""
            check("非法偏色段写 __op.log", grew and "非法偏色段" in content,
                  "log 增长且含'非法偏色段'" if grew else "未检测到日志输出")

            # 字库路径冒烟（传统 OcrFromFile，字库未加载允许失败但不许崩）
            f["OpOcrFromFile"](op, white_bg, "000000", 0.9)
            check("OcrFromFile 调用不崩溃", True)
    finally:
        f["OpDestroy"](op)

    print(f"\n共 {CHECKS[0]} 项，失败 {len(FAILURES)} 项")
    if FAILURES:
        print("失败项: " + ", ".join(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
