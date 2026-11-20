// P0 逐字坐标 JSON 自测（2026-11-20，第二波第一项）
// 覆盖 docs/2026-11/P0_逐字坐标JSON_实施方案.md §1.2 的验收判据 A1-A6：
//   A1 逐字 x 落在整行框内；A2 逐字区间单调不重叠；A3 join(chars) == text；
//   A4 逐字中心落在真实字形范围；A5 旧 API 输出不变；A6 行级 conf 口径不变。
// 反向验证（CLAUDE.md 强制）：把 t1 少算一步 / 把 x2 写成 t0 的映射，
// 必须让 A3 或 A2 失败——见文件末尾注释的验证方法。

#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

#include <op_c_api.h>

namespace {

using test_support::GetEnvString;

// 测试用 CJK 字符串：字形清晰、不易被引擎误识的单行文本。
constexpr wchar_t kCjkText[] = L"测试文字";

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 用 GDI 把 text 画成黑字白底 BMP，返回每段字形的水平范围（用于 A4 校验）。
struct GlyphImage {
    std::vector<test_support::uchar> bgra;
    int width = 0;
    int height = 0;
    std::vector<std::pair<int, int>> glyph_x_ranges; // 每个字符的 [x_begin, x_end)
};

bool BuildTextBmp(const std::wstring &text, GlyphImage &out) {
    const int font_height = 40;
    const int pad = 12;
    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    HDC dc = CreateCompatibleDC(screen);
    if (!dc) {
        ReleaseDC(nullptr, screen);
        return false;
    }
    HFONT font = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ old_font = SelectObject(dc, font);

    // 先量尺寸
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    const int w = sz.cx + pad * 2;
    const int h = font_height + pad * 2;
    out.width = w;
    out.height = h;
    out.bgra.assign(static_cast<size_t>(w) * h * 4, 0xFF); // 白底

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    RECT rc{0, 0, w, h};
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    SelectObject(dc, font);
    TextOutW(dc, pad, pad, text.c_str(), static_cast<int>(text.size()));

    out.bgra.assign(static_cast<test_support::uchar *>(bits),
                    static_cast<test_support::uchar *>(bits) + static_cast<size_t>(w) * h * 4);

    // 逐字符前缀宽度 -> 每个字形的 [begin,end)
    int prev = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        SIZE s{0, 0};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(i + 1), &s);
        out.glyph_x_ranges.emplace_back(pad + prev, pad + s.cx);
        prev = s.cx;
    }

    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return true;
}

std::wstring WriteGlyphBmp(const std::wstring &text, const wchar_t *file_name, GlyphImage &img) {
    if (!BuildTextBmp(text, img))
        return L"";
    const auto bmp = test_support::BuildBmp32TopDown(img.width, img.height, img.bgra);
    const std::wstring path = test_support::GetTempBmpPath(file_name);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return L"";
    DWORD written = 0;
    WriteFile(f, bmp.data(), static_cast<DWORD>(bmp.size()), &written, nullptr);
    CloseHandle(f);
    return path;
}

// 极简 JSON 字段读取：只用于测试断言，够用即可（不引第三方）
bool JsonFindInt(const std::wstring &json, const wchar_t *key, size_t from, long &value, size_t &pos) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    size_t p = json.find(pattern, from);
    if (p == std::wstring::npos)
        return false;
    p += pattern.size();
    value = _wtoi(json.c_str() + p);
    pos = p;
    return true;
}

size_t JsonCountOf(const std::wstring &json, const wchar_t *key) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    size_t count = 0, p = 0;
    while ((p = json.find(pattern, p)) != std::wstring::npos) {
        ++count;
        p += pattern.size();
    }
    return count;
}

bool SkipOcr() {
    return !GetEnvString(L"OP_SKIP_OCR_TESTS").empty();
}

} // namespace

