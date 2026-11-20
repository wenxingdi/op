// 图像句柄注册表自测（P4 2026-11-21，内存直操作一期地基）
// 覆盖设计文档 docs/2026-11/内存模块优化_图像句柄注册表_方案_20261121.md §五 的 M1~M9。
//
// 结构：
//   1) ImageRegistryTest.*  —— C API 层（不依赖 OCR 引擎）
//   2) ImageRegistryFindStr —— 依赖 OCR 引擎（画字图后 FindStrMem）
//
// 反向验证（CLAUDE.md 强制）：见文件末尾注释。

#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

#include <op_c_api.h>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

bool SkipOcr() {
    return !test_support::GetEnvString(L"OP_SKIP_OCR_TESTS").empty();
}

// 构造可逐字节校验的花纹像素（避免纯色导致"复制错了也看不出来"）。
std::vector<unsigned char> MakePatternBgra(int w, int h) {
    std::vector<unsigned char> px(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            px[i + 0] = static_cast<unsigned char>((x * 7 + y * 13) % 251);
            px[i + 1] = static_cast<unsigned char>((x * 29 + y * 3) % 241);
            px[i + 2] = static_cast<unsigned char>((x * 5 + y * 31) % 239);
            px[i + 3] = 255;
        }
    }
    return px;
}

std::wstring RegisterBgra(op_handle h, int w, int h_, const std::vector<unsigned char> &px) {
    const wchar_t *handle = OpLoadImageMem(h, px.data(), w, h_, 0, L"bgra");
    return handle ? std::wstring(handle) : std::wstring();
}

} // namespace

// M1：注册后尺寸正确
TEST(ImageRegistryTest, LoadReturnsHandleAndSize) {
    CApiHandle api;
    const int w = 37, h = 19;
    auto px = MakePatternBgra(w, h);
    const std::wstring handle = RegisterBgra(api.handle, w, h, px);
    ASSERT_FALSE(handle.empty()) << "valid pixel buffer must register";
    EXPECT_EQ(handle.rfind(L"img:", 0), 0u) << "handle format must be img:<n>";

    int rw = 0, rh = 0, ch = 0;
    ASSERT_EQ(OpGetImageSize(api.handle, handle.c_str(), &rw, &rh, &ch), 1);
    EXPECT_EQ(rw, w);
    EXPECT_EQ(rh, h);
    EXPECT_EQ(ch, 4) << "internal format is always BGRA";
}

// M2：读回像素逐字节一致
TEST(ImageRegistryTest, GetImageDataIsByteExact) {
    CApiHandle api;
    const int w = 23, h = 11;
    auto px = MakePatternBgra(w, h);
    const std::wstring handle = RegisterBgra(api.handle, w, h, px);
    ASSERT_FALSE(handle.empty());

    std::vector<unsigned char> out(size_t(w) * h * 4, 0);
    const int n = OpGetImageData(api.handle, handle.c_str(), L"raw", out.data(), static_cast<int>(out.size()));
    ASSERT_EQ(n, static_cast<int>(out.size()));
    EXPECT_EQ(out, px) << "raw readback must be byte-exact";
}

// M3：BGR / 灰度输入的转换
TEST(ImageRegistryTest, BgrAndGrayInputConversion) {
    CApiHandle api;
    const int w = 8, h = 6;

    // BGR：每像素 3 字节
    std::vector<unsigned char> bgr(size_t(w) * h * 3);
    for (size_t i = 0; i < bgr.size(); ++i)
        bgr[i] = static_cast<unsigned char>((i * 17) % 256);
    const wchar_t *h1 = OpLoadImageMem(api.handle, bgr.data(), w, h, 0, L"bgr");
    ASSERT_NE(h1, nullptr);
    ASSERT_GT(wcslen(h1), 0u) << "bgr input must be accepted";
    std::vector<unsigned char> out(size_t(w) * h * 4, 0);
    ASSERT_EQ(OpGetImageData(api.handle, h1, L"raw", out.data(), static_cast<int>(out.size())),
              static_cast<int>(out.size()));
    // BGR 转换后 alpha 必须是 255（不透明）
    for (size_t i = 0; i < size_t(w) * h; ++i)
        EXPECT_EQ(out[i * 4 + 3], 255) << "bgr->bgra must set alpha=255 at pixel " << i;
    // B/G/R 顺序保持
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        EXPECT_EQ(out[i * 4 + 0], bgr[i * 3 + 0]) << "B channel mismatch at " << i;
        EXPECT_EQ(out[i * 4 + 1], bgr[i * 3 + 1]) << "G channel mismatch at " << i;
        EXPECT_EQ(out[i * 4 + 2], bgr[i * 3 + 2]) << "R channel mismatch at " << i;
    }

    // 灰度：每像素 1 字节，转换后三通道相等
    std::vector<unsigned char> gray(size_t(w) * h);
    for (size_t i = 0; i < gray.size(); ++i)
        gray[i] = static_cast<unsigned char>((i * 23) % 256);
    const wchar_t *h2 = OpLoadImageMem(api.handle, gray.data(), w, h, 0, L"gray");
    ASSERT_NE(h2, nullptr);
    ASSERT_GT(wcslen(h2), 0u) << "gray input must be accepted";
    std::fill(out.begin(), out.end(), 0);
    ASSERT_EQ(OpGetImageData(api.handle, h2, L"raw", out.data(), static_cast<int>(out.size())),
              static_cast<int>(out.size()));
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        EXPECT_EQ(out[i * 4 + 0], out[i * 4 + 1]) << "gray must replicate to G at " << i;
        EXPECT_EQ(out[i * 4 + 1], out[i * 4 + 2]) << "gray must replicate to R at " << i;
        EXPECT_EQ(out[i * 4 + 0], gray[i]) << "gray value must be preserved at " << i;
    }
}

