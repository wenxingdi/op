#pragma once

#include "OpContext.h"
#include "base/Utils.h"

#include <algorithm>
#include <cstring>   // memcpy（with_mem_region 裁图）
#include <memory>    // shared_ptr（acquire_image返回值）
#include <string>
#include <utility>

namespace op::internal {

// C++ 异常不允许穿出 COM 方法边界（宿主进程会直接 terminate）。
// 统一在此兜底：记录日志后吞掉异常，出参保持调用方已初始化的值。
template <typename Fn>
void guard_exceptions(const char *what, Fn &&fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception &e) {
        setlog("%s: std::exception: %s", what, e.what());
    } catch (const char *msg) {
        setlog("%s: %s", what, msg);
    } catch (...) {
        setlog("%s: unknown exception", what);
    }
}

template <typename Fn>
void capture_region(OpContext *context, long x, long y, long width, long height, Fn &&fn) {
    if (!context)
        return;

    guard_exceptions("capture_region", [&]() {
        if (!context->bkproc.requestCapture(x, y, width, height, context->image_proc._src)) {
            setlog("error requestCapture");
            return;
        }

        // 截图偏移必须和成功捕获后的坐标保持一致，后续找图、找色会用它还原窗口坐标。
        context->image_proc.set_offset(x, y);
        std::forward<Fn>(fn)();
    });
}

template <typename Fn>
void capture_converted_region(OpContext *context, long x1, long y1, long x2, long y2, Fn &&fn) {
    // 这里的坐标已经过 RectConvert，直接作为截图偏移使用。
    capture_region(context, x1, y1, x2 - x1, y2 - y1, std::forward<Fn>(fn));
}

template <typename Fn>
void with_captured_region(OpContext *context, long &x1, long &y1, long &x2, long &y2, long width, long height,
                          Fn &&fn) {
    if (!context || !context->bkproc.check_bind() || !context->bkproc.RectConvert(x1, y1, x2, y2))
        return;

    const long capture_width = (std::min)(width, x2 - x1);
    const long capture_height = (std::min)(height, y2 - y1);
    if (capture_width <= 0 || capture_height <= 0)
        return;

    capture_region(context, x1, y1, capture_width, capture_height, std::forward<Fn>(fn));
}

template <typename Fn>
void with_captured_region(OpContext *context, long &x1, long &y1, long &x2, long &y2, Fn &&fn) {
    if (!context || !context->bkproc.check_bind() || !context->bkproc.RectConvert(x1, y1, x2, y2))
        return;

    const long width = x2 - x1;
    const long height = y2 - y1;
    // 区域被裁剪后再设置偏移，调用方看到的仍是实际截图范围。
    capture_region(context, x1, y1, width, height, std::forward<Fn>(fn));
}


// ============================================================================
// 内存版区域上下文（二期 2026-11-21）
// ============================================================================
//
// 与 with_captured_region 的签名**完全一致**，区别只在"图像从哪来"：
//   - with_captured_region：截屏 -> _src，set_offset(x1,y1)
//   - with_mem_region     ：从 img: 句柄裁出 [x1,y1,x2,y2) -> _src，set_offset(0,0)
//
// 这样屏幕版的全部识别实现（OCR / 找图 / 找色 / YOLO）都能**零改动复用**，
// 新入口只需换一个上下文函数 —— 这是二期"行为与屏幕版严格一致"的结构保证。
//
// 坐标语义：
//   传入的 x1,y1,x2,y2 是**图像内坐标**（相对于 img 句柄的左上角），不是屏幕坐标。
//   回调里 image_proc 的返回值已经是"图像内坐标"（因为 _x1/_y1 与 _dx/_dy 都是 0），
//   调用方不需要再做任何加减。
//
// 边界：
//   - 句柄无效 / 图像为空 -> 不执行回调（与 with_captured_region 截屏失败时一致）
//   - 区域自动 clamp 到图像内；裁剪后为空 -> 不执行回调
//   - 传全 0 区域（0,0,0,0）视为"整张图"，这是脚本里最常见的用法

// 从图像句柄裁出区域并设为当前搜索源。返回 false = 无法继续（回调不会执行）。
inline bool bind_image_region(OpContext *context, const std::wstring &handle, long x1, long y1, long x2, long y2,
                                long *origin_x = nullptr, long *origin_y = nullptr) {
    if (!context)
        return false;

    auto img = context->image_proc.acquire_image(handle);
    if (!img || img->width <= 0 || img->height <= 0 || img->pdata == nullptr)
        return false;

    long left = x1, top = y1, right = x2, bottom = y2;
    // (0,0,0,0) = 整图（脚本习惯写法）
    if (left == 0 && top == 0 && right == 0 && bottom == 0) {
        right = img->width;
        bottom = img->height;
    }
    // clamp（负数/超界都收敛，不报错——与一期 CropImage 的宽容语义一致）
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > img->width)
        right = img->width;
    if (bottom > img->height)
        bottom = img->height;
    if (right <= left || bottom <= top)
        return false;

    const long w = right - left;
    const long h = bottom - top;
    // 整图区域直接复用原像素（零拷贝）；局部区域才裁
    if (left == 0 && top == 0 && w == img->width && h == img->height) {
        context->image_proc._src = *img;
    } else {
        context->image_proc._src.create(static_cast<int>(w), static_cast<int>(h));
        if (context->image_proc._src.pdata == nullptr)
            return false;
        const auto *src = reinterpret_cast<const unsigned char *>(img->pdata);
        auto *dst = reinterpret_cast<unsigned char *>(context->image_proc._src.pdata);
        for (long row = 0; row < h; ++row) {
            const unsigned char *s = src + (size_t(top + row) * size_t(img->width) + size_t(left)) * 4;
            memcpy(dst + size_t(row) * size_t(w) * 4, s, size_t(w) * 4);
        }
    }
    // 关键：偏移清零。回调用的是子区域内坐标，
    // 因此需要把子区域原点（left,top）回传给调用方，由它加回去得到「图像内坐标」。
    // 屏幕版对应的是 _x1 + _dx（截屏偏移 + 客户区偏移），这里对应 left/top。
    context->image_proc.set_offset(0, 0);
    if (origin_x)
        *origin_x = left;
    if (origin_y)
        *origin_y = top;
    return true;
}

template <typename Fn>
void with_mem_region(OpContext *context, const std::wstring &handle, long x1, long y1, long x2, long y2, Fn &&fn,
                     long *origin_x = nullptr, long *origin_y = nullptr) {
    guard_exceptions("with_mem_region", [&]() {
        if (!bind_image_region(context, handle, x1, y1, x2, y2, origin_x, origin_y))
            return;
        std::forward<Fn>(fn)();
    });
}

} // namespace op::internal
