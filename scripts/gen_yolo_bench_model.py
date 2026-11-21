# -*- coding: utf-8 -*-
"""生成"算力接近 YOLOv8n"的探针模型（**仅用于线程/EP 扩展性基准**，不是可用检测模型）。

为什么需要它：本机没有真实 YOLO 模型（仓库里只有 tests/testdata/yolo_meta_test.onnx，
那是单 Constant 节点的假模型，跑一次不到 1ms，测不出线程差异）。
DirectML / 线程数这类决策**必须先用真实算力测**，否则就是自嗨。

模型结构（顺序 conv 堆，无残差，输入 1x3x640x640）：
  3->16 s2 | 16->32 s2 | 32->64 s2 | 64->64 3x3 ×8 @80x80 | 1x1 头 -> [1,84,8400]
FLOPs 量级 ≈ 2.4 GFLOP，与 YOLOv8n(640) 的 ~8.7 GFLOP 同量级偏小，
够看出 1 线程 vs N 线程的差异。

输出：workbench/tmp/yolo_bench_heavy.onnx
用法：& "C:/Users/lc/.workbuddy/binaries/python/envs/default/Scripts/python.exe" \\
        scripts/gen_yolo_bench_model.py
"""
import os

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "workbench", "tmp", "yolo_bench_heavy.onnx")

NODES, INITS = [], []


def conv(src, dst, cin, cout, k, stride, seed):
    w = np.random.RandomState(seed).randn(cout, cin, k, k).astype(np.float32) * 0.05
    b = np.zeros(cout, dtype=np.float32)
    INITS.append(numpy_helper.from_array(w, dst + "_w"))
    INITS.append(numpy_helper.from_array(b, dst + "_b"))
    NODES.append(helper.make_node("Conv", [src, dst + "_w", dst + "_b"], [dst + "_c"],
                                  kernel_shape=[k, k], strides=[stride, stride],
                                  pads=[k // 2, k // 2, k // 2, k // 2]))
    NODES.append(helper.make_node("Relu", [dst + "_c"], [dst]))


# 主干：三次下采样
conv("images", "c1", 3, 16, 3, 2, 1)
conv("c1", "c2", 16, 32, 3, 2, 2)
conv("c2", "c3", 32, 64, 3, 2, 3)

# 8 个 64->64 3x3 @80x80，制造主要算力（简单串联）
src = "c3"
for i in range(8):
    conv(src, "b%d" % i, 64, 64, 3, 1, 10 + i)
    src = "b%d" % i

# 头：1x1 把 64 通道压到 84，再 reshape 成 [1,84,6400] 并补零到 8400
NODES.append(helper.make_node("Conv", [src, "h_w", "h_b"], ["head"],
                              kernel_shape=[1, 1], strides=[1, 1]))
INITS.append(numpy_helper.from_array(
    np.random.RandomState(99).randn(84, 64, 1, 1).astype(np.float32) * 0.05, "h_w"))
INITS.append(numpy_helper.from_array(np.zeros(84, dtype=np.float32), "h_b"))
INITS.append(numpy_helper.from_array(np.array([1, 84, 6400], dtype=np.int64), "shape"))
INITS.append(numpy_helper.from_array(np.zeros((1, 84, 2000), dtype=np.float32), "pad"))
NODES.append(helper.make_node("Reshape", ["head", "shape"], ["flat"]))
NODES.append(helper.make_node("Concat", ["flat", "pad"], ["out"], axis=2))

graph = helper.make_graph(
    NODES, "yolo_bench_heavy",
    [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
    [helper.make_tensor_value_info("out", TensorProto.FLOAT, [1, 84, 8400])],
    initializer=INITS,
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
onnx.checker.check_model(model)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
onnx.save(model, OUT)
print("written:", OUT, os.path.getsize(OUT), "bytes")
