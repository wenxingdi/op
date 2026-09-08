// NOMINMAX must precede any Windows header so std::min/std::max aren't
// hijacked by the min/max macros (error C2589 with onnxruntime_cxx_api.h
// pulling in windows.h).
#define NOMINMAX
#include "OnnxOcrEngine.h"
#include "ocr_models.h"
#include "../base/Utils.h"
#include <onnxruntime_cxx_api.h>
#include <windows.h>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <thread>

using std::cout;
using std::endl;

namespace op::ocr {

namespace {
const double PI = 3.14159265358979323846;

// UTF-8 字节串 -> wstring。PP-OCR 的 keys/txt 均为 UTF-8 编码，必须用 CP_UTF8 解码；
// 不能用 Utils::_s2wstring（它按 CP_ACP 解码，会令中文乱码）。
std::wstring utf8_to_wstring(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// 从当前 DLL 资源段加载模型 buffer（资源随 op_x64.dll / op_c_api_x64.dll 编译进去）
std::vector<uint8_t> LoadRes(int id) {
    HMODULE h = GetModuleHandleW(L"op_x64.dll");
    if (!h) h = GetModuleHandleW(L"op_c_api_x64.dll");
    if (!h) h = GetModuleHandleW(nullptr);
    if (!h) return {};
    HRSRC hrs = FindResourceW(h, MAKEINTRESOURCE(id), L"OCRMODEL");
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
BGR crop(const BGR &s, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(s.w, x1);
    y1 = std::min(s.h, y1);
    if (x1 <= x0 || y1 <= y0) return BGR{};
    BGR o = make_bgr(y1 - y0, x1 - x0);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            const uint8_t *p = s.at(y0 + y, x0 + x);
            uint8_t *q = o.at(y, x);
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
        }
    return o;
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
// 将内容逆时针旋转 theta 度（围绕中心），空白填充为黑。
BGR rotate(const BGR &s, double theta_deg) {
    if (s.h == 0 || s.w == 0) return BGR{};
    double rad = theta_deg * PI / 180.0;
    double ca = std::cos(rad), sa = std::sin(rad);
    BGR o = make_bgr(s.h, s.w);
    int cx = s.w / 2, cy = s.h / 2;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            int dx = x - cx, dy = y - cy;
            double sx2 = cx + (dx * ca - dy * sa);
            double sy2 = cy + (dx * sa + dy * ca);
            int ix = int(std::round(sx2)), iy = int(std::round(sy2));
            if (ix >= 0 && ix < s.w && iy >= 0 && iy < s.h) {
                const uint8_t *p = s.at(iy, ix);
                uint8_t *q = o.at(y, x);
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
            }
        }
    return o;
}
// 主成分分析求内容主轴角度（度）。水平文字≈0，旋转文字≈其倾斜角。
double pca_angle(const BGR &m) {
    long long n = long long(m.h) * m.w;
    if (n < 2) return 0.0;
    double mx = 0, my = 0;
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            mx += x;
            my += y;
        }
    mx /= n;
    my /= n;
    double sxx = 0, syy = 0, sxy = 0;
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            double dx = x - mx, dy = y - my;
            sxx += dx * dx;
            syy += dy * dy;
            sxy += dx * dy;
        }
    sxx /= n;
    syy /= n;
    sxy /= n;
    return 0.5 * std::atan2(2.0 * sxy, sxx - syy) * 180.0 / PI;
}

// 把打包 BGR（HWC）转成平面 CHW float，并转为 RGB 顺序（PP-OCR 训练约定用 RGB）。
// det 用 ImageNet 归一化；rec 用 (x-0.5)/0.5。两者都要求 RGB 通道顺序。
void planar_rgb_imagenet(const BGR &s, std::vector<float> &dst) {
    static const float mean[3] = {0.485f, 0.456f, 0.406f};
    static const float stdv[3] = {0.229f, 0.224f, 0.225f};
    dst.resize(size_t(3) * s.h * s.w);
    for (int c = 0; c < 3; ++c) {
        int src = 2 - c; // c=0->R(BGR idx2), 1->G(1), 2->B(0)
        float m = mean[c], sd = stdv[c];
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x)
                dst[size_t(c) * s.h * s.w + size_t(y) * s.w + x] =
                    (s.at(y, x)[src] / 255.0f - m) / sd;
    }
}