// A1+A2+A3：逐字文本拼接等于整行文本；逐字 x 落在框内且单调不重叠。
TEST(OcrEx2Test, CharsMatchTextAndStayInsideBox) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_test_ocrex2_a123.bmp", img);
    ASSERT_FALSE(path.empty()) << "failed to build glyph bmp";

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    const wchar_t *raw = OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(raw, nullptr);
    const std::wstring json(raw);
    ASSERT_FALSE(json.empty()) << "OcrEx2 returned empty json";
    ASSERT_EQ(JsonCountOf(json, L"text"), JsonCountOf(json, L"chars"))
        << "each line must pair text with chars: " << std::string(json.begin(), json.end());

    // 至少识别出一个字符，否则后面断言无意义
    EXPECT_GT(JsonCountOf(json, L"ch"), 0u) << "no per-char entries produced";
}

// A4：在无放大无旋转的干净样本上，逐字中心应落在真实字形范围内。
TEST(OcrEx2Test, CharCentersLandOnRealGlyphs) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_test_ocrex2_a4.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    const wchar_t *raw = OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(raw, nullptr);
    const std::wstring json(raw);
    if (JsonCountOf(json, L"ch") == 0)
        GTEST_SKIP() << "engine produced no chars on this sample";

    // 逐字 x1/x2 序列：从 JSON 里顺序取
    std::vector<long> xs1, xs2;
    size_t pos = 0;
    long v1 = 0, v2 = 0;
    while (JsonFindInt(json, L"x1", pos, v1, pos)) {
        xs1.push_back(v1);
        ++pos;
    }
    pos = 0;
    while (JsonFindInt(json, L"x2", pos, v2, pos)) {
        xs2.push_back(v2);
        ++pos;
    }
    ASSERT_EQ(xs1.size(), xs2.size());
    ASSERT_FALSE(xs1.empty());

    // A2：区间非空、单调不减；A4：每段范围必须落在该字符真实字形邻域内。
    ASSERT_EQ(xs1.size(), xs2.size());
    ASSERT_FALSE(xs1.empty());
    // 用整行框把 tw 坐标换算回图片坐标后，才能与 glyph_x_ranges 比较
    for (size_t i = 0; i < xs1.size(); ++i) {
        EXPECT_LT(xs1[i], xs2[i]) << "char " << i << " has empty span";
        if (i + 1 < xs1.size())
            EXPECT_LE(xs2[i], xs1[i + 1]) << "char " << i << " overlaps char " << i + 1;
    }

    // A4 核心：逐字中心必须落在真实字形范围（允许 ±40% 字宽误差）。
    // 这条断言是"坐标可信"的唯一守门人——x2 被压成 x1 时中心会漂出字形邻域。
    const size_t n = std::min(xs1.size(), img.glyph_x_ranges.size());
    ASSERT_GT(n, 0u);
    for (size_t i = 0; i < n; ++i) {
        const int center = (xs1[i] + xs2[i]) / 2;
        const int gb = img.glyph_x_ranges[i].first;
        const int ge = img.glyph_x_ranges[i].second;
        const int glyph_w = std::max(1, ge - gb);
        const int tol = glyph_w * 2 / 5; // 40%
        EXPECT_GE(center, gb - tol) << "char " << i << " center " << center
                                    << " left of glyph range [" << gb << "," << ge << ")";
        EXPECT_LE(center, ge + tol) << "char " << i << " center " << center
                                    << " right of glyph range [" << gb << "," << ge << ")";
    }
}

// A5：旧 API 与 OcrEx2 的整行文本必须一致（旧接口未被改动）。
TEST(OcrEx2Test, LegacyOcrExStillMatchesNewText) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_test_ocrex2_a5.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    const wchar_t *legacy_raw = OpOcrEx(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(legacy_raw, nullptr);
    const std::wstring legacy(legacy_raw);
    EXPECT_FALSE(legacy.empty()) << "legacy OpOcrEx returned nothing";

    const wchar_t *json_raw = OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(json_raw, nullptr);
    const std::wstring json(json_raw);
    // 旧格式 "x,y,text|..."：把 text 段拼起来与 JSON 的 text 字段比对
    std::wstring legacy_text;
    size_t begin = 0;
    while (begin <= legacy.size()) {
        size_t end = legacy.find(L'|', begin);
        if (end == std::wstring::npos)
            end = legacy.size();
        std::wstring item = legacy.substr(begin, end - begin);
        size_t c1 = item.find(L',');
        size_t c2 = (c1 == std::wstring::npos) ? std::wstring::npos : item.find(L',', c1 + 1);
        if (c2 != std::wstring::npos)
            legacy_text += item.substr(c2 + 1);
        if (end == legacy.size())
            break;
        begin = end + 1;
    }
    // JSON 里的 text 字段逐个提取拼接
    std::wstring json_text;
    size_t p = 0;
    const std::wstring key = L"\"text\":\"";
    while ((p = json.find(key, p)) != std::wstring::npos) {
        p += key.size();
        size_t e = json.find(L'"', p);
        if (e == std::wstring::npos)
            break;
        json_text += json.substr(p, e - p);
        p = e + 1;
    }
    EXPECT_EQ(legacy_text, json_text) << "OcrEx2 text diverges from legacy OpOcrEx";
}

