// NOMINMAX must precede any Windows header so std::min/std::max aren't
// hijacked by the min/max macros (error C2589 with onnxruntime_cxx_api.h
// pulling in windows.h).
#define NOMINMAX
#include "OnnxYoloEngine.h"
#include "yolo_models.h"
#include "../base/Utils.h"
#include "../algorithm/MiniImage.h"
#include <onnxruntime_cxx_api.h>
#include <windows.h>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

using std::cout;
using std::endl;

namespace op::yolo {
// 极简 BGR 工具链五件已单源化到 algorithm/MiniImage.h（与另一引擎 byte 级相同的副本删除）。
using op::img::BGR;
using op::img::make_bgr;
using op::img::resize_bilinear;
using op::img::to_bgr;
using op::img::utf8_to_wstring;

namespace {

// 从当前 DLL 资源段加载内置模型（若有；方案B 下默认构建不含）
// 用「函数地址反查模块句柄」：不再硬编码 DLL 名，x64/x86 及任意重命名均正确。
std::vector<uint8_t> LoadYoloRes(int id) {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadYoloRes), &h);
    if (!h) h = GetModuleHandleW(nullptr);
    if (!h) return {};
    HRSRC hrs = FindResourceW(h, MAKEINTRESOURCE(id), L"YOLOMODEL");
    if (!hrs) return {};
    HGLOBAL hg = LoadResource(h, hrs);
    if (!hg) return {};
    uint8_t *p = static_cast<uint8_t *>(LockResource(hg));
    DWORD sz = SizeofResource(h, hrs);
    return {p, p + sz};
}

// letterbox：等比缩放进 SH×SW 画布（灰 114 填充），返回缩放系数与 pad 偏移
BGR letterbox(const BGR &img, int SH, int SW, float &scale, int &pad_x, int &pad_y) {
    scale = std::min(float(SH) / img.h, float(SW) / img.w);
    int nw = std::max(1, int(std::round(img.w * scale)));
    int nh = std::max(1, int(std::round(img.h * scale)));
    pad_x = (SW - nw) / 2;
    pad_y = (SH - nh) / 2;
    BGR canvas = make_bgr(SH, SW);
    std::fill(canvas.d.begin(), canvas.d.end(), 114);
    BGR rs = resize_bilinear(img, nh, nw);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            const uint8_t *p = rs.at(y, x);
            uint8_t *q = canvas.at(pad_y + y, pad_x + x);
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
        }
    return canvas;
}

