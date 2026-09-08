#pragma once
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
