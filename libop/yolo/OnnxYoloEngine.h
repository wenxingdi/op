#pragma once
#include "YoloService.h"
#include <memory>

namespace op::yolo {

// 进程内 ONNX Runtime YOLO 引擎。pimpl 隔离 onnxruntime 头依赖。
//
// 模型来源（方案B，默认不内置）：
//   * SetYoloEngine("onnx", "", ...)            -> 内置资源段模型（构建期检测到
//     build/_deps/yolo_models/yolo.onnx 才编入，否则 init 报错）
//   * SetYoloEngine("onnx", "D:/xx/best.onnx", ...) -> 外挂模型文件
//
// 版本自适应：按输出张量形状自动识别，无需显式版本参数——
//   [1, anchors, 5+nc]（anchors=3*8400@640）-> v5 系（objectness）
//   [1, 4+nc, 8400]                          -> v8/v11 通道在前
//   [1, 8400, 4+nc]                          -> v8/v11 转置
// nc 从形状推导；标准 ultralytics export format=onnx 检测模型均可。
//
// argv：
//   --conf=0.25   默认置信度阈值（detect 传 conf<=0 时生效）
//   --iou=0.45    默认 NMS IoU 阈值（detect 传 iou<=0 时生效）
//   --labels=a,b,c 可选类别名（UTF-8 逗号分隔；缺省 label 为空、只用 class_id）
class OnnxYoloEngine : public YoloEngine {
  public:
    OnnxYoloEngine();
    ~OnnxYoloEngine() override;
    int init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) override;
    int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace op::yolo