// M4：stride > width*bpp 时按行正确复制（不串行）
//
// 这是最容易写错的一处：忘记用 stride 而用 width*bpp 会把上一行的填充字节
// 读成下一行的像素，导致整图斜移。断言用"每行首尾像素"精确校验。
TEST(ImageRegistryTest, StrideWithPaddingIsRespected) {
    CApiHandle api;
    const int w = 5, h = 4;
    const int bpp = 4;
    const int min_stride = w * bpp;
    const int stride = min_stride + 12; // 每行 12 字节填充
    std::vector<unsigned char> padded(size_t(stride) * h, 0xEE); // 填充区填 0xEE

    // 每行像素填可区分的值：B = 行号*40 + 列号
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * stride + size_t(x) * bpp;
            padded[i + 0] = static_cast<unsigned char>(y * 40 + x);
            padded[i + 1] = static_cast<unsigned char>(200 - y * 10);
            padded[i + 2] = static_cast<unsigned char>(50 + x * 5);
            padded[i + 3] = 255;
        }
    }

    const wchar_t *raw = OpLoadImageMem(api.handle, padded.data(), w, h, stride, L"bgra");
    ASSERT_NE(raw, nullptr);
    ASSERT_GT(wcslen(raw), 0u);

    std::vector<unsigned char> out(size_t(w) * h * 4, 0);
    ASSERT_EQ(OpGetImageData(api.handle, raw, L"raw", out.data(), static_cast<int>(out.size())),
              static_cast<int>(out.size()));

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t o = (size_t(y) * w + x) * 4;
            EXPECT_EQ(out[o + 0], static_cast<unsigned char>(y * 40 + x))
                << "row " << y << " col " << x << " B mismatched (stride not respected?)";
            EXPECT_EQ(out[o + 1], static_cast<unsigned char>(200 - y * 10)) << "row " << y << " G mismatch";
            EXPECT_EQ(out[o + 2], static_cast<unsigned char>(50 + x * 5)) << "row " << y << " R mismatch";
            // 关键：填充区的 0xEE 绝不能出现在结果里
            EXPECT_NE(out[o + 0], 0xEE) << "padding byte leaked into pixel (row " << y << ", col " << x << ")";
        }
    }
}

// M5：裁剪的像素等于原图对应区域
TEST(ImageRegistryTest, CropMatchesSourceRegion) {
    CApiHandle api;
    const int w = 40, h = 30;
    auto src = MakePatternBgra(w, h);
    const std::wstring handle = RegisterBgra(api.handle, w, h, src);
    ASSERT_FALSE(handle.empty());

    const int cx = 7, cy = 5, cw = 16, ch = 9;
    const wchar_t *cropped = OpCropImage(api.handle, handle.c_str(), cx, cy, cw, ch);
    ASSERT_NE(cropped, nullptr);
    ASSERT_GT(wcslen(cropped), 0u) << "valid crop region must succeed";

    int rw = 0, rh = 0, chan = 0;
    ASSERT_EQ(OpGetImageSize(api.handle, cropped, &rw, &rh, &chan), 1);
    EXPECT_EQ(rw, cw);
    EXPECT_EQ(rh, ch);

    std::vector<unsigned char> out(size_t(cw) * ch * 4, 0);
    ASSERT_EQ(OpGetImageData(api.handle, cropped, L"raw", out.data(), static_cast<int>(out.size())),
              static_cast<int>(out.size()));

    // 逐像素与源图对应位置比对
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const size_t o = (size_t(y) * cw + x) * 4;
            const size_t s = (size_t(cy + y) * w + (cx + x)) * 4;
            EXPECT_EQ(out[o + 0], src[s + 0]) << "crop B mismatch at (" << x << "," << y << ")";
            EXPECT_EQ(out[o + 1], src[s + 1]) << "crop G mismatch at (" << x << "," << y << ")";
            EXPECT_EQ(out[o + 2], src[s + 2]) << "crop R mismatch at (" << x << "," << y << ")";
            EXPECT_EQ(out[o + 3], src[s + 3]) << "crop A mismatch at (" << x << "," << y << ")";
        }
    }
}

