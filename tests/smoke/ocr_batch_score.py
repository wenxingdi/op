#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OCR 批量识别打分工具（上机验证用）。

对目录下的截图逐张跑同一种识别参数，汇总命中率，用于验证某个
颜色/容差/sim 组合在真实场景下的合格率。2026-10-16 游戏批次实测：
默认 0x60 合格率 5%，"E01111-202020" 84%，双色枚举 "E01111-202020|33EE33-202020" 58/58。

用法示例：
    # 颜色二值化路径（传空颜色则走免字库直识）
    python ocr_batch_score.py --dll ...\\op_c_api_x64.dll --dir D:\\GMPLUG\\captures ^
        --color "E01111-202020|33EE33-202020" --sim 0.8 --expected "12345"

    # 免字库直识（空颜色）
    python ocr_batch_score.py --dll ... --dir ... --color ""

退出码恒为 0（这是度量工具不是门槛测试）；结果可 --csv 落盘。
"""
import argparse
import ctypes
import csv
import sys
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--dir", required=True, help="截图目录（png/bmp/jpg）")
    ap.add_argument("--color", required=True, help="颜色串；空串 = 免字库直识 OcrAutoFromFile")
    ap.add_argument("--sim", type=float, default=0.8)
    ap.add_argument("--expected", default="", help="期望子串；提供后统计命中率")
    ap.add_argument("--ext", default=".png,.bmp,.jpg,.jpeg")
    ap.add_argument("--csv", default="", help="可选：逐文件结果落盘路径")
    ap.add_argument("--timeout", default="3000")
    args = ap.parse_args()

    dll = ctypes.WinDLL(str(Path(args.dll)))
    dll.OpCreate.restype = ctypes.c_void_p
    dll.OpSetOcrEngine.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p]
    dll.OpSetOcrEngine.restype = ctypes.c_int
    dll.OpOcrAutoColorFromFile.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_double]
    dll.OpOcrAutoColorFromFile.restype = ctypes.c_wchar_p
    dll.OpOcrAutoFromFile.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_double]
    dll.OpOcrAutoFromFile.restype = ctypes.c_wchar_p
    dll.OpDestroy.argtypes = [ctypes.c_void_p]

    op = dll.OpCreate()
    if not op:
        print("OpCreate 失败")
        return 1
    try:
        if dll.OpSetOcrEngine(op, "", "", f"--timeout={args.timeout}") != 1:
            print("SetOcrEngine 失败")
            return 1

        exts = tuple(args.ext.lower().split(","))
        files = sorted(p for p in Path(args.dir).iterdir() if p.suffix.lower() in exts)
        if not files:
            print(f"目录无截图: {args.dir}")
            return 1

        rows = []
        t0 = time.time()
        for p in files:
            if args.color:
                text = dll.OpOcrAutoColorFromFile(op, str(p), args.color, args.sim) or ""
            else:
                text = dll.OpOcrAutoFromFile(op, str(p), args.sim) or ""
            text = text.strip()
            hit = (args.expected in text) if args.expected else bool(text)
            rows.append((p.name, text, hit))
            print(f"{'HIT ' if hit else 'miss'} {p.name}: {text[:60]}")
        dt = time.time() - t0

        nonempty = sum(1 for _, t, _ in rows if t)
        hits = sum(1 for _, _, h in rows if h)
        mode = f"color='{args.color}'" if args.color else "免字库直识"
        print(f"\n=== 汇总（{mode}, sim={args.sim}）===")
        print(f"总张数 {len(rows)} | 非空 {nonempty} | 命中 {hits} ({hits * 100 // max(len(rows), 1)}%) | 耗时 {dt:.1f}s")
        if args.csv:
            with open(args.csv, "w", newline="", encoding="utf-8-sig") as fp:
                w = csv.writer(fp)
                w.writerow(["file", "text", "hit"])
                w.writerows(rows)
            print(f"已落盘: {args.csv}")
    finally:
        dll.OpDestroy(op)
    return 0


if __name__ == "__main__":
    sys.exit(main())
