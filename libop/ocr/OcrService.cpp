#include "OcrService.h"
#include "OnnxOcrEngine.h"
#include "../base/Utils.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>

using std::cout;
using std::endl;

namespace op::ocr {

namespace {
// HTTP 后端隐藏期的远程名判定：http(s):// 地址与旧远程别名（tesseract/paddle 系）
// 一律视为"已移除"，init 显式拒绝并提示，避免静默落到内置引擎造成语义漂移。
// 必须自包含：trim/to_lower 曾定义于 network/HttpClient.h（该文件 2026-11-26 已随
// HTTP 后端删除），不能为省 13 行把它们拖回依赖。
bool is_removed_remote_backend(const std::string &engine) {
    std::string key;
    key.reserve(engine.size());
    for (unsigned char ch : engine) {
        if (ch == ' ' || ch == '\t')
            continue; // trim
        key += static_cast<char>(std::tolower(ch));
    }
    return key.rfind("http", 0) == 0 || key == "tesseract" || key == "tess" ||
           key == "paddle" || key == "paddleocr" || key == "paddle_ocr" || key == "paddle_ncnn";
}
} // namespace

// ---- OcrService（OCR 引擎管理器单例）----
OcrService::OcrService() = default;
OcrService::~OcrService() = default;

OcrService *OcrService::getInstance() {
    // 故意泄漏（leak-by-design）：单例持有 OnnxOcrEngine -> Ort::Env/Session。
    // 若由静态对象析构，进程退出时 CRT 会在 DLL_PROCESS_DETACH 里拆除 ORT
    // 线程池，与尚未退出的 intra-op worker 死锁（实测：任何用过 OCR 的宿主
    // 进程退出必挂起，minidump 证实主线程卡在 op 静态析构 -> ORT teardown
    // -> WaitForMultipleObjects）。泄漏后退出阶段完全不触碰 ORT，由进程
    // 回收一切资源。宿主是短生命周期自动化脚本，泄漏可接受。
    static OcrService *sInstance = new OcrService();
    return sInstance;
}

int OcrService::init(const std::wstring &engine, const std::wstring &dllName,
                        const std::vector<std::string> &argv) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return init_unlocked(engine, dllName, argv);
}

int OcrService::init_unlocked(const std::wstring &engine, const std::wstring &dllName,
                                  const std::vector<std::string> &argv) {
    const std::string eng = _ws2string(engine);
    const std::string name = normalize_engine_name(eng);
    if (eng == "v4" || eng == "V4")
        cout << "SetOcrEngine: v4 fast tier removed, falling back to v6" << endl;
    // HTTP 远程后端已移除（2026-11-26）：http(s):// 与旧远程别名（tesseract/paddle 系）
    // 在 init 显式拒绝（dll_name 位同样检查——旧远程写法允许经 dll_name 传 URL），
    // 其余按档位名归一化后选内置引擎（统一 v6 精度档，v4 兼容回落）。
    if (is_removed_remote_backend(eng) || is_removed_remote_backend(_ws2string(dllName))) {
        cout << "SetOcrEngine: remote OCR backend removed (built-in ONNX v6 only)" << endl;
        return -1;
    }
    // 淘汰判据是"是否被 m_engine 引用"。此处 m_engine 仍指向**旧**实例，
    // 若不先松手，evict 会把旧实例当成在用而永远跳过它，缓存上限失效。
    // 先释放引用（调用方已持锁，且在锁内取 shared_ptr 快照后才推理，
    // 在途推理由各自的快照持有，不受影响），再交给 builtin_engine_unlocked 重设。
    auto old_engine = m_engine;
    m_engine.reset();
    m_engine = builtin_engine_unlocked(name, dllName, argv);
    // 引擎代次：活动**实例**真正更换才 +1（命中引擎缓存的零成本切换不加，
    // 否则 OCR 帧 memo 会被同配置重 init 无谓失效）。
    if (m_engine != old_engine)
        ++m_generation;
    if (!m_engine)
        return -1;
    cout << "OcrService: selected OnnxOcrEngine (built-in, tier=" << name
         << ", cached=" << m_engine_cache.size() << ")" << endl;
    m_lazy_failed = false; // 显式 init 成功后清掉懒初始化失败记忆
    return 0;
}

