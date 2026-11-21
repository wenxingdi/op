# -*- coding: utf-8 -*-
"""Python ctypes 绑定端到端冒烟（覆盖 2026-11 批次新增的 39 个接口）。

背景：`bindings/python/op/api.py` / `_ffi.py` 一度落后 40 个接口（正好是 2026-11
整批：图像句柄 / 内存直操作二期 / 音频 / OCR&FindStr 增强 / YOLO Ex2），
`scripts/check_swig_sync.py` 只能查"名字有没有"，查不出"调得通不通"。
本脚本是真调用的那一层：每一个新增能力至少走一次真实 DLL 调用。

用法：
    & "D:\\Program Files\\Python312\\python.exe" scripts/probes/py_binding_smoke.py

退出码 0 = 全部通过。
"""
from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "bindings", "python"))

from op import Op  # noqa: E402

DLL = os.path.join(ROOT, "build", "nmake-x64-Release", "libop", "op_c_api_x64.dll")
MULTI_MODEL = os.path.join(ROOT, "tests", "testdata", "yolo_multi_test.onnx")

FAILS: list[str] = []


def is_json_or_empty(text: str) -> bool:
    """空串 = 无命中；"[]" / "{...}" 都是合法 JSON。"""
    text = text.strip()
    if not text:
        return True
    try:
        import json

        json.loads(text)
        return True
    except ValueError:
        return False



def check(name: str, cond: bool, detail: object = "") -> None:
    if cond:
        print(f"  OK   {name}")
    else:
        print(f"  FAIL {name}  {detail}")
        FAILS.append(name)


def make_bgra(width: int, height: int, rgb: tuple[int, int, int]) -> bytes:
    """BGRA，自上而下紧凑排列。"""
    b, g, r = rgb[2], rgb[1], rgb[0]  # 传入按 RGB 语义，内部转 BGRA
    return bytes([b, g, r, 255]) * (width * height)


def make_bmp(path: str, width: int, height: int, rgb: tuple[int, int, int]) -> None:
    import struct

    b, g, r = rgb[2], rgb[1], rgb[0]
    pixels = bytes([b, g, r, 255]) * (width * height)
    info = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(pixels), 0, 0, 14 + 40)
    with open(path, "wb") as f:
        f.write(header + info + pixels)