bool ort_run(Ort::Session &sess, const std::vector<float> &in, const std::vector<int64_t> &shape,
             std::vector<float> &out, std::vector<int64_t> &oshape) {
    try {
        // 借用输入内存（同 OnnxOcrEngine::ort_run）：CreateTensor/Run 期间 ORT 不写
        // 输入张量，省一次 640x640x3 float（约 4.7MB）memcpy。
        Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value v = Ort::Value::CreateTensor<float>(mi, const_cast<float *>(in.data()), in.size(),
                                                       shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions alloc;
        auto iname = sess.GetInputNameAllocated(0, alloc);
        auto oname = sess.GetOutputNameAllocated(0, alloc);
        const char *iname_c = iname.get();
        const char *oname_c = oname.get();
        Ort::RunOptions ropts;
        auto res = sess.Run(ropts, &iname_c, &v, 1, &oname_c, 1);
        oshape = res[0].GetTensorTypeAndShapeInfo().GetShape();
        const float *p = res[0].GetTensorData<float>();
        size_t n = 1;
        for (auto d : oshape) n *= (d > 0 ? size_t(d) : 1);
        out.assign(p, p + n);
        return true;
    } catch (const Ort::Exception &e) {
        cout << "OnnxYoloEngine: ort_run failed: " << e.what() << endl;
        return false;
    }
}

struct Cand {
    float x1, y1, x2, y2; // 原图像素坐标
    float conf;
    int cls;
};

float iou_of(const Cand &a, const Cand &b) {
    float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float uni = area_a + area_b - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

using DmlAppendFn = OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int);

// DML provider 内部若触发 SEH（如驱动/运行时异常），必须在这里截住，
// 不能让它穿透到 C API 边界导致整个 SetYoloEngine 静默失败。
OrtStatus *call_dml_append_seh(DmlAppendFn fn, OrtSessionOptions *options, int device, DWORD *seh_code) {
    *seh_code = 0;
    __try {
        return fn(options, device);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *seh_code = GetExceptionCode();
        return nullptr;
    }
}

// ---- 张量布局判定（Pose/Obb/Segment 共用）----
// 检测头形状只有 [1, nattr, nanch] 或其转置。真实模型 nanch 恒为 8400/25200 级，
// 与输入尺寸推得的 anchor 数对上；测试模型可能只有 1~2 个 anchor，此时用
// "属性维至少要有 cx,cy,w,h + 任务附加位" 作兜底判据。
struct Layout {
    long long nattr = 0;
    long long nanch = 0;
    bool ch_first = true; // true=[1,nattr,nanch]，false=[1,nanch,nattr]
};

bool resolve_layout(long long A, long long B, long long na_expected, long long min_attr, Layout &out) {
    const long long na8 = na_expected;
    if (A == na8 || A == 3 * na8) {
        out = {B, A, false};
    } else if (B == na8 || B == 3 * na8) {
        out = {A, B, true};
    } else if (A < min_attr && B >= min_attr) {
        out = {B, A, false};
    } else if (B < min_attr && A >= min_attr) {
        out = {A, B, true};
    } else if (A >= min_attr && B >= min_attr) {
        out = {std::min(A, B), std::max(A, B), A < B};
    } else {
        return false;
    }
    return out.nattr >= min_attr && out.nanch > 0;
}

struct PtF {
    float x, y;
};

std::array<PtF, 4> obb_corners(float cx, float cy, float w, float h, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float hw = w * 0.5f, hh = h * 0.5f;
    const PtF local[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    std::array<PtF, 4> out;
    for (int i = 0; i < 4; ++i) {
        out[i] = {cx + local[i].x * c - local[i].y * s, cy + local[i].x * s + local[i].y * c};
    }
    return out;
}

float signed_area(const std::vector<PtF> &poly) {
    float a = 0.0f;
    for (size_t i = 0; i < poly.size(); ++i) {
        const PtF &p = poly[i], &q = poly[(i + 1) % poly.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5f;
}

// Sutherland-Hodgman：用凸多边形 clip 裁剪 subject，返回交集多边形。
std::vector<PtF> clip_convex(const std::vector<PtF> &subject, std::vector<PtF> clip) {
    if (signed_area(clip) < 0.0f)
        std::reverse(clip.begin(), clip.end());
    std::vector<PtF> out = subject;
    for (size_t e = 0; e < clip.size() && !out.empty(); ++e) {
        const PtF a = clip[e], b = clip[(e + 1) % clip.size()];
        auto side = [&](const PtF &p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
        std::vector<PtF> in = std::move(out);
        out.clear();
        for (size_t i = 0; i < in.size(); ++i) {
            const PtF &p = in[i], &q = in[(i + 1) % in.size()];
            const float sp = side(p), sq = side(q);
            if (sp >= 0.0f)
                out.push_back(p);
            if ((sp >= 0.0f) != (sq >= 0.0f)) {
                const float t = sp / (sp - sq);
                out.push_back({p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t});
            }
        }
    }
    return out;
}

float obb_iou(const std::array<PtF, 4> &a, const std::array<PtF, 4> &b) {
    const std::vector<PtF> subject(a.begin(), a.end());
    const std::vector<PtF> clip(b.begin(), b.end());
    const std::vector<PtF> inter_poly = clip_convex(subject, clip);
    if (inter_poly.size() < 3)
        return 0.0f;
    const float inter = std::fabs(signed_area(inter_poly));
    const float area_a = std::fabs(signed_area(subject));
    const float area_b = std::fabs(signed_area(clip));
    const float uni = area_a + area_b - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// 解析 ultralytics 嵌入 ONNX metadata 的类别名字典串，两种 repr 均兼容：
//   {0: 'person', 1: 'car'}   （单引号、数字键）
//   {"0": "person", ...}      （双引号、字符串键）
// 按索引位填 vector（索引空洞留空串）；解析不出任何条目时返回空 vector。
std::vector<std::wstring> parse_names_dict(const char *s) {
    std::vector<std::wstring> names;
    if (!s || !*s)
        return names;
    static const std::regex re(R"(['\"]?(\d+)['\"]?\s*:\s*['\"]([^'\"]*)['\"])");
    const std::string text(s);
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
        const unsigned long long idx = std::strtoull((*it)[1].str().c_str(), nullptr, 10);
        if (idx >= 4096) // 防御异常索引，避免恶意 metadata 撑爆内存
            continue;
        if (names.size() <= idx)
            names.resize(size_t(idx) + 1);
        names[size_t(idx)] = utf8_to_wstring((*it)[2].str());
    }
    return names;
}

// 类别感知 NMS：同类才互相抑制
std::vector<Cand> class_aware_nms(std::vector<Cand> &cands, float iou_thr) {
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.conf > b.conf; });
    std::vector<char> suppressed(cands.size(), 0);
    std::vector<Cand> keep;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (suppressed[i]) continue;
        keep.push_back(cands[i]);
        for (size_t j = i + 1; j < cands.size(); ++j) {
            if (suppressed[j]) continue;
            if (cands[j].cls == cands[i].cls && iou_of(cands[i], cands[j]) > iou_thr)
                suppressed[j] = 1;
        }
    }
    return keep;
}

} // namespace

class OnnxYoloEngine::Impl {
  public:
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "op-yolo"};
    Ort::SessionOptions sopts;
    std::unique_ptr<Ort::Session> sess;
    std::vector<uint8_t> m_buf;   // 持有模型 buffer 保证会话生命周期内有效
    float m_conf_def = 0.25f;     // --conf=
    float m_iou_def = 0.45f;      // --iou=
    std::vector<std::wstring> m_labels; // --labels=
    int m_in_h = 640, m_in_w = 640;
    int m_threads = 0;            // --threads=N；0 = 不设置（用 ORT 默认：物理核数）
    int m_threads_applied = 0;    // 真正写进 SessionOptions 的值（0 = 没设），供诊断/测试
    int m_nc_hint = 0;            // --nc=N：Pose 属性维拆不出类别数时用
    int m_kpt = 0;                // --kpt=N 或 metadata kpt_shape；0 = 未知
    int m_device = -1;            // --device=N：DirectML 适配器序号；-1/cpu = CPU
    int m_device_applied = -1;    // 真正生效的设备（-1 = CPU），供 GetModelInfo 诊断
    Task m_force_task = Task::Unknown; // --task=...：元数据缺失时强制任务
    Task m_task = Task::Unknown;  // 加载后判定（元数据优先，其次形状推断）
    bool ok = false;

    Impl() {
        // ⚠ 原来这里写死 SetIntraOpNumThreads(1)。2026-11-21 实测（探针
        // scripts/probes/yolo_thread_probe.cpp + 2.4GFLOP 的合成模型）：
        //   1 线程 36.9ms / 2 线程 19.0ms / 4 线程 10.6ms / 8 线程 8.0ms / ORT 默认 7.6ms
        // 单线程把 YOLO 推理拖慢 3.5~4.8 倍。极小模型（单 Constant 节点 0.2ms）上
        // 各档无差异，说明放开线程不会在"小图快跑"场景反而变慢。
        // 因此默认交给 ORT（物理核数），需要压线程时用 --threads=N（如多实例并发，
        // 避免 N 个会话各开满线程互相抢核）。
        reset_options();
    }

    // 每次建会话前重建 options：DML 追加/失败回退都要一份干净的对象，
    // 否则失败的 provider 会残留在 sopts 里影响下一次尝试。
    void reset_options() {
        sopts = Ort::SessionOptions();
        sopts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        if (m_threads > 0) {
            sopts.SetIntraOpNumThreads(m_threads);
            m_threads_applied = m_threads;
        }
    }

    // 通过 OrtDmlApi（GetExecutionProviderApi）追加，不链 DML 导入库。
    // 注意：provider 初始化会执行外部 DLL，本机实测 DML 版 ORT 上 append 仍可能 AV
    // （0xc0000005），因此调用点外包 SEH；失败一律回退 CPU。调用方不能假设"加载了
    // DML 运行库 = 一定能用 GPU"——适配器/驱动/会话环境都可能让它失败。
    struct DmlApiV1 {
        OrtStatus *(ORT_API_CALL *SessionOptionsAppendExecutionProvider_DML)(OrtSessionOptions *, int);
    };

    bool append_dml_provider() {
        try {
            const void *raw_api = nullptr;
            OrtStatus *st =
                Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, &raw_api);
            if (st) {
                const char *msg = Ort::GetApi().GetErrorMessage(st);
                cout << "OnnxYoloEngine: DirectML EP API unavailable: " << (msg ? msg : "unknown") << endl;
                Ort::GetApi().ReleaseStatus(st);
                return false;
            }
            if (!raw_api) return false;
            const auto *dml = static_cast<const DmlApiV1 *>(raw_api);
            DWORD seh = 0;
            st = call_dml_append_seh(dml->SessionOptionsAppendExecutionProvider_DML, sopts, m_device, &seh);
            if (seh) {
                cout << "OnnxYoloEngine: DirectML append raised SEH 0x" << std::hex << seh << std::dec << endl;
                return false;
            }
            if (!st) return true;
            const char *msg = Ort::GetApi().GetErrorMessage(st);
            cout << "OnnxYoloEngine: DirectML provider rejected: " << (msg ? msg : "unknown") << endl;
            Ort::GetApi().ReleaseStatus(st);
            return false;
        } catch (const std::exception &e) {
            cout << "OnnxYoloEngine: DirectML provider unavailable: " << e.what() << endl;
            return false;
        } catch (...) {
            cout << "OnnxYoloEngine: DirectML provider unavailable (unknown exception)" << endl;
            return false;
        }
    }

    // 解析加载参数。任务/类别/关键点提示只对本次加载有效；不重置会让上一次的
    // --task 粘住新模型。init 与 init_from_memory 共用（两份拷贝必然漂移）。
    void parse_argv(const vector<string> &argv) {
        m_force_task = Task::Unknown;
        m_nc_hint = 0;
        m_kpt = 0;
        m_device = -1;
        m_device_applied = -1;
        // 2026-11-26 修复：m_labels 同样必须随本次加载重置——此前漏掉，
        // 进程内连续加载两个模型时，后加载模型的 metadata names 因
        // "if (m_labels.empty())" 门禁被跳过，直接继承上一个模型的类别名
        // （实测：seg 模型后再加载 cls 模型，class 0 报成 "blob"，其余为空）。
        // 显式 --labels= 在下面的解析里重新填充；未给则由 metadata 自动加载。
        m_labels.clear();
        for (const std::string &a : argv) {
            if (a.rfind("--conf=", 0) == 0) {
                float v = float(atof(a.c_str() + 7));
                if (v > 0.0f && v < 1.0f) m_conf_def = v;
            } else if (a.rfind("--threads=", 0) == 0) {
                // 0 = 显式回到 auto（YoloDetector 是进程级单例，前一次 SetYoloEngine 的
                // --threads 会留在同一个 Impl 上，需要能表达"改回默认"）；
                // 上限 64：超了多半是写错，且会静默吃掉宿主线程配额。
                const int v = atoi(a.c_str() + 10);
                if (v >= 0 && v <= 64) m_threads = v;
            } else if (a.rfind("--iou=", 0) == 0) {
                float v = float(atof(a.c_str() + 6));
                if (v > 0.0f && v < 1.0f) m_iou_def = v;
            } else if (a.rfind("--task=", 0) == 0) {
                std::string t = a.substr(7);
                for (auto &ch : t)
                    ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                if (t == "detect")
                    m_force_task = Task::Detect;
                else if (t == "segment")
                    m_force_task = Task::Segment;
                else if (t == "classify")
                    m_force_task = Task::Classify;
                else if (t == "pose")
                    m_force_task = Task::Pose;
                else if (t == "obb")
                    m_force_task = Task::Obb;
                else if (!t.empty() && t != "auto")
                    cout << "OnnxYoloEngine: unknown --task=" << t << endl;
            } else if (a.rfind("--nc=", 0) == 0) {
                const int v = atoi(a.c_str() + 5);
                if (v > 0 && v <= 4096) m_nc_hint = v;
            } else if (a.rfind("--kpt=", 0) == 0) {
                const int v = atoi(a.c_str() + 6);
                if (v > 0 && v <= 256) m_kpt = v;
            } else if (a.rfind("--device=", 0) == 0) {
                // N>=0 请求 DirectML 适配器；cpu/-1 显式回 CPU（覆盖上一次的 sticky 配置）。
                std::string d = a.substr(9);
                for (auto &ch : d)
                    ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                if (d == "cpu" || d == "-1" || d.empty() || d == "auto") {
                    m_device = -1;
                } else {
                    const int v = atoi(d.c_str());
                    if (v >= 0 && v < 64)
                        m_device = v;
                    else
                        cout << "OnnxYoloEngine: invalid --device=" << d << " (expect 0..63 or cpu)" << endl;
                }
            } else if (a.rfind("--labels=", 0) == 0) {
                m_labels.clear();
                std::string list = a.substr(9);
                if (!list.empty() && list[0] == '@') {
                    // @file：从 UTF-8 文本文件按行读取类别（ultralytics classes.txt 格式，每行一个）
                    const std::string path = list.substr(1);
                    std::ifstream f(path, std::ios::binary);
                    if (!f) {
                        cout << "OnnxYoloEngine: labels file not found: " << path << endl;
                    } else {
                        std::string line;
                        while (std::getline(f, line)) {
                            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                                line.pop_back();
                            if (!line.empty())
                                m_labels.push_back(utf8_to_wstring(line));
                        }
                    }
                } else {
                    size_t pos = 0;
                    while (pos <= list.size()) {
                        size_t comma = list.find(',', pos);
                        std::string one =
                            (comma == std::string::npos) ? list.substr(pos) : list.substr(pos, comma - pos);
                        if (!one.empty()) m_labels.push_back(utf8_to_wstring(one));
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                }
            }
        }
    }

    bool load(const std::wstring &model_path, const uint8_t *mem = nullptr, size_t mem_len = 0) {
        if (mem && mem_len) {
            m_buf.assign(mem, mem + mem_len);
        } else if (model_path.empty()) {
            m_buf = LoadYoloRes(IDR_YOLO_MODEL);
            if (m_buf.empty()) {
                cout << "OnnxYoloEngine: embedded yolo model missing "
                        "(build without yolo model; use SetYoloEngine(\"onnx\", <model_path>, ...))"
                     << endl;
                return false;
            }
        } else {
            std::ifstream f(model_path, std::ios::binary);
            if (!f) {
                cout << "OnnxYoloEngine: model file not found" << endl;
                return false;
            }
            f.seekg(0, std::ios::end);
            std::streamoff sz = f.tellg();
            f.seekg(0, std::ios::beg);
            if (sz <= 0) return false;
            m_buf.resize(size_t(sz));
            f.read(reinterpret_cast<char *>(m_buf.data()), sz);
            if (!f) {
                m_buf.clear();
                return false;
            }
        }
        // 线程数在建会话前才生效（init 里解析 argv 晚于构造函数）。
        m_device_applied = -1;
        reset_options();
        if (m_device >= 0) {
            if (append_dml_provider()) {
                m_device_applied = m_device;
                cout << "OnnxYoloEngine: execution provider=DirectML device=" << m_device << endl;
            } else {
                cout << "OnnxYoloEngine: DirectML not available in current onnxruntime.dll "
                        "(deploy the DirectML build to enable --device), falling back to CPU"
                     << endl;
                reset_options();
            }
        }
        try {
            sess = std::make_unique<Ort::Session>(env, m_buf.data(), m_buf.size(), sopts);
        } catch (const Ort::Exception &e) {
            if (m_device_applied >= 0) {
                // provider 已追加但会话建不起来（常见：DirectML.dll 缺失/版本不符）。
                // 明确告警后回退 CPU，避免整个引擎不可用。
                cout << "OnnxYoloEngine: DirectML session create failed (" << e.what()
                     << "), falling back to CPU" << endl;
                m_device_applied = -1;
                reset_options();
                try {
                    sess = std::make_unique<Ort::Session>(env, m_buf.data(), m_buf.size(), sopts);
                } catch (const Ort::Exception &e2) {
                    cout << "OnnxYoloEngine: session create failed: " << e2.what() << endl;
                    sess.reset();
                    m_buf.clear();
                    return false;
                }
            } else {
                cout << "OnnxYoloEngine: session create failed: " << e.what() << endl;
                sess.reset();
                m_buf.clear();
                return false;
            }
        }
        // 输入形状 [1,3,H,W]（动态维度 -1 时保持 640 默认）
        try {
            auto ishape = sess->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            if (ishape.size() == 4) {
                if (ishape[2] > 0) m_in_h = int(ishape[2]);
                if (ishape[3] > 0) m_in_w = int(ishape[3]);
            }
        } catch (...) {
        }
        // 自动读取 ultralytics 嵌入的类别名（metadata "names"）。
        // 显式 --labels 优先；模型未嵌入 metadata（或解析失败）时保持空——JSON 只给 class_id。
        if (m_labels.empty()) {
            try {
                Ort::AllocatorWithDefaultOptions alloc;
                auto names_val = sess->GetModelMetadata().LookupCustomMetadataMapAllocated("names", alloc);
                if (names_val) {
                    m_labels = parse_names_dict(names_val.get());
                    if (!m_labels.empty())
                        cout << "OnnxYoloEngine: labels auto-loaded from model metadata, n=" << m_labels.size()
                             << endl;
                }
            } catch (const Ort::Exception &) {
                // 无 metadata 的旧模型/其他框架导出：静默，JSON 只给 class_id
            }
        }
        // Pose 模型的关键点形状（ultralytics 写法 "[17, 3]"）；CLI --kpt= 优先。
        if (m_kpt == 0) {
            try {
                Ort::AllocatorWithDefaultOptions alloc;
                auto kv = sess->GetModelMetadata().LookupCustomMetadataMapAllocated("kpt_shape", alloc);
                if (kv) {
                    const char *p = kv.get();
                    while (*p && (*p < '0' || *p > '9'))
                        ++p;
                    const int v = atoi(p);
                    if (v > 0 && v <= 256)
                        m_kpt = v;
                }
            } catch (const Ort::Exception &) {
            }
        }
        // 任务识别（批 C）：优先读模型元数据 task，其次按输出形状推断。
        // 元数据 task 由 ultralytics 导出时写入（detect/segment/classify/pose/obb）。
        m_task = detect_task_from_metadata();
        if (m_task == Task::Unknown)
            m_task = infer_task_from_shapes();
        if (m_force_task != Task::Unknown)
            m_task = m_force_task;
        cout << "OnnxYoloEngine: task=" << static_cast<int>(m_task) << endl;
        ok = true; // 会话就绪，detect 门禁（修复：load 成功后此前未置位，ONNX detect 恒返 -1）
        cout << "OnnxYoloEngine: model loaded, input=" << m_in_h << "x" << m_in_w << endl;
        return true;
    }

    Task detect_task_from_metadata() {
        try {
            Ort::AllocatorWithDefaultOptions alloc;
            auto t = sess->GetModelMetadata().LookupCustomMetadataMapAllocated("task", alloc);
            if (!t)
                return Task::Unknown;
            std::wstring w = utf8_to_wstring(t.get());
            for (auto &ch : w)
                ch = static_cast<wchar_t>(towlower(ch));
            if (w == L"detect")
                return Task::Detect;
            if (w == L"segment")
                return Task::Segment;
            if (w == L"classify")
                return Task::Classify;
            if (w == L"pose")
                return Task::Pose;
            if (w == L"obb")
                return Task::Obb;
        } catch (const Ort::Exception &) {
        }
        return Task::Unknown;
    }

    // 按输出张量形状推断任务。判据顺序（宁可保守）：
    //   输出数 >= 2 且其中一个 4 维 [1,nm,ph,pw]  -> Segment
    //   输出数 == 1 且总元素数 <= 4096 且维数 <= 4 -> Classify（检测头动辄上万，不会这么小）
    //   输出数 == 1 且 3 维，属性维与 labels/kpt 对上 -> Obb / Pose
    //   其余                                        -> Detect
    Task infer_task_from_shapes() const {
        try {
            const size_t n_out = sess->GetOutputCount();
            if (n_out >= 2) {
                for (size_t i = 0; i < n_out; ++i) {
                    auto s = sess->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
                    if (s.size() == 4 && s[1] > 0 && s[2] > 0 && s[3] > 0)
                        return Task::Segment;
                }
            }
            if (n_out == 1) {
                auto shape = sess->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
                int64_t total = 1;
                for (auto d : shape)
                    total *= (d > 0 ? d : 1);
                if (total > 0 && total <= 4096 && shape.size() <= 4)
                    return Task::Classify;
                if (shape.size() == 3) {
                    const long long na_expected = (long long)(m_in_h / 8) * (m_in_w / 8) +
                                                  (long long)(m_in_h / 16) * (m_in_w / 16) +
                                                  (long long)(m_in_h / 32) * (m_in_w / 32);
                    Layout ly;
                    if (resolve_layout(shape[1], shape[2], na_expected, 5, ly)) {
                        const int nc = static_cast<int>(ly.nattr - 5);
                        if (!m_labels.empty() && nc == static_cast<int>(m_labels.size()))
                            return Task::Obb;
                        if (m_kpt > 0 &&
                            ly.nattr == 4 + static_cast<long long>(m_labels.size()) + 3LL * m_kpt)
                            return Task::Pose;
                    }
                }
            }
        } catch (const Ort::Exception &) {
        }
        return Task::Detect;
    }

    // 按输出形状解码（版本自适应）。anchor 数由输入尺寸推得：
    //   anchor-free（v8/v11）: (H/8)^2+(H/16)^2+(H/32)^2
    //   anchor-based（v5）  : 3x 上值
    int decode(const std::vector<float> &out, const std::vector<int64_t> &oshape,
               float scale, int pad_x, int pad_y, int iw, int ih,
               float conf_thr, float iou_thr, vyolo_rec_t &result) {
        if (oshape.size() != 3) {
            cout << "OnnxYoloEngine: unsupported output dims=" << oshape.size() << endl;
            return -8;
        }
        long long A = oshape[1], B = oshape[2];
        // 属性维（4+nc / 5+nc）远小于 anchor 数：nattr=较小维，nanch=较大维
        long long nattr = std::min(A, B), nanch = std::max(A, B);
        const bool ch_first = (A < B); // A==nattr -> [1, nattr, nanch]；B==nattr -> [1, nanch, nattr]
        const long long na_af = (long long)(m_in_h / 8) * (m_in_w / 8) + (long long)(m_in_h / 16) * (m_in_w / 16) +
                                 (long long)(m_in_h / 32) * (m_in_w / 32);
        const long long na_v5 = 3 * na_af;
        int nc = 0, obj_off = 0;
        if (nanch == na_v5) {
            nc = int(nattr - 5); // v5: cx,cy,w,h,obj,nc 类
            obj_off = 1;
        } else if (nanch == na_af) {
            nc = int(nattr - 4); // v8/v11: cx,cy,w,h,nc 类
            obj_off = 0;
        } else {
            cout << "OnnxYoloEngine: unsupported output shape [" << A << "," << B
                 << "] (anchor dim != " << na_af << "/" << na_v5 << ")" << endl;
            return -8;
        }
        if (nc <= 0) {
            cout << "OnnxYoloEngine: bad class count nc=" << nc << endl;
            return -8;
        }

        // 元素访问：i=属性维 [0,nattr)，j=anchor 维 [0,nanch)
        auto elem = [&](long long i, long long j) -> float {
            return ch_first ? out[size_t(i * nanch + j)] : out[size_t(j * nattr + i)];
        };

        std::vector<Cand> cands;
        for (long long j = 0; j < nanch; ++j) {
            // 找最优类
            float best_cls = -1.0f;
            int best_c = 0;
            for (int c = 0; c < nc; ++c) {
                float v = elem(4 + obj_off + c, j);
                if (v > best_cls) {
                    best_cls = v;
                    best_c = c;
                }
            }
            float conf = best_cls;
            if (obj_off) conf *= elem(4, j); // v5: obj * cls
            if (conf < conf_thr) continue;

            float cx = elem(0, j), cy = elem(1, j), w = elem(2, j), h = elem(3, j);
            // letterbox 空间 -> 原图像素
            float x1 = (cx - w * 0.5f - pad_x) / scale;
            float y1 = (cy - h * 0.5f - pad_y) / scale;
            float x2 = (cx + w * 0.5f - pad_x) / scale;
            float y2 = (cy + h * 0.5f - pad_y) / scale;
            x1 = std::max(0.0f, std::min(x1, float(iw)));
            y1 = std::max(0.0f, std::min(y1, float(ih)));
            x2 = std::max(0.0f, std::min(x2, float(iw)));
            y2 = std::max(0.0f, std::min(y2, float(ih)));
            if (x2 - x1 < 1.0f || y2 - y1 < 1.0f) continue;
            cands.push_back({x1, y1, x2, y2, conf, best_c});
        }

        auto kept = class_aware_nms(cands, iou_thr);
        for (const auto &c : kept) {
            yolo_rec_t rec;
            rec.class_id = c.cls;
            if (c.cls >= 0 && c.cls < (int)m_labels.size())
                rec.label = m_labels[c.cls];
            rec.left_top = point_t(int(std::lround(c.x1)), int(std::lround(c.y1)));
            rec.right_bottom = point_t(int(std::lround(c.x2)), int(std::lround(c.y2)));
            rec.confidence = c.conf;
            result.push_back(rec);
        }
        return (int)kept.size();
    }

    // ---- 分类解码（批 C 2026-11-22）----
    // 接受三类常见输出布局：
    //   [1, nc]            / [nc]           softmax 后
    //   [1, nc, 1, 1]      / [1, 1, nc]     ultralytics classify 导出（保留 NCHW 维度）
    //   [1, nc, 8400]      —— 形状像检测但没有框属性（nc < 6），按"每 anchor 一类分数"取全局最大
    // 不做 NMS（分类本来无框）；按 score 降序取 top_k。
    int decode_classify(const std::vector<float> &out, const std::vector<int64_t> &oshape, int top_k,
                        vyolo_cls_t &result) {
        size_t total = 1;
        for (auto d : oshape)
            total *= (d > 0 ? size_t(d) : 1);
        if (total == 0 || total > out.size())
            return -8;
        const size_t n = std::min(total, out.size());
        if (top_k <= 0)
            top_k = 5;
        result.clear();
        result.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            yolo_cls_t item;
            item.class_id = static_cast<int>(i);
            if (static_cast<size_t>(item.class_id) < m_labels.size())
                item.label = m_labels[size_t(item.class_id)];
            item.score = out[i];
            result.push_back(std::move(item));
        }
        std::sort(result.begin(), result.end(), [](const yolo_cls_t &a, const yolo_cls_t &b) {
            return a.score > b.score;
        });
        if (static_cast<int>(result.size()) > top_k)
            result.resize(static_cast<size_t>(top_k));
        return static_cast<int>(result.size());
    }

    // ---- 分割解码（批 C 2026-11-22）----
    // Segment 模型的输出是"检测头 + 掩码系数"：
    //   det:  [1, 4+nc+nm, nanch]（通道在前）
    //   proto:[1, nm, ph, pw]（ph/pw 通常是 输入尺寸/4）
    // 掩码 = sigmoid(coeff · proto)，再二值化 + 轮廓抽取 + 还原到原图坐标。
    //
    // 简化决策（明确记录）：
    //   * proto 分辨率固定按 **32 倍**（ph = SH/32 时也接受，按实际形状取）；
    //   * 掩码按 **letterbox 空间** 计算后线性映射回原图（含去 pad），不做逆仿射——
    //     这与框的解码口径一致；
    //   * 轮廓用 **8 邻域边界跟踪 + 抽稀**，只输出**外轮廓**（不做孔洞/多连通域）。
    //     对"取一块可点区域"的用途足够，且实现可控。
    int decode_segment(const std::vector<float> &det, const std::vector<int64_t> &dshape,
                       const std::vector<float> &proto, const std::vector<int64_t> &pshape,
                       float scale, int pad_x, int pad_y, int iw, int ih, int nm,
                       float conf_thr, float iou_thr, vyolo_seg_t &result) {
        if (dshape.size() != 3 || pshape.size() != 4)
            return -8;
        long long A = dshape[1], B = dshape[2];
        // 不能简单用 min/max 判属性维与 anchor 维：测试模型 nanch=1 时
        // A=7,B=1，max 会把"属性维"当成 anchor 维（实测 nc=-5）。
        // 判据改为：**anchor 数由输入尺寸决定**（v8/v11 anchor-free 为
        // (H/8)^2+(H/16)^2+(H/32)^2，v5 为其 3 倍），哪个轴与它对上就是 anchor 维；
        // 都对不上再退回 min/max 启发式。真实模型 nanch 恒为 8400/25200 级，
        // 这条判据比"取较大值"可靠。
        // anchor 数由**模型输入尺寸**决定（不是原图尺寸）。这里用 m_in_h/m_in_w：
        // decode_segment 只在 segment() 里被调用，那时 load() 早已完成、尺寸已就绪。
        const long long na_expected = (long long)(m_in_h / 8) * (m_in_w / 8) +
                                      (long long)(m_in_h / 16) * (m_in_w / 16) +
                                      (long long)(m_in_h / 32) * (m_in_w / 32);
        // 三档判据，从强到弱：
        //   1) 与"按输入尺寸推出的 anchor 数"对上（真实 v5/v8/v11 模型都命中这条）；
        //   2) 属性维必须 >= 4+nm（cx,cy,w,h 起头 + nm 个掩码系数）——真实 anchor 数
        //      永远远大于属性维，所以 **较小的一维必然是属性维**。这条对任意 anchor 数
        //      （含单 anchor 的测试模型）都成立，是第一档失效时的正确兜底；
        //   3) 两维都 >= 4+nm 且相等时无法判断，按通道在前处理（ultralytics 默认导出）。
        // 注意不能直接用 min/max 当作第 2 档：当 A < 4+nm 时（如 A=7,B=1），
        // 较小维反而是 anchor 维，min/max 会把 nattr 算成 1。
        const long long min_attr = 4 + nm;
        long long nattr = 0, nanch = 0;
        bool ch_first = true;
        if (A == na_expected || A == 3 * na_expected) {
            nanch = A;
            nattr = B;
            ch_first = false; // [1, nanch, nattr]
        } else if (B == na_expected || B == 3 * na_expected) {
            nanch = B;
            nattr = A;
            ch_first = true; // [1, nattr, nanch]
        } else if (A >= min_attr && B >= min_attr) {
            nattr = std::min(A, B);
            nanch = std::max(A, B);
            ch_first = (A < B);
        } else if (A >= min_attr) {
            nattr = A;
            nanch = B;
            ch_first = true;
        } else {
            nattr = B;
            nanch = A;
            ch_first = false;
        }
        const int nc = static_cast<int>(nattr) - 4 - nm;
        if (nc <= 0) {
            cout << "OnnxYoloEngine: segment nc=" << nc << " (nattr=" << nattr << ", nm=" << nm << ")" << endl;
            return -8;
        }
        const long long ph = pshape[2], pw = pshape[3];
        if (ph <= 0 || pw <= 0 || pshape[1] != nm) {
            cout << "OnnxYoloEngine: segment bad proto shape [" << pshape[1] << "," << ph << "," << pw << "] nm=" << nm
                 << endl;
            return -8;
        }
        // 元素访问：i = 属性下标 [0,nattr)，j = anchor 下标 [0,nanch)。
        // first=true 表示布局是 [1, nattr, nanch]（属性在前 -> 步长为 nanch）；
        // first=false 表示 [1, nanch, nattr]（anchor 在前 -> 步长为 nattr）。
        // 两种布局的 **stride 都是"另一维的长度"**，用错会把所有值读串（实测表现为
        // 框跑到画面外、掩码面积恒 0），所以参数名就叫 st_attr/st_anch 不叫 n_*。
        auto at = [&](const std::vector<float> &t, long long n_attr, long long n_anch, bool first, long long i,
                      long long j) -> float {
            return first ? t[size_t(i * n_anch + j)] : t[size_t(j * n_attr + i)];
        };

        // 1) 先按检测头解框 + NMS（复用同一套类别感知 NMS）
        std::vector<Cand> cands;
        std::vector<long long> cand_index; // 保留 anchor 下标，用于取掩码系数
        for (long long j = 0; j < nanch; ++j) {
            float best = -1.0f;
            int best_c = 0;
            for (int c = 0; c < nc; ++c) {
                const float v = at(det, nattr, nanch, ch_first, 4 + c, j);
                if (v > best) {
                    best = v;
                    best_c = c;
                }
            }
            if (best < conf_thr)
                continue;
            const float cx = at(det, nattr, nanch, ch_first, 0, j);
            const float cy = at(det, nattr, nanch, ch_first, 1, j);
            const float w = at(det, nattr, nanch, ch_first, 2, j);
            const float h = at(det, nattr, nanch, ch_first, 3, j);
            if (getenv("OP_YOLO_SEG_DEBUG")) {
                cout << "OnnxYoloEngine: seg cand cx=" << cx << " cy=" << cy << " w=" << w << " h=" << h
                     << " conf=" << best << " cls=" << best_c << " nattr=" << nattr << " nanch=" << nanch
                     << " ch_first=" << ch_first << " j=" << j << endl;
            }
            cands.push_back({cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, best, best_c});
            cand_index.push_back(j);
        }
        // 需要 cand -> anchor 的映射，故自己跑一遍 NMS（class_aware_nms 会重排 cands）
        const size_t n_cand = cands.size();
        std::vector<size_t> order(n_cand);
        for (size_t i = 0; i < n_cand; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&cands](size_t a, size_t b) { return cands[a].conf > cands[b].conf; });
        std::vector<char> suppressed(n_cand, 0);
        std::vector<size_t> kept;
        for (size_t oi = 0; oi < n_cand; ++oi) {
            const size_t i = order[oi];
            if (suppressed[i])
                continue;
            kept.push_back(i);
            for (size_t oj = oi + 1; oj < n_cand; ++oj) {
                const size_t k = order[oj];
                if (suppressed[k])
                    continue;
                if (cands[k].cls == cands[i].cls && iou_of(cands[i], cands[k]) > iou_thr)
                    suppressed[k] = 1;
            }
        }

        // 2) 逐框生成掩码多边形
        result.clear();
        for (size_t idx : kept) {
            const Cand &c = cands[idx];
            yolo_seg_t seg;
            // 框：letterbox 空间 -> 原图
            float x1 = (c.x1 - pad_x) / scale, y1 = (c.y1 - pad_y) / scale;
            float x2 = (c.x2 - pad_x) / scale, y2 = (c.y2 - pad_y) / scale;
            x1 = std::max(0.0f, std::min(x1, float(iw)));
            y1 = std::max(0.0f, std::min(y1, float(ih)));
            x2 = std::max(0.0f, std::min(x2, float(iw)));
            y2 = std::max(0.0f, std::min(y2, float(ih)));
            if (getenv("OP_YOLO_SEG_DEBUG")) {
                cout << "OnnxYoloEngine: seg map scale=" << scale << " pad=" << pad_x << "," << pad_y
                     << " iw/ih=" << iw << "/" << ih << " box=" << x1 << "," << y1 << "," << x2 << "," << y2 << endl;
            }
            seg.box.class_id = c.cls;
            if (c.cls >= 0 && c.cls < (int)m_labels.size())
                seg.box.label = m_labels[c.cls];
            seg.box.confidence = c.conf;
            seg.box.left_top = point_t(int(std::lround(x1)), int(std::lround(y1)));
            seg.box.right_bottom = point_t(int(std::lround(x2)), int(std::lround(y2)));

            // 掩码：在 **proto 网格**上逐格算 coeff·proto>0，只算框覆盖的格（+2 格余量）。
            // ⚠ 关键换算：框坐标是 letterbox 像素（模型输入尺寸，如 640），
            // proto 是降采样网格（如 20x20），两者相差 proto_stride 倍。
            // 早期版本直接拿框的像素坐标当网格下标，clamp 到 19 后区间恒为空 ->
            // mask_area 恒 0、polygon 恒空（实测踩到）。
            const long long j = cand_index[idx];
            // 步长 = 模型输入边长 / proto 边长（ultralytics segment 固定 4 倍下采样
            // 相对 stride-32 特征图，即输入/4；这里按实际形状算，不写死 4/8/32）
            const float proto_stride_x = (pw > 0) ? float(m_in_w) / float(pw) : 32.0f;
            const float proto_stride_y = (ph > 0) ? float(m_in_h) / float(ph) : 32.0f;
            int lx1 = int(std::floor(c.x1 / proto_stride_x)) - 2;
            int ly1 = int(std::floor(c.y1 / proto_stride_y)) - 2;
            int lx2 = int(std::ceil(c.x2 / proto_stride_x)) + 2;
            int ly2 = int(std::ceil(c.y2 / proto_stride_y)) + 2;
            lx1 = std::max(0, lx1);
            ly1 = std::max(0, ly1);
            lx2 = std::min(int(pw) - 1, lx2);
            ly2 = std::min(int(ph) - 1, ly2);
            if (lx2 < lx1 || ly2 < ly1) {
                result.push_back(std::move(seg));
                continue;
            }
            std::vector<uint8_t> mask(size_t(lx2 - lx1 + 1) * size_t(ly2 - ly1 + 1), 0);
            const int mw = lx2 - lx1 + 1;
            int area = 0;
            for (int y = ly1; y <= ly2; ++y) {
                for (int x = lx1; x <= lx2; ++x) {
                    float acc = 0.0f;
                    for (int k = 0; k < nm; ++k) {
                        const float coeff = at(det, nattr, nanch, ch_first, 4 + nc + k, j);
                        const float pv = proto[size_t((size_t(k) * ph + y) * pw + x)];
                        acc += coeff * pv;
                    }
                    if (acc > 0.0f) { // sigmoid(acc) > 0.5
                        mask[size_t(y - ly1) * mw + (x - lx1)] = 1;
                        ++area;
                    }
                }
            }
            if (area < 8) { // 太小的掩码是噪声，不输出多边形（框仍然有效）
                result.push_back(std::move(seg));
                continue;
            }
            // proto 格面积 -> 原图像素面积：一格覆盖 (stride/scale)^2 个原图像素
            const float cell_px = (proto_stride_x / scale) * (proto_stride_y / scale);
            seg.mask_area = float(area) * cell_px;
            seg.polygon = trace_outer_polygon(mask, mw, ly2 - ly1 + 1, lx1, ly1, scale, pad_x, pad_y, iw, ih);
            result.push_back(std::move(seg));
        }
        return static_cast<int>(result.size());
    }

    // ---- 姿态解码（Pose）----
    // 输出：[1, 4+nc+3*kpt, nanch]（ultralytics pose，anchor-free）。
    // 每个 anchor：cx,cy,w,h + nc 个类分数 + kpt 组 (x,y,conf)。
    // 关键点坐标与框同在 letterbox 空间，必须一起做逆变换（scale/pad），
    // 否则宽图上的点会整体偏移——这是 Pose 最容易写错的一段。
    int decode_pose(const std::vector<float> &out, const std::vector<int64_t> &oshape,
                    float scale, int pad_x, int pad_y, int iw, int ih,
                    float conf_thr, float iou_thr, vyolo_pose_t &result) {
        if (oshape.size() != 3)
            return -8;
        const long long na_expected = (long long)(m_in_h / 8) * (m_in_w / 8) +
                                      (long long)(m_in_h / 16) * (m_in_w / 16) +
                                      (long long)(m_in_h / 32) * (m_in_w / 32);
        Layout ly;
        if (!resolve_layout(oshape[1], oshape[2], na_expected, 4 + 3, ly)) {
            cout << "OnnxYoloEngine: pose unsupported output shape [" << oshape[1] << "," << oshape[2] << "]"
                 << endl;
            return -8;
        }
        const long long nattr = ly.nattr, nanch = ly.nanch;
        const bool ch_first = ly.ch_first;

        int nc = m_nc_hint;
        int kpt = m_kpt;
        if (kpt > 0) {
            const long long calc = nattr - 4 - 3LL * kpt;
            if (calc <= 0) {
                cout << "OnnxYoloEngine: pose nattr=" << nattr << " cannot fit kpt=" << kpt << endl;
                return -8;
            }
            if (nc > 0 && nc != calc)
                cout << "OnnxYoloEngine: pose --nc=" << nc << " ignored, shape says " << calc << endl;
            nc = static_cast<int>(calc);
        } else if (!m_labels.empty()) {
            nc = static_cast<int>(m_labels.size());
            const long long rem = nattr - 4 - nc;
            if (rem <= 0 || rem % 3 != 0) {
                cout << "OnnxYoloEngine: pose nattr=" << nattr << " inconsistent with labels=" << nc << endl;
                return -8;
            }
            kpt = static_cast<int>(rem / 3);
        } else if (nc > 0) {
            const long long rem = nattr - 4 - nc;
            if (rem <= 0 || rem % 3 != 0) {
                cout << "OnnxYoloEngine: pose nattr=" << nattr << " inconsistent with --nc=" << nc << endl;
                return -8;
            }
            kpt = static_cast<int>(rem / 3);
        } else {
            // 无 metadata、无 labels、无 --kpt/--nc：按 COCO 17 点兜底。
            kpt = 17;
            const long long rem = nattr - 4 - 3LL * kpt;
            if (rem <= 0) {
                cout << "OnnxYoloEngine: pose cannot infer nc/kpt (nattr=" << nattr
                     << "); pass --nc=N --kpt=N" << endl;
                return -8;
            }
            nc = static_cast<int>(rem);
        }
        if (nc <= 0 || kpt <= 0) {
            cout << "OnnxYoloEngine: pose bad nc=" << nc << " kpt=" << kpt << endl;
            return -8;
        }

        auto at = [&](long long i, long long j) -> float {
            return ch_first ? out[size_t(i * nanch + j)] : out[size_t(j * nattr + i)];
        };

        std::vector<Cand> cands;
        std::vector<long long> cand_index;
        for (long long j = 0; j < nanch; ++j) {
            float best = -1.0f;
            int best_c = 0;
            for (int c = 0; c < nc; ++c) {
                const float v = at(4 + c, j);
                if (v > best) {
                    best = v;
                    best_c = c;
                }
            }
            if (best < conf_thr)
                continue;
            const float cx = at(0, j), cy = at(1, j), w = at(2, j), h = at(3, j);
            if (w < 1.0f || h < 1.0f)
                continue;
            cands.push_back({cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, best, best_c});
            cand_index.push_back(j);
        }

        // 与 segment 同样的"保留 anchor 下标"的 NMS：关键点按 anchor 取，不能重排后丢映射。
        const size_t n_cand = cands.size();
        std::vector<size_t> order(n_cand);
        for (size_t i = 0; i < n_cand; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&cands](size_t a, size_t b) { return cands[a].conf > cands[b].conf; });
        std::vector<char> suppressed(n_cand, 0);
        std::vector<size_t> kept;
        for (size_t oi = 0; oi < n_cand; ++oi) {
            const size_t i = order[oi];
            if (suppressed[i])
                continue;
            kept.push_back(i);
            for (size_t oj = oi + 1; oj < n_cand; ++oj) {
                const size_t k = order[oj];
                if (suppressed[k])
                    continue;
                if (cands[k].cls == cands[i].cls && iou_of(cands[i], cands[k]) > iou_thr)
                    suppressed[k] = 1;
            }
        }

        result.clear();
        for (size_t idx : kept) {
            const Cand &c = cands[idx];
            yolo_pose_t item;
            item.box.class_id = c.cls;
            if (c.cls >= 0 && c.cls < static_cast<int>(m_labels.size()))
                item.box.label = m_labels[c.cls];
            item.box.confidence = c.conf;
            const float x1 = std::max(0.0f, std::min((c.x1 - pad_x) / scale, float(iw)));
            const float y1 = std::max(0.0f, std::min((c.y1 - pad_y) / scale, float(ih)));
            const float x2 = std::max(0.0f, std::min((c.x2 - pad_x) / scale, float(iw)));
            const float y2 = std::max(0.0f, std::min((c.y2 - pad_y) / scale, float(ih)));
            item.box.left_top = point_t(int(std::lround(x1)), int(std::lround(y1)));
            item.box.right_bottom = point_t(int(std::lround(x2)), int(std::lround(y2)));

            const long long j = cand_index[idx];
            item.keypoints.reserve(static_cast<size_t>(kpt));
            for (int k = 0; k < kpt; ++k) {
                const long long off = 4 + nc + 3LL * k;
                yolo_kpt_t kp;
                kp.x = std::max(0.0f, std::min((at(off, j) - pad_x) / scale, float(iw)));
                kp.y = std::max(0.0f, std::min((at(off + 1, j) - pad_y) / scale, float(ih)));
                kp.confidence = at(off + 2, j);
                item.keypoints.push_back(kp);
            }
            result.push_back(std::move(item));
        }
        return static_cast<int>(result.size());
    }

    // ---- 旋转框解码（OBB）----
    // 输出：[1, 4+nc+1, nanch] = cx,cy,w,h + nc 类分数 + angle(弧度)。
    // NMS 必须用**旋转 IoU**：轴对齐 NMS 会把"斜着相邻的两条船"误判成重叠。
    int decode_obb(const std::vector<float> &out, const std::vector<int64_t> &oshape,
                   float scale, int pad_x, int pad_y, int iw, int ih,
                   float conf_thr, float iou_thr, vyolo_obb_t &result) {
        if (oshape.size() != 3)
            return -8;
        const long long na_expected = (long long)(m_in_h / 8) * (m_in_w / 8) +
                                      (long long)(m_in_h / 16) * (m_in_w / 16) +
                                      (long long)(m_in_h / 32) * (m_in_w / 32);
        Layout ly;
        if (!resolve_layout(oshape[1], oshape[2], na_expected, 5, ly)) {
            cout << "OnnxYoloEngine: obb unsupported output shape [" << oshape[1] << "," << oshape[2] << "]"
                 << endl;
            return -8;
        }
        const long long nattr = ly.nattr, nanch = ly.nanch;
        const bool ch_first = ly.ch_first;
        const int nc = static_cast<int>(nattr - 5);
        if (nc <= 0) {
            cout << "OnnxYoloEngine: obb bad class count nc=" << nc << " (nattr=" << nattr << ")" << endl;
            return -8;
        }

        auto at = [&](long long i, long long j) -> float {
            return ch_first ? out[size_t(i * nanch + j)] : out[size_t(j * nattr + i)];
        };

        struct ObbCand {
            float cx, cy, w, h, angle, conf;
            int cls;
            std::array<PtF, 4> pts;
        };
        std::vector<ObbCand> cands;
        for (long long j = 0; j < nanch; ++j) {
            float best = -1.0f;
            int best_c = 0;
            for (int c = 0; c < nc; ++c) {
                const float v = at(4 + c, j);
                if (v > best) {
                    best = v;
                    best_c = c;
                }
            }
            if (best < conf_thr)
                continue;
            const float cx = at(0, j), cy = at(1, j), w = at(2, j), h = at(3, j), angle = at(4 + nc, j);
            if (w < 1.0f || h < 1.0f)
                continue;
            cands.push_back({cx, cy, w, h, angle, best, best_c, obb_corners(cx, cy, w, h, angle)});
        }

        std::vector<size_t> order(cands.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&cands](size_t a, size_t b) {
            return cands[a].conf > cands[b].conf;
        });
        std::vector<char> suppressed(cands.size(), 0);
        std::vector<size_t> kept;
        for (size_t oi = 0; oi < order.size(); ++oi) {
            const size_t i = order[oi];
            if (suppressed[i])
                continue;
            kept.push_back(i);
            for (size_t oj = oi + 1; oj < order.size(); ++oj) {
                const size_t k = order[oj];
                if (suppressed[k])
                    continue;
                if (cands[k].cls == cands[i].cls && obb_iou(cands[i].pts, cands[k].pts) > iou_thr)
                    suppressed[k] = 1;
            }
        }

        result.clear();
        for (size_t idx : kept) {
            const ObbCand &c = cands[idx];
            yolo_obb_t item;
            item.class_id = c.cls;
            if (c.cls >= 0 && c.cls < static_cast<int>(m_labels.size()))
                item.label = m_labels[c.cls];
            item.confidence = c.conf;
            item.angle = c.angle;
            item.cx = (c.cx - pad_x) / scale;
            item.cy = (c.cy - pad_y) / scale;
            item.w = c.w / scale;
            item.h = c.h / scale;

            float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
            item.corners.reserve(4);
            for (const PtF &p : c.pts) {
                float px = (p.x - pad_x) / scale;
                float py = (p.y - pad_y) / scale;
                px = std::max(0.0f, std::min(px, float(iw)));
                py = std::max(0.0f, std::min(py, float(ih)));
                item.corners.push_back(point_t(int(std::lround(px)), int(std::lround(py))));
                min_x = std::min(min_x, px);
                min_y = std::min(min_y, py);
                max_x = std::max(max_x, px);
                max_y = std::max(max_y, py);
            }
            item.left_top = point_t(int(std::lround(std::max(0.0f, min_x))),
                                    int(std::lround(std::max(0.0f, min_y))));
            item.right_bottom = point_t(int(std::lround(std::min(max_x, float(iw)))),
                                        int(std::lround(std::min(max_y, float(ih)))));
            result.push_back(std::move(item));
        }
        return static_cast<int>(result.size());
    }

    // 两个输出的推理（Segment 用）。按维数分派：3 维 = 检测头，4 维 = proto。
    // 输出顺序不固定（不同导出工具不同），所以不按 index 硬编码。
    bool run_two_outputs(const std::vector<float> &in, const std::vector<int64_t> &shape, vyolo_seg_t &result,
                         float scale, int pad_x, int pad_y, int iw, int ih, float conf_thr, float iou_thr) {
        try {
            Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value v =
                Ort::Value::CreateTensor<float>(mi, const_cast<float *>(in.data()), in.size(), shape.data(), shape.size());
            Ort::AllocatorWithDefaultOptions alloc;
            const size_t n_out = sess->GetOutputCount();
            if (n_out < 2)
                return false;
            auto iname = sess->GetInputNameAllocated(0, alloc);
            std::vector<std::string> onames;
            std::vector<const char *> oname_c;
            for (size_t i = 0; i < n_out; ++i) {
                auto nm = sess->GetOutputNameAllocated(i, alloc);
                onames.emplace_back(nm.get());
            }
            for (auto &s : onames)
                oname_c.push_back(s.c_str());
            const char *iname_c = iname.get();
            Ort::RunOptions ropts;
            auto res = sess->Run(ropts, &iname_c, &v, 1, oname_c.data(), oname_c.size());

            const std::vector<float> *det = nullptr, *proto = nullptr;
            std::vector<int64_t> dshape, pshape;
            std::vector<std::vector<float>> buffers(n_out);
            std::vector<std::vector<int64_t>> shapes(n_out);
            for (size_t i = 0; i < n_out && i < res.size(); ++i) {
                auto info = res[i].GetTensorTypeAndShapeInfo();
                shapes[i] = info.GetShape();
                const float *p = res[i].GetTensorData<float>();
                size_t n = 1;
                for (auto d : shapes[i])
                    n *= (d > 0 ? size_t(d) : 1);
                buffers[i].assign(p, p + n);
            }
            for (size_t i = 0; i < n_out; ++i) {
                if (shapes[i].size() == 4 && !proto) {
                    proto = &buffers[i];
                    pshape = shapes[i];
                } else if (shapes[i].size() == 3 && !det) {
                    det = &buffers[i];
                    dshape = shapes[i];
                }
            }
            if (!det || !proto) {
                cout << "OnnxYoloEngine: run_two_outputs could not classify output shapes ("
                     << n_out << " outputs)" << endl;
                return false;
            }

            const int nm = int(pshape[1]);
            result.clear();
            m_task = Task::Segment;
            const int rc = decode_segment(*det, dshape, *proto, pshape, scale, pad_x, pad_y, iw, ih, nm, conf_thr,
                                          iou_thr, result);
            return rc >= 0;
        } catch (const Ort::Exception &e) {
            cout << "OnnxYoloEngine: run_two_outputs failed: " << e.what() << endl;
            return false;
        }
    }

  private:
    // 8 邻域边界跟踪：从左上第一个前景点起沿边界走一圈，返回外轮廓（原图坐标）。
    // 实现为"行进方格"简化版：只保证给出闭合、单调推进的外轮廓，允许少量重复点，
    // 由随后的 Douglas-Peucker 抽稀消除。数值上足够稳定，且不引入第三方依赖。
    static std::vector<point_t> trace_outer_polygon(const std::vector<uint8_t> &mask, int w, int h, int off_x,
                                                    int off_y, float scale, int pad_x, int pad_y, int iw, int ih) {
        std::vector<point_t> poly;
        int sx = -1, sy = -1;
        for (int y = 0; y < h && sx < 0; ++y)
            for (int x = 0; x < w; ++x)
                if (mask[size_t(y) * w + x]) {
                    sx = x;
                    sy = y;
                    break;
                }
        if (sx < 0)
            return poly;
        auto on = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < w && y < h && mask[size_t(y) * w + x] != 0;
        };
        // 4 方向 Moore 跟踪
        static const int dx[4] = {1, 0, -1, 0};
        static const int dy[4] = {0, 1, 0, -1};
        int cx = sx, cy = sy, dir = 0;
        const int max_steps = w * h * 4 + 8;
        for (int step = 0; step < max_steps; ++step) {
            poly.push_back(point_t(cx, cy));
            bool moved = false;
            for (int t = 0; t < 4; ++t) {
                const int nd = (dir + 3 + t) % 4; // 先试右转，保证贴着边界走
                const int nx = cx + dx[nd], ny = cy + dy[nd];
                if (on(nx, ny)) {
                    cx = nx;
                    cy = ny;
                    dir = nd;
                    moved = true;
                    break;
                }
            }
            if (!moved)
                break;
            if (cx == sx && cy == sy && poly.size() > 2)
                break;
        }
        // 抽稀：至少每 8 个点保留一个，再映射回原图
        std::vector<point_t> out;
        const size_t stride = poly.size() > 64 ? 8 : 1;
        for (size_t i = 0; i < poly.size(); i += stride) {
            float px = (float(poly[i].x + off_x) - pad_x) / scale;
            float py = (float(poly[i].y + off_y) - pad_y) / scale;
            px = std::max(0.0f, std::min(px, float(iw)));
            py = std::max(0.0f, std::min(py, float(ih)));
            out.push_back(point_t(int(std::lround(px)), int(std::lround(py))));
        }
        return out;
    }
};

