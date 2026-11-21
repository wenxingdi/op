# -*- coding: utf-8 -*-
"""YOLO 线程数端到端基准（走真实库，不是裸 ORT）。

为什么需要它：`--threads=N` 只影响速度、不影响结果，任何"结果等价"类断言
都抓不到"参数其实没生效"。唯一能判真假的就是**实测耗时比**。

做法：ctypes 调 op_c_api_x64.dll
    OpSetYoloEngine(model, "", "--threads=N")  -> OpYoloDetectFromFile(大图)
分别测 threads=1 / 4 / 不传，比 median。

判据（不是"看着变快就行"）：
    t(threads=4) / t(threads=1) < 0.6   -> PASS
    t(auto)      / t(threads=1) < 0.6   -> PASS
本机（UHD730 + 8 核）实测约 0.29 / 0.21。把实现改回写死单线程时比值≈1.0，
脚本会非 0 退出 —— 这是本改动**唯一**可信的反向验证手段。

依赖：workbench/tmp/yolo_bench_heavy.onnx（scripts/gen_yolo_bench_model.py 生成）
用法：
    & "D:\\Program Files\\Python312\\python.exe" scripts/probes/yolo_thread_bench.py
"""
import ctypes
import os
import statistics
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "build", "nmake-x64-Release", "libop", "op_c_api_x64.dll")
MODEL = os.path.join(ROOT, "workbench", "tmp", "yolo_bench_heavy.onnx")
IMG = os.path.join(ROOT, "workbench", "tmp", "yolo_bench_input.bmp")

RUNS = 12


def write_bmp(path: str, w: int, h: int) -> None:
    """32 位 top-down BMP（op 的 YoloDetectFromFile 走 GDI+/BMP 解码）。"""
    import struct

    row = w * 4
    pixels = bytes([127, 127, 127, 255]) * (w * h)
    info = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(pixels), 0, 0, 14 + 40)
    with open(path, "wb") as f:
        f.write(header + info + pixels)


def bind(dll):
    dll.OpCreate.restype = ctypes.c_void_p
    dll.OpDestroy.argtypes = [ctypes.c_void_p]
    dll.OpSetYoloEngine.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p]
    dll.OpSetYoloEngine.restype = ctypes.c_int
    dll.OpYoloDetectFromFile.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_double,
                                         ctypes.c_double]
    dll.OpYoloDetectFromFile.restype = ctypes.c_wchar_p


def bench(dll, argv: str) -> float:
    handle = dll.OpCreate()
    if not handle:
        raise SystemExit("OpCreate failed")
    try:
        ret = dll.OpSetYoloEngine(handle, MODEL, "", argv)
        if ret != 1:
            raise SystemExit("OpSetYoloEngine(%r) failed: %d" % (argv, ret))
        samples = []
        for _ in range(2):  # 预热
            dll.OpYoloDetectFromFile(handle, IMG, 0.25, 0.45)
        for _ in range(RUNS):
            t0 = time.perf_counter()
            dll.OpYoloDetectFromFile(handle, IMG, 0.25, 0.45)
            samples.append((time.perf_counter() - t0) * 1000.0)
        return statistics.median(samples)
    finally:
        dll.OpDestroy(handle)


def main() -> int:
    if not os.path.exists(DLL):
        print("missing dll:", DLL)
        return 2
    if not os.path.exists(MODEL):
        print("missing model:", MODEL, "(run scripts/gen_yolo_bench_model.py)")
        return 2
    write_bmp(IMG, 640, 640)

    dll = ctypes.CDLL(DLL)
    bind(dll)

    t1 = bench(dll, "--threads=1")
    t4 = bench(dll, "--threads=4")
    # YoloDetector 是进程级单例：不传 --threads 会沿用上次的 4，
    # 必须显式 --threads=0 才能测到 ORT 默认
    tauto = bench(dll, "--threads=0")
    print("threads=1    median %7.2f ms" % t1)
    print("threads=4    median %7.2f ms   ratio %.2f" % (t4, t4 / t1))
    print("auto(ORT)    median %7.2f ms   ratio %.2f" % (tauto, tauto / t1))

    ok = (t4 / t1) < 0.6 and (tauto / t1) < 0.6
    print("RESULT:", "PASS" if ok else "FAIL (threads 没生效? 是不是又把 SetIntraOpNumThreads 写死了)")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