// 运行一次 ONNX 推理（单输入单输出，输入由 shape 描述）。
bool ort_run(Ort::Session &sess, const std::vector<float> &in, const std::vector<int64_t> &shape,
             std::vector<float> &out, std::vector<int64_t> &oshape) {
    Ort::AllocatorWithDefaultOptions alloc;
    std::vector<float> buf(in); // CreateTensor 需要非 const 指针
    Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value v = Ort::Value::CreateTensor<float>(mi, buf.data(), buf.size(), shape.data(), shape.size());
    // 1.19.2 API：返回 RAII 智能指针，作用域结束自动释放，无需 ReleaseInputName/OutputName。
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
}

// DB 后处理：对 det 概率图做阈值+膨胀+连通域，输出原图坐标轴对齐框。
struct Box {
    int x0, y0, x1, y1;
};
void db_postprocess(const std::vector<float> &score, int HH, int WW, float sx, float sy,
                    int iw, int ih, std::vector<Box> &boxes) {
    std::vector<uint8_t> bin(size_t(HH) * WW);
    const float thr = 0.3f;
    // 自动探测输出是否已归一化：PP-OCRv4 det 导出通常含 sigmoid（值∈[0,1]），
    // 若最大值 >1 说明是原始 logits，需补 sigmoid。
    float mx = 0.0f;
    for (float v : score) mx = std::max(mx, v);
    bool is_logit = (mx > 1.0f);
    for (size_t i = 0; i < bin.size(); ++i) {
        float p = is_logit ? (1.0f / (1.0f + std::exp(-score[i]))) : score[i];
        bin[i] = (p > thr) ? 1 : 0;
    }
    // 3x3 膨胀两次，连接同一词/行内的相邻字符
    for (int it = 0; it < 2; ++it) {
        std::vector<uint8_t> tmp = bin;
        for (int y = 0; y < HH; ++y)
            for (int x = 0; x < WW; ++x) {
                if (!bin[y * WW + x]) continue;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int ny = y + dy, nx = x + dx;
                        if (ny < 0 || nx < 0 || ny >= HH || nx >= WW) continue;
                        tmp[ny * WW + nx] = 1;
                    }
            }
        bin = std::move(tmp);
    }
    // 8 连通域标记
    std::vector<int> label(size_t(HH) * WW, 0);
    int nlab = 0;
    std::vector<int> stack;
    for (int y = 0; y < HH; ++y)
        for (int x = 0; x < WW; ++x) {
            int idx = y * WW + x;
            if (!bin[idx] || label[idx]) continue;
            nlab++;
            stack.clear();
            stack.push_back(idx);
            label[idx] = nlab;
            int minx = WW, miny = HH, maxx = 0, maxy = 0;
            size_t head = 0;
            while (head < stack.size()) {
                int c = stack[head++];
                int cy = c / WW, cx = c % WW;
                if (cx < minx) minx = cx;
                if (cx > maxx) maxx = cx;
                if (cy < miny) miny = cy;
                if (cy > maxy) maxy = cy;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dy && !dx) continue;
                        int ny = cy + dy, nx = cx + dx;
                        if (ny < 0 || nx < 0 || ny >= HH || nx >= WW) continue;
                        int nidx = ny * WW + nx;
                        if (bin[nidx] && !label[nidx]) {
                            label[nidx] = nlab;
                            stack.push_back(nidx);
                        }
                    }
            }
            if ((int)stack.size() < 16) continue; // 过滤过小的噪声
            // 分数图坐标 -> 原图：通用映射（不依赖下采样倍数）= 坐标 × (原图宽/WW)
            // 本模型 det 输出与输入同分辨率（WW==rw），故等效 ×1；若模型为 1/4 下采样（WW==rw/4）
            // 则等效 ×4。统一用 iw/WW、ih/HH 覆盖两种情况。
            int X0 = int(minx * (float)iw / WW), Y0 = int(miny * (float)ih / HH);
            int X1 = int(maxx * (float)iw / WW), Y1 = int(maxy * (float)ih / HH);
            // 外扩近似 unclip，保证识别裁剪包含完整文字
            int cw = X1 - X0, ch = Y1 - Y0;
            int mx = int(cw * 0.1f) + 5, my = int(ch * 0.1f) + 5;
            boxes.push_back({X0 - mx, Y0 - my, X1 + mx, Y1 + my});
        }
}

// 字符白名单规则（--charset= 参数）。ASCII 语法：
//   @zh           展开 keys 内全部中文（CJK Unified U+4E00-U+9FFF）
//   其余可打印 ASCII 字符（如 0-9 [ ] , - +）按字面加入
// 示例: --charset=@zh0123456789[],-+   （只识别中文 + 数字 + 五个符号）
// 空规则 = 全字典（默认，与原行为一致）。keys/rule 均单字节 ASCII 时编码无关。
struct CharSetRule {
    bool zh = false;
    std::string chars; // 显式允许的单字节 ASCII 字符集合（去重）
};

