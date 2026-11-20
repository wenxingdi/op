#pragma once
#include "OcrService.h"
#include <memory>
#include <string>
#include <vector>

// OP_HAS_ONNX 由 CMake 按「本架构是否存在 onnxruntime 包」定义（x86 = 0）。
// 为 0 时整个类不声明：OnnxOcrEngine.cpp 也会从源列表剔除，调用方必须用同一宏
// 包住使用点，否则会链接到不存在的实现。
#if defined(OP_HAS_ONNX) && OP_HAS_ONNX

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
  // 带逐字信息的 ocr（P0）：result 与 ocr() 完全一致，ext 为等长逐字附加信息。
  int ocr_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t &ext) override;
  // 单行直识别：整图直接 rec，跳过 det。bbox 恒为整图 (0,0)-(w,h)。
  int ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) override;
  int ocr_line_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t &ext) override;

private:
  // ocr/ocr_ext 共享实现：ext_out 非空时额外回填逐字信息。
  int ocr_common(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t *ext_out);
  // ocr_line/ocr_line_ext 共享实现。
  int ocr_line_common(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t *ext_out);
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace op::ocr

#endif // OP_HAS_ONNX
