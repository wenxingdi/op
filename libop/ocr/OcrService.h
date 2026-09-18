#pragma once
#include "../base/Types.h"
#include <mutex>
#include <memory>
#include <vector>
#include <string>

namespace op::ocr {

// 抽象 OCR 引擎接口：所有 OCR 后端（HTTP 远程 / 进程内 ONNX）统一实现此接口。
class OcrEngine {
public:
  virtual ~OcrEngine() = default;
  // engine: 引擎标识（"onnx"/空=内置；http(s):// 或 backend 别名=HTTP）
  // dllName/argv: 透传参数（如 --timeout=3000、--model-dir=path）
  virtual int init(const std::wstring &engine, const std::wstring &dllName,
                   const std::vector<std::string> &argv) = 0;
  // 对 w×h×bpp 像素 buffer 做 OCR，结果写入 result（bbox + text + confidence）
  virtual int ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) = 0;
  // 单行直识别：跳过检测，对整图直接做 rec（快路径，适用于读出区/固定单行文本）。
  // 默认实现转调 ocr()（det+rec），兼容无单行能力的后端（如 HTTP）。
  virtual int ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    return ocr(data, w, h, bpp, result);
  }
};

// HTTP 后端（原 HttpOcrService 的 HTTP 实现），保留作可选远程兜底。
class HttpOcrEngine : public OcrEngine {
public:
  HttpOcrEngine();
  ~HttpOcrEngine() override;
  int init(const std::wstring &engine, const std::wstring &dllName,
           const std::vector<std::string> &argv) override;
  int ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) override;
  int release();

private:
  std::mutex m_mutex;
  std::string m_endpoint;
  int m_timeout_ms;
};

class OnnxOcrEngine; // 前向声明，完整定义见 OnnxOcrEngine.h

// OCR 引擎管理器（单例）：按 engine 名选择具体引擎实现，对外保持原 HttpOcrService 调用接口不变。
class HttpOcrService {
public:
  static HttpOcrService *getInstance();
  ~HttpOcrService();
  int init(const std::wstring &engine, const std::wstring &dllName,
           const std::vector<std::string> &argv);
  int release();
  int ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result);
  int ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result);

private:
  HttpOcrService();
  HttpOcrService(const HttpOcrService &) = delete;
  HttpOcrService &operator=(const HttpOcrService &) = delete;
  // 锁内复用的引擎选择+加载逻辑（init/懒初始化共用，调用方必须已持锁）。
  int init_unlocked(const std::wstring &engine, const std::wstring &dllName,
                    const std::vector<std::string> &argv);
  std::mutex m_mutex;
  std::unique_ptr<OcrEngine> m_engine; // 当前选中的引擎（默认内置 OnnxOcrEngine）
  bool m_lazy_failed = false;          // 懒初始化已尝试且失败：只试一次，后续快速返回 -1
};

} // namespace op::ocr