CharSetRule parse_charset_rule(const std::string &rule) {
    CharSetRule r;
    size_t i = 0;
    while (i < rule.size()) {
        unsigned char u = static_cast<unsigned char>(rule[i]);
        if (u == '@') {
            if (i + 2 < rule.size() && (rule[i + 1] == 'z' || rule[i + 1] == 'Z') &&
                (rule[i + 2] == 'h' || rule[i + 2] == 'H')) {
                r.zh = true;
                i += 3;
                continue;
            }
            ++i; // 孤立 @ 忽略
            continue;
        }
        if (u >= 0x21 && u <= 0x7E && r.chars.find(rule[i]) == std::string::npos)
            r.chars.push_back(rule[i]);
        // 其余（非 ASCII，如 ACP 中文残留）忽略：显式中文请用 @zh 全量展开
        ++i;
    }
    return r;
}

// CTC 贪婪解码：argmax 后折叠重复并去 blank(0)。每步以最优类别概率作为置信度。
// 维度判定：类别数 C 是较大的那一维，序列长 T 是较小的那一维。
// mask：可选类别白名单（非空即启用）。mask[0] 恒 1=blank 始终允许；mask[c]==1
//   允许类别 c 参与 argmax，c >= mask.size() 视为禁止（覆盖模型类别多于 keys 的
//   尾部错位，无需 mask 与 C 等长）。白名单外字符概率被屏蔽，形近字符
//   （0/O、,/.、1/l）竞争被消除。空 mask = 全字典（默认）。
std::string ctc_greedy(const std::vector<float> &out, const std::vector<int64_t> &shape,
                        const std::vector<std::string> &keys, float &conf,
                        const std::vector<uint8_t> &mask) {
    int A = int(shape[1]), B = int(shape[2]);
    int C = std::max(A, B), T = std::min(A, B);
    bool tc_layout = (B == C); // [N,T,C]：类别在 dim2(B)；[N,C,T]：类别在 dim1(A)
    const bool constrained = !mask.empty();
    const size_t MC = mask.size();
    double conf_sum = 0.0;
    int nsteps = 0;
    std::string text;
    int lastc = -1;
    for (int t = 0; t < T; ++t) {
        // 在（白名单内）类别中找最大/最小值做数值稳定，并同时定位 argmax
        float mx = -1e30f, bestv = -1e30f, mn = 1e30f;
        int best = 0;
        for (int c = 0; c < C; ++c) {
            if (constrained && ((size_t)c >= MC || !mask[c])) continue; // mask[0]=1 保证 blank 可选
            float v = tc_layout ? out[size_t(t) * C + c] : out[size_t(c) * T + t];
            if (v > mx) mx = v;
            if (v < mn) mn = v;
            if (v > bestv) {
                bestv = v;
                best = c;
            }
        }
        // 判定模型输出是否已为概率（softmax）：所有值非负且 ≤1。
        // 若是，则 out[best] 本身即该步置信度（直接取最优类概率）；
        // 否则视为 logits，按 softmax 求最优类概率。白名单约束下 softmax
        // 只在允许类别子集上归一，避免 conf 被屏蔽类别摊薄导致阈值误滤。
        float prob;
        if (mn >= -1e-5f && mx <= 1.0f + 1e-5f) {
            prob = bestv; // 已是概率
        } else {
            float sum = 0.0f;
            for (int c = 0; c < C; ++c) {
                if (constrained && ((size_t)c >= MC || !mask[c])) continue;
                float v = tc_layout ? out[size_t(t) * C + c] : out[size_t(c) * T + t];
                sum += std::exp(v - mx);
            }
            prob = sum > 0.f ? std::exp(bestv - mx) / sum : 0.f;
        }
        if (best != 0 && best != lastc) {
            int ki = best - 1; // blank=0，keys[i] 对应类别 i+1
            if (ki >= 0 && ki < (int)keys.size()) text += keys[ki];
        }
        lastc = best;
        conf_sum += prob;
        ++nsteps;
    }
    conf = nsteps > 0 ? float(conf_sum / nsteps) : 0.0f;
    return text;
}
} // namespace

