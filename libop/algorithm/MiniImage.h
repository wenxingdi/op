#ifndef OP_ALGORITHM_MINIIMAGE_H_
#define OP_ALGORITHM_MINIIMAGE_H_
// 极简 BGR 图像容器与基础转换——OnnxOcrEngine 与 OnnxYoloEngine 共用。
// 2026-10-15 自两引擎匿名命名空间提取单源化（原两份 byte 级相同，逐函数对比实证）。
// 仅收拢完全相同的五件；OCR 自用的 crop/rotate/pca_angle/planar_rgb_imagenet 与
// YOLO 自用的 letterbox 仍各留本地。
#include "../base/Types.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace op::img {

// UTF-8 字节串 -> wstring。模型 keys/类别名均为 UTF-8 编码，必须用 CP_UTF8 解码；
// 不能用 Utils::_s2wstring（它按 CP_ACP 解码，会令中文乱码）。
inline std::wstring utf8_to_wstring(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// ---- 极简 BGR 图像容器（打包 HWC，每像素 3 字节）----
struct BGR {
    int h = 0, w = 0;
    std::vector<uint8_t> d;
    const uint8_t *at(int y, int x) const { return &d[(size_t(y) * w + x) * 3]; }
    uint8_t *at(int y, int x) { return &d[(size_t(y) * w + x) * 3]; }
};

inline BGR make_bgr(int h, int w) {
    BGR m;
    m.h = h;
    m.w = w;
    m.d.assign(size_t(h) * w * 3, 0);
    return m;
}

inline BGR to_bgr(byte *data, int w, int h, int bpp) {
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

inline BGR resize_bilinear(const BGR &s, int dh, int dw) {
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

} // namespace op::img

#endif // OP_ALGORITHM_MINIIMAGE_H_
