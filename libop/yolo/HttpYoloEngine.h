#pragma once
// 【已隐藏，不入构建】2026-09-18 决策：YOLO 依赖自训 ONNX 模型，不走 HTTP 服务。
// 本文件保留供后期需要时恢复：把 yolo/HttpYoloEngine.cpp 与 network/HttpClient.cpp
// 加回 libop/CMakeLists.txt 源列表，并在 YoloDetector.cpp 定义 OP_ENABLE_HTTP_YOLO_BACKEND。
#include "YoloService.h"
#include <string>

namespace op::yolo {

// HTTP 远程推理引擎（原 YoloDetector 逻辑平移）。
// endpoint 来自 OP_YOLO_URL / OP_YOLO_BACKEND 环境变量或 init() 的 configure_endpoint。
class HttpYoloEngine : public YoloEngine {
  public:
    HttpYoloEngine();
    ~HttpYoloEngine() override;
    int init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) override;
    int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) override;

  private:
    std::string m_endpoint;
    int m_timeout_ms;
};

} // namespace op::yolo
