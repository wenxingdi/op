// NOMINMAX must precede any Windows header so std::min/std::max aren't
// hijacked by the min/max macros (error C2589 with onnxruntime_cxx_api.h
// pulling in windows.h).
#define NOMINMAX
#include "OnnxOcrEngine.h"
#include "ocr_models.h"
#include "../base/Utils.h"
#include "../algorithm/MiniImage.h"
#include <onnxruntime_cxx_api.h>
#include <windows.h>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <thread>

using std::cout;
using std::endl;

namespace op::ocr {
// 极简 BGR 工具链五件已单源化到 algorithm/MiniImage.h（与另一引擎 byte 级相同的副本删除）。
using op::img::BGR;
using op::img::make_bgr;
using op::img::resize_bilinear;
using op::img::to_bgr;
using op::img::utf8_to_wstring;

namespace {
const double PI = 3.14159265358979323846;

// 从当前 DLL 资源段加载模型 buffer（资源随 op DLL 一起编译进去）
// 用「函数地址反查模块句柄」：不再硬编码 DLL 名，x64/x86 及任意重命名均正确。
std::vector<uint8_t> LoadRes(int id) {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadRes), &h);
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
// 180-degree flip (used by cls orientation fix).
BGR rotate180(const BGR &s) {
    if (s.h == 0 || s.w == 0) return BGR{};
    BGR o = make_bgr(s.h, s.w);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            const uint8_t *p = s.at(s.h - 1 - y, s.w - 1 - x);
            uint8_t *q = o.at(y, x);
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
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

// 判断紧贴段内是否存在"内部墨迹间断"：单行段的上下边界就是墨迹边界（上游水平
// 投影分段已紧贴墨迹），若段内出现 >=2 连续空行，说明竖直方向上还叠着别的行
// （多行内容）。1px 空洞视为二值化噪声桥接。行判据用"有/无墨迹"（>2% 行宽），
// 不用 20% 阈值——窄行在宽图里大部分字行达不到 20%，会把单行误判成多行。
// 背景色用 64 档粗量化直方图众数估计（与 ink_extent 同一套判据）。
bool has_internal_ink_gap(const BGR &img) {
    if (img.w <= 0 || img.h <= 0) return false;
    auto bin_of = [](const uint8_t *p) {
        return (((p[0] >> 4) & 3) << 4) | (((p[1] >> 4) & 3) << 2) | ((p[2] >> 4) & 3);
    };
    std::vector<int> histogram(64, 0);
    for (int y = 0; y < img.h; y += 2)
        for (int x = 0; x < img.w; x += 2) histogram[bin_of(img.at(y, x)) & 63]++;
    const int bg_bin = int(std::max_element(histogram.begin(), histogram.end()) - histogram.begin());
    const int min_ink = (std::max)(1, img.w / 50); // >2% 行宽视为有墨
    auto row_ink = [&](int y) {
        int ink = 0;
        for (int x = 0; x < img.w; ++x)
            if ((bin_of(img.at(y, x)) & 63) != bg_bin) ++ink;
        return ink;
    };
    int first = -1, last = -1;
    for (int y = 0; y < img.h; ++y)
        if (row_ink(y) >= min_ink) {
            if (first < 0) first = y;
            last = y;
        }
    if (first < 0 || last - first < 2) return false;
    int gap = 0;
    for (int y = first; y <= last; ++y) {
        if (row_ink(y) < min_ink) {
            if (++gap >= 2) return true;
        } else {
            gap = 0;
        }
    }
    return false;
}

// 运行一次 ONNX 推理（单输入单输出，输入由 shape 描述）。
// ms（可选出参，P3）：累加本次 Run 的墙钟耗时；nullptr 不计时（零开销）。
// failed（可选出参）：Run 抛异常或被吞时 +1，供 profile 暴露"静默失败"次数。
bool ort_run(Ort::Session &sess, const std::vector<float> &in, const std::vector<int64_t> &shape,
             std::vector<float> &out, std::vector<int64_t> &oshape, double *ms = nullptr,
             long *failed = nullptr) {
    Ort::AllocatorWithDefaultOptions alloc;
    // CreateTensor 只要非 const 指针（签名历史原因）；ORT 推理不写输入张量，
    // 直接借用调用方 buffer，省一次整份拷贝（det 输入 960×960×3 float ≈ 11MB/次 OCR）。
    Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value v = Ort::Value::CreateTensor<float>(mi, const_cast<float *>(in.data()), in.size(), shape.data(),
                                                   shape.size());
    // 1.19.2 API：返回 RAII 智能指针，作用域结束自动释放，无需 ReleaseInputName/OutputName。
    auto iname = sess.GetInputNameAllocated(0, alloc);
    auto oname = sess.GetOutputNameAllocated(0, alloc);
    const char *iname_c = iname.get();
    const char *oname_c = oname.get();
    Ort::RunOptions ropts;
    std::vector<Ort::Value> res;
    const auto t_begin = std::chrono::steady_clock::now();
    try {
        res = sess.Run(ropts, &iname_c, &v, 1, &oname_c, 1);
    } catch (const std::exception &e) {
        // ORT 抛异常时必须在引擎内吞掉并返回 false，不能让它冒到 C 边界
        // （call_string 只回空串，上层无法区分"识别失败"和"没有文字"）。
        std::cerr << "ort_run: Run threw: " << e.what() << std::endl;
        if (ms) *ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin).count();
        if (failed) ++*failed;
        return false;
    } catch (...) {
        std::cerr << "ort_run: Run threw unknown exception" << std::endl;
        if (ms) *ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin).count();
        if (failed) ++*failed;
        return false;
    }
    if (ms) *ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin).count();
    if (res.empty()) {
        if (failed) ++*failed;
        return false;
    }
    oshape = res[0].GetTensorTypeAndShapeInfo().GetShape();
    const float *p = res[0].GetTensorData<float>();
    size_t n = 1;
    for (auto d : oshape) n *= (d > 0 ? size_t(d) : 1);
    out.assign(p, p + n);
    return true;
}

