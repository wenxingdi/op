// P1 FindStr 增强 + P3 OCR 耗时自省 自测（2026-11-21）
// 覆盖 docs/2026-11/P1_FindStr增强与P3耗时自省_实施方案.md §4 的验收判据 F1-F10。
//
// 结构：
//   1) StrMatchTest.*   —— 纯匹配内核单测（不需要 OCR 引擎，任何环境都必须绿）
//   2) FindStrBatchTest.* —— 端到端（GDI 合成图 + 真实引擎）
//
// 反向验证（CLAUDE.md 强制）：见文件末尾注释，每条都做过"故意改错 -> 必须 FAIL"。

#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

#include <op_c_api.h>

#include "../libop/algorithm/StrMatch.h"

namespace {

using test_support::GetEnvString;

constexpr wchar_t kCjkText[] = L"测试文字";

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

bool SkipOcr() {
    return !GetEnvString(L"OP_SKIP_OCR_TESTS").empty();
}

// 用 GDI 把 text 画成黑字白底的 BMP（与 ocr_ex2_test.cpp 同一套做法），
// 返回每段字形的水平范围，供"字符级坐标落在真实字形上"的断言使用。
struct GlyphImage {
    std::vector<test_support::uchar> bgra;
    int width = 0;
    int height = 0;
    std::vector<std::pair<int, int>> glyph_x_ranges;
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
    if (!font) { // 字体缺失时明确失败，别退化成"全白图 -> 识别不到字"的迷惑现象
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old_font = SelectObject(dc, font);

    SIZE sz{0, 0};
    if (!GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz) || sz.cx <= 0) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    const int w = sz.cx + pad * 2;
    const int h = font_height + pad * 2;
    out.width = w;
    out.height = h;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
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
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return L"";
    DWORD written = 0;
    WriteFile(f, bmp.data(), static_cast<DWORD>(bmp.size()), &written, nullptr);
    CloseHandle(f);
    return path;
}

// --- 极简 JSON 读取（只供断言用，不引第三方） ---

// 取某个键在 json 中第 n 次出现的数值
bool JsonNthInt(const std::wstring &json, const wchar_t *key, int n, long &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    size_t p = 0;
    int seen = 0;
    while ((p = json.find(pattern, p)) != std::wstring::npos) {
        p += pattern.size();
        if (seen++ == n) {
            value = _wtoi(json.c_str() + p);
            return true;
        }
    }
    return false;
}

bool JsonFirstInt(const std::wstring &json, const wchar_t *key, long &value) {
    return JsonNthInt(json, key, 0, value);
}

bool JsonFirstDouble(const std::wstring &json, const wchar_t *key, double &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    const size_t p = json.find(pattern);
    if (p == std::wstring::npos)
        return false;
    value = _wtof(json.c_str() + p + pattern.size());
    return true;
}

bool JsonFirstBool(const std::wstring &json, const wchar_t *key, bool &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    const size_t p = json.find(pattern);
    if (p == std::wstring::npos)
        return false;
    const std::wstring tail = json.substr(p + pattern.size(), 5);
    if (tail.rfind(L"true", 0) == 0) {
        value = true;
        return true;
    }
    if (tail.rfind(L"false", 0) == 0) {
        value = false;
        return true;
    }
    return false;
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

} // namespace

// ===========================================================================
// 1) 匹配内核单测：不依赖 OCR 引擎
// ===========================================================================

// F2 的基础：contains 会命中子串，exact 不会。这是本次增强最核心的语义区分。
TEST(StrMatchTest, ExactRejectsSubstring) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"1909", L"9090", L"909"};

    Target contains;
    contains.text = L"909";
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, contains, hits));
    EXPECT_EQ(hits.size(), 3u) << "contains must hit all three lines";

    Target exact;
    exact.text = L"909";
    exact.mode = MatchMode::Exact;
    ASSERT_TRUE(MatchAll(lines, exact, hits));
    ASSERT_EQ(hits.size(), 1u) << "exact must only match the line that IS 909";
    EXPECT_EQ(hits[0].line, 2);
    EXPECT_EQ(hits[0].begin, 0);
    EXPECT_EQ(hits[0].end, 3);
}

