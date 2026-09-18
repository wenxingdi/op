#include "OcrService.h"
#include "OnnxOcrEngine.h"
#include "../network/HttpClient.h"
#include "../base/Utils.h"
#include <iostream>
#include <regex>

using std::cout;
using std::endl;

namespace op::ocr {

namespace {
constexpr const char *kTesseractDefaultEndpoint = "http://127.0.0.1:8080/api/v1/ocr";
constexpr const char *kPaddleOcrDefaultEndpoint = "http://127.0.0.1:8081/api/v1/ocr";
constexpr const char *kPaddleNcnnOcrDefaultEndpoint = "http://127.0.0.1:8082/api/v1/ocr";
constexpr const char *kOcrDefaultPathSuffix = "api/v1/ocr";

bool try_resolve_ocr_backend(const std::string &candidate, std::string &endpoint) {
    const std::string key = to_lower_copy(trim_copy(candidate));
    if (key.empty()) {
        return false;
    }
    if (key == "tesseract" || key == "tess") {
        endpoint = kTesseractDefaultEndpoint;
        return true;
    }
    if (key == "paddle_ncnn") {
        endpoint = kPaddleNcnnOcrDefaultEndpoint;
        return true;
    }
    if (key == "paddle" || key == "paddleocr" || key == "paddle_ocr") {
        endpoint = kPaddleOcrDefaultEndpoint;
        return true;
    }
    return false;
}

std::string resolve_default_endpoint() {
    std::string endpoint;
    if (resolve_endpoint_candidate(getenv_trimmed("OP_OCR_URL"), endpoint, try_resolve_ocr_backend)) {
        return endpoint;
    }
    if (resolve_endpoint_candidate(getenv_trimmed("OP_OCR_BACKEND"), endpoint, try_resolve_ocr_backend)) {
        return endpoint;
    }
    return kPaddleNcnnOcrDefaultEndpoint;
}

int resolve_default_timeout_ms() {
    return parse_positive_int(getenv_trimmed("OP_OCR_TIMEOUT_MS"), 3000);
}
} // namespace

// ---- HttpOcrEngine（原 HttpOcrService 的 HTTP 实现，作可选远程兜底）----
HttpOcrEngine::HttpOcrEngine() : m_endpoint(resolve_default_endpoint()), m_timeout_ms(resolve_default_timeout_ms()) {
    if (!normalize_endpoint(m_endpoint, kOcrDefaultPathSuffix)) {
        // 配置写错时退回主 OCR 服务，避免静默切到旧 Tesseract 后端。
        m_endpoint = kPaddleNcnnOcrDefaultEndpoint;
    }
    cout << "HttpOcrEngine::HttpOcrEngine(), endpoint=" << m_endpoint << endl;
}

HttpOcrEngine::~HttpOcrEngine() {
    release();
}

int HttpOcrEngine::init(const std::wstring &engine, const std::wstring &dllName, const std::vector<std::string> &argvs) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Resolve fallback endpoint/timeout if not yet set
    if (m_endpoint.empty() || m_timeout_ms <= 0) {
        std::string cur = m_endpoint.empty() ? resolve_default_endpoint() : m_endpoint;
        int timeout = m_timeout_ms > 0 ? m_timeout_ms : resolve_default_timeout_ms();
        m_endpoint = cur;
        m_timeout_ms = timeout;
    }

    return configure_endpoint(m_endpoint, m_timeout_ms, engine, dllName, argvs, try_resolve_ocr_backend,
                              kOcrDefaultPathSuffix, "SetOcrEngine", nullptr, nullptr,
                              3000);
}

int HttpOcrEngine::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return 0;
}

