#pragma once
#include "../base/Types.h"
#include <mutex>
#include <memory>
#include <map>
#include <vector>
#include <string>

namespace op::ocr {

// 逐字识别结果（P0 2026-11-20）：一行文本里单个字符的置信度与几何信息。
// 与 ocr_rec_t 的整行 confidence 口径**不同、不可互推**（见
// docs/2026-11/P0_逐字坐标JSON_实施方案.md）：
//   - 行级 confidence：所有时间步（含 blank 与重复步）的概率平均（历史口径，保持不变）
//   - 字级 confidence：仅该字符占用的时间步的概率平均
// 坐标恒为原图像素坐标（已除回入口放大 oscale）。
struct ocr_char_t {
  std::wstring ch;      // 单字符（CTCLoss 一个输出步对应一个字典条目 = 一个完整字符）
  float confidence = 0; // 字级置信度（见上）
  int t0 = 0;           // 占用时间步区间（闭区间）
  int t1 = 0;
  int x1 = 0; // 原图像素坐标 [x1, x2)
  int x2 = 0;
};
struct ocr_line_ext_t {
  std::vector<ocr_char_t> chars;
  bool partial_chars = false; // true = 该行被旋转，chars 已退化为整行框
  bool is_partial = false;    // true = 框底贴图边界，可能没读全（P5）
};
using vocr_line_ext_t = std::vector<ocr_line_ext_t>;

// OCR 分项耗时自省（P3 2026-11-21）。
// 只保留"上一次 ocr_ext/ocr_line_ext 调用"的数据——不做历史环形缓冲，也不做
// p50/p99 聚合：那会让锁与内存开销失控，而统计交给调用方拿多次 total_ms 自己算。
// ⚠ 并发调用同一引擎实例时，数值对应"任意一次"调用（最后写入者胜：
//   profile 是诊断信息，加锁采样的开销会污染计时本身）。
struct ocr_profile_t {
    double pre_ms = 0;   // 入口预处理（放大 + det 输入 resize/归一化）
    double det_ms = 0;   // det 的 session.Run
    double cls_ms = 0;   // cls 的 session.Run（0/180 方向修正）
    double rec_ms = 0;   // rec 的 session.Run（批量，N 框一次）
    double post_ms = 0;  // DB 后处理 + 逐字映射 + 结果组装
    double total_ms = 0; // 从入口到返回的总耗时
    long boxes = 0;      // det 出来的框数
    long lines = 0;      // 最终非空文本行数
    long chars = 0;      // 逐字条目总数
    long det_calls = 0;
    long rec_calls = 0;
    long cls_calls = 0;
    long runs_failed = 0; // ort_run 失败次数（异常被吞的情况）
    bool upscale = false; // 是否走了入口 2x 放大
    float oscale = 1.0f;  // 放大倍数
    long rec_width = 0;   // 本次 rec 实际用的 RW（P2 动态宽度的可观测值）
    bool valid = false;   // false = 还没跑过任何一次 OCR
};

// 抽象 OCR 引擎接口：所有 OCR 后端统一实现此接口。
// 当前仅内置进程内 ONNX 引擎；HTTP 远程后端已隐藏（条件宏 OP_ENABLE_HTTP_OCR_BACKEND，
// 定义后在 OcrService.cpp 恢复，且需把 network/HttpClient.cpp 加回 CMake 源列表）。
class OcrEngine {
public:
  virtual ~OcrEngine() = default;
  // engine: 引擎标识（"onnx"/空/未知=内置；http(s):// 及远程别名=已移除，init 拒绝）
  // dllName/argv: 透传参数（如 --threads=4、--model-dir=path）
  virtual int init(const std::wstring &engine, const std::wstring &dllName,
                   const std::vector<std::string> &argv) = 0;
  // 对 w×h×bpp 像素 buffer 做 OCR，结果写入 result（bbox + text + confidence）
  virtual int ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) = 0;
  // 带逐字信息的 ocr（P0）：result 与 ocr() 逐项一致（同序、同坐标、同文本、
  // 同行级 confidence），ext 为等长的逐字附加信息。默认实现退化为 ocr() 且
  // ext 置空——调用方据此判断"本引擎不支持逐字"。
  virtual int ocr_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t &ext) {
    ext.clear();
    int ret = ocr(data, w, h, bpp, result);
    ext.assign(result.size(), ocr_line_ext_t{});
    return ret;
  }
  // 单行直识别：跳过检测，对整图直接做 rec（快路径，适用于读出区/固定单行文本）。
  // 默认实现转调 ocr()（det+rec），兼容无单行能力的后端。
  virtual int ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    return ocr(data, w, h, bpp, result);
  }
  // 带逐字信息的单行直识别（P0）。默认实现退化：ext 置空。
  virtual int ocr_line_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result,
                           vocr_line_ext_t &ext) {
    ext.clear();
    int ret = ocr_line(data, w, h, bpp, result);
    ext.assign(result.size(), ocr_line_ext_t{});
    return ret;
  }
  // 上一次 OCR 的分项耗时（P3）。默认实现返回 valid=false（本引擎不采集）。
  virtual ocr_profile_t profile() const { return ocr_profile_t{}; }
};