class OnnxOcrEngine::Impl {
public:
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "op-ocr"};
    Ort::SessionOptions sopts;
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;
    std::vector<std::string> keys;
    std::vector<uint8_t> m_det, m_rec, m_keys; // 持有模型 buffer 保证会话生命周期内有效
    CharSetRule m_rule;                        // --charset= 解析结果（空=全字典）
    std::vector<uint8_t> m_mask;               // 类别白名单（尺寸=keys+1；空=全放行）
    bool ok = false;

    // 对单张 BGR 图直接做 rec（跳过检测）：缩放+归一化+CTC 解码。
    // 返回 false = 推理失败；txt 为 UTF-8（可为空）。
    bool rec_image(const BGR &roi, std::string &txt, float &conf) {
        const int RH = 48, RW = 320;
        float ratio = float(roi.w) / roi.h;
        int tw = int(std::ceil(48.0f * ratio));
        if (tw > RW) tw = RW;
        BGR rs = resize_bilinear(roi, RH, tw);
        std::vector<float> rin(size_t(3) * RH * RW, 0.0f);
        for (int c = 0; c < 3; ++c) {
            int src = 2 - c; // RGB 顺序（PP-OCR rec 约定）
            for (int y = 0; y < RH; ++y)
                for (int x = 0; x < tw; ++x) {
                    float v = rs.at(y, x)[src] / 255.0f;
                    v = (v - 0.5f) / 0.5f; // rec 归一化：[-1,1]
                    rin[size_t(c) * RH * RW + size_t(y) * RW + x] = v;
                }
        }
        std::vector<int64_t> rshape = {1, 3, RH, RW};
        std::vector<float> rout;
        std::vector<int64_t> roshape;
        if (!ort_run(*rec, rin, rshape, rout, roshape)) return false;
        if (roshape.size() != 3) return false;
        txt = ctc_greedy(rout, roshape, keys, conf, m_mask);
        return true;
    }

    Impl() {
        // 推理线程数不在构造里设（避免锁死单线程）：init() 按 --threads= 参数决定，
        // 缺省 min(4, 核) —— 单线程跑 PP-OCRv4 rec 约 40ms+，远达不到 9-13ms 目标。
        sopts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    }

    // keys 就绪后构建类别 mask：blank(0) 恒允许；keys[i] 对应类别 i+1。
    // 尺寸 = keys+1；模型类别多于 keys 的尾部类别在 ctc_greedy 中按越界视为禁止。
    void build_mask() {
        m_mask.clear();
        if (!m_rule.zh && m_rule.chars.empty())
            return; // 未设置白名单 -> 空 mask = 全字典（默认行为）
        m_mask.assign(keys.size() + 1, 0);
        m_mask[0] = 1; // blank 恒允许
        for (size_t i = 0; i < keys.size(); ++i) {
            bool allow = false;
            if (m_rule.zh) {
                std::wstring w = utf8_to_wstring(keys[i]);
                if (w.size() == 1) {
                    wchar_t u = w[0];
                    if (u >= 0x4E00 && u <= 0x9FFF) allow = true; // CJK 统一表意
                }
            }
            if (!allow && keys[i].size() == 1 &&
                m_rule.chars.find(keys[i][0]) != std::string::npos)
                allow = true;
            m_mask[i + 1] = allow ? 1 : 0;
        }
    }

    bool load() {
        m_det = LoadRes(IDR_OCR_DET);
        m_rec = LoadRes(IDR_OCR_REC);
        m_keys = LoadRes(IDR_OCR_KEYS);
        if (m_det.empty() || m_rec.empty()) {
            cout << "OnnxOcrEngine: model resource missing (det/rec)" << endl;
            return false;
        }
        det = std::make_unique<Ort::Session>(env, m_det.data(), m_det.size(), sopts);
        rec = std::make_unique<Ort::Session>(env, m_rec.data(), m_rec.size(), sopts);
        if (!m_keys.empty()) {
            std::string kt(reinterpret_cast<const char *>(m_keys.data()), m_keys.size());
            std::istringstream iss(kt);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                keys.push_back(line);
            }
        }
        build_mask();
        cout << "OnnxOcrEngine: models loaded, keys=" << keys.size()
             << ", charset_mask=" << (m_mask.empty() ? 0 : int(m_mask.size()) - 1) << endl;
        ok = true;
        return true;
    }
};

OnnxOcrEngine::OnnxOcrEngine() : m_impl(std::make_unique<Impl>()) {}
OnnxOcrEngine::~OnnxOcrEngine() = default;

