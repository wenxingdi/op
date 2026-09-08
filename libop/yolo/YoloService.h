#pragma once
#include "../base/Types.h"
#include <string>
#include <vector>

namespace op::yolo {

// YOLO 引擎抽象：HTTP 远程 / 进程内 ONNX。
// init 语义与 SetYoloEngine(engine, dllName, argv) 对齐；detect 输入像素
// buffer（bpp=1/3/4），conf/iou <=0 时用引擎默认值。
class YoloEngine {
  public:
    virtual ~YoloEngine() = default;
    virtual int init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) = 0;
    virtual int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) = 0;
};

} // namespace op::yolo
