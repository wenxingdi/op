# -*- coding: utf-8 -*-
"""生成"多目标"YOLO 测试资产：tests/testdata/yolo_multi_test.onnx

为什么需要它（2026-11-21）：
`yolo_meta_test.onnx` 是单 Constant 节点、NMS 后恒定只输出 **1 条**，
所以 `MaxResultsTruncatesAndReportsTotal` 的"截断到小于总数"这一支
一直只能用 `GTEST_SKIP` 记录（见 docs/2026-11/YOLO模块优化_缺口与方案 §7.4）。
本模型给出 **4 条互不重叠、类别互不相同**的检测，让截断/排序/total 语义
可以真正闭环。

输出布局与 yolo_meta_test 一致：`[1, 84, 8400]`（v8 通道在前），
通道 0..3 = cx,cy,w,h（模型输入 640x640 空间），通道 4+cls = 该类分数。

四条检测（按 conf 降序，便于断言"截断保留的是高分那几条"）：

| # | cx,cy | w,h | class | conf |
|---|---|---|---|---|
| 0 | 100,100 | 40,40 | 0 | 0.90 |
| 1 | 400,100 | 40,40 | 1 | 0.80 |
| 2 | 100,400 | 40,40 | 2 | 0.70 |
| 3 | 400,400 | 40,40 | 3 | 0.60 |

类各不相同 -> 类别感知 NMS 天生不会互相抑制；坐标两两相隔 300px -> 即使同类也不会被 NMS 吃掉。

依赖：onnx + numpy（托管 venv）
重生成：python scripts/gen_yolo_multi_test_model.py
"""
import os

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "testdata", "yolo_multi_test.onnx")

NC = 80      # 与 84 = 4 + 80 属性维一致
NANCH = 8400

DETS = [
    (0, 100.0, 100.0, 40.0, 40.0, 0.90),
    (1, 400.0, 100.0, 40.0, 40.0, 0.80),
    (2, 100.0, 400.0, 40.0, 40.0, 0.70),
    (3, 400.0, 400.0, 40.0, 40.0, 0.60),
]

data = np.zeros((1, 4 + NC, NANCH), dtype=np.float32)
for cls, cx, cy, w, h, conf in DETS:
    data[0, 0, cls] = cx
    data[0, 1, cls] = cy
    data[0, 2, cls] = w
    data[0, 3, cls] = h
    data[0, 4 + cls, cls] = conf

const = helper.make_node("Constant", inputs=[], outputs=["out"],
                         value=numpy_helper.from_array(data))
graph = helper.make_graph(
    [const], "multi_test",
    [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
    [helper.make_tensor_value_info("out", TensorProto.FLOAT, [1, 4 + NC, NANCH])],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
model.metadata_props.add(
    key="names",
    value="{0: 'zero_cls', 1: 'one_cls', 2: 'two_cls', 3: 'three_cls'}")
model.metadata_props.add(key="task", value="detect")

onnx.checker.check_model(model)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
onnx.save(model, OUT)
print("written:", OUT, os.path.getsize(OUT), "bytes")