// F3 的基础：命中窗口的下标必须覆盖被找子串本身。
TEST(StrMatchTest, ContainsReportsCorrectSpan) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"击倒机关兽(909m)0/5"};
    Target t;
    t.text = L"909";
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].begin, 6);
    EXPECT_EQ(hits[0].end, 9);
    // 自校验：按下标取出的子串必须等于目标
    EXPECT_EQ(lines[0].substr(size_t(hits[0].begin), size_t(hits[0].end - hits[0].begin)), L"909");
}

TEST(StrMatchTest, PrefixSuffixMode) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"abcXYZ", L"XYZabc"};
    Target prefix;
    prefix.text = L"abc";
    prefix.mode = MatchMode::Prefix;
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, prefix, hits));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].line, 0);

    Target suffix;
    suffix.text = L"abc";
    suffix.mode = MatchMode::Suffix;
    ASSERT_TRUE(MatchAll(lines, suffix, hits));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].line, 1);
}

// 跨行不命中：这是本内核与旧 FindStr（全文拼接后 find）的关键差别。
TEST(StrMatchTest, NoCrossLineMatch) {
    using namespace op::str_match;
    // 若把两行拼起来，"OK" 会命中；按行匹配则不应命中
    const std::vector<std::wstring> lines = {L"O", L"K"};
    Target t;
    t.text = L"OK";
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    EXPECT_TRUE(hits.empty()) << "a match must not span two lines";
}

// F5：fuzzy=0 不命中形近错误，fuzzy=1 命中。
TEST(StrMatchTest, FuzzyTolerance) {
    using namespace op::str_match;
    // 模型把 "割喉台" 认成 "割喉合"（单字错误）
    const std::vector<std::wstring> lines = {L"割喉合"};

    Target strict;
    strict.text = L"割喉台";
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, strict, hits));
    EXPECT_TRUE(hits.empty()) << "fuzzy=0 must not tolerate a substitution";

    Target loose;
    loose.text = L"割喉台";
    loose.fuzzy = 1;
    ASSERT_TRUE(MatchAll(lines, loose, hits));
    ASSERT_EQ(hits.size(), 1u) << "fuzzy=1 must tolerate exactly one substitution";
    EXPECT_EQ(hits[0].ed, 1);
    EXPECT_EQ(hits[0].begin, 0);
    EXPECT_EQ(hits[0].end, 3);
}

// 模糊不应把完全无关的串报成命中（距离超限必须拒绝）。
TEST(StrMatchTest, FuzzyRejectsDistantText) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"完全不同的内容"};
    Target t;
    t.text = L"割喉台";
    t.fuzzy = 1;
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    EXPECT_TRUE(hits.empty());
}

// F6 的一半：正则命中范围 == 该 match 的范围。
TEST(StrMatchTest, RegexSpanMatchesGroup) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"击倒机关兽(909m)0/5"};
    Target t;
    t.text = L"\\d+m";
    t.mode = MatchMode::Regex;
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(lines[0].substr(size_t(hits[0].begin), size_t(hits[0].end - hits[0].begin)), L"909m");
}

// F6 的另一半：非法正则不崩溃、不产生命中（MatchAll 返回 false）。
TEST(StrMatchTest, BadRegexIsSafe) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"anything"};
    Target t;
    t.text = L"([unclosed";
    t.mode = MatchMode::Regex;
    std::vector<Hit> hits;
    EXPECT_FALSE(MatchAll(lines, t, hits)) << "an invalid regex must be reported as failure, not as a hit";
    EXPECT_TRUE(hits.empty());
}

TEST(StrMatchTest, CaseInsensitiveAsciiOnly) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"QuestDone"};
    Target t;
    t.text = L"questdone";
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    EXPECT_TRUE(hits.empty()) << "ci=false must be case sensitive";

    t.ci = true;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    EXPECT_EQ(hits.size(), 1u) << "ci=true must fold ASCII case";
}

// max_hits 截断（多命中场景下只保留最靠前的若干条）。
TEST(StrMatchTest, MaxHitsTruncates) {
    using namespace op::str_match;
    const std::vector<std::wstring> lines = {L"aa", L"bb", L"aa"};
    Target t;
    t.text = L"aa";
    std::vector<Hit> hits;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    EXPECT_EQ(hits.size(), 2u);

    t.max_hits = 1;
    ASSERT_TRUE(MatchAll(lines, t, hits));
    EXPECT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].line, 0);
}

