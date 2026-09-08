#include "YoloDetector.h"
#include "HttpYoloEngine.h"
#include "OnnxYoloEngine.h"
#include <iostream>

using std::cout;
using std::endl;

namespace op::yolo {

namespace {
// engine 名以 "onnx" 开头（onnx / onnx_yolo / onnxruntime ...）-> 内置 ONNX 引擎
bool is_onnx_engine_name(const std::wstring &engine) {
    std::wstring e = engine;
    for (auto &c : e)
        c = static_cast<wchar_t>(towlower(c));
    return e.rfind(L"onnx", 0) == 0;
}
} // namespace

YoloDetector::YoloDetector() : m_engine(std::make_unique<HttpYoloEngine>()) {
    cout << "YoloDetector::YoloDetector(), default engine=http" << endl;
}

YoloDetector::~YoloDetector() {
    release();
}

YoloDetector *YoloDetector::getInstance() {
    static YoloDetector sYoloEngine;
    return &sYoloEngine;
}

int YoloDetector::init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argvs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 注意：一旦 init 成功切到 onnx 引擎，后续再调 init("yolo",...) 会切回 HTTP。
    // 这是显式选择，不做粘性记忆。
    if (is_onnx_engine_name(engine)) {
        if (dynamic_cast<OnnxYoloEngine *>(m_engine.get()) == nullptr)
            m_engine = std::make_unique<OnnxYoloEngine>();
    } else {
        if (dynamic_cast<HttpYoloEngine *>(m_engine.get()) == nullptr)
            m_engine = std::make_unique<HttpYoloEngine>();
    }
    return m_engine->init(engine, dllName, argvs);
}

int YoloDetector::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 释放引擎持有的模型资源（ONNX 会话/内置模型 buffer），回到默认 HTTP
    if (dynamic_cast<HttpYoloEngine *>(m_engine.get()) == nullptr)
        m_engine = std::make_unique<HttpYoloEngine>();
    return 0;
}

int YoloDetector::detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 串行化 detect：ONNX 会话共享，避免并发内存峰值；HTTP 旧版虽支持并发，
    // 但推理本身是重负载，串行影响可忽略。
    return m_engine->detect(data, w, h, bpp, conf, iou, result);
}

} // namespace op::yolo