OnnxYoloEngine::OnnxYoloEngine() : m_impl(std::make_unique<Impl>()) {}
OnnxYoloEngine::~OnnxYoloEngine() = default;

int OnnxYoloEngine::intra_op_threads() const {
    return m_impl ? m_impl->m_threads_applied : 0;
}

Task OnnxYoloEngine::task() const {
    return m_impl ? m_impl->m_task : Task::Unknown;
}

Task OnnxYoloEngine::detected_task() const {
    return m_impl ? m_impl->m_task : Task::Unknown;
}

int OnnxYoloEngine::input_width() const {
    return m_impl ? m_impl->m_in_w : 0;
}

int OnnxYoloEngine::input_height() const {
    return m_impl ? m_impl->m_in_h : 0;
}

int OnnxYoloEngine::device_id() const {
    return m_impl ? m_impl->m_device_applied : -1;
}

bool OnnxYoloEngine::dml_active() const {
    return m_impl && m_impl->m_device_applied >= 0;
}

bool OnnxYoloEngine::dml_supported() const {
    // 只探测 EP 是否注册，不建会话、不加载 provider（避免在 CPU 版 DLL 上触发加载失败路径）。
    const void *api = nullptr;
    OrtStatus *st = Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, &api);
    if (st) {
        Ort::GetApi().ReleaseStatus(st);
        return false;
    }
    return api != nullptr;
}