// flags 位解析：互斥组合必须被拒绝，不能静默降级。
TEST(StrMatchTest, FlagsParsing) {
    using namespace op::str_match;
    Target t;
    ASSERT_TRUE(TargetFromFlags(L"909", 0, t));
    EXPECT_EQ(t.mode, MatchMode::Contains);
    EXPECT_EQ(t.fuzzy, 0);

    ASSERT_TRUE(TargetFromFlags(L"909", 2, t));
    EXPECT_EQ(t.mode, MatchMode::Exact);

    ASSERT_TRUE(TargetFromFlags(L"\\d+", 1, t));
    EXPECT_EQ(t.mode, MatchMode::Regex);

    ASSERT_TRUE(TargetFromFlags(L"909", 64, t));
    EXPECT_EQ(t.fuzzy, 1);

    ASSERT_TRUE(TargetFromFlags(L"909", 128 | 4, t));
    EXPECT_EQ(t.fuzzy, 2);
    EXPECT_TRUE(t.ci);

    // 互斥：exact + fuzzy 同时给 -> 非法
    EXPECT_FALSE(TargetFromFlags(L"909", 2 | 64, t));
    // 互斥：regex + fuzzy
    EXPECT_FALSE(TargetFromFlags(L"909", 1 | 128, t));
}

TEST(StrMatchTest, EditDistanceBasic) {
    using namespace op::str_match;
    EXPECT_EQ(EditDistance(L"abc", L"abc", 3), 0);
    EXPECT_EQ(EditDistance(L"abc", L"abd", 3), 1);
    EXPECT_EQ(EditDistance(L"abc", L"ab", 3), 1);
    EXPECT_GT(EditDistance(L"abc", L"xyz", 1), 1) << "distance over the limit must be reported as > limit";
}

// ===========================================================================
// 2) 端到端：GDI 合成图 + 真实引擎
// ===========================================================================

// F1 + F3 + F4：多目标一次调用；字符级范围落在被找子串的真实字形上，
// 并且包含在所属行框内。
TEST(FindStrBatchTest, BatchFindsTargetAndLocatesChars) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    // "测试文字" 4 字，无任何括号/数字干扰
    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_findstr_batch_a1.bmp", img);
    ASSERT_FALSE(path.empty()) << "failed to build glyph bmp";

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    // 目标 0 = 整行（应命中）；目标 1 = 不存在的串（不应命中）
    const wchar_t *json = OpFindStrBatch(api.handle, 0, 0, img.width, img.height,
                                         L"[\"测试文字\",\"不存在的字符串\"]", L"", 0.3);
    ASSERT_NE(json, nullptr) << "OpFindStrBatch returned null for a valid arrays-of-strings input";
    const std::wstring out(json);

    long targets = 0, found = 0, hits = 0;
    ASSERT_TRUE(JsonFirstInt(out, L"targets", targets));
    ASSERT_TRUE(JsonFirstInt(out, L"found", found));
    ASSERT_TRUE(JsonFirstInt(out, L"hits", hits));
    EXPECT_EQ(targets, 2);
    EXPECT_EQ(found, 1) << "only the real line should be found: " << std::string(out.begin(), out.end());
    EXPECT_EQ(hits, 1);

    // 第一个命中的字符级范围
    long cx1 = 0, cx2 = 0, x = 0, x2 = 0;
    ASSERT_TRUE(JsonFirstInt(out, L"cx1", cx1)) << "missing cx1";
    ASSERT_TRUE(JsonFirstInt(out, L"cx2", cx2)) << "missing cx2";
    ASSERT_TRUE(JsonFirstInt(out, L"x", x)) << "missing x";
    ASSERT_TRUE(JsonFirstInt(out, L"x2", x2)) << "missing x2";

    // F4：字符框不越出行框，且非空
    EXPECT_GE(cx1, x) << "char range must start inside the line box";
    EXPECT_LE(cx2, x2) << "char range must end inside the line box";
    EXPECT_LT(cx1, cx2) << "char range must be non-empty";

    // F3：整行命中时 cx1..cx2 应基本覆盖整行（首字从左边界开始）
    const int line_w = x2 - x;
    ASSERT_GT(line_w, 0);
    EXPECT_LE(cx1 - x, line_w / 4) << "a whole-line hit should start near the line's left edge";
    EXPECT_GE(cx2, x2 - line_w / 4) << "a whole-line hit should end near the line's right edge";
}

