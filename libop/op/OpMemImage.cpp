// 内存直操作二期：内存版识别入口门面（2026-11-21）
//
// 设计见 docs/2026-11/内存直操作二期_全家桶_方案_20261121.md
//
// 实现原则：**逐行照抄屏幕版，只把 with_captured_region 换成 with_mem_region**。
// 这样屏幕版的全部识别逻辑（OCR / 找图 / 找色 / YOLO）零改动复用，
// "行为与屏幕版严格一致"是结构保证，不靠测试去发现不一致。
//
// 唯一有意的差异：区域参数 (0,0,0,0) 在屏幕版无效（无区域可截），
// 在内存版表示**整张图** —— 脚本里这就是"全图查找"的自然写法。

#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "image/Image.h"
#include "YoloDetectCore.h"      // YOLO 检测核心（与 OpYolo.cpp 共用）
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <string>

namespace {

// 把 img: 句柄与矩形转成 with_mem_region 的入参。
// 句柄为空时返回 true（调用方会走 with_mem_region 内部的失败分支，
// 从而得到与屏幕版"截屏失败"一致的默认返回值），保持各门面代码形态统一。
std::wstring SafeHandle(const wchar_t *handle) {
    return handle ? std::wstring(handle) : std::wstring();
}

} // namespace

// ---- 识字 ----

void op::Op::OcrFromMemEx2(const wchar_t *image_handle, long x1, long y1, long x2, long y2, const wchar_t *color,
                           double sim, std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    const std::wstring color_text = color ? color : L"";
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        // 与屏幕版 OcrEx2 同源：有字库走字库匹配，无字库原图直识
        m_context->image_proc.ocr_ex2(color_text, sim, retstr);
    });
}

long op::Op::OcrAutoColorFromMemEx2(const wchar_t *image_handle, long x1, long y1, long x2, long y2,
                                    const wchar_t *color, double sim, std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    const std::wstring color_text = color ? color : L"";
    long ct = 0;
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        // 与屏幕版 OcrAutoColorEx2 同源（含智能选路：图里没目标色时自动回退原图）
        ct = m_context->image_proc.ocr_auto_color_ex2(color_text, sim, retstr);
    });
    return ct;
}

// ---- 找图 ----

void op::Op::FindPicFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2, const wchar_t *files,
                            const wchar_t *delta_color, double sim, long dir, long *x, long *y, long *ret) {
    const std::wstring handle = SafeHandle(image_handle);
    long found_x = -1;
    long found_y = -1;
    // 与屏幕版一致：FindPic 是**大漠索引语义**（返回命中图片序号，-1=未找到），
    // 不是布尔契约。初值必须是 -1L。
    internal::set_result(ret, -1L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);
    long ox = 0, oy = 0;
    internal::with_mem_region(
        m_context.get(), handle, x1, y1, x2, y2,
        [&]() {
            internal::set_result(ret,
                                 m_context->image_proc.FindPic(files, delta_color, sim, dir, found_x, found_y));
            internal::set_result(x, found_x);
            internal::set_result(y, found_y);
        },
        &ox, &oy);
    // 能力层返回的是 _src（即裁剪后的子区域）内坐标；加回区域原点才是「图像内坐标」，
    // 否则调用方传 (60,0,120,60) 却收到 x=1，与屏幕版语义不一致。
    if (found_x >= 0)
        internal::set_result(x, found_x + ox);
    if (found_y >= 0)
        internal::set_result(y, found_y + oy);
}

void op::Op::FindPicExFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2, const wchar_t *files,
                              const wchar_t *delta_color, double sim, long dir, std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindPicEx(files, delta_color, sim, dir, retstr);
    });
}

// ---- 找色 / 取色 / 比色 / 计数 ----

void op::Op::FindColorFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2, const wchar_t *color,
                              double sim, long dir, long *x, long *y, long *ret) {
    const std::wstring handle = SafeHandle(image_handle);
    long found_x = -1;
    long found_y = -1;
    internal::set_result(ret, 0L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);
    long ox = 0, oy = 0;
    internal::with_mem_region(
        m_context.get(), handle, x1, y1, x2, y2,
        [&]() {
            internal::set_result(ret, m_context->image_proc.FindColor(color, sim, dir, found_x, found_y));
            internal::set_result(x, found_x);
            internal::set_result(y, found_y);
        },
        &ox, &oy);
    if (found_x >= 0)
        internal::set_result(x, found_x + ox);
    if (found_y >= 0)
        internal::set_result(y, found_y + oy);
}