// 裁剪越界应 clamp 而不是失败（宽容语义，见方案 §3.4）
TEST(ImageRegistryTest, CropClampsOutOfRange) {
    CApiHandle api;
    const int w = 20, h = 10;
    auto src = MakePatternBgra(w, h);
    const std::wstring handle = RegisterBgra(api.handle, w, h, src);
    ASSERT_FALSE(handle.empty());

    const wchar_t *cropped = OpCropImage(api.handle, handle.c_str(), 15, 5, 100, 100);
    ASSERT_NE(cropped, nullptr);
    ASSERT_GT(wcslen(cropped), 0u);
    int rw = 0, rh = 0, ch = 0;
    ASSERT_EQ(OpGetImageSize(api.handle, cropped, &rw, &rh, &ch), 1);
    EXPECT_EQ(rw, w - 15) << "width must clamp to image edge";
    EXPECT_EQ(rh, h - 5) << "height must clamp to image edge";

    // 完全在图像外的区域应返回空串
    const wchar_t *outside = OpCropImage(api.handle, handle.c_str(), 100, 100, 10, 10);
    ASSERT_NE(outside, nullptr);
    EXPECT_EQ(wcslen(outside), 0u) << "region fully outside must return empty handle";
}

// M6：释放后句柄失效且不崩溃
TEST(ImageRegistryTest, FreeInvalidatesHandle) {
    CApiHandle api;
    const int w = 12, h = 12;
    auto src = MakePatternBgra(w, h);
    const std::wstring handle = RegisterBgra(api.handle, w, h, src);
    ASSERT_FALSE(handle.empty());

    EXPECT_EQ(OpFreeImage(api.handle, handle.c_str()), 1) << "first free must succeed";
    EXPECT_EQ(OpFreeImage(api.handle, handle.c_str()), 0) << "double free must report 0, not crash";

    int rw = 0, rh = 0, ch = 0;
    EXPECT_EQ(OpGetImageSize(api.handle, handle.c_str(), &rw, &rh, &ch), 0) << "freed handle must be invalid";
    EXPECT_EQ(rw, 0);
    std::vector<unsigned char> out(64, 0);
    EXPECT_EQ(OpGetImageData(api.handle, handle.c_str(), L"raw", out.data(), static_cast<int>(out.size())), 0);

    // 不存在的句柄同样安全
    EXPECT_EQ(OpGetImageSize(api.handle, L"img:999999999", &rw, &rh, &ch), 0);
    EXPECT_EQ(OpFreeImage(api.handle, L"img:999999999"), 0);
    EXPECT_EQ(OpGetImageSize(api.handle, L"not-a-handle", &rw, &rh, &ch), 0);
    EXPECT_EQ(OpGetImageSize(api.handle, L"", &rw, &rh, &ch), 0);
}