// DB 后处理：对 det 概率图做阈值+膨胀+连通域，输出原图坐标轴对齐框。
// 坐标映射用 iw/WW、ih/HH（见下），无需调用方传缩放比。
struct Box {
    int x0, y0, x1, y1;
};
void db_postprocess(const std::vector<float> &score, int HH, int WW, int iw, int ih, std::vector<Box> &boxes) {
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
            int pad_x = int(cw * 0.1f) + 5, pad_y = int(ch * 0.1f) + 5;
            boxes.push_back({X0 - pad_x, Y0 - pad_y, X1 + pad_x, Y1 + pad_y});
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

// 逐字跨度（P0）：CTC 折叠后一个字符所占的时间步区间与字级置信度。
struct CharSpan {
    int ki;          // 字典下标（keys[ki]），非类别号
    float prob;      // 字级置信度（仅该字符占用步的概率均值）
    int t0, t1;      // 占用的时间步闭区间
    int x1, x2;      // ROI 内像素坐标 [x1,x2)，rec_images 内由 t 映射得出
    float conf_sum;  // 该字符占用步的概率累加（用于求均值）
};

// CTC 贪婪解码：argmax 后折叠重复并去 blank(0)。每步以最优类别概率作为置信度。
// 维度判定：类别数 C 是较大的那一维，序列长 T 是较小的那一维。
// mask：可选类别白名单（非空即启用）。mask[0] 恒 1=blank 始终允许；mask[c]==1
//   允许类别 c 参与 argmax，c >= mask.size() 视为禁止（覆盖模型类别多于 keys 的
//   尾部错位，无需 mask 与 C 等长）。白名单外字符概率被屏蔽，形近字符
//   （0/O、,/.、1/l）竞争被消除。空 mask = 全字典（默认）。
// out 为指针以支持批量推理按行切片（batch Run 后每行是连续的 {1,T,C}/{1,C,T}）。
// chars（可选出参）：非空时回填逐字跨度。字级概率与行级 conf 口径不同——
//   行级 conf 是**全部时间步**（含 blank 与重复步）的均值，字级只统计该字符
//   占用的步，两者不可互推（见 OcrService.h 的 ocr_char_t 注释）。
std::string ctc_greedy(const float *out, const std::vector<int64_t> &shape,
                        const std::vector<std::string> &keys, float &conf,
                        const std::vector<uint8_t> &mask,
                        std::vector<CharSpan> *chars = nullptr) {
    int A = int(shape[1]), B = int(shape[2]);
    int C = std::max(A, B), T = std::min(A, B);
    bool tc_layout = (B == C); // [N,T,C]：类别在 dim2(B)；[N,C,T]：类别在 dim1(A)
    const bool constrained = !mask.empty();
    const size_t MC = mask.size();
    double conf_sum = 0.0;
    int nsteps = 0;
    std::string text;
    int lastc = -1;
    if (chars) chars->clear();
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
        if (chars) {
            // 维护"当前字符"的时间跨度。CTC collapse 下同一字符连续重复只保留
            // 首个输出字符，但区间要延伸到最后一次出现的步（t1 随重复步更新）。
            // 当前字符在遇到 blank 或切换到别的字符时关闭（t1 固定）。
            if (best != 0 && best == lastc) {
                if (!chars->empty()) { // 重复步：延长区间并累计概率
                    chars->back().t1 = t;
                    chars->back().conf_sum += prob;
                }
            } else if (best == 0) {
                if (!chars->empty() && chars->back().t1 < 0) chars->back().t1 = t - 1;
            } else {
                if (!chars->empty() && chars->back().t1 < 0) chars->back().t1 = t - 1;
                CharSpan cs;
                cs.ki = best - 1; // blank=0，keys[i] 对应类别 i+1
                cs.prob = 0.f;
                cs.t0 = t;
                cs.t1 = -1; // 未关闭，守到下一个 blank/切换或 T 末尾
                cs.conf_sum = prob;
                chars->push_back(cs);
            }
        } else if (best != 0 && best != lastc) {
            int ki = best - 1; // blank=0，keys[i] 对应类别 i+1
            if (ki >= 0 && ki < (int)keys.size()) text += keys[ki];
        }
        lastc = best;
        conf_sum += prob;
        ++nsteps;
    }
    conf = nsteps > 0 ? float(conf_sum / nsteps) : 0.0f;
    if (chars) {
        if (!chars->empty() && chars->back().t1 < 0) chars->back().t1 = T - 1;
        for (CharSpan &cs : *chars) {
            int n = cs.t1 - cs.t0 + 1;
            cs.prob = n > 0 ? cs.conf_sum / n : cs.conf_sum;
            if (cs.ki >= 0 && cs.ki < (int)keys.size()) text += keys[cs.ki];
        }
    }
    return text;
}
} // namespace

// 进程级共享 Ort::Env。
// 每个引擎实例各自持有 Env 会在 LRU 淘汰（析构实例）时反复创建/销毁 Env，
// 实测导致后续新建实例偶发推理失败（模型能加载，但识别结果为空）。
// ONNX Runtime 也建议全进程复用一个 Env；Env 本身不持有模型，共享没有副作用。
Ort::Env &shared_env() {
    static Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "op-ocr"};
    return env;
}

class OnnxOcrEngine::Impl {
public:
    Ort::SessionOptions sopts;
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;
    std::unique_ptr<Ort::Session> cls; // 0/180 orientation fix
    std::vector<std::string> keys;
    std::vector<uint8_t> m_det, m_rec, m_cls, m_keys; // 持有模型 buffer 保证会话生命周期内有效
    CharSetRule m_rule;                        // --charset= 解析结果（空=全字典）
    std::vector<uint8_t> m_mask;               // 类别白名单（尺寸=keys+1；空=全放行）
    float oscale = 2.0f;                       // entry upscale factor for low-res small text (<=1 disables)
    bool cls_on = false;                       // --cls= 0/180 orientation fix (default: off, speed first)
    bool ok = false;

    // P3 profile (2026-11-21): per-section timings of the last OCR call.
    // Deliberately lock-free: this is diagnostic data, and taking a lock inside the
    // hot path would perturb the very numbers we measure. Concurrent callers on the
    // same instance may therefore see any one call's values (last writer wins) --
    // documented in OcrService.h's ocr_profile_t comment.
    ocr_profile_t m_prof;
    bool m_prof_on = true; // OP_OCR_PROFILE=0 disables it (saves two clock reads per call)