// F3 核心：在较长的一行里找一个子串，cx1/cx2 必须落在该子串自己的字形范围内，
// 而不是整行范围。这是 P0 逐字坐标带来的新增价值。
//
// 样本选择说明：引擎对"合成 CJK 字体"的识别并不稳定（本例首 5 个字常被认成乱码），
// 因此本用例**不**依赖 CJK 部分识别正确——它只要求 "909" 被识别出来（引擎对数字很稳），
// 然后断言 cx1/cx2 恰好对齐 909 三个字符的逐字区间，且明显窄于整行。
TEST(FindStrBatchTest, CharRangeIsSubstringNotWholeLine) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    // 数字嵌在一串 CJK 中间，保证命中片段左右都还有别的字形
    const std::wstring text = L"击倒机关兽909米";
    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(text, L"op_findstr_batch_span.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    const wchar_t *json = OpFindStrBatch(api.handle, 0, 0, img.width, img.height, L"[\"909\"]", L"", 0.3);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);

    long found = 0;
    ASSERT_TRUE(JsonFirstInt(out, L"found", found));
    ASSERT_EQ(found, 1) << "engine must recognize 909 on this sample: " << std::string(out.begin(), out.end());

    long cx1 = 0, cx2 = 0, x = 0, x2 = 0, begin = -1, end = -1;
    ASSERT_TRUE(JsonFirstInt(out, L"cx1", cx1));
    ASSERT_TRUE(JsonFirstInt(out, L"cx2", cx2));
    // x/y = 行框左上角（与旧 FindStr 语义一致）；cx1/cx2 = 命中片段范围
    ASSERT_TRUE(JsonFirstInt(out, L"x", x));
    ASSERT_TRUE(JsonFirstInt(out, L"x2", x2));
    ASSERT_TRUE(JsonFirstInt(out, L"begin", begin));
    ASSERT_TRUE(JsonFirstInt(out, L"end", end));

    const int line_w = x2 - x;
    ASSERT_GT(line_w, 0);

    // 1) 命中片段必须窄于整行（909 只占三个字形）
    EXPECT_LT(cx2 - cx1, line_w) << "substring range must be narrower than the whole line";

    // 2) 命中片段前必须有字形（有前导 CJK），所以 cx1 不能等于行左边界。
    //    这里用 begin>0 表达"行内确实有前导字符"，再要求 cx1 > x 把
    //    "字符级定位"与"直接返回整行框"区分开——后者会让 cx1 == x 恒成立。
    ASSERT_GT(begin, 0) << "this sample must have leading glyphs before 909";
    EXPECT_GT(cx1, x) << "909 must not start at the very left edge of the line";

    // 3) 909 的宽度应约为整行的 3/7（7 个字形），允许宽松区间
    const double ratio = double(cx2 - cx1) / double(line_w);
    EXPECT_GT(ratio, 0.20) << "909 is 3 of 7 glyphs, width ratio too small: " << ratio;
    EXPECT_LT(ratio, 0.90) << "909 is 3 of 7 glyphs, width ratio too large: " << ratio;

    // 4) 关键交叉校验：cx1/cx2 必须与逐字数据里 909 三个字的区间端点一致。
    //    这正是"字符级定位"的定义——拿 OpOcrEx2 的 chars 独立复算一遍。
    const wchar_t *ocr = OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(ocr, nullptr);
    const std::wstring ocr_json(ocr);
    // 该图上只有一行，取它的 chars 序列；用 x1/x2 键按顺序读
    std::vector<long> xs1, xs2;
    size_t pos = 0;
    long v = 0;
    while (true) {
        const size_t p = ocr_json.find(L"\"x1\":", pos);
        if (p == std::wstring::npos)
            break;
        xs1.push_back(_wtoi(ocr_json.c_str() + p + 5));
        pos = p + 5;
    }
    pos = 0;
    while (true) {
        const size_t p = ocr_json.find(L"\"x2\":", pos);
        if (p == std::wstring::npos)
            break;
        xs2.push_back(_wtoi(ocr_json.c_str() + p + 5));
        pos = p + 5;
    }
    ASSERT_EQ(xs1.size(), xs2.size());
    ASSERT_TRUE(size_t(end) <= xs1.size()) << "begin/end must index into the per-char arrays";
    ASSERT_GE(begin, 0);
    EXPECT_EQ(cx1, xs1[size_t(begin)]) << "cx1 must equal chars[begin].x1 (character-level anchor)";
    EXPECT_EQ(cx2, xs2[size_t(end) - 1]) << "cx2 must equal chars[end-1].x2 (character-level anchor)";
    (void)v;
}