int HttpOcrEngine::ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    result.clear();

    // Snapshot endpoint/timeout under lock; the HTTP request below runs concurrently
    // so multiple ocr() calls from different threads are not serialized.
    std::string endpoint;
    int timeout_ms = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        endpoint = m_endpoint;
        timeout_ms = m_timeout_ms;
    }

    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) {
        return -1;
    }

    const size_t pixel_bytes_size_t = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(bpp);
    if (pixel_bytes_size_t > 64 * 1024 * 1024) {
        return -2;
    }

    std::string image_b64;
    if (!base64_encode(data, static_cast<int>(pixel_bytes_size_t), image_b64)) {
        cout << "ocr base64 encode failed" << endl;
        return -3;
    }

    std::string req;
    req.reserve(image_b64.size() + 96);
    req += "{\"image\":\"";
    req += image_b64;
    req += "\",\"width\":";
    req += std::to_string(w);
    req += ",\"height\":";
    req += std::to_string(h);
    req += ",\"bpp\":";
    req += std::to_string(bpp);
    req += "}";

    ParsedUrl parsed;
    if (!parse_url(endpoint, parsed)) {
        cout << "ocr endpoint invalid: " << endpoint << endl;
        return -4;
    }

    std::string resp;
    DWORD status_code = 0;
    if (!http_post_json(parsed, req, timeout_ms, resp, status_code, L"op-ocr-client/1.0")) {
        cout << "ocr request failed: endpoint=" << endpoint << endl;
        return -5;
    }
    if (status_code != 200) {
        cout << "ocr http status=" << status_code << ", body=" << resp << endl;
        return -6;
    }

    static const std::regex code_re("\\\"code\\\"\\s*:\\s*(-?\\d+)");
    std::smatch code_match;
    if (!std::regex_search(resp, code_match, code_re)) {
        cout << "ocr parse response code failed" << endl;
        return -7;
    }
    const int code = atoi(code_match[1].str().c_str());
    if (code != 0) {
        cout << "ocr server code=" << code << ", body=" << resp << endl;
        return -8;
    }

    static const std::regex obj_re("\\{[^\\{\\}]*\\}");
    static const std::regex text_re("\\\"text\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\"");
    static const std::regex bbox_re(
        "\\\"bbox\\\"\\s*:\\s*\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*\\]");
    static const std::regex conf_re("\\\"confidence\\\"\\s*:\\s*([-+]?\\d*\\.?\\d+(?:[eE][-+]?\\d+)?)");

    int n = 0;
    for (std::sregex_iterator it(resp.begin(), resp.end(), obj_re), end; it != end; ++it) {
        const std::string obj = it->str();
        std::smatch text_m;
        std::smatch bbox_m;
        std::smatch conf_m;
        if (!std::regex_search(obj, text_m, text_re) || !std::regex_search(obj, bbox_m, bbox_re) ||
            !std::regex_search(obj, conf_m, conf_re)) {
            continue;
        }

        ocr_rec_t ts;
        ts.left_top = point_t(atoi(bbox_m[1].str().c_str()), atoi(bbox_m[2].str().c_str()));
        ts.right_bottom = point_t(atoi(bbox_m[3].str().c_str()), atoi(bbox_m[4].str().c_str()));
        ts.confidence = static_cast<float>(atof(conf_m[1].str().c_str()));
        const std::string text = json_unescape(text_m[1].str());
        ts.text = _s2wstring(utf8_to_ansi(text));
        result.push_back(ts);
        ++n;
    }

    if (n == 0) {
        static const std::regex empty_result_re("\\\"results\\\"\\s*:\\s*\\[\\s*\\]");
        if (!std::regex_search(resp, empty_result_re)) {
            cout << "ocr parse results failed, body=" << resp << endl;
            return -9;
        }
    }

    return n;
}

// ---- HttpOcrService（OCR 引擎管理器单例）----
HttpOcrService::HttpOcrService() = default;
HttpOcrService::~HttpOcrService() = default;

HttpOcrService *HttpOcrService::getInstance() {
    static HttpOcrService sInstance;
    return &sInstance;
}

int HttpOcrService::init(const std::wstring &engine, const std::wstring &dllName,
                        const std::vector<std::string> &argv) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return init_unlocked(engine, dllName, argv);
}

int HttpOcrService::init_unlocked(const std::wstring &engine, const std::wstring &dllName,
                                  const std::vector<std::string> &argv) {
    const std::string eng = _ws2string(engine);
    std::string dummy;
    const bool is_http = (eng.rfind("http", 0) == 0) || try_resolve_ocr_backend(eng, dummy);
    if (is_http) {
        m_engine = std::make_unique<HttpOcrEngine>();
        cout << "HttpOcrService: selected HttpOcrEngine (remote)" << endl;
    } else {
        // 空 / "onnx" / "builtin" / 未知 → 内置进程内引擎（默认）
        m_engine = std::make_unique<OnnxOcrEngine>();
        cout << "HttpOcrService: selected OnnxOcrEngine (built-in)" << endl;
    }
    m_lazy_failed = false; // 显式 init 成功后清掉懒初始化失败记忆
    return m_engine->init(engine, dllName, argv);
}

int HttpOcrService::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_engine.reset();
    return 0;
}

// 懒初始化：脚本未显式 SetOcrEngine 时，首次免字库调用自动就绪内置 ONNX 引擎。
// 失败只试一次（m_lazy_failed），避免每次调用重复加载/刷日志；显式 init 会清除该记忆。
int HttpOcrService::ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_engine) {
        if (m_lazy_failed)
            return -1;
        if (init_unlocked(L"", L"", {}) != 0 || !m_engine) {
            m_engine.reset(); // 加载失败的引擎实例不留着（其内部 ok=false，只会持续返回 -1）
            m_lazy_failed = true;
            cout << "ocr: lazy init of built-in engine failed, call SetOcrEngine explicitly" << endl;
            return -1;
        }
    }
    return m_engine->ocr(data, w, h, bpp, result);
}

int HttpOcrService::ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_engine) {
        if (m_lazy_failed)
            return -1;
        if (init_unlocked(L"", L"", {}) != 0 || !m_engine) {
            m_engine.reset();
            m_lazy_failed = true;
            cout << "ocr_line: lazy init of built-in engine failed, call SetOcrEngine explicitly" << endl;
            return -1;
        }
    }
    return m_engine->ocr_line(data, w, h, bpp, result);
}

} // namespace op::ocr