int OnnxYoloEngine::init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) {
    (void)engine;
    (void)dllName;
    m_impl->parse_argv(argv);
    cout << "OnnxYoloEngine: conf=" << m_impl->m_conf_def << " iou=" << m_impl->m_iou_def
         << " labels=" << m_impl->m_labels.size()
         << " threads=" << (m_impl->m_threads > 0 ? std::to_string(m_impl->m_threads) : "auto")
         << " device=" << (m_impl->m_device >= 0 ? std::to_string(m_impl->m_device) : "cpu") << endl;
    return m_impl->load(dllName) ? 0 : -1;
}

int OnnxYoloEngine::init_from_memory(const std::vector<uint8_t> &model, const vector<string> &argv) {
    if (model.empty())
        return -1;
    m_impl->parse_argv(argv);
    cout << "OnnxYoloEngine: memory model, bytes=" << model.size() << " conf=" << m_impl->m_conf_def
         << " iou=" << m_impl->m_iou_def << " labels=" << m_impl->m_labels.size() << endl;
    return m_impl->load(L"", model.data(), model.size()) ? 0 : -1;
}
int OnnxYoloEngine::detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    const float conf_thr = conf > 0.0 ? float(conf) : m_impl->m_conf_def;
    const float iou_thr = iou > 0.0 ? float(iou) : m_impl->m_iou_def;
    const int SH = m_impl->m_in_h, SW = m_impl->m_in_w;

    BGR img = to_bgr(data, w, h, bpp);
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    BGR lb = letterbox(img, SH, SW, scale, pad_x, pad_y);

    // blob：CHW /255，RGB 通道序（ultralytics 预处理约定）
    std::vector<float> bin(size_t(3) * SH * SW);
    for (int c = 0; c < 3; ++c) {
        int src = 2 - c; // BGR 容器 -> RGB
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x)
                bin[size_t(c) * SH * SW + size_t(y) * SW + x] = lb.at(y, x)[src] / 255.0f;
    }
    std::vector<int64_t> shape = {1, 3, SH, SW};
    std::vector<float> out;
    std::vector<int64_t> oshape;
    if (!ort_run(*m_impl->sess, bin, shape, out, oshape)) return -3;

    return m_impl->decode(out, oshape, scale, pad_x, pad_y, w, h, conf_thr, iou_thr, result);
}

