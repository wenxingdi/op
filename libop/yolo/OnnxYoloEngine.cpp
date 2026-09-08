// NOMINMAX must precede any Windows header so std::min/std::max aren't
// hijacked by the min/max macros (error C2589 with onnxruntime_cxx_api.h
// pulling in windows.h).
#define NOMINMAX
#include "OnnxYoloEngine.h"
#include "yolo_models.h"
#include "../base/Utils.h"
#include <onnxruntime_cxx_api.h>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using std::cout;
using std::endl;

namespace op::yolo {

namespace {

// UTF-8 字节串 -> wstring（--labels 类别名按 UTF-8 解码）
std::wstring utf8_to_wstring(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// 从当前 DLL 资源段加载内置模型（若有；方案B 下默认构建不含）
std::vector<uint8_t> LoadYoloRes(int id) {
    HMODULE h = GetModuleHandleW(L"op_x64.dll");
    if (!h) h = GetModuleHandleW(L"op_c_api_x64.dll");
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

// ---- 极简 BGR 图像容器（打包 HWC，每像素 3 字节）----
struct BGR {
    int h = 0, w = 0;
    std::vector<uint8_t> d;
    const uint8_t *at(int y, int x) const { return &d[(size_t(y) * w + x) * 3]; }
    uint8_t *at(int y, int x) { return &d[(size_t(y) * w + x) * 3]; }
};
BGR make_bgr(int h, int w) {
    BGR m;
    m.h = h;
    m.w = w;
    m.d.assign(size_t(h) * w * 3, 0);
    return m;
}
BGR to_bgr(byte *data, int w, int h, int bpp) {
    BGR m = make_bgr(h, w);
    if (bpp == 1) {
        for (int i = 0; i < w * h; ++i) {
            uint8_t g = data[i];
            m.d[i * 3] = g;
            m.d[i * 3 + 1] = g;
            m.d[i * 3 + 2] = g;
        }
    } else if (bpp == 3) {
        for (int i = 0; i < w * h; ++i) {
            m.d[i * 3] = data[i * 3];
            m.d[i * 3 + 1] = data[i * 3 + 1];
            m.d[i * 3 + 2] = data[i * 3 + 2];
        }
    } else { // bpp == 4 : BGRA
        for (int i = 0; i < w * h; ++i) {
            m.d[i * 3] = data[i * 4];
            m.d[i * 3 + 1] = data[i * 4 + 1];
            m.d[i * 3 + 2] = data[i * 4 + 2];
        }
    }
    return m;
}
BGR resize_bilinear(const BGR &s, int dh, int dw) {
    BGR o = make_bgr(dh, dw);
    if (s.h == 0 || s.w == 0) return o;
    float sx = float(s.w) / dw, sy = float(s.h) / dh;
    for (int y = 0; y < dh; ++y) {
        float sy0 = sy * y;
        int y0 = int(sy0), y1 = std::min(y0 + 1, s.h - 1);
        float fy = sy0 - y0;
        for (int x = 0; x < dw; ++x) {
            float sx0 = sx * x;
            int x0 = int(sx0), x1 = std::min(x0 + 1, s.w - 1);
            float fx = sx0 - x0;
            const uint8_t *a = s.at(y0, x0), *b = s.at(y0, x1);
            const uint8_t *c = s.at(y1, x0), *e = s.at(y1, x1);
            uint8_t *q = o.at(y, x);
            for (int k = 0; k < 3; ++k) {
                float v = a[k] * (1 - fx) * (1 - fy) + b[k] * fx * (1 - fy) +
                          c[k] * (1 - fx) * fy + e[k] * fx * fy;
                q[k] = uint8_t(std::round(v));
            }
        }
    }
    return o;
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
        std::vector<float> buf(in); // CreateTensor 需要非 const 指针
        Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value v = Ort::Value::CreateTensor<float>(mi, buf.data(), buf.size(), shape.data(), shape.size());
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
    bool ok = false;

    Impl() {
        sopts.SetIntraOpNumThreads(1);
        sopts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    }

    bool load(const std::wstring &model_path) {
        if (model_path.empty()) {
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
        try {
            sess = std::make_unique<Ort::Session>(env, m_buf.data(), m_buf.size(), sopts);
        } catch (const Ort::Exception &e) {
            cout << "OnnxYoloEngine: session create failed: " << e.what() << endl;
            sess.reset();
            m_buf.clear();
            return false;
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
        cout << "OnnxYoloEngine: model loaded, input=" << m_in_h << "x" << m_in_w << endl;
        return true;
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
};

OnnxYoloEngine::OnnxYoloEngine() : m_impl(std::make_unique<Impl>()) {}
OnnxYoloEngine::~OnnxYoloEngine() = default;

int OnnxYoloEngine::init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) {
    (void)engine;
    (void)dllName;
    // 解析 --conf= / --iou= / --labels=
    for (const std::string &a : argv) {
        if (a.rfind("--conf=", 0) == 0) {
            float v = float(atof(a.c_str() + 7));
            if (v > 0.0f && v < 1.0f) m_impl->m_conf_def = v;
        } else if (a.rfind("--iou=", 0) == 0) {
            float v = float(atof(a.c_str() + 6));
            if (v > 0.0f && v < 1.0f) m_impl->m_iou_def = v;
        } else if (a.rfind("--labels=", 0) == 0) {
            m_impl->m_labels.clear();
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
                            m_impl->m_labels.push_back(utf8_to_wstring(line));
                    }
                }
            } else {
                size_t pos = 0;
                while (pos <= list.size()) {
                    size_t comma = list.find(',', pos);
                    std::string one = (comma == std::string::npos) ? list.substr(pos) : list.substr(pos, comma - pos);
                    if (!one.empty()) m_impl->m_labels.push_back(utf8_to_wstring(one));
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
        }
    }
    cout << "OnnxYoloEngine: conf=" << m_impl->m_conf_def << " iou=" << m_impl->m_iou_def
         << " labels=" << m_impl->m_labels.size() << endl;
    return m_impl->load(dllName) ? 0 : -1;
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

} // namespace op::yolo
