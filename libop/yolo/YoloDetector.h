#pragma once
#include "YoloService.h"
#include <memory>
#include <mutex>

namespace op::yolo {

// 引擎选择器（单例，对外接口不变）。
// SetYoloEngine 的 engine 参数以 "onnx" 开头 -> 进程内 OnnxYoloEngine；
// 其余（含空、"yolo"、"yolo_http" 等）-> HttpYoloEngine（旧行为，默认）。
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
