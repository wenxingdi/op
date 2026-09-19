#include "YoloDetector.h"
#include "OnnxYoloEngine.h"
#ifdef OP_ENABLE_HTTP_YOLO_BACKEND
#include "HttpYoloEngine.h"
#endif
#include <iostream>

using std::cout;
using std::endl;

namespace op::yolo {

namespace {
#ifdef OP_ENABLE_HTTP_YOLO_BACKEND
// engine 名以 "onnx" 开头（onnx / onnx_yolo / onnxruntime ...）-> 内置 ONNX 引擎
bool is_onnx_engine_name(const std::wstring &engine) {
    std::wstring e = engine;
    for (auto &c : e)
        c = static_cast<wchar_t>(towlower(c));
    return e.rfind(L"onnx", 0) == 0;
}
#endif

#ifndef OP_ENABLE_HTTP_YOLO_BACKEND
// HTTP 远程后端隐藏期：http(s):// 地址与旧远程别名一律视为"已移除"，
// init 显式拒绝并提示，避免静默落到 ONNX 引擎造成语义漂移。
bool is_removed_remote_name(const std::wstring &engine) {
    std::wstring e = engine;
    for (auto &c : e)
        c = static_cast<wchar_t>(towlower(c));
    return e.rfind(L"http", 0) == 0 || e == L"yolo" || e == L"yolo11" || e == L"yolov11" ||
           e == L"yolo_http" || e == L"yolo_server";
}
#endif
} // namespace

// 默认引擎 = OnnxYoloEngine（构造轻量，模型在 init 时才加载）。
// detect 在未 init 时经引擎内部 ok 门禁返回 -1，不崩溃。
YoloDetector::YoloDetector() : m_engine(std::make_unique<OnnxYoloEngine>()) {
    cout << "YoloDetector::YoloDetector(), default engine=onnx (built-in)" << endl;
}

YoloDetector::~YoloDetector() {
    release();
}

YoloDetector *YoloDetector::getInstance() {
    // 故意泄漏（同 HttpOcrService::getInstance）：持有 ORT Session 的引擎
    // 不能在进程退出的静态析构里拆除，否则 ORT 线程池 teardown 死锁挂起。
    static YoloDetector *sYoloEngine = new YoloDetector();
    return sYoloEngine;
}

int YoloDetector::init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argvs) {
    std::lock_guard<std::mutex> lock(m_mutex);
#ifdef OP_ENABLE_HTTP_YOLO_BACKEND
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
#else
    if (is_removed_remote_name(engine)) {
        cout << "SetYoloEngine: HTTP backend removed (self-trained ONNX model only, remote aliases "
                "yolo/yolo11/yolo_http/yolo_server rejected); define OP_ENABLE_HTTP_YOLO_BACKEND to restore"
             << endl;
        return -1;
    }
    if (dynamic_cast<OnnxYoloEngine *>(m_engine.get()) == nullptr)
        m_engine = std::make_unique<OnnxYoloEngine>();
    return m_engine->init(engine, dllName, argvs);
#endif
}

int YoloDetector::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 释放引擎持有的模型资源（ONNX 会话/内置模型 buffer），回到默认 ONNX 引擎（未加载态）
    m_engine = std::make_unique<OnnxYoloEngine>();
    return 0;
}

int YoloDetector::detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 串行化 detect：ONNX 会话共享，避免并发内存峰值；推理本身是重负载，串行影响可忽略。
    return m_engine->detect(data, w, h, bpp, conf, iou, result);
}

} // namespace op::yolo