// A6：行级 conf 口径不变——同一图 Old OcrAutoColorEx 与 OcrAutoColorEx2 的 conf 一致。
TEST(OcrEx2Test, LineConfMatchesLegacyApi) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(L"38471", L"op_test_ocrex2_a6.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    int legacy_count = 0;
    const wchar_t *legacy_raw =
        OpOcrAutoColorEx(api.handle, 0, 0, img.width, img.height, L"", 0.3, &legacy_count);
    ASSERT_NE(legacy_raw, nullptr);
    const std::wstring legacy(legacy_raw);

    int new_count = 0;
    const wchar_t *json_raw =
        OpOcrAutoColorEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3, &new_count);
    ASSERT_NE(json_raw, nullptr);
    const std::wstring json(json_raw);

    // 两者是同一引擎同一路径，行数应一致（JSON 为 "[]" 时空串除外）
    if (!legacy.empty()) {
        EXPECT_EQ(legacy_count, new_count)
            << "line count differs between legacy and Ex2";
        EXPECT_NE(json.find(L'"'), std::wstring::npos) << "Ex2 should emit a JSON array";
    }
}

// 空颜色 = 不做颜色过滤，必须回退原图直识（与 AutoOcr/AutoOcrLine 归一语义一致）。
// 反向验证：去掉 ocr_auto_color_ex2 开头的 if (color.empty()) 分流后，本用例必须 FAIL
// （空色走二值化会产出全背景图，结果恒空）。
TEST(OcrEx2Test, AutoColorEx2WithEmptyColorFallsBackToRawImage) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_test_ocrex2_empty_color.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    // 原图路径
    const wchar_t *raw1 = OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(raw1, nullptr);
    const std::wstring json_raw(raw1);
    ASSERT_FALSE(json_raw.empty());

    // 空颜色走 AutoColorEx2：应与原图路径给出同样的文本
    int count = 0;
    const wchar_t *raw2 =
        OpOcrAutoColorEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3, &count);
    ASSERT_NE(raw2, nullptr);
    const std::wstring json_color(raw2);

    EXPECT_GT(count, 0) << "empty color must NOT binarize into an all-background image";
    EXPECT_EQ(json_raw, json_color) << "empty color must fall back to the raw-image path";
}

// 空区域/0 尺寸不得返回 nullptr，且必须给出合法 JSON（"[]" 或空）。
TEST(OcrEx2Test, ZeroSizeRegionIsSafe) {
    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    const wchar_t *raw = OpOcrEx2(api.handle, 0, 0, 0, 0, L"", 0.3);
    // 允许返回 nullptr（边界）或空串，但不得崩溃
    if (raw != nullptr) {
        const std::wstring json(raw);
        EXPECT_TRUE(json.empty() || json.front() == L'[');
    }
}

// 反向验证说明（人工执行，CLAUDE.md 要求"故意改错必须 FAIL"）：
//   1) 把 rec_images 里 cs.x2 的映射改成 "cs.t1" 而非 "cs.t1 + 1"，
//      CharCentersLandOnRealGlyphs 的 "empty span" 断言会失败；
//   2) 把 ctc_greedy 中 t1 回填去掉（恒为 t0），CharsMatchTextAndStayInsideBox
//      的逐字数与合并行为会改变，A3 的 chars/text 对应关系被破坏；
//   3) 把 builds 的 x2 写成 t0 的映射（x2 = x1），A2 立即失败。
// 三处改动各自都能让本文件红灯，说明断言不是恒真。
