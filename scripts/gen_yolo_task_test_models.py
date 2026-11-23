# -*- coding: utf-8 -*-
"""生成批 C（YOLO 任务维度）的测试资产。

两个模型，对应两条新增的任务路径：

1) tests/testdata/yolo_cls_test.onnx   —— 分类
   输入 images [1,3,640,640]，输出 out [1,5]（softmax 后的分数）。
   期望：YoloClassify 返回 5 条候选、第 0 条 class_id=0，且按 score 降序。
   故意让 class 2 得分最高（0.71），用来验证"排序"而不是"数组顺序"。

2) tests/testdata/yolo_seg_test.onnx   —— 分割
   两个输出：
     detection [1, 6+2, 1]   检测头：cx,cy,w,h + 1 类分数 + 2 个掩码系数（nm=2）
     注意行数 = 4 + nc + nm = 4 + 1 + 2 = 7 —— 下面用 NC+NM 算出
     proto     [1, 2, 20, 20] 掩码原型
   模型只有一个 anchor（nanch=1），所以解码路径必然命中，且不受 NMS 影响。
   期望：YoloSegment 返回 1 条，polygon 非空、mask_area>0，框落在 letterbox 映射后的
   位置（用 640x640 输入、640x640 原图，scale=1、pad=0，断言可以做得很准）。

依赖：onnx + numpy（托管 venv：C:/Users/lc/.workbuddy/binaries/python/envs/default/Scripts/python.exe）
重生成：python scripts/gen_yolo_task_test_models.py
"""
import os

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "tests", "testdata")

# ---------------- 分类模型 ----------------
CLS_NAMES = ["alpha", "bravo", "charlie", "delta", "echo"]
CLS_SCORES = [0.15, 0.05, 0.71, 0.06, 0.03]  # 和 = 1.00，class 2 最高


