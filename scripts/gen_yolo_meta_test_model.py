# -*- coding: utf-8 -*-
"""生成 YOLO metadata 端到端测试资产：tests/testdata/yolo_meta_test.onnx

极小合法 ONNX（单 Constant 节点），输出固定张量 [1, 84, 8400]（v8 通道在前布局），
并在 metadata_props 嵌入 ultralytics 风格的 "names" 类别名字典——用于验证
OnnxYoloEngine 加载时自动读出类别名（SetYoloEngine 统一入口免 --labels）。

张量内容：全部 anchor 的 cx=100,cy=100,w=20,h=20、类别 1 分数 0.99（其余 0）。
detect 期望产出 1 个目标：class_id=1、label="target_cls"。

依赖：onnx + numpy（托管 venv：C:/Users/lc/.workbuddy/binaries/python/envs/default）
重生成：python scripts/gen_yolo_meta_test_model.py
"""
import os

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "testdata", "yolo_meta_test.onnx")

NC = 80          # 类别数（与 84=4+80 属性维一致）
NANCH = 8400     # 640 输入 anchor-free 总数：80^2+40^2+20^2

data = np.zeros((1, 4 + NC, NANCH), dtype=np.float32)
data[0, 0, :] = 100.0  # cx
data[0, 1, :] = 100.0  # cy
data[0, 2, :] = 20.0   # w
data[0, 3, :] = 20.0   # h
data[0, 5, :] = 0.99   # 类别 1 分数（索引 4+1），其余类别 0

const = helper.make_node("Constant", inputs=[], outputs=["out"],
                         value=numpy_helper.from_array(data))
graph = helper.make_graph(
    [const], "meta_test",
    [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
    [helper.make_tensor_value_info("out", TensorProto.FLOAT, [1, 4 + NC, NANCH])],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
# ultralytics 实际嵌入格式：Python dict repr，单引号 + 数字键
model.metadata_props.add(key="names", value="{0: 'zero_cls', 1: 'target_cls'}")
model.metadata_props.add(key="task", value="detect")

onnx.checker.check_model(model)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
onnx.save(model, OUT)
print("written:", OUT, os.path.getsize(OUT), "bytes")
