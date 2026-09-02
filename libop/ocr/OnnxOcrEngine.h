#pragma once
#include "OcrService.h"
#include <memory>
#include <string>
#include <vector>

namespace op::ocr {

// 进程内 ONNX Runtime OCR 引擎（内置，默认）。
// 使用 pimpl 隔离 onnxruntime 头依赖，使其它翻译单元（如 ImageSearchService.cpp）无需 onnxruntime 头即可编译。
class OnnxOcrEngine : public OcrEngine {
public:
  OnnxOcrEngine();
  ~OnnxOcrEngine() override;
  int init(const std::wstring &engine, const std::wstring &dllName,
           const std::vector<std::string> &argv) override;
  int ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) override;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace op::ocr
