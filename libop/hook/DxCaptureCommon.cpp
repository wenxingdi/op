#include "DxCaptureCommon.h"

#include "DisplayHook.h"
#include "../base/AutomationModes.h"

namespace op::hook {

void CopyImageData(char *dst_, const char *src_, int rows_, int cols_, int rowPitch, int fmt_) {
    // 未识别格式不该走到这里；真走到说明调用方漏判了。宁可不写，也不要按错误的
    // 字节布局把像素涂进共享内存（那正是 HDR 交换链静默错色的成因）。
    if (fmt_ == IBF_UNSUPPORTED) {
        return;
    }
    // assert(rowsPitch >= cols_ * 4);
    if (rowPitch == cols_ * (fmt_ == IBF_R8G8B8 ? 3 : 4)) {
        if (fmt_ == IBF_B8G8R8A8) {
            ::memcpy(dst_, src_, rows_ * cols_ * 4);
        } else if (fmt_ == IBF_R8G8B8A8) {
            // pixels count
            int n = rows_ * cols_;

            for (int i = 0; i < n; ++i) {
                dst_[0] = src_[2]; // b
                dst_[1] = src_[1]; // g
                dst_[2] = src_[0]; // r
                dst_[3] = src_[3]; // a
                dst_ += 4;
                src_ += 4;
            }
        } else {
            // pixels count
            int n = rows_ * cols_;
            for (int i = 0; i < n; ++i) {
                *dst_++ = *src_++;
                *dst_++ = *src_++;
                *dst_++ = *src_++;
                *dst_++ = (char)0xff; // dst is 4 B
            }
        }
    } else {
        const int dstPitch = cols_ * 4;
        if (fmt_ == IBF_B8G8R8A8) {
            for (int i = 0; i < rows_; ++i) {
                ::memcpy(dst_, src_, dstPitch);
                dst_ += dstPitch;
                src_ += rowPitch;
            }

        } else if (fmt_ == IBF_R8G8B8A8) {
            // pixels count

            for (int i = 0; i < rows_; ++i) {
                for (int j = 0; j < cols_; ++j) {
                    const char *p = src_ + j * 4; // offset
                    dst_[0] = p[2];               // b
                    dst_[1] = p[1];               // g
                    dst_[2] = p[0];               // r
                    dst_[3] = p[3];               // a
                    dst_ += 4;                    // notirc that dst ptr is increasing
                }
                src_ += rowPitch; // row increase
            }

        } else {
            for (int i = 0; i < rows_; ++i) {
                for (int j = 0; j < cols_; ++j) {
                    const char *p = src_ + j * 3; // offset
                    dst_[0] = p[0];               // b
                    dst_[1] = p[1];               // g
                    dst_[2] = p[2];               // r
                    dst_[3] = (char)0xff;         // a
                    dst_ += 4;                    // notice that dst ptr is increasing
                }
                src_ += rowPitch; // row increase
            }
        }
    }
}

DXGI_FORMAT NormalizeDxgiFormat(DXGI_FORMAT format) {
    // 读回到 CPU 前统一转成非 SRGB 格式，避免 staging/resolve 阶段因格式不匹配失败。
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return format;
}

int GetImageBufferFormat(DXGI_FORMAT format) {
    // 白名单：只对"读回后能按已知字节布局解释"的格式返回构造值。
    // 未列出的格式一律返回 IBF_UNSUPPORTED —— 曾经的默认分支是 "return IBF_R8G8B8A8"，
    // 会把 R10G10B10A2(HDR 常见) / R16G16B16A16_FLOAT(ScRGB) 按 4 字节 RGBA 逐通道重排，
    // 颜色完全错乱且全程静默（Capture ret=1、尺寸字节数都正常）。调用方必须显式处理本值。
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return IBF_B8G8R8A8;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return IBF_R8G8B8A8;
    default:
        return IBF_UNSUPPORTED;
    }
}

} // namespace op::hook
