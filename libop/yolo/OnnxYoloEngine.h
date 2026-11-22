#pragma once
#include "YoloService.h"
#include <memory>

// 同 OnnxOcrEngine.h：OP_HAS_ONNX=0（如 x86 无 onnxruntime）时整个类不声明。
#if defined(OP_HAS_ONNX) && OP_HAS_ONNX

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
//   --threads=N    intra-op 线程数；0/缺省 = 交给 ORT 用物理核（不要再写死 1，
//                  2026-11-21 实测单线程比默认慢 3.5~4.8 倍）。引擎是进程级单例，
//                  上一次的 --threads 会保留，传 --threads=0 可显式改回默认。
class OnnxYoloEngine : public YoloEngine {
  public:
    OnnxYoloEngine();
    ~OnnxYoloEngine() override;
    int init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) override;
    int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) override;
    // 批 C（2026-11-22）：任务维度。任务不符时返回 -1（明确"不支持"），不是 0。
    int classify(byte *data, int w, int h, int bpp, int top_k, vyolo_cls_t &result) override;
    int segment(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_seg_t &result) override;
    Task task() const override;

    // 诊断/测试专用：**实际写进 SessionOptions** 的 intra-op 线程数（0 = 未设置，用 ORT 默认）。
    // 故意暴露"已生效值"而不是"请求值"——否则把实现改回写死单线程时，用例照样绿。
    int intra_op_threads() const;

    // 诊断/测试专用：加载时判定的任务类型。
    Task detected_task() const;

    // 诊断/测试专用：模型输入边长。
    int input_width() const;
    int input_height() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace op::yolo

#endif // OP_HAS_ONNX