    // 对单张 BGR 图直接做 rec（跳过检测）：缩放+归一化+CTC 解码。
    // 返回 false = 推理失败；txt 为 UTF-8（可为空）。
    // spans（可选出参）：逐字跨度（P0），t→x 已按该图自己的 tw 映射为 ROI 内像素坐标。
    //
    // P2 动态长宽比（2026-11-25，与批量路径 rec_images 对齐）：宽度不再固定 320，
    //   而是按本图长宽比动态决定 RW = clamp(align32(ceil(48*ratio)), 320, 960)。
    //   原实现固定 320（6.67:1），超过 ~40 字的长行会被横向压扁、字宽趋于 1~2px，
    //   识别精度显著下降。rec 模型是动态 T 输入，ONNX 原生支持，无需重新导出。
    //   短行（ratio <= 6.67）RW 仍为 320，行为与改动前完全一致。
    bool rec_image(const BGR &roi, std::string &txt, float &conf,
                   std::vector<CharSpan> *spans = nullptr) {
        const int RH = 48;
        const int RW_MIN = 320;  // 基线宽度（6.67:1），短行不变
        const int RW_MAX = 960;  // 与 det 的 960 限制对齐
        const int RW_ALIGN = 32;
        float ratio = float(roi.w) / roi.h;
        const int want = int(std::ceil(48.0f * ratio));
        int RW = std::max(RW_MIN, ((want + RW_ALIGN - 1) / RW_ALIGN) * RW_ALIGN);
        if (RW > RW_MAX) RW = RW_MAX;
        int tw = want;
        if (tw > RW) tw = RW;
        m_prof.rec_width = RW; // 单行路径同样暴露实际生效宽度（profile 口径与批量一致）
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
        ++m_prof.rec_calls;
        if (!ort_run(*rec, rin, rshape, rout, roshape, m_prof_on ? &m_prof.rec_ms : nullptr,
                     m_prof_on ? &m_prof.runs_failed : nullptr))
            return false;
        if (roshape.size() != 3) return false;
        txt = ctc_greedy(rout.data(), roshape, keys, conf, m_mask, spans);
        if (spans && !spans->empty()) {
            // Same rule as rec_images: anchor the step->pixel mapping to the
            // ROI's ink extent, then split at neighbour midpoints (raw spans are
            // 1-2 steps wide and mark centers, not extents).
            const int T = int(std::min(roshape[1], roshape[2]));
            int ink_b = 0, ink_e = tw;
            ink_extent(roi, tw, ink_b, ink_e);
            const double span_px = double(ink_e - ink_b);
            std::vector<double> cents(spans->size());
            for (size_t k = 0; k < spans->size(); ++k) {
                const double step_mid =
                    double((*spans)[k].t0) + 0.5 * double((*spans)[k].t1 - (*spans)[k].t0 + 1);
                cents[k] = double(ink_b) + step_mid * span_px / double(T);
            }
            double gap = (spans->size() > 1) ? (cents.back() - cents.front()) / double(spans->size() - 1)
                                             : span_px / 4.0;
            if (gap <= 0.0) gap = span_px / 4.0;
            const double lo = std::max(0.0, cents.front() - gap * 0.5);
            const double hi = std::min(double(tw), cents.back() + gap * 0.5);
            for (size_t k = 0; k < spans->size(); ++k) {
                double left = (k == 0) ? lo : 0.5 * (cents[k - 1] + cents[k]);
                double right = (k + 1 == spans->size()) ? hi : 0.5 * (cents[k] + cents[k + 1]);
                (*spans)[k].x1 = int(std::lround(left));
                (*spans)[k].x2 = int(std::lround(right));
                if ((*spans)[k].x2 <= (*spans)[k].x1) (*spans)[k].x2 = (*spans)[k].x1 + 1;
            }
        }
        return true;
    }