void op::Op::FindColorExFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2,
                                const wchar_t *color, double sim, long dir, std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindColorEx(color, sim, dir, retstr);
    });
}

void op::Op::GetColorFromMem(const wchar_t *image_handle, long x, long y, std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    // 单点取色：区域收成 1x1（与屏幕版 CmpColor 的 1x1 区域做法一致）
    internal::with_mem_region(m_context.get(), handle, x, y, x + 1, y + 1, [&]() {
        // 注意：with_mem_region 已把原点设为该点，故这里用 (0,0)
        retstr = m_context->image_proc.GetColor(0, 0);
    });
}

void op::Op::CmpColorFromMem(const wchar_t *image_handle, long x, long y, const wchar_t *color, double sim,
                             long *ret) {
    internal::set_result(ret, 0L);
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x, y, x + 1, y + 1, [&]() {
        internal::set_result(ret, m_context->image_proc.CmpColor(0, 0, color, sim));
    });
}

void op::Op::GetColorNumFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2,
                                const wchar_t *color, double sim, long *ret) {
    internal::set_result(ret, 0L);
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        // 注意：GetColorNum 的签名是 (color, sim)，区域由 _src 本身决定
        // （with_mem_region 已把 _src 裁成目标区域），与屏幕版同源。
        internal::set_result(ret, m_context->image_proc.GetColorNum(color, sim));
    });
}

// ---- 多色串 / 色块 ----

void op::Op::FindMultiColorFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2,
                                   const wchar_t *first_color, const wchar_t *offset_color, double sim, long dir,
                                   long *x, long *y, long *ret) {
    const std::wstring handle = SafeHandle(image_handle);
    long found_x = -1;
    long found_y = -1;
    internal::set_result(ret, 0L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);
    long ox = 0, oy = 0;
    internal::with_mem_region(
        m_context.get(), handle, x1, y1, x2, y2,
        [&]() {
            internal::set_result(ret, m_context->image_proc.FindMultiColor(first_color, offset_color, sim, dir,
                                                                           found_x, found_y));
            internal::set_result(x, found_x);
            internal::set_result(y, found_y);
        },
        &ox, &oy);
    if (found_x >= 0)
        internal::set_result(x, found_x + ox);
    if (found_y >= 0)
        internal::set_result(y, found_y + oy);
}

void op::Op::FindMultiColorExFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2,
                                     const wchar_t *first_color, const wchar_t *offset_color, double sim, long dir,
                                     std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindMultiColorEx(first_color, offset_color, sim, dir, retstr);
    });
}

void op::Op::FindColorBlockExFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2,
                                     const wchar_t *color, double sim, long count, long height, long width,
                                     std::wstring &retstr) {
    retstr.clear();
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindColorBlockEx(color, sim, count, height, width, retstr);
    });
}


// ---- YOLO ----

void op::Op::YoloDetectFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2, double conf,
                               double iou, std::wstring &retjson, long *ret) {
    retjson = op::yolo::core::kFailureJson;
    internal::set_result(ret, 0L);
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        // 与屏幕版 YoloDetect 同一套检测 + JSON 构建（共享 YoloDetectCore.h）；
        // with_mem_region 已把偏移设为 0，故**不加任何平移**，结果就是图像内坐标。
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        op::yolo::core::Options no_filter;
        op::yolo::core::BuildJson(res, no_filter, /*extra_fields=*/false, retjson);
        internal::set_result(ret, n);
    });
}

long op::Op::YoloDetectFromMemEx2(const wchar_t *image_handle, long x1, long y1, long x2, long y2, double conf,
                                  double iou, const wchar_t *json_options, std::wstring &retjson, long *ret) {
    retjson = op::yolo::core::kFailureJson;
    internal::set_result(ret, 0L);

    op::yolo::core::Options opt;
    const std::wstring options_text = json_options ? json_options : L"";
    if (!op::yolo::core::ParseOptions(options_text, opt))
        return -1; // 参数非法：显式失败，与屏幕版 Ex2 一致

    const std::wstring handle = SafeHandle(image_handle);
    long found = 0;
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        op::yolo::core::BuildJson(res, opt, /*extra_fields=*/true, retjson);
        found = 0;
        for (size_t i = 0; i < res.size(); ++i) {
            if (op::yolo::core::KeepDetection(res, i, opt))
                ++found;
        }
        if (opt.max_results > 0 && found > opt.max_results)
            found = opt.max_results;
    });
    internal::set_result(ret, found);
    return found;
}