int OnnxOcrEngine::init(const std::wstring &engine, const std::wstring &dllName,
                        const std::vector<std::string> &argv) {
    (void)engine;
    (void)dllName;
    bool have_threads = false;
    int threads = 0;
    // 解析 --charset=<规则>：如 --charset=@zh0123456789[],-+；--threads=N：推理 intra-op 线程数
    for (const std::string &a : argv) {
        if (a.rfind("--charset=", 0) == 0) {
            m_impl->m_rule = parse_charset_rule(a.substr(10));
        } else if (a.rfind("--threads=", 0) == 0) {
            threads = atoi(a.c_str() + 10);
            have_threads = true;
        }
    }
    // 线程策略：--threads=N>0 显式指定；--threads=0 不调用 Set（走 ORT 默认=全物理核）；
    // 缺省 min(4, 核) —— rec 单行 320x48 4 线程约 10-15ms，覆盖 9-13ms 目标且不失控。
    if (have_threads && threads > 0) {
        m_impl->sopts.SetIntraOpNumThreads(threads);
    } else if (!have_threads) {
        unsigned hw = std::thread::hardware_concurrency();
        int def = (hw == 0) ? 4 : int(std::min<unsigned>(4u, hw));
        m_impl->sopts.SetIntraOpNumThreads(def);
    }
    return m_impl->load() ? 0 : -1;
}

int OnnxOcrEngine::ocr(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    BGR img = to_bgr(data, w, h, bpp);

    // ---- 检测 ----
    int limit = 960;
    float scale = std::min(float(limit) / std::max(h, w), 1.0f);
    int rw = int(w * scale), rh = int(h * scale);
    rw = std::max(int(std::round(float(rw) / 32.0f)) * 32, 32);
    rh = std::max(int(std::round(float(rh) / 32.0f)) * 32, 32);
    BGR rimg = resize_bilinear(img, rh, rw);
    float sx = float(w) / rw, sy = float(h) / rh;

    std::vector<float> din;
    planar_rgb_imagenet(rimg, din);
    std::vector<int64_t> dshape = {1, 3, rh, rw};
    std::vector<float> dout;
    std::vector<int64_t> doshape;
    if (!ort_run(*m_impl->det, din, dshape, dout, doshape)) return -3;
    int nd = int(doshape.size());
    int HH = (nd == 4) ? int(doshape[2]) : (nd == 3 ? int(doshape[1]) : 0);
    int WW = (nd == 4) ? int(doshape[3]) : (nd == 3 ? int(doshape[2]) : 0);
    if (HH <= 0 || WW <= 0) return -3;

    std::vector<Box> boxes;
    db_postprocess(dout, HH, WW, sx, sy, w, h, boxes);
    if (boxes.empty()) return 0;

    // 按阅读顺序排序：先 y 后 x
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) {
        if (std::abs(a.y0 - b.y0) > 9) return a.y0 < b.y0;
        return a.x0 < b.x0;
    });

    // ---- 识别 ----
    int n = 0;
    for (const Box &box : boxes) {
        BGR roi = crop(img, box.x0, box.y0, box.x1, box.y1);
        if (roi.h < 2 || roi.w < 2) continue;
        // 倾斜校正（PCA 主轴）
        double ang = pca_angle(roi);
        if (std::abs(ang) > 15.0 && std::abs(ang) < 75.0 && roi.w > 1.3 * roi.h)
            roi = rotate(roi, -ang);

        std::string txt;
        float conf = 0;
        if (!m_impl->rec_image(roi, txt, conf)) continue;
        if (txt.empty()) continue;

        ocr_rec_t rec;
        rec.left_top = point_t(std::max(0, box.x0), std::max(0, box.y0));
        rec.right_bottom = point_t(std::min(w, box.x1), std::min(h, box.y1));
        rec.text = utf8_to_wstring(txt);  // txt 是 PP-OCR 输出的 UTF-8，必须用 CP_UTF8 解码
        rec.confidence = conf;
        result.push_back(rec);
        ++n;
    }
    return n;
}

int OnnxOcrEngine::ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    result.clear();
    if (!m_impl->ok) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    BGR img = to_bgr(data, w, h, bpp);

    // 倾斜校正（PCA 主轴）：与 ocr() 的单行 rec 分支一致
    BGR roi = img;
    double ang = pca_angle(roi);
    if (std::abs(ang) > 15.0 && std::abs(ang) < 75.0 && roi.w > 1.3 * roi.h)
        roi = rotate(roi, -ang);

    std::string txt;
    float conf = 0;
    if (!m_impl->rec_image(roi, txt, conf)) return -3;
    if (txt.empty()) return 0;

    ocr_rec_t rec;
    rec.left_top = point_t(0, 0);
    rec.right_bottom = point_t(w, h);
    rec.text = utf8_to_wstring(txt); // txt 是 PP-OCR 输出的 UTF-8，必须用 CP_UTF8 解码
    rec.confidence = conf;
    result.push_back(rec);
    return 1;
}

} // namespace op::ocr