    // 计算 ROI 的墨迹水平范围，换算到 resize 后的 tw 宽坐标系（逐字映射用）。
    // CTC 时间步只铺在文字上，不铺在整个 ROI 上；det 框左右常带空白边距，
    // 若按 [0,tw] 映射会把每个字心整体平移一个左边距（实测 CJK 行偏移 ~11px）。
    // 判据：按列统计非背景像素，取第一个与最后一个"有笔墨"的列；
    // 三档容差从严到宽，全失败时退化整幅 [0,tw] 不报错（保证不影响识别本身）。
    static void ink_extent(const BGR &roi, int tw, int &out_begin, int &out_end) {
        out_begin = 0;
        out_end = tw;
        if (tw <= 0 || roi.w <= 0 || roi.h <= 0) return;
        // 以 64 档粗量化颜色直方图的众数估计背景色。
        // 必须扫描**整幅** ROI，不能只看第一列：二值化路径的 ROI 左上角可能正好
        // 落在笔画上（颜色路径输入是白字黑底，x=0 列常是文字），只采样一列会把
        // 前景当背景，导致墨迹范围判成整幅之外。
        auto bin_of = [](const uint8_t *p) {
            return (((p[0] >> 4) & 3) << 4) | (((p[1] >> 4) & 3) << 2) | ((p[2] >> 4) & 3);
        };
        std::vector<int> histogram(64, 0);
        for (int y = 0; y < roi.h; y += 2)
            for (int x = 0; x < roi.w; x += 2) histogram[bin_of(roi.at(y, x)) & 63]++;
        const int bg_bin = int(std::max_element(histogram.begin(), histogram.end()) - histogram.begin());
        auto is_ink = [&](int x) {
            int count = 0;
            for (int y = 0; y < roi.h; ++y) {
                if ((bin_of(roi.at(y, x)) & 63) != bg_bin) ++count;
            }
            return count * 5 > roi.h; // >20% 的行非背景视为该列有笔墨
        };
        int first = -1, last = -1;
        for (int x = 0; x < roi.w; ++x) {
            if (is_ink(x)) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (first < 0 || last < first) return; // 无笔墨：保持整幅
        const double sx = double(tw) / double(roi.w); // ROI 像素 -> tw 坐标
        out_begin = std::max(0, int(std::floor(first * sx)));
        out_end = std::min(tw, int(std::ceil((last + 1) * sx)));
        if (out_end <= out_begin) {
            out_begin = 0;
            out_end = tw;
        }
    }

    // 批量 rec：所有 ROI 统一 pad 到 RH×RWmax 后叠成 {N,3,RH,RWmax} 一次 Run，再逐行 CTC
    // 解码。每行输入与 rec_image 单跑逐字节一致（同缩放/归一化代码），数学等价；
    // batch 数值可能有微差（同模型同权重，行为邻近）。
    // 失败语义：整批 Run 失败返回 false（单次 session 故障），由调用方按推理失败处理。
    //
    // P2 动态长宽比（2026-11-20）：RW 不再固定 320，而是按**本批最大长宽比**动态决定
    //   RWmax = min(kRecMaxW, max(320, ceil(48*ratio_max/kRecWAlign)*kRecWAlign))。
    //   原实现固定 320（6.67:1），超过 ~40 字的长行会被横向压扁、字宽趋于 1~2px，
    //   识别精度显著下降（实测 220x236 任务追踪图 17 行里有长行被压扁）。
    //   rec 模型是动态 T 输入，ONNX 原生支持，无需重新导出。
    //   短行（绝大多数）RW 仍为 320，行为与改动前**完全一致**；
    //   只有批次内存在超宽行时才加宽，且同一批次共用同一 RW（batch 要求张量同形）。
    bool rec_images(const std::vector<BGR> &rois, std::vector<std::string> &texts,
                    std::vector<float> &confs, std::vector<std::vector<CharSpan>> *char_spans = nullptr) {
        const int RH = 48;
        const int RW_MIN = 320;   // 基线宽度（6.67:1），短行不变
        const int RW_MAX = 960;   // 上限：与 det 的 960 限制对齐，避免超长行吃满内存
        const int RW_ALIGN = 32;  // 与 det 输入对齐习惯一致，便于 ORT 走优化 kernel
        const size_t N = rois.size();
        texts.assign(N, std::string());
        confs.assign(N, 0.0f);
        if (char_spans) char_spans->assign(N, {});
        if (N == 0) return true;
        // 先算本批动态 RW：取各 ROI 所需宽度（按 RH=48 等比缩放后的 ceil）的最大值
        int RW = RW_MIN;
        for (size_t n = 0; n < N; ++n) {
            const BGR &roi = rois[n];
            if (roi.h <= 0) continue;
            const double want = std::ceil(double(RH) * double(roi.w) / double(roi.h));
            const int aligned = int(std::ceil(want / RW_ALIGN) * RW_ALIGN);
            if (aligned > RW) RW = aligned;
        }
        if (RW > RW_MAX) RW = RW_MAX;
        if (RW < RW_MIN) RW = RW_MIN;
        std::vector<float> rin(size_t(3) * RH * RW * N, 0.0f);
        std::vector<int> widths(N, 0); // each line's own resized width tw (P0 char mapping)
        for (size_t n = 0; n < N; ++n) {
            const BGR &roi = rois[n];
            float ratio = roi.h > 0 ? float(roi.w) / roi.h : 1.0f;
            int tw = int(std::ceil(48.0f * ratio));
            if (tw > RW) tw = RW;
            widths[n] = tw;
            BGR rs = resize_bilinear(roi, RH, tw);
            float *dst = rin.data() + size_t(3) * RH * RW * n;
            for (int c = 0; c < 3; ++c) {
                int src = 2 - c; // RGB 顺序（PP-OCR rec 约定）
                for (int y = 0; y < RH; ++y)
                    for (int x = 0; x < tw; ++x) {
                        float v = rs.at(y, x)[src] / 255.0f;
                        v = (v - 0.5f) / 0.5f; // rec 归一化：[-1,1]
                        dst[size_t(c) * RH * RW + size_t(y) * RW + x] = v;
                    }
            }
        }
        std::vector<int64_t> rshape = {int64_t(N), 3, RH, RW};
        std::vector<float> rout;
        std::vector<int64_t> roshape;
        ++m_prof.rec_calls;
        m_prof.rec_width = RW; // P2 dynamic width is worth exposing in profile
        if (!ort_run(*rec, rin, rshape, rout, roshape, m_prof_on ? &m_prof.rec_ms : nullptr,
                     m_prof_on ? &m_prof.runs_failed : nullptr))
            return false;
        if (roshape.size() != 3 || roshape[0] != int64_t(N)) return false;
        const size_t per = size_t(roshape[1]) * size_t(roshape[2]); // 每行输出 {T,C} 或 {C,T}
        const int T = int(std::min(roshape[1], roshape[2]));
        for (size_t n = 0; n < N; ++n) {
            if (char_spans) {
                texts[n] = ctc_greedy(rout.data() + per * n, {1, roshape[1], roshape[2]}, keys,
                                      confs[n], m_mask, &(*char_spans)[n]);
                // Map time steps to ROI pixel x.
                //
                // Subtlety 1: the denominator is this line's own resized width tw
                // (NOT T or 320) - each ROI is resized to its own tw and then
                // zero-padded to RW, so t/T*320 would land in the padded
                // coordinate system instead of the ROI's.
                //
                // Subtlety 2: the CTC steps only span the *text*, not the whole
                // ROI. Measured on a CJK line: det box = [1,144] (tw covers 143px)
                // but the ink runs 13..135, and the steps are distributed evenly
                // across the ink, not across the box. Mapping t->[0,tw] therefore
                // offsets every char center left by the box's left blank margin
                // (measured ~11px). Anchor the mapping to the ROI's ink extent.
                const int tw = widths[n];
                std::vector<CharSpan> &sp = (*char_spans)[n];
                if (!sp.empty()) {
                    // Ink columns of this ROI in its resized (tw-wide) space.
                    int ink_b = 0, ink_e = tw;
                    ink_extent(rois[n], tw, ink_b, ink_e);
                    // tw is the rec input width at RH=48, i.e. ceil(48*roi.w/roi.h).
                    // It is NOT roi.w: for a 66x16 ROI tw≈198, so a span expressed in
                    // tw units is ~3x too large for the caller, which adds the det box
                    // origin (in roi pixels) to it. Convert back to ROI pixels here so
                    // every consumer sees one consistent coordinate system.
                    const double to_roi = double(rois[n].w) / double(std::max(1, tw));
                    ink_b = int(std::lround(ink_b * to_roi));
                    ink_e = int(std::lround(ink_e * to_roi));
                    if (ink_e <= ink_b) {
                        ink_b = 0;
                        ink_e = rois[n].w;
                    }
                    const double ink_w = double(ink_e - ink_b);
                    // Place char centers using the *actual* CTC step positions,
                    // affine-fitted so the first char starts at the ink's left
                    // edge and the last ends at its right edge.
                    //
                    // Why not just k/(n-1)? Real lines are not evenly spaced
                    // (measured on 需要80点元神力: mixed CJK + digits make the
                    // per-char advance vary ~2x), and why not mid_k/T? The model
                    // output is padded to T steps while the decoded chars only
                    // occupy a sub-range (measured T=40 with chars at steps
                    // 5,12,19,26), so /T squashes the line into the left half.
                    // Fit t -> x through the step extents instead: robust to both
                    // padding and uneven spacing.
                    const double t_first = double(sp.front().t0);
                    const double t_last = double(sp.back().t1 + 1);
                    const double t_span = std::max(1.0, t_last - t_first);
                    std::vector<double> cents(sp.size());
                    for (size_t k = 0; k < sp.size(); ++k) {
                        const double mid = double(sp[k].t0) + 0.5 * double(sp[k].t1 - sp[k].t0 + 1);
                        const double frac = (mid - t_first) / t_span;
                        cents[k] = double(ink_b) + frac * ink_w;
                    }
                    // Extend the outer two chars by half an average advance so the
                    // first/last glyph owns its full width, then clip to the ink.
                    double gap = (sp.size() > 1) ? (cents.back() - cents.front()) / double(sp.size() - 1)
                                                 : ink_w;
                    if (gap <= 0.0) gap = ink_w;
                    const double lo = std::max(double(ink_b), cents.front() - gap * 0.5);
                    const double hi = std::min(double(ink_e), cents.back() + gap * 0.5);
                    for (size_t k = 0; k < sp.size(); ++k) {
                        double left = (k == 0) ? lo : 0.5 * (cents[k - 1] + cents[k]);
                        double right = (k + 1 == sp.size()) ? hi : 0.5 * (cents[k] + cents[k + 1]);
                        sp[k].x1 = int(std::lround(left));
                        sp[k].x2 = int(std::lround(right));
                        if (sp[k].x2 <= sp[k].x1) sp[k].x2 = sp[k].x1 + 1;
                    }
                }
            } else {
                texts[n] = ctc_greedy(rout.data() + per * n, {1, roshape[1], roshape[2]}, keys,
                                      confs[n], m_mask);
            }
        }
        return true;
    }

    // 批量 CLS 方向分类（PP-LCNet x0.25 textline_ori，0/180 两分类）。
    // 输入直接 resize 到 80x160（H,W，官方预处理不保持长宽比）+ ImageNet 归一化，
    // 与 planar_rgb_imagenet 完全一致；rois 中被判为 180 度的元素原地翻转。
    // 返回 false = cls 会话缺失或推理失败（调用方忽略，按未旋转继续）。
    bool cls_fix(std::vector<BGR> &rois) {
        if (!cls || rois.empty()) return false;
        const int CH = 80, CW = 160;
        const size_t N = rois.size();
        const size_t plane = size_t(3) * CH * CW;
        std::vector<float> cin(plane * N);
        for (size_t n = 0; n < N; ++n) {
            BGR rs = resize_bilinear(rois[n], CH, CW);
            std::vector<float> one;
            planar_rgb_imagenet(rs, one); // resizes to exactly plane floats
            std::copy(one.begin(), one.end(), cin.begin() + plane * n);
        }
        std::vector<int64_t> cshape = {int64_t(N), 3, CH, CW};
        std::vector<float> cout_;
        std::vector<int64_t> coshape;
        ++m_prof.cls_calls;
        if (!ort_run(*cls, cin, cshape, cout_, coshape, m_prof_on ? &m_prof.cls_ms : nullptr,
                     m_prof_on ? &m_prof.runs_failed : nullptr))
            return false;
        if (coshape.size() != 2 || coshape[0] != int64_t(N) || coshape[1] < 2) return false;
        for (size_t n = 0; n < N; ++n) {
            const float *row = cout_.data() + size_t(n) * size_t(coshape[1]);
            int best = 0;
            float bv = -1e30f;
            for (int64_t c = 0; c < coshape[1]; ++c)
                if (row[c] > bv) {
                    bv = row[c];
                    best = int(c);
                }
            if (best == 1) rois[n] = rotate180(rois[n]); // 180_degree
        }
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
        // 2026-11-24: V4 快档已移除，固定加载 PP-OCRv6 small 资源。
        m_det = LoadRes(IDR_OCR_DET);
        m_rec = LoadRes(IDR_OCR_REC);
        m_keys = LoadRes(IDR_OCR_KEYS);
        m_cls = LoadRes(IDR_OCR_CLS); // absence is non-fatal
        if (m_det.empty() || m_rec.empty()) {
            cout << "OnnxOcrEngine: model resource missing (det/rec)" << endl;
            return false;
        }
        det = std::make_unique<Ort::Session>(shared_env(), m_det.data(), m_det.size(), sopts);
        rec = std::make_unique<Ort::Session>(shared_env(), m_rec.data(), m_rec.size(), sopts);
        if (!m_cls.empty())
            cls = std::make_unique<Ort::Session>(shared_env(), m_cls.data(), m_cls.size(), sopts);
        else
            cout << "OnnxOcrEngine: cls resource missing, orientation fix disabled" << endl;
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
        cout << "OnnxOcrEngine: models loaded (v6), keys=" << keys.size()
             << ", charset_mask=" << (m_mask.empty() ? 0 : int(m_mask.size()) - 1)
             << ", oscale=" << oscale << ", cls=" << (cls ? (cls_on ? "on" : "off") : "missing") << endl;
        ok = true;
        return true;
    }
};

OnnxOcrEngine::OnnxOcrEngine() : m_impl(std::make_unique<Impl>()) {}
OnnxOcrEngine::~OnnxOcrEngine() = default;

// P3: expose the last call's per-section timings. Returns a copy so callers never
// see a torn read while a concurrent inference is updating the counters.
ocr_profile_t OnnxOcrEngine::profile() const {
    if (!m_impl) return ocr_profile_t{};
    return m_impl->m_prof;
}

int OnnxOcrEngine::init(const std::wstring &engine, const std::wstring &dllName,
                        const std::vector<std::string> &argv) {
    (void)dllName;
    (void)engine; // 档位名已在 OcrService 归一化；V4 快档 2026-11-24 移除，统一走 v6
    // P3: OP_OCR_PROFILE=0 turns off the per-section timers. Default on: the cost is
    // two steady_clock reads per OCR call (well under 1us), negligible next to det/rec.
    {
        wchar_t buf[8] = {0};
        const DWORD n = GetEnvironmentVariableW(L"OP_OCR_PROFILE", buf, 8);
        if (n > 0 && n < 8 && buf[0] == L'0')
            m_impl->m_prof_on = false;
    }
    bool have_threads = false;
    int threads = 0;
    bool cls_set = false;
    // 解析 --charset=<规则>：如 --charset=@zh0123456789[],-+；--threads=N：推理 intra-op 线程数
    for (const std::string &a : argv) {
        if (a.rfind("--charset=", 0) == 0) {
            m_impl->m_rule = parse_charset_rule(a.substr(10));
        } else if (a.rfind("--threads=", 0) == 0) {
            threads = atoi(a.c_str() + 10);
            have_threads = true;
        } else if (a.rfind("--oscale=", 0) == 0) {
            m_impl->oscale = float(atof(a.c_str() + 9));
        } else if (a.rfind("--cls=", 0) == 0) {
            m_impl->cls_on = atoi(a.c_str() + 6) != 0;
            cls_set = true;
        }
    }
    // CLS 默认关闭：0/180 度翻转检测额外一次批量推理，实测约 2ms；
    // 有倒置文本的场景用 --cls=1 打开（此时才做翻转修正）。
    if (!cls_set) m_impl->cls_on = false;
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
    return ocr_common(data, w, h, bpp, result, nullptr);
}

// 带逐字信息的 ocr（P0）：result 与 ocr() 完全一致，ext 为等长逐字附加信息。
int OnnxOcrEngine::ocr_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result,
                           vocr_line_ext_t &ext) {
    ext.clear();
    return ocr_common(data, w, h, bpp, result, &ext);
}