// ---- 分类（批 C）----
// 与 detect 共用同一套 letterbox + CHW/255 预处理（ultralytics classify 也是这么送进去的）。
// 任务不符时返回 -1（明确不支持），让调用方能把"模型选错了"和"没有结果"分开。
int OnnxYoloEngine::classify(byte *data, int w, int h, int bpp, int top_k, vyolo_cls_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (m_impl->m_task != Task::Classify) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    const int SH = m_impl->m_in_h, SW = m_impl->m_in_w;
    BGR img = to_bgr(data, w, h, bpp);
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    BGR lb = letterbox(img, SH, SW, scale, pad_x, pad_y);
    std::vector<float> bin(size_t(3) * SH * SW);
    for (int c = 0; c < 3; ++c) {
        const int src = 2 - c;
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x)
                bin[size_t(c) * SH * SW + size_t(y) * SW + x] = lb.at(y, x)[src] / 255.0f;
    }
    std::vector<int64_t> shape = {1, 3, SH, SW};
    std::vector<float> out;
    std::vector<int64_t> oshape;
    if (!ort_run(*m_impl->sess, bin, shape, out, oshape)) return -3;
    return m_impl->decode_classify(out, oshape, top_k, result);
}

// ---- 分割（批 C）----
// Segment 模型有两个输出：检测头（带 nm 个掩码系数）与 proto 掩码原型。
// 这里按"哪个是 3 维、哪个是 4 维"自动分派，不依赖输出顺序（不同导出工具顺序不同）。
int OnnxYoloEngine::segment(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_seg_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (m_impl->m_task != Task::Segment) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    const float conf_thr = conf > 0.0 ? float(conf) : m_impl->m_conf_def;
    const float iou_thr = iou > 0.0 ? float(iou) : m_impl->m_iou_def;
    const int SH = m_impl->m_in_h, SW = m_impl->m_in_w;

    BGR img = to_bgr(data, w, h, bpp);
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    BGR lb = letterbox(img, SH, SW, scale, pad_x, pad_y);
    std::vector<float> bin(size_t(3) * SH * SW);
    for (int c = 0; c < 3; ++c) {
        const int src = 2 - c;
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x)
                bin[size_t(c) * SH * SW + size_t(y) * SW + x] = lb.at(y, x)[src] / 255.0f;
    }
    std::vector<int64_t> shape = {1, 3, SH, SW};
    if (!m_impl->run_two_outputs(bin, shape, result, scale, pad_x, pad_y, w, h, conf_thr, iou_thr))
        return -3;
    return static_cast<int>(result.size());
}