// F2 端到端：exact 模式不把子串当命中。
TEST(FindStrBatchTest, ExactModeRejectsSubstringInJson) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(L"38471", L"op_findstr_batch_exact.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    // 行文本是 "38471"。找 "8471"：contains 命中，exact 不命中。
    const wchar_t *contains_json =
        OpFindStrBatch(api.handle, 0, 0, img.width, img.height, L"[\"8471\"]", L"", 0.3);
    ASSERT_NE(contains_json, nullptr);
    long contains_found = 0;
    ASSERT_TRUE(JsonFirstInt(std::wstring(contains_json), L"found", contains_found));
    ASSERT_EQ(contains_found, 1) << "contains must hit the substring 8471 inside 38471";

    // 注意：contains 的命中依赖引擎把整行识别成一个 "38471"。若引擎切成了多行，
    // 这里会 found=0，此时跳过 exact 的对比（样本不适用，不算失败）。
    const wchar_t *exact_json = OpFindStrBatch(
        api.handle, 0, 0, img.width, img.height, L"[{\"text\":\"8471\",\"match\":\"exact\"}]", L"", 0.3);
    ASSERT_NE(exact_json, nullptr);
    long exact_found = 0;
    ASSERT_TRUE(JsonFirstInt(std::wstring(exact_json), L"found", exact_found));
    EXPECT_EQ(exact_found, 0) << "exact must NOT match 8471 inside 38471";
}

// F8：FindStrEx2 的 flags 与 FindStrBatch 的对象配置给出相同结果。
TEST(FindStrBatchTest, Ex2MatchesBatchResult) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(L"任务追踪615", L"op_findstr_ex2.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    // 同一个目标、同一份 flags 语义：contains
    const wchar_t *via_batch = OpFindStrBatch(api.handle, 0, 0, img.width, img.height, L"[\"任务\"]", L"", 0.3);
    ASSERT_NE(via_batch, nullptr);
    const wchar_t *via_ex2 = OpFindStrEx2(api.handle, 0, 0, img.width, img.height, L"任务", L"", 0.3, 0);
    ASSERT_NE(via_ex2, nullptr);

    long b_found = 0, e_found = 0, b_hits = 0, e_hits = 0;
    ASSERT_TRUE(JsonFirstInt(std::wstring(via_batch), L"found", b_found));
    ASSERT_TRUE(JsonFirstInt(std::wstring(via_batch), L"hits", b_hits));
    ASSERT_TRUE(JsonFirstInt(std::wstring(via_ex2), L"found", e_found));
    ASSERT_TRUE(JsonFirstInt(std::wstring(via_ex2), L"hits", e_hits));
    EXPECT_EQ(b_found, e_found) << "flags path and JSON path must agree on 'found'";
    EXPECT_EQ(b_hits, e_hits) << "flags path and JSON path must agree on hit count";
}