// 共享实现：ext_out 非空时额外回填逐字信息（不影响 result/返回值的既有语义）。
int OnnxOcrEngine::ocr_common(byte *data, int w, int h, int bpp, vocr_rec_t &result,
                              vocr_line_ext_t *ext_out) {
    // P3 profile: reset per-section counters for this call, then stamp the entry time.
    // The scope guard writes total_ms and the final line/char counts on every exit
    // path (including early returns), so the profile is never left half-filled.
    Impl &impl = *m_impl;
    const auto t_entry = std::chrono::steady_clock::now();
    impl.m_prof = ocr_profile_t{};
    impl.m_prof.oscale = impl.oscale;
    struct ProfileGuard {
        Impl &impl;
        std::chrono::steady_clock::time_point t_entry;
        vocr_rec_t &result;
        vocr_line_ext_t *ext;
        ~ProfileGuard() {
            impl.m_prof.lines = long(result.size());
            if (ext) {
                long n = 0;
                for (const auto &le : *ext) n += long(le.chars.size());
                impl.m_prof.chars = n;
            }
            impl.m_prof.total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_entry).count();
            impl.m_prof.valid = true;
        }
    } guard{impl, t_entry, result, ext_out};

    result.clear();
    if (!impl.ok) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    BGR img = to_bgr(data, w, h, bpp);

    // ---- 低清小字增强：入口 2x 放大（--oscale= 可配，<=1 关闭）----
    // 小图放大后 det 输入分辨率直接翻倍（仍在 960 limit 内），rec 裁剪源也更清晰；
    // 大图放大无益（det 本就被限到 960），按 max side 阈值跳过，零额外成本。
    // 坐标最后统一除回 up，调用方看到的仍是原图像素坐标。
    // 已二值化的输入（指定颜色路径喂入的纯白纯黑图）不放大：灰度细节已不存在，
    // 放大只白花 4 倍 det 成本。按 >2% 非黑白像素判定为非二值图。
    // 颜色路径需要的"二值化前放大"不在这一层做——一旦二值化完成，抗锯齿边缘已经
    // 被判成背景，再放大补不回来。那一步由 ImageSearchService 的 pre_scale
    // （--pre_scale=，默认 2x）在 str2binaryfbk 之前完成。
    const float up = impl.oscale;
    const int ow = w, oh = h;
    bool binarized = false;
    if (up > 1.0f && std::max(w, h) < 960) {
        const size_t n = size_t(w) * h;
        size_t bad = 0, lim = n / 50 + 1;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t *p = &img.d[i * 3];
            const uint8_t mx = (std::max)({p[0], p[1], p[2]});
            const uint8_t mn = (std::min)({p[0], p[1], p[2]});
            if ((mx > 8) && (mn < 247)) { // 中间调像素：非二值图
                if (++bad > lim)
                    break;
            }
        }
        binarized = bad <= lim;
        if (!binarized) {
            img = resize_bilinear(img, int(h * up + 0.5f), int(w * up + 0.5f));
            w = img.w;
            h = img.h;
        }
    }
    impl.m_prof.upscale = (!binarized && up > 1.0f);
    const auto t_pre = std::chrono::steady_clock::now();
    impl.m_prof.pre_ms = std::chrono::duration<double, std::milli>(t_pre - t_entry).count();

    // ---- 检测 ----
    int limit = 960;
    float scale = std::min(float(limit) / std::max(h, w), 1.0f);
    int rw = int(w * scale), rh = int(h * scale);
    rw = std::max(int(std::round(float(rw) / 32.0f)) * 32, 32);
    rh = std::max(int(std::round(float(rh) / 32.0f)) * 32, 32);
    BGR rimg = resize_bilinear(img, rh, rw);

    std::vector<float> din;
    planar_rgb_imagenet(rimg, din);
    std::vector<int64_t> dshape = {1, 3, rh, rw};
    std::vector<float> dout;
    std::vector<int64_t> doshape;
    ++impl.m_prof.det_calls;
    if (!ort_run(*impl.det, din, dshape, dout, doshape, impl.m_prof_on ? &impl.m_prof.det_ms : nullptr,
                 impl.m_prof_on ? &impl.m_prof.runs_failed : nullptr))
        return -3;
    int nd = int(doshape.size());
    int HH = (nd == 4) ? int(doshape[2]) : (nd == 3 ? int(doshape[1]) : 0);
    int WW = (nd == 4) ? int(doshape[3]) : (nd == 3 ? int(doshape[2]) : 0);
    if (HH <= 0 || WW <= 0) return -3;

    std::vector<Box> boxes;
    db_postprocess(dout, HH, WW, w, h, boxes);
    impl.m_prof.boxes = long(boxes.size());
    if (boxes.empty()) return 0;

    // 按阅读顺序排序：先 y 后 x
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) {
        if (std::abs(a.y0 - b.y0) > 9) return a.y0 < b.y0;
        return a.x0 < b.x0;
    });

    // ---- 识别 ----
    // 先收集全部 ROI（含倾斜校正），再一次性批量 rec：N 框共用一次 session.Run
    // （原逐框各跑一次，N 框串行 N 次，约 10-15ms/次）。
    std::vector<BGR> rois;
    std::vector<Box> kept;
    rois.reserve(boxes.size());
    kept.reserve(boxes.size());
    // Track which lines were PCA-rotated: their per-char x offsets cannot be mapped
    // back to the axis-aligned box without an inverse rotation, so chars degrade to
    // the whole-line box instead of reporting wrong coordinates.
    std::vector<uint8_t> rotated;
    rotated.reserve(boxes.size());
    for (const Box &box : boxes) {
        BGR roi = crop(img, box.x0, box.y0, box.x1, box.y1);
        if (roi.h < 2 || roi.w < 2) continue;
        // 倾斜校正（PCA 主轴）：仅扁矩形（行文本）才计算 PCA 角度——
        // 旋转守卫本就含 w>1.3h，非扁矩形算了也不会转，纯浪费三趟全像素遍历。
        uint8_t was_rotated = 0;
        if (roi.w > 1.3 * roi.h) {
            double ang = pca_angle(roi);
            if (std::abs(ang) > 15.0 && std::abs(ang) < 75.0) {
                roi = rotate(roi, -ang);
                was_rotated = 1;
            }
        }
        rois.push_back(std::move(roi));
        kept.push_back(box);
        rotated.push_back(was_rotated);
    }
    if (rois.empty()) return 0;
    // CLS 0/180 方向修正：批量一次 Run，180 度的 ROI 原地翻转后再进 rec。
    // 失败静默忽略保持原图。
    if (impl.cls_on) impl.cls_fix(rois);
    std::vector<std::string> texts;
    std::vector<float> confs;
    std::vector<std::vector<CharSpan>> spans;
    const bool want_chars = (ext_out != nullptr);
    if (!impl.rec_images(rois, texts, confs, want_chars ? &spans : nullptr)) return -3;
    // Everything after rec is post-processing (per-char mapping + result assembly).
    const auto t_post = std::chrono::steady_clock::now();

    if (ext_out) ext_out->clear();
    int n = 0;
    for (size_t i = 0; i < rois.size(); ++i) {
        if (texts[i].empty()) continue;

        ocr_rec_t rec;
        const Box &box = kept[i];
        // 放大识别时框坐标处于放大图像素系，除回 up 还原为原图坐标
        const float inv = (!binarized && up > 1.0f) ? 1.0f / up : 1.0f;
        rec.left_top =
            point_t(int(std::lround(std::max(0, box.x0) * inv)), int(std::lround(std::max(0, box.y0) * inv)));
        rec.right_bottom =
            point_t(int(std::lround(std::min(w, box.x1) * inv)), int(std::lround(std::min(h, box.y1) * inv)));
        rec.text = utf8_to_wstring(texts[i]); // txt 是 PP-OCR 输出的 UTF-8，必须用 CP_UTF8 解码
        rec.confidence = confs[i];
        result.push_back(rec);
        if (ext_out) {
            ocr_line_ext_t le;
            // 框底贴图边界 < 2px 视为可能没读全。判据必须在除回 inv 之后，
            // 否则 2x 放大时阈值实际退化为 1px。
            le.is_partial = (int(std::lround(std::min(h, box.y1) * inv)) >= h - 2);
            if (rotated[i]) {
                // 旋转行：不给逐字坐标，退化为整行框
                le.partial_chars = true;
                for (const CharSpan &cs : spans[i]) {
                    ocr_char_t c;
                    c.ch = utf8_to_wstring(impl.keys[cs.ki]);
                    if (c.ch.empty()) continue; // 空串字典条目，不对应可见字形
                    c.confidence = cs.prob;
                    c.t0 = cs.t0;
                    c.t1 = cs.t1;
                    c.x1 = rec.left_top.x;
                    c.x2 = rec.right_bottom.x;
                    le.chars.push_back(std::move(c));
                }
            } else {
                // 未旋转：t→ROI x 已在 rec_images 内用该行自己的 tw 映射好，
                // 这里乘 inv 除回放大并加框左偏移还原到原图坐标。
                for (const CharSpan &cs : spans[i]) {
                    ocr_char_t c;
                    c.ch = utf8_to_wstring(impl.keys[cs.ki]);
                    // 字典里存在空串条目（空格/占位符），模型偶尔会输出它。它不对应
                    // 任何可见字形，必须丢弃：否则 chars 会比 text 多一项，且带一个
                    // 无意义的坐标区间，污染"逐字与文本一一对应"的契约（A3/A5）。
                    if (c.ch.empty()) continue;
                    c.confidence = cs.prob;
                    c.t0 = cs.t0;
                    c.t1 = cs.t1;
                    c.x1 = int(std::lround(cs.x1 * inv)) + rec.left_top.x;
                    c.x2 = int(std::lround(cs.x2 * inv)) + rec.left_top.x;
                    // Clamp into the line box: A1 (chars inside box) must hold for
                    // every consumer, including the color path which re-binarizes
                    // and may shift the glyphs slightly. Order matters: clamp x2
                    // first, then x1 against the (possibly already clamped) x2 --
                    // doing it the other way round can yield x1 > x2.
                    if (c.x2 > rec.right_bottom.x) c.x2 = rec.right_bottom.x;
                    if (c.x1 < rec.left_top.x) c.x1 = rec.left_top.x;
                    if (c.x1 >= c.x2) c.x1 = std::max(rec.left_top.x, c.x2 - 1);
                    if (c.x2 <= c.x1) c.x2 = std::min(rec.right_bottom.x, c.x1 + 1);
                    le.chars.push_back(std::move(c));
                }
            }
            ext_out->push_back(std::move(le));
        }
        ++n;
    }
    impl.m_prof.post_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_post).count();
    return n;
}

