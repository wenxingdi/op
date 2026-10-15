# -*- coding: utf-8 -*-
"""E-A 回归探针：OCR 管理器锁外推后并发推理不再串行化。

判据（反向可验证）：
  - 同一 BMP 顺序跑 2 次取单次耗时 t1（稳态）。
  - 两线程并发各跑 1 次取总耗时 t2。
  - 锁外推生效：t2 ≈ t1（< 1.6*t1，ORT intra-op 线程共享有少量争抢）。
  - 旧实现（整推理持锁）必然 t2 ≈ 2*t1 > 1.6*t1 → FAIL。

用法（cwd=仓库根）：
  D:\\Program Files\\Python312\\python.exe scripts/probes/_t_ocr_concurrency.py
默认 DLL 取 build/nmake-x64-Release/libop/op_c_api_x64.dll（_wb_build.py 产物）。
"""
import ctypes
import os
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "build", "nmake-x64-Release", "libop", "op_c_api_x64.dll")
# 1920x1080 黑底中央单块文本 ≈ 500ms/次（见 OP_OCR_INTERNAL_PLAN 9.12 基准）
BMP = os.path.join(ROOT, "workbench", "ocr_bench_big_1920x1080_single.bmp")
N_CONCURRENT = int(os.environ.get("N", "2"))
# 回归绊线：若管理器锁回归（重新包住推理），比值会回到 ≈N（旧实现实测 N=2→2.30）。
# 锁外推后本机实测 N=2→1.59（本机单路接近带宽饱和，拿不到理想 1.0，能抓到回归即可）。
RATIO_LIMIT = float(os.environ.get("RATIO_LIMIT", str(N_CONCURRENT - 0.3)))


def load_api():
    if not os.path.exists(DLL):
        sys.exit(f"SKIP: 未找到 DLL {DLL}（先跑 scripts/build/_wb_build.py）")
    if not os.path.exists(BMP):
        sys.exit(f"SKIP: 未找到测试图 {BMP}")
    op = ctypes.windll.LoadLibrary(DLL)
    op.OpCreate.restype = ctypes.c_void_p
    op.OpOcrFromFile.restype = ctypes.c_wchar_p
    op.OpOcrFromFile.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_double]
    op.OpSetOcrEngine.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p]
    op.OpDestroy.argtypes = [ctypes.c_void_p]
    return op


def make_handle(op):
    h = op.OpCreate()
    # --threads=1：ORT 推理在调用线程内单线程执行。两线程并发时各自的
    # 单线程前/后处理天然可重叠——无锁实现总耗时 ≈ 1×t1，锁实现 = 2×t1。
    # 这是本机（负载波动 3 倍、带宽易饱和）上最稳的判别器。
    op.OpSetOcrEngine(h, "onnx", "", "--threads=1")
    return h


def ocr_with_handle(op, h):
    return op.OpOcrFromFile(h, BMP, "", 0.0)  # sim=0 越界 → 默认 0.7


def main():
    op = load_api()

    # 预热 + 预建句柄：OpCreate/SetOcrEngine（含引擎初始化 ~325ms）不计入计时区。
    # 单 Op 实例非线程安全（libop.h 并发约定），故每线程一个句柄，计时区只并发 OpOcrFromFile。
    handles = [make_handle(op) for _ in range(N_CONCURRENT)]
    warm = ocr_with_handle(op, handles[0])
    print(f"预热识别结果: {warm!r}")

    t0 = time.perf_counter()
    for h in handles:
        txt = ocr_with_handle(op, h)
    t_seq = (time.perf_counter() - t0) / N_CONCURRENT
    if not txt:
        print("FAIL: OCR 返回空串，探针前置不成立")
        sys.exit(1)
    print(f"顺序单次 t1 = {t_seq*1000:.0f} ms")

    results = [None] * N_CONCURRENT

    def worker(i):
        results[i] = ocr_with_handle(op, handles[i])

    t0 = time.perf_counter()
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(N_CONCURRENT)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    t_con = time.perf_counter() - t0
    print(f"并发{N_CONCURRENT}线程总耗时 t2 = {t_con*1000:.0f} ms  (t2/t1 = {t_con/t_seq:.2f})")

    for h in handles:
        op.OpDestroy(h)

    ok_text = all(r for r in results)
    ratio = t_con / t_seq
    ok_ratio = ratio < RATIO_LIMIT
    print(f"{'PASS' if ok_text and ok_ratio else 'FAIL'}: 文本齐={ok_text} 并发比={ratio:.2f} (<{RATIO_LIMIT} 判并发未串行)")
    sys.exit(0 if ok_text and ok_ratio else 1)


if __name__ == "__main__":
    main()