// 非法输入必须明确失败（返回 nullptr），不能静默当成"没找到"。
TEST(FindStrBatchTest, InvalidInputReturnsNull) {
    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);

    // JSON 语法错误
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10, L"[not json", L"", 0.3), nullptr);
    // 不是数组
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10, L"{\"text\":\"a\"}", L"", 0.3), nullptr);
    // 未知 match 取值
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10, L"[{\"text\":\"a\",\"match\":\"nope\"}]", L"", 0.3),
              nullptr);
    // 未知字段（拼写错误必须报错，不能静默忽略）
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10, L"[{\"text\":\"a\",\"fuzZy\":1}]", L"", 0.3), nullptr);
    // 对象缺 text
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10, L"[{\"match\":\"exact\"}]", L"", 0.3), nullptr);
    // fuzzy 超上限
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10, L"[{\"text\":\"a\",\"fuzzy\":9}]", L"", 0.3), nullptr);
    // fuzzy + regex 互斥
    EXPECT_EQ(OpFindStrBatch(api.handle, 0, 0, 10, 10,
                             L"[{\"text\":\"a\",\"match\":\"regex\",\"fuzzy\":1}]", L"", 0.3),
              nullptr);
    // flags 互斥
    EXPECT_EQ(OpFindStrEx2(api.handle, 0, 0, 10, 10, L"a", L"", 0.3, 2 | 64), nullptr);
}

// 空数组合法：返回合法 JSON 且 0 目标。
TEST(FindStrBatchTest, EmptyTargetArrayIsValid) {
    CApiHandle api;
    const wchar_t *json = OpFindStrBatch(api.handle, 0, 0, 10, 10, L"[]", L"", 0.3);
    ASSERT_NE(json, nullptr) << "an empty target list is valid input, not an error";
    long targets = -1;
    ASSERT_TRUE(JsonFirstInt(std::wstring(json), L"targets", targets));
    EXPECT_EQ(targets, 0);
}

// F7：旧 FindStr / FindStrEx 输出不受影响（同图下与新接口的结论一致）。
TEST(FindStrBatchTest, LegacyFindStrStillWorks) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_findstr_legacy.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    int x = -1, y = -1;
    const int idx = OpFindStr(api.handle, 0, 0, img.width, img.height, L"测试文字", L"", 0.3, &x, &y);
    EXPECT_EQ(idx, 0) << "legacy OpFindStr must still report index 0 for a hit";
    EXPECT_GE(x, 0);
    EXPECT_GE(y, 0);

    // 注意：FindStrEx 的单条命中格式是 "idx,x,y"（**没有**尾随 '|'），
    // 只有 ≥2 条命中时条目之间才有 '|' 分隔。实测确认：
    //   单命中 -> "0,1,20"；多命中 -> "0,x,y|0,x,y|..."
    // 所以这里按"逗号分隔的三段"来校验，而不是找 '|'。
    const wchar_t *ex = OpFindStrEx(api.handle, 0, 0, img.width, img.height, L"测试文字", L"", 0.3);
    ASSERT_NE(ex, nullptr);
    const std::wstring ex_str(ex);
    ASSERT_FALSE(ex_str.empty()) << "legacy OpFindStrEx must report the hit";
    size_t commas = 0;
    for (wchar_t c : ex_str)
        if (c == L',')
            ++commas;
    EXPECT_EQ(commas, 2u) << "legacy OpFindStrEx single-hit format must be 'idx,x,y', got: "
                           << std::string(ex_str.begin(), ex_str.end());

    // 新接口对同一目标必须给出一致的结论（都命中）
    const wchar_t *batch = OpFindStrBatch(api.handle, 0, 0, img.width, img.height, L"[\"测试文字\"]", L"", 0.3);
    ASSERT_NE(batch, nullptr);
    long found = 0;
    ASSERT_TRUE(JsonFirstInt(std::wstring(batch), L"found", found));
    EXPECT_EQ(found, 1) << "new FindStrBatch and legacy OpFindStr must agree on the hit";
}

// ===========================================================================
// 3) P3：OCR 分项耗时自省
// ===========================================================================