def main() -> int:
    if not os.path.exists(DLL):
        print("missing dll:", DLL)
        return 2

    with Op(dll_path=DLL, raise_on_error=False) as op:
        # ---- 图像句柄注册表 ----
        handle = op.load_image_mem(make_bgra(8, 4, (0x10, 0x20, 0x30)), 8, 4)
        check("load_image_mem -> img:", handle.startswith("img:"), handle)
        check("get_image_count", op.get_image_count() >= 1, op.get_image_count())

        size = op.get_image_size(handle)
        check("get_image_size", size == (8, 4, 4), size)

        raw = op.get_image_data(handle, "raw")
        check("get_image_data(raw) 长度", len(raw) == 8 * 4 * 4, len(raw))
        check("get_image_data 首像素 BGRA", raw[:4] == bytes([0x30, 0x20, 0x10, 0xFF]), raw[:4])
        bmp_bytes = op.get_image_data(handle, "bmp")
        check("get_image_data(bmp) 有 BMP 头", bmp_bytes[:2] == b"BM", bmp_bytes[:2])

        color = op.get_color_from_mem(handle, 0, 0)
        check("get_color_from_mem == 102030", color.upper() == "102030", color)
        check("cmp_color_from_mem 命中", op.cmp_color_from_mem(handle, 0, 0, "102030", 0.9))
        check("cmp_color_from_mem 不命中", not op.cmp_color_from_mem(handle, 0, 0, "FFFFFF", 0.9))
        check("get_color_num_from_mem", op.get_color_num_from_mem(handle, 0, 0, 8, 4, "102030", 0.9) == 32)

        crop = op.crop_image(handle, 0, 0, 4, 2)
        check("crop_image -> img:", crop.startswith("img:"), crop)
        check("crop_image 尺寸", op.get_image_size(crop) == (4, 2, 4), op.get_image_size(crop))
        check("free_image", op.free_image(crop))

        # ---- 文件版图像加载 ----
        bmp_path = os.path.join(ROOT, "workbench", "tmp", "py_smoke.bmp")
        make_bmp(bmp_path, 16, 8, (0, 0, 255))
        fh = op.load_image_bmp(bmp_path)
        check("load_image_bmp(路径) -> img:", fh.startswith("img:"), fh)
        check("load_image_bmp 尺寸", op.get_image_size(fh) == (16, 8, 4), op.get_image_size(fh))
        check("load_image_bmp 建色", op.get_color_from_mem(fh, 3, 3).upper() == "0000FF", op.get_color_from_mem(fh, 3, 3))

        # ---- 内存版找色 / 多色 / 色块（不依赖字库/模板）----
        ret, x, y = op.find_color_from_mem(fh, 0, 0, 16, 8, "0000FF", 0.9)
        check("find_color_from_mem 命中", ret == 1 and (x, y) == (0, 0), (ret, x, y))
        # Ex 系列返回**大漠串**（"x,y|x,y|..."），不是 JSON —— 与屏幕版一致
        ex = op.find_color_ex_from_mem(fh, 0, 0, 16, 8, "0000FF", 0.9)
        check("find_color_ex_from_mem 大漠串", ex.startswith("0,0"), ex[:40])
        ret2, mx, my = op.find_multi_color_from_mem(fh, 0, 0, 16, 8, "0000FF", "", 0.9)
        check("find_multi_color_from_mem 命中", ret2 == 1, (ret2, mx, my))
        mjson = op.find_multi_color_ex_from_mem(fh, 0, 0, 16, 8, "0000FF", "", 0.9)
        check("find_multi_color_ex_from_mem 大漠串", mjson.startswith("0,0"), mjson[:40])
        blk = op.find_color_block_ex_from_mem(fh, 0, 0, 16, 8, "0000FF", 0.9, 32, 8, 16)
        check("find_color_block_ex_from_mem 单点串", blk.strip() == "0,0", repr(blk[:40]))

        # ---- 内存版找图：把同色块当模板找（找不到也要给出合法 JSON）----
        pic_ret, px, py = op.find_pic_from_mem(fh, 0, 0, 16, 8, "no_such_pic.bmp", "000000", 0.9)
        check("find_pic_from_mem 未找到返回 -1", pic_ret == -1, pic_ret)
        # Ex 版找不到模板时返回空串（屏幕版同语义），不是失败
        picjson = op.find_pic_ex_from_mem(fh, 0, 0, 16, 8, "no_such_pic.bmp", "000000", 0.9)
        check("find_pic_ex_from_mem 未找到返回空串", picjson == "", repr(picjson[:40]))

        # ---- OCR：结构化 + 耗时 + 免字库 ----
        # 纯色图上没有文字 -> 返回空串是正常的；这里只要求"调得通、不抛、形态合法"
        # （OCR 准确性由 gtest + workbench/test_gmplug.py 真实素材负责）
        ocr_json = op.ocr_ex2(0, 0, 16, 8, "", 0.9)
        check("ocr_ex2 可调用（空串/[]/JSON）", is_json_or_empty(ocr_json), ocr_json[:60])
        prof = op.ocr_last_profile()
        check("ocr_last_profile 非空", bool(prof.strip()), prof[:40])
        ocmem = op.ocr_from_mem_ex2(fh, 0, 0, 0, 0, "", 0.9)
        check("ocr_from_mem_ex2 可调用（空串/[]/JSON）", is_json_or_empty(ocmem), ocmem[:60])
        auto_json, auto_n = op.ocr_auto_color_from_mem_ex2(fh, 0, 0, 16, 8, "0000FF", 0.9)
        check("ocr_auto_color_from_mem_ex2 返回 (json, n)", isinstance(auto_n, int), auto_n)

        # ---- YOLO Ex2 / 内存版（多目标模型 -> total 必须 4）----
        if os.path.exists(MULTI_MODEL):
            # 注意：YOLO 用的图必须够大（>= 640 宽）。16x8 会把 40x40 的框缩成 h=0，
            # 被当退化框丢掉 —— 那是测试靶子选错，不是绑定缺陷。
            yolo_bmp = os.path.join(ROOT, "workbench", "tmp", "py_smoke_yolo.bmp")
            make_bmp(yolo_bmp, 640, 480, (30, 30, 30))
            yh = op.load_image_bmp(yolo_bmp)
            check("yolo 靶子图 img:", yh.startswith("img:"), yh)
            check("set_yolo_engine", op.set_yolo_engine(MULTI_MODEL, "", "--threads=2"))
            yjson, ycount = op.yolo_detect_from_file_ex2(yolo_bmp, 0.01, 0.45)
            check("yolo_detect_from_file_ex2 命中 4", ycount == 4 and '"total":4' in yjson, (ycount, yjson[:60]))
            yjson2, ycount2 = op.yolo_detect_from_file_ex2(yolo_bmp, 0.01, 0.45, '{"max_results":2}')
            check("yolo_detect_from_file_ex2 截断到 2", ycount2 == 2, ycount2)
            ymjson, ymcount = op.yolo_detect_from_mem(yh, 0, 0, 0, 0, 0.01, 0.45)
            check("yolo_detect_from_mem 命中 4", ymcount == 4, (ymcount, ymjson[:60]))
            ymx, ymxcount = op.yolo_detect_from_mem_ex2(yh, 0, 0, 0, 0, 0.01, 0.45, '{"max_results":1}')
            check("yolo_detect_from_mem_ex2 截断到 1", ymxcount == 1, ymxcount)
        else:
            print("  SKIP YOLO（缺 yolo_multi_test.onnx）")

        # ---- 音频 ----
        own_pid_json = op.audio_query_pid(os.getpid())
        check("audio_query_pid 是 JSON", own_pid_json.strip().startswith("{"), own_pid_json[:40])
        check("audio_get_pid_from_hwnd(0) 不崩", isinstance(op.audio_get_pid_from_hwnd(0), int))
        sysvol = op.audio_get_system_volume()
        check("audio_get_system_volume 是 JSON", sysvol.strip().startswith("{"), sysvol[:40])
        check("audio_set_system_volume(0.5)", op.audio_set_system_volume(0.5))
        check("audio_set_system_mute(False)", op.audio_set_system_mute(False))
        check("audio_query_window(0) 不崩", isinstance(op.audio_query_window(0), str))
        # 非法 PID 必须失败（契约改过：0=失败），不能抛
        check("audio_set_session_volume(0, ...) 返回 False", op.audio_set_session_volume(0, 0.5) is False)

        # ---- 清理 ----
        released = op.free_all_images()
        check("free_all_images >= 2", released >= 2, released)
        check("释放后计数归零", op.get_image_count() == 0, op.get_image_count())

        for path in (bmp_path, os.path.join(ROOT, "workbench", "tmp", "py_smoke_yolo.bmp")):
            if os.path.exists(path):
                os.remove(path)

    print()
    if FAILS:
        print("FAILED %d 项：%s" % (len(FAILS), ", ".join(FAILS)))
        return 1
    print("PASSED 全部通过（Python 绑定 2026-11 批次）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