std::string OcrService::normalize_engine_name(const std::string &eng) {
    // 2026-11-24：V4 快档已移除。兼容旧脚本："v4"/"V4" 静默回落 v6。
    // 空 / "onnx" / "builtin" / "v6" / 未知 → 默认精度档 v6。
    return "v6";
}

// LRU 淘汰：调用方必须已持锁。每个内置引擎常驻约 240MB（Ort::Env + 模型 + 3 session），
// 无上限缓存会在进程内切过几种配置后耗尽内存，导致新实例 session 创建失败、静默返回 0 行。
// 保留 m_engine 正在引用的实例（shared_ptr 让在途推理也安全），其余按 m_cache_lru 淘汰。
void OcrService::evict_cache_unlocked() { evict_cache_to_unlocked(kMaxCachedEngines); }

void OcrService::evict_cache_to_unlocked(size_t cap) {
    // 最多尝试 LRU 队列长度的次数，避免"全是当前实例"时死循环
    size_t guard = m_cache_lru.size();
    while (m_engine_cache.size() > cap && guard-- > 0 && !m_cache_lru.empty()) {
        const std::string victim = m_cache_lru.front();
        m_cache_lru.erase(m_cache_lru.begin());
        auto it = m_engine_cache.find(victim);
        if (it == m_engine_cache.end()) continue; // 已在淘汰过程中移除
        if (it->second == m_engine) {
            // 正在使用的实例不可淘汰：挪到队尾，下一轮再看别人
            m_cache_lru.push_back(victim);
            continue;
        }
        cout << "OcrService: evict cached engine (cap=" << cap << ")" << endl;
        cout << "  evict key=" << victim << " engine=" << it->second.get() << " cur=" << m_engine.get()
             << " use_count=" << it->second.use_count() << endl;
        m_engine_cache.erase(it);
    }
}

std::shared_ptr<OcrEngine> OcrService::builtin_engine_unlocked(
    const std::string &name, const std::wstring &dllName, const std::vector<std::string> &argv) {
#if defined(OP_HAS_ONNX) && OP_HAS_ONNX
    // Cache key must include argv: --charset/--threads/--oscale/--cls are per-instance
    // settings; keying on tier name alone silently discards them on re-init (found
    // during CLS testing: "--cls=0" returned the cached cls=on engine).
    std::string key = name;
    for (const auto &a : argv) {
        key += '\x1f';
        key += a;
    }
    auto it = m_engine_cache.find(key);
    if (it != m_engine_cache.end()) {
        // 命中：标记为最近使用（移到 LRU 队尾）后零成本复用
        auto lru = std::find(m_cache_lru.begin(), m_cache_lru.end(), key);
        if (lru != m_cache_lru.end()) m_cache_lru.erase(lru);
        m_cache_lru.push_back(key);
        return it->second;
    }
    // 新建前先淘汰，给新实例腾出名额：缓存里最多留 kMaxCachedEngines-1 个，
    // 否则加载新实例时进程内会同时存在 kMaxCachedEngines+1 份模型（各约 240MB），
    // 峰值内存不足会让新 session 创建失败 → 该档位静默返回 0 行。
    // 注意：evict_cache_unlocked 的判据是 size() > kMaxCachedEngines，直接调用并
    // 不能在"刚好等于上限"时腾出名额，这里必须先收紧上限再调用。
    evict_cache_to_unlocked(kMaxCachedEngines > 0 ? kMaxCachedEngines - 1 : 0);
    auto e = std::make_shared<OnnxOcrEngine>();
    const std::wstring wname(name.begin(), name.end()); // 档位名 ASCII，直接扩宽
    if (e->init(wname, dllName, argv) != 0) {
        cout << "OcrService: engine init failed (tier=" << name << ")" << endl;
        return nullptr;
    }
    m_engine_cache[key] = e;
    m_cache_lru.push_back(key);
    // 建好后立刻把新实例登记为"当前"，再淘汰一次：新实例因此受保护，
    // 淘汰只可能落在别人身上。
    m_engine = e;
    evict_cache_unlocked();
    return e;
#else
    (void)name;
    (void)dllName;
    (void)argv;
    cout << "SetOcrEngine: built-in ONNX OCR not compiled (OP_HAS_ONNX=0); "
            "use dict-based ocr on this architecture" << endl;
    return nullptr;
#endif
}