int OnnxOcrEngine::ocr_line(byte *data, int w, int h, int bpp, vocr_rec_t &result) {
    return ocr_line_common(data, w, h, bpp, result, nullptr);
}

int OnnxOcrEngine::ocr_line_ext(byte *data, int w, int h, int bpp, vocr_rec_t &result,
                                vocr_line_ext_t &ext) {
    ext.clear();
    return ocr_line_common(data, w, h, bpp, result, &ext);
}

int OnnxOcrEngine::ocr_line_common(byte *data, int w, int h, int bpp, vocr_rec_t &result,
                                   vocr_line_ext_t *ext_out) {
    // P3 profile: same contract as ocr_common (reset on entry, totals on every exit).
    Impl &impl = *m_impl;
    const auto t_entry = std::chrono::steady_clock::now();
    impl.m_prof = ocr_profile_t{};
    impl.m_prof.oscale = impl.oscale;
    impl.m_prof.rec_width = 0; // rec_image 内部会回填实际生效的动态宽度
    struct ProfileGuard {
        Impl &impl;
        std::chrono::steady_clock::time_point t_entry;
        vocr_rec_t &result;
        vocr_line_ext_t *ext;
        ~ProfileGuard() {
            impl.m_prof.lines = long(result.size());
            if (ext) {
                long n = 0;
                for (const auto &le : *ext) n += long(le.chars.size());
                impl.m_prof.chars = n;
            }
            impl.m_prof.total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_entry).count();
            impl.m_prof.valid = true;
        }
    } guard{impl, t_entry, result, ext_out};

    result.clear();
    if (!impl.ok) return -1;
    if (data == nullptr || w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) return -1;
    if (size_t(w) * h * bpp > 64ULL * 1024 * 1024) return -2;

    BGR img = to_bgr(data, w, h, bpp);

    // 多行内容自动分流：rec 是单行识别器，整幅多行图被压到 48px 高必然糊成一条
    // 条纹带（220x236 的 17 行窗口 -> 每行约 2.8px，远低于训练分辨率），输出注定
    // 不可读。判据用"紧贴段内是否有竖直墨迹间断"（>=2 连续空行）：单行段的墨迹
    // 上下连续，出现内部空行即说明叠了多行。误分流代价仅多一次 det（数 ms），
    // 漏分流则结果必然错误，故判据取宽。
    if (img.h >= 40 && has_internal_ink_gap(img)) {
        cout << "OnnxOcrEngine: ocr_line input looks multi-line (" << img.w << "x" << img.h
             << "), routing to det path" << endl;
        return ocr_common(data, w, h, bpp, result, ext_out);
    }

    // 倾斜校正（PCA 主轴）：与 ocr() 的单行 rec 分支一致——仅扁矩形才计算 PCA。
    BGR roi = img;
    bool rotated = false;
    if (roi.w > 1.3 * roi.h) {
        double ang = pca_angle(roi);
        if (std::abs(ang) > 15.0 && std::abs(ang) < 75.0) {
            roi = rotate(roi, -ang);
            rotated = true;
        }
    }
    impl.m_prof.pre_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_entry).count();

    // CLS 0/180 方向修正（与 ocr() 批量路径同一模型/策略）
    if (impl.cls_on) {
        std::vector<BGR> one{roi};
        if (impl.cls_fix(one)) roi = std::move(one[0]);
    }

    const auto t_post = std::chrono::steady_clock::now();
    std::string txt;
    float conf = 0;
    std::vector<CharSpan> spans;
    if (!impl.rec_image(roi, txt, conf, ext_out ? &spans : nullptr)) return -3;
    if (txt.empty()) return 0;

    ocr_rec_t rec;
    rec.left_top = point_t(0, 0);
    rec.right_bottom = point_t(w, h);
    rec.text = utf8_to_wstring(txt); // txt 是 PP-OCR 输出的 UTF-8，必须用 CP_UTF8 解码
    rec.confidence = conf;
    result.push_back(rec);
    if (ext_out) {
        ocr_line_ext_t le;
        le.is_partial = false; // single-line path has no det box to clip against
        le.partial_chars = rotated;
        for (const CharSpan &cs : spans) {
            ocr_char_t c;
            c.ch = utf8_to_wstring(impl.keys[cs.ki]);
            if (c.ch.empty()) continue; // 空串字典条目，不对应可见字形
            c.confidence = cs.prob;
            c.t0 = cs.t0;
            c.t1 = cs.t1;
            if (rotated) {
                c.x1 = 0;
                c.x2 = w;
            } else {
                // rec_image already mapped t→x in whole-image pixels (whole image == ROI here).
                c.x1 = std::max(0, std::min(cs.x1, w - 1));
                c.x2 = std::max(c.x1 + 1, std::min(cs.x2, w));
            }
            le.chars.push_back(std::move(c));
        }
        ext_out->push_back(std::move(le));
    }
    impl.m_prof.post_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_post).count();
    return 1;
}

} // namespace op::ocr