#ifdef OP_ENABLE_HTTP_OCR_BACKEND
// HTTP 后端（原 HttpOcrService 的 HTTP 实现），保留作可选远程兜底。
// 当前默认不编译：需在编译定义 OP_ENABLE_HTTP_OCR_BACKEND 且加回 network/HttpClient.cpp。
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
#endif // OP_ENABLE_HTTP_OCR_BACKEND

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
  // 带逐字信息的变体（P0）：引擎不支持时 ext 为空（长度 0），result 语义不变。
  int ocr_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t &ext);
  int ocr_line_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t &ext);
  // 上一次 OCR 的分项耗时（P3）。引擎不支持 profile 或还没跑过 → valid=false。
  ocr_profile_t last_profile();
  // OCR 预热（P2 第二十六批 2026-10-16）：触发懒加载 + 一次小图推理，
  // 消掉首次真实调用的模型加载延迟。返回 0 成功（引擎就绪），-1 引擎不可用。
  int warmup();

private:
  HttpOcrService();
  HttpOcrService(const HttpOcrService &) = delete;
  HttpOcrService &operator=(const HttpOcrService &) = delete;
  // 锁内复用的引擎选择+加载逻辑（init/懒初始化共用，调用方必须已持锁）。
  int init_unlocked(const std::wstring &engine, const std::wstring &dllName,
                    const std::vector<std::string> &argv);
  // 懒初始化公共路径（ocr/ocr_line 共用）：加锁 → 判空 → 懒初始化 → 返回引擎快照。
  // 失败只试一次（m_lazy_failed），显式 init 会清除该记忆；返回 nullptr = 引擎不可用。
  std::shared_ptr<OcrEngine> lazy_engine(const char *caller);
  std::mutex m_mutex;
  // shared_ptr 而非 unique_ptr：ocr()/ocr_line() 锁内取快照后在锁外调用引擎（引擎可重入），
  // 并发推理期间 release()/重 init 不会销毁正在使用的引擎实例。
  std::shared_ptr<OcrEngine> m_engine; // 当前选中的引擎（默认内置 OnnxOcrEngine）
  bool m_lazy_failed = false;          // 懒初始化已尝试且失败：只试一次，后续快速返回 -1
  // B8 多档热切换：已加载引擎缓存（key -> engine）。同一档位+同一 argv 命中缓存后，
  // 切换为零成本（只换活动指针），首次使用的档位懒加载。
  //
  // 容量上限（2026-11-20 修 bug）：每个内置引擎实例各持有一份 Ort::Env + 完整模型
  // buffer + det/rec/cls 三个 session，实测常驻约 240MB。原实现无上限缓存且永不释放，
  // 进程内切过 4 种配置后内存涨到 ~950MB，此后新建实例的 session 创建失败 →
  // 该档位静默返回 0 行结果（不是崩溃，最难察觉）。改为 LRU 上限，
  // 超出时淘汰最久未用的实例（正在被 m_engine 引用的不会被淘汰）。
  static constexpr size_t kMaxCachedEngines = 2;
  // 插入顺序 + 命中时移到末尾 = LRU；值为 (cache_key, engine)。
  std::map<std::string, std::shared_ptr<OcrEngine>> m_engine_cache;
  std::vector<std::string> m_cache_lru; // 最近使用顺序，末尾最新
  // 记录每个引擎实例是否已成功跑过至少一次推理（失败实例不占缓存名额，下次重建）。
  // 归一化档位名：空/"onnx"/"builtin"/未知 -> "v6"（默认精度的档）；"v4" -> 快档。
  static std::string normalize_engine_name(const std::string &eng);
  // 缓存淘汰：调用方必须已持锁。保留 m_engine 引用的实例，其余按 LRU 淘汰到 cap 以内。
  void evict_cache_unlocked();
  void evict_cache_to_unlocked(size_t cap);
  // 取（或首次创建并初始化）名为 name 的内置引擎；调用方必须已持锁。失败返回 nullptr。
  std::shared_ptr<OcrEngine> builtin_engine_unlocked(const std::string &name,
                                                     const std::wstring &dllName,
                                                     const std::vector<std::string> &argv);
};

} // namespace op::ocr