def build_classify():
    data = np.array([CLS_SCORES], dtype=np.float32).reshape(1, len(CLS_SCORES))
    const = helper.make_node("Constant", inputs=[], outputs=["out"],
                             value=numpy_helper.from_array(data))
    graph = helper.make_graph(
        [const], "cls_test",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
        [helper.make_tensor_value_info("out", TensorProto.FLOAT, list(data.shape))],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    names = "{0: 'alpha', 1: 'bravo', 2: 'charlie', 3: 'delta', 4: 'echo'}"
    model.metadata_props.add(key="names", value=names)
    model.metadata_props.add(key="task", value="classify")
    onnx.checker.check_model(model)
    out = os.path.join(OUT_DIR, "yolo_cls_test.onnx")
    onnx.save(model, out)
    print("written:", out, os.path.getsize(out), "bytes")


# ---------------- 分割模型 ----------------
NC = 1     # 1 个类别
NM = 2     # 2 个掩码系数
NANCH = 1  # 单 anchor：解码必然命中，且不会被 NMS 吃掉

# 检测头：一个居中的 40x40 框（640 空间），类别分数 0.9
CX, CY, W, H, CONF = 320.0, 320.0, 160.0, 160.0, 0.9

# proto 是 [2, 20, 20]（640/32 = 20）。让两个原型在中心区域分别为正/负，
# 系数取 (1.0, 1.0) 时中心 8x8 区域 sigmoid(proto0+proto1) > 0.5 形成实心块。
PROTO_PS = 20


def build_segment():
    rows = 4 + NC + NM  # 7
    det = np.zeros((1, rows, NANCH), dtype=np.float32)
    det[0, 0, 0] = CX
    det[0, 1, 0] = CY
    det[0, 2, 0] = W
    det[0, 3, 0] = H
    det[0, 4, 0] = CONF          # class 0 分数
    det[0, 5, 0] = 1.0           # mask coeff 0
    det[0, 6, 0] = 1.0           # mask coeff 1

    proto = np.zeros((1, NM, PROTO_PS, PROTO_PS), dtype=np.float32)
    lo, hi = 6, 14               # 中心 8x8 的 proto 区域
    proto[0, 0, lo:hi, lo:hi] = 2.0   # sigmoid(2+2)=0.98 > 0.5
    proto[0, 1, lo:hi, lo:hi] = 2.0

    det_const = helper.make_node("Constant", inputs=[], outputs=["detection"],
                                 value=numpy_helper.from_array(det))
    proto_const = helper.make_node("Constant", inputs=[], outputs=["proto"],
                                   value=numpy_helper.from_array(proto))
    graph = helper.make_graph(
        [det_const, proto_const], "seg_test",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
        [helper.make_tensor_value_info("detection", TensorProto.FLOAT, [1, rows, NANCH]),
         helper.make_tensor_value_info("proto", TensorProto.FLOAT, [1, NM, PROTO_PS, PROTO_PS])],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    model.metadata_props.add(key="names", value="{0: 'blob'}")
    model.metadata_props.add(key="task", value="segment")
    onnx.checker.check_model(model)
    out = os.path.join(OUT_DIR, "yolo_seg_test.onnx")
    onnx.save(model, out)
    print("written:", out, os.path.getsize(out), "bytes")


# ---------------- 姿态模型 ----------------
POSE_NC = 1
POSE_KPT = 3
POSE_CX, POSE_CY, POSE_W, POSE_H = 320.0, 320.0, 160.0, 160.0
POSE_CONF = 0.9
# 关键点在 640x640 模型输入空间的位置与可见度
POSE_KEYPOINTS = [(320.0, 320.0, 0.9), (300.0, 320.0, 0.8), (340.0, 320.0, 0.7)]


def build_pose():
    rows = 4 + POSE_NC + 3 * POSE_KPT
    det = np.zeros((1, rows, 1), dtype=np.float32)
    det[0, 0, 0] = POSE_CX
    det[0, 1, 0] = POSE_CY
    det[0, 2, 0] = POSE_W
    det[0, 3, 0] = POSE_H
    det[0, 4, 0] = POSE_CONF
    for i, (x, y, c) in enumerate(POSE_KEYPOINTS):
        base = 4 + POSE_NC + 3 * i
        det[0, base + 0, 0] = x
        det[0, base + 1, 0] = y
        det[0, base + 2, 0] = c

    const = helper.make_node("Constant", inputs=[], outputs=["pose"],
                             value=numpy_helper.from_array(det))
    graph = helper.make_graph(
        [const], "pose_test",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
        [helper.make_tensor_value_info("pose", TensorProto.FLOAT, [1, rows, 1])],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    model.metadata_props.add(key="names", value="{0: 'person'}")
    model.metadata_props.add(key="task", value="pose")
    model.metadata_props.add(key="kpt_shape", value=f"[{POSE_KPT}, 3]")
    onnx.checker.check_model(model)
    out = os.path.join(OUT_DIR, "yolo_pose_test.onnx")
    onnx.save(model, out)
    print("written:", out, os.path.getsize(out), "bytes")


# ---------------- 旋转框模型 ----------------
OBB_NC = 1
OBB_NANCH = 2
OBB_CONF = 0.9


def build_obb():
    rows = 4 + OBB_NC + 1
    det = np.zeros((1, rows, OBB_NANCH), dtype=np.float32)
    # anchor 0：中心 320,320，160x80，角度 0
    det[0, 0, 0], det[0, 1, 0], det[0, 2, 0], det[0, 3, 0] = 320.0, 320.0, 160.0, 80.0
    det[0, 4, 0], det[0, 5, 0] = OBB_CONF, 0.0
    # anchor 1：几乎重合、置信度略低 -> 必须被旋转 NMS 抑制
    det[0, 0, 1], det[0, 1, 1], det[0, 2, 1], det[0, 3, 1] = 322.0, 321.0, 160.0, 80.0
    det[0, 4, 1], det[0, 5, 1] = 0.8, 0.0

    const = helper.make_node("Constant", inputs=[], outputs=["obb"],
                             value=numpy_helper.from_array(det))
    graph = helper.make_graph(
        [const], "obb_test",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])],
        [helper.make_tensor_value_info("obb", TensorProto.FLOAT, [1, rows, OBB_NANCH])],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    model.metadata_props.add(key="names", value="{0: 'ship'}")
    model.metadata_props.add(key="task", value="obb")
    onnx.checker.check_model(model)
    out = os.path.join(OUT_DIR, "yolo_obb_test.onnx")
    onnx.save(model, out)
    print("written:", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    build_classify()
    build_segment()
    build_pose()
    build_obb()