int OcrService::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_engine.reset();
    ++m_generation; // 引擎代次 +1：OCR 帧 memo 等结果缓存据此失效
    return 0;
}

// 懒初始化：脚本未显式 SetOcrEngine 时，首次免字库调用自动就绪内置 ONNX 引擎。
// 失败只试一次（m_lazy_failed），避免每次调用重复加载/刷日志；显式 init 会清除该记忆。
std::shared_ptr<OcrEngine> OcrService::lazy_engine(const char *caller) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine) {
        if (m_lazy_failed)
            return nullptr;
        if (init_unlocked(L"", L"", {}) != 0 || !m_engine) {
            m_engine.reset(); // 加载失败的引擎实例不留着（其内部 ok=false，只会持续返回 -1）
            m_lazy_failed = true;
            cout << caller << ": lazy init of built-in engine failed, call SetOcrEngine explicitly" << endl;
            return nullptr;
        }
    }
    return m_engine;
}

//
// 锁纪律：m_mutex 只保护"引擎选择/懒初始化"这一小段，推理本体在锁外执行。
// OnnxOcrEngine 内部 Ort::Session 只读、无共享可变状态，可重入（见 OP_OCR_INTERNAL_PLAN 9.2）；
// 锁内 shared_ptr 快照保证并发 release()/重 init 时本调用持有的引擎不被销毁。
// 原先锁包住整个推理，多线程并发 OCR 被完全串行化（每次 130ms+），这里解锁拿回并发吞吐。
int OcrService::ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    const auto engine = lazy_engine("ocr");
    if (!engine)
        return -1;
    return engine->ocr(data, w, h, bpp, result);
}

int OcrService::ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    const auto engine = lazy_engine("ocr_line");
    if (!engine)
        return -1;
    return engine->ocr_line(data, w, h, bpp, result);
}

// 带逐字信息的变体（P0）：不解锁额外路径，直接转发到引擎的 ocr_ext/ocr_line_ext。
// 引擎不支持逐字时（基类默认实现）ext 为空，result 语义与 ocr()/ocr_line() 完全一致。
int OcrService::ocr_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result, vocr_line_ext_t &ext) {
    const auto engine = lazy_engine("ocr_ext");
    if (!engine) {
        ext.clear();
        return -1;
    }
    return engine->ocr_ext(data, w, h, bpp, result, ext);
}

int OcrService::ocr_line_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result,
                                 vocr_line_ext_t &ext) {
    const auto engine = lazy_engine("ocr_line_ext");
    if (!engine) {
        ext.clear();
        return -1;
    }
    return engine->ocr_line_ext(data, w, h, bpp, result, ext);
}

int OcrService::warmup() {
    const auto engine = lazy_engine("warmup");
    if (!engine)
        return -1;
    // 16x16 纯白 BGRA。注意引擎接口的 bpp 是**每像素字节数**（1/3/4），不是位深。
    // ocr() 返回值语义是"识别出的文本条数"：>=0 即引擎可用
    // （0=无文本/无检出，正数=检出条数，均视为预热成功；负数才是错误码）。
    std::vector<byte> pixels(16 * 16 * 4, 255);
    vocr_rec_t result;
    const int rc = engine->ocr(pixels.data(), 16, 16, 4, result);
    return rc >= 0 ? 0 : -1;
}

// 上一次 OCR 的分项耗时（P3 2026-11-21）。
// 与 warmup 同样走 lazy_engine：首次调用前若有显式 SetOcrEngine 则用该档位，
// 否则触发默认档位懒初始化（这样"先 OCR 再读 profile"与"先读 profile 再 OCR"
// 两条调用顺序都能拿到合理结果，不会因为没跑过 OCR 就永远 valid=false）。
// 引擎不支持 profile（基类默认实现）时返回 valid=false 的全零结构。
ocr_profile_t OcrService::last_profile() {
    const auto engine = lazy_engine("last_profile");
    if (!engine)
        return ocr_profile_t{};
    return engine->profile();
}

} // namespace op::ocr
