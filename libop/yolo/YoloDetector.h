#pragma once
#include "YoloService.h"
#include <memory>
#include <mutex>

namespace op::yolo {

// 引擎选择器（单例，对外接口不变）。
// HTTP 远程后端已隐藏（2026-09-18 决策）：YOLO 依赖自训 ONNX 模型，不走 HTTP 服务。
// 现行为：engine 以 "onnx" 开头 / 空 / 未知 / .onnx 路径（OpYolo 层已归一）-> OnnxYoloEngine；
// http(s):// 及旧远程别名（yolo/yolo11/yolo_http/yolo_server）-> init 拒绝返 0。
// 如需恢复 HTTP：定义 OP_ENABLE_HTTP_YOLO_BACKEND，把 yolo/HttpYoloEngine.cpp 与
// network/HttpClient.cpp 加回 CMake 源列表，并在 YoloDetector.cpp 打开对应分支。
class YoloDetector {
  private:
    YoloDetector();

  public:
    YoloDetector(const YoloDetector &) = delete;
    YoloDetector &operator=(const YoloDetector &) = delete;
    static YoloDetector *getInstance();
    ~YoloDetector();
    int init(const std::wstring &enginePath, const std::wstring &dllName, const vector<string> &argv);
    int release();
    int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result);

  private:
    std::mutex m_mutex;
    std::unique_ptr<YoloEngine> m_engine;
};

} // namespace op::yolo