// ---- 姿态（Pose）----
int OnnxYoloEngine::pose(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_pose_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (m_impl->m_task != Task::Pose) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    const float conf_thr = conf > 0.0 ? float(conf) : m_impl->m_conf_def;
    const float iou_thr = iou > 0.0 ? float(iou) : m_impl->m_iou_def;
    const int SH = m_impl->m_in_h, SW = m_impl->m_in_w;

    BGR img = to_bgr(data, w, h, bpp);
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    BGR lb = letterbox(img, SH, SW, scale, pad_x, pad_y);
    std::vector<float> bin(size_t(3) * SH * SW);
    for (int c = 0; c < 3; ++c) {
        const int src = 2 - c;
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x)
                bin[size_t(c) * SH * SW + size_t(y) * SW + x] = lb.at(y, x)[src] / 255.0f;
    }
    std::vector<int64_t> shape = {1, 3, SH, SW};
    std::vector<float> out;
    std::vector<int64_t> oshape;
    if (!ort_run(*m_impl->sess, bin, shape, out, oshape)) return -3;
    return m_impl->decode_pose(out, oshape, scale, pad_x, pad_y, w, h, conf_thr, iou_thr, result);
}

// ---- 旋转框（OBB）----
int OnnxYoloEngine::obb(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_obb_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (m_impl->m_task != Task::Obb) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    const float conf_thr = conf > 0.0 ? float(conf) : m_impl->m_conf_def;
    const float iou_thr = iou > 0.0 ? float(iou) : m_impl->m_iou_def;
    const int SH = m_impl->m_in_h, SW = m_impl->m_in_w;

    BGR img = to_bgr(data, w, h, bpp);
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    BGR lb = letterbox(img, SH, SW, scale, pad_x, pad_y);
    std::vector<float> bin(size_t(3) * SH * SW);
    for (int c = 0; c < 3; ++c) {
        const int src = 2 - c;
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x)
                bin[size_t(c) * SH * SW + size_t(y) * SW + x] = lb.at(y, x)[src] / 255.0f;
    }
    std::vector<int64_t> shape = {1, 3, SH, SW};
    std::vector<float> out;
    std::vector<int64_t> oshape;
    if (!ort_run(*m_impl->sess, bin, shape, out, oshape)) return -3;
    return m_impl->decode_obb(out, oshape, scale, pad_x, pad_y, w, h, conf_thr, iou_thr, result);
}

} // namespace op::yolo