// M7：注册表满时拒绝新增，且**不淘汰**既有句柄
TEST(ImageRegistryTest, RegistryFullRejectsButKeepsExisting) {
    CApiHandle api;
    OpFreeAllImages(api.handle);
    ASSERT_EQ(OpGetImageCount(api.handle), 0) << "start from a clean registry";

    const int w = 4, h = 4;
    auto src = MakePatternBgra(w, h);
    const std::wstring first = RegisterBgra(api.handle, w, h, src);
    ASSERT_FALSE(first.empty());

    // 上限是 256（实现常量）；一直注册到被拒
    int registered = 1;
    std::wstring last_ok = first;
    for (int i = 0; i < 400; ++i) {
        const wchar_t *hnd = OpLoadImageMem(api.handle, src.data(), w, h, 0, L"bgra");
        if (hnd == nullptr || wcslen(hnd) == 0)
            break;
        last_ok = hnd;
        ++registered;
    }
    EXPECT_GE(registered, 1) << "must accept at least one";
    EXPECT_LE(registered, 256) << "must not exceed the documented cap of 256";
    EXPECT_EQ(OpGetImageCount(api.handle), registered);

    // 关键：满之后**最先注册的句柄仍然有效**（不是整体清空）
    int rw = 0, rh = 0, ch = 0;
    EXPECT_EQ(OpGetImageSize(api.handle, first.c_str(), &rw, &rh, &ch), 1)
        << "the FIRST handle must survive a full registry (no silent eviction)";
    EXPECT_EQ(rw, w);
    EXPECT_EQ(OpGetImageSize(api.handle, last_ok.c_str(), &rw, &rh, &ch), 1) << "the LAST accepted handle must be valid";

    EXPECT_GT(OpFreeAllImages(api.handle), 0);
    EXPECT_EQ(OpGetImageCount(api.handle), 0);
    EXPECT_EQ(OpGetImageSize(api.handle, first.c_str(), &rw, &rh, &ch), 0) << "FreeAllImages must invalidate all";
}

// M9：非法输入全部安全返回，不崩溃
TEST(ImageRegistryTest, InvalidInputIsSafe) {
    CApiHandle api;

    // 空指针 / 0 尺寸 / 负尺寸
    EXPECT_EQ(wcslen(OpLoadImageMem(api.handle, nullptr, 10, 10, 0, L"bgra")), 0u);
    auto px = MakePatternBgra(4, 4);
    EXPECT_EQ(wcslen(OpLoadImageMem(api.handle, px.data(), 0, 10, 0, L"bgra")), 0u);
    EXPECT_EQ(wcslen(OpLoadImageMem(api.handle, px.data(), 10, 0, 0, L"bgra")), 0u);
    EXPECT_EQ(wcslen(OpLoadImageMem(api.handle, px.data(), -5, 10, 0, L"bgra")), 0u);

    // 未知格式必须显式失败（不猜）
    EXPECT_EQ(wcslen(OpLoadImageMem(api.handle, px.data(), 4, 4, 0, L"rgb565")), 0u);

    // stride 小于一行所需（非法）
    EXPECT_EQ(wcslen(OpLoadImageMem(api.handle, px.data(), 4, 4, 4, L"bgra")), 0u)
        << "stride smaller than width*bpp must be rejected";

    // BMP 入口：坏数据
    EXPECT_EQ(wcslen(OpLoadImageBmp(api.handle, nullptr, 100)), 0u);
    const char junk[16] = {0};
    EXPECT_EQ(wcslen(OpLoadImageBmp(api.handle, junk, 16)), 0u);

    // GetImageData 容量不足：返回 0 且不写越界
    {
        auto big = MakePatternBgra(8, 8);
        const std::wstring handle = RegisterBgra(api.handle, 8, 8, big);
        ASSERT_FALSE(handle.empty());
        std::vector<unsigned char> tiny(16, 0xAB);
        EXPECT_EQ(OpGetImageData(api.handle, handle.c_str(), L"raw", tiny.data(), static_cast<int>(tiny.size())), 0)
            << "insufficient buffer must report 0";
        EXPECT_EQ(tiny[0], 0xAB) << "must not write anything into an insufficient buffer";
        // 未知 fmt 也返回 0
        std::vector<unsigned char> full(size_t(8) * 8 * 4, 0);
        EXPECT_EQ(OpGetImageData(api.handle, handle.c_str(), L"png", full.data(), static_cast<int>(full.size())), 0);
    }
}

// BMP 往返：注册 -> 导出 bmp -> 再注册 -> 尺寸与像素一致
TEST(ImageRegistryTest, BmpRoundTrip) {
    CApiHandle api;
    const int w = 13, h = 7;
    auto src = MakePatternBgra(w, h);
    const std::wstring handle = RegisterBgra(api.handle, w, h, src);
    ASSERT_FALSE(handle.empty());

    const int bmp_size = w * h * 4 + 54;
    std::vector<unsigned char> bmp(bmp_size, 0);
    const int n = OpGetImageData(api.handle, handle.c_str(), L"bmp", bmp.data(), bmp_size);
    ASSERT_EQ(n, bmp_size) << "bmp export size must be header(54) + w*h*4";
    // BMP 文件头必须是 'BM'
    ASSERT_EQ(bmp[0], 'B');
    ASSERT_EQ(bmp[1], 'M');

    const wchar_t *reloaded = OpLoadImageBmp(api.handle, bmp.data(), n);
    ASSERT_NE(reloaded, nullptr);
    ASSERT_GT(wcslen(reloaded), 0u) << "exported bmp must be re-registrable";

    int rw = 0, rh = 0, ch = 0;
    ASSERT_EQ(OpGetImageSize(api.handle, reloaded, &rw, &rh, &ch), 1);
    EXPECT_EQ(rw, w);
    EXPECT_EQ(rh, h);

    std::vector<unsigned char> back(size_t(w) * h * 4, 0);
    ASSERT_EQ(OpGetImageData(api.handle, reloaded, L"raw", back.data(), static_cast<int>(back.size())),
              static_cast<int>(back.size()));
    EXPECT_EQ(back, src) << "bmp round trip must preserve pixels";
}