// F9：各分项非负，且 total_ms 覆盖各分项之和（不等于常量 0）。
TEST(OcrProfileTest, SectionsAreConsistent) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_ocr_profile.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    // 必须先跑一次 OCR，profile 才有数据
    const wchar_t *ocr_json = OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    ASSERT_NE(ocr_json, nullptr);

    const wchar_t *prof = OpOcrLastProfile(api.handle);
    ASSERT_NE(prof, nullptr);
    const std::wstring p(prof);
    ASSERT_FALSE(p.empty());

    bool valid = false;
    ASSERT_TRUE(JsonFirstBool(p, L"valid", valid)) << "profile must carry a valid flag: "
                                                   << std::string(p.begin(), p.end());
    ASSERT_TRUE(valid) << "profile must be valid right after an OCR call";

    double total = 0, det = 0, cls = 0, rec = 0, pre = 0, post = 0;
    ASSERT_TRUE(JsonFirstDouble(p, L"total_ms", total));
    ASSERT_TRUE(JsonFirstDouble(p, L"det_ms", det));
    ASSERT_TRUE(JsonFirstDouble(p, L"cls_ms", cls));
    ASSERT_TRUE(JsonFirstDouble(p, L"rec_ms", rec));
    ASSERT_TRUE(JsonFirstDouble(p, L"pre_ms", pre));
    ASSERT_TRUE(JsonFirstDouble(p, L"post_ms", post));

    EXPECT_GE(total, 0.0);
    EXPECT_GE(det, 0.0);
    EXPECT_GE(rec, 0.0);
    EXPECT_GT(det, 0.0) << "det must have actually been run and timed";
    EXPECT_GT(rec, 0.0) << "rec must have actually been run and timed";
    // 分项之和不能超过总耗时（允许少量计时误差的浮点容差）
    EXPECT_LE(det + cls + rec, total + 1.0)
        << "sum of Run sections must not exceed total; det=" << det << " rec=" << rec << " total=" << total;
    EXPECT_LE(pre + post, total + 1.0);
}

// F10：rec_width 反映 P2 的动态宽度（超长行 > 320）。
TEST(OcrProfileTest, RecWidthReflectsDynamicWidth) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    constexpr wchar_t kWide[] = L"击倒青螺山妖人需要八十点元神力完成任务获得奖励三个金币";
    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kWide, L"op_ocr_profile_wide.bmp", img);
    ASSERT_FALSE(path.empty());
    ASSERT_GT(double(img.width) / double(img.height), 6.67) << "sample must be wider than 320x48 baseline";

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);

    (void)OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);
    const wchar_t *prof = OpOcrLastProfile(api.handle);
    ASSERT_NE(prof, nullptr);
    long rec_width = 0;
    ASSERT_TRUE(JsonFirstInt(std::wstring(prof), L"rec_width", rec_width));
    EXPECT_GT(rec_width, 320) << "a wide line must drive rec_width above the 320 baseline (P2)";
}

// profile 是"上一次"的快照：连续读两次不应改变数值（不触发新 OCR）。
TEST(OcrProfileTest, ReadingProfileDoesNotRunOcr) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    GlyphImage img;
    const std::wstring path = WriteGlyphBmp(kCjkText, L"op_ocr_profile_stable.bmp", img);
    ASSERT_FALSE(path.empty());

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    ASSERT_EQ(OpSetDisplayInput(api.handle, (L"pic:" + path).c_str()), 1);
    (void)OpOcrEx2(api.handle, 0, 0, img.width, img.height, L"", 0.3);

    const std::wstring first(OpOcrLastProfile(api.handle));
    const std::wstring second(OpOcrLastProfile(api.handle));
    EXPECT_EQ(first, second) << "reading the profile must not itself perform an OCR";
}

// 反向验证说明（人工执行，CLAUDE.md 要求"故意改错必须 FAIL"）：
//   1) 把 StrMatch.h 的 MatchMode::Exact 分支改成"不做位置约束"（即退化为 contains），
//      StrMatchTest.ExactRejectsSubstring 与 FindStrBatchTest.ExactModeRejectsSubstringInJson 必须 FAIL。
//   2) 把 collect_ocr_lines 里的 cx1/cx2 改成直接用行框 ln.x1/ln.x2，
//      FindStrBatchTest.CharRangeIsSubstringNotWholeLine 的 "must be narrower than the whole line" 必须 FAIL。
//   3) 在 cx1 上再额外加一次 det 框原点（重复偏移），
//      FindStrBatchTest.BatchFindsTargetAndLocatesChars 的 "char range must start inside the line box" 必须 FAIL。
//   4) 把 fuzzy 判据改成恒真（MatchAll 不比对 ed），
//      StrMatchTest.FuzzyRejectsDistantText 与 FuzzyTolerance 必须 FAIL。
//   5) 把 ocr_profile_t::total_ms 写成常量 0（或让 ProfileGuard 不写 total_ms），
//      OcrProfileTest.SectionsAreConsistent 的 EXPECT_GT(det, 0) / 分项和断言必须 FAIL。