// M8：在图像句柄上做 FindStr（依赖 OCR 引擎）
TEST(ImageRegistryFindStr, FindsTextInsideRegisteredImage) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    // 用 GDI 画一行文字，取出原始 BGRA 再注册（不落盘，正是本功能的目标场景）
    const wchar_t *text = L"测试文字";
    const int font_height = 40;
    const int pad = 12;
    HDC screen = GetDC(nullptr);
    ASSERT_NE(screen, nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ASSERT_NE(dc, nullptr);
    HFONT font = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    ASSERT_NE(font, nullptr);
    HGDIOBJ old_font = SelectObject(dc, font);

    SIZE sz{0, 0};
    ASSERT_TRUE(GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz));
    ASSERT_GT(sz.cx, 0);
    const int w = sz.cx + pad * 2;
    const int h = font_height + pad * 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ASSERT_NE(bmp, nullptr);
    ASSERT_NE(bits, nullptr);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    RECT rc{0, 0, w, h};
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    SelectObject(dc, font);
    TextOutW(dc, pad, pad, text, static_cast<int>(wcslen(text)));

    std::vector<unsigned char> pixels(static_cast<unsigned char *>(bits),
                                     static_cast<unsigned char *>(bits) + size_t(w) * h * 4);

    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);

    // 注意：C API 的字符串返回值由 handle->string_result 复用缓冲承载，
    // **下一次调用会覆盖它**（既有设计，见 call_string）。所以必须立刻复制成 wstring。
    const wchar_t *raw_handle = OpLoadImageMem(api.handle, pixels.data(), w, h, w * 4, L"bgra");
    ASSERT_NE(raw_handle, nullptr);
    ASSERT_GT(wcslen(raw_handle), 0u);
    const std::wstring handle(raw_handle);

    const wchar_t *json = OpFindStrMem(api.handle, handle.c_str(), L"[\"测试文字\"]", 0.3);
    ASSERT_NE(json, nullptr) << "FindStrMem must return JSON for a valid handle";
    const std::wstring out(json);
    EXPECT_NE(out.find(L"\"found\":1"), std::wstring::npos)
        << "must find the text drawn inside the registered image: " << std::string(out.begin(), out.end());

    // 无效句柄必须明确失败（nullptr），不是"没找到"
    EXPECT_EQ(OpFindStrMem(api.handle, L"img:999999", L"[\"x\"]", 0.3), nullptr);
    EXPECT_EQ(OpFindStrMem(api.handle, L"", L"[\"x\"]", 0.3), nullptr);
    // 句柄有效但 JSON 非法 -> 也是 nullptr
    EXPECT_EQ(OpFindStrMem(api.handle, handle.c_str(), L"[not json", 0.3), nullptr);
}

// 反向验证说明（人工执行，CLAUDE.md 要求"故意改错必须 FAIL"）：
//   1) 把 copy_to_bgra 里 "row = src + y*stride" 改成 "y*width*bpp"，
//      ImageRegistryTest.StrideWithPaddingIsRespected 必须 FAIL（填充字节串入像素）。
//   2) 把 crop_image 里源的 "(y0+row)*img->width + x0" 去掉 x0，
//      ImageRegistryTest.CropMatchesSourceRegion 必须 FAIL。
//   3) 把 register_image_shared 的上限行为改成 "clear + 存入"（静默淘汰），
//      ImageRegistryTest.RegistryFullRejectsButKeepsExisting 必须 FAIL
//      （first 句柄会失效）。
//   4) 把 find_str_mem 的 acquire_image 返回 nullptr 直接用，
//      ImageRegistryFindStr.FindsTextInsideRegisteredImage 必须 FAIL。
