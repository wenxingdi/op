#include "test_support.h"

#include "op_c_api.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

using namespace std;
using namespace op;
using test_support::BuildBmp32TopDown;
using test_support::PtrToWString;

namespace {
vector<uchar> MakePixels(int width, int height, uchar b = 0xff, uchar g = 0xff, uchar r = 0xff) {
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto idx = static_cast<size_t>(y * width + x) * 4;
            pixels[idx + 0] = b;
            pixels[idx + 1] = g;
            pixels[idx + 2] = r;
        }
    }
    return pixels;
}

void PaintPixel(vector<uchar> &pixels, int width, int x, int y, uchar b, uchar g, uchar r) {
    const auto idx = static_cast<size_t>(y * width + x) * 4;
    pixels[idx + 0] = b;
    pixels[idx + 1] = g;
    pixels[idx + 2] = r;
    pixels[idx + 3] = 0xff;
}

void PaintGlyphA(vector<uchar> &pixels, int width, int offset_x, int offset_y, uchar b, uchar g, uchar r) {
    static const vector<pair<int, int>> points = {{2, 1}, {1, 2}, {3, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {3, 4}};
    for (auto [x, y] : points)
        PaintPixel(pixels, width, x + offset_x, y + offset_y, b, g, r);
}

void PaintScaledGlyphA(vector<uchar> &pixels, int width, int offset_x, int offset_y, int scale, uchar b, uchar g,
                       uchar r) {
    static const vector<pair<int, int>> points = {{2, 1}, {1, 2}, {3, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {3, 4}};
    for (auto [x, y] : points) {
        for (int dy = 0; dy < scale; ++dy) {
            for (int dx = 0; dx < scale; ++dx) {
                PaintPixel(pixels, width, offset_x + x * scale + dx, offset_y + y * scale + dy, b, g, r);
            }
        }
    }
}

void PaintOutlinedGlyphA(vector<uchar> &pixels, int width, int offset_x, int offset_y, uchar outline_b, uchar outline_g,
                         uchar outline_r, uchar fill_b, uchar fill_g, uchar fill_r) {
    static const vector<pair<int, int>> points = {{2, 1}, {1, 2}, {3, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {3, 4}};
    for (auto [x, y] : points) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                PaintPixel(pixels, width, offset_x + x + dx, offset_y + y + dy, outline_b, outline_g, outline_r);
            }
        }
    }
    for (auto [x, y] : points)
        PaintPixel(pixels, width, x + offset_x, y + offset_y, fill_b, fill_g, fill_r);
}

void PaintGameBackground(vector<uchar> &pixels, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto b = static_cast<uchar>(0x50 + (x * 17 + y * 29) % 0x50);
            const auto g = static_cast<uchar>(0x58 + (x * 31 + y * 11) % 0x48);
            const auto r = static_cast<uchar>(0x48 + (x * 13 + y * 23) % 0x58);
            PaintPixel(pixels, width, x, y, b, g, r);
        }
    }
}

void SetMemBmp(op::Op &op, int width, int height, const vector<uchar> &pixels, long &ret) {
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
}

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

void UseSingleWordDict(op::Op &op, int width, int height, const wchar_t *color, const wchar_t *word, long &ret) {
    wstring dict_entry;
    op.FetchWord(0, 0, width, height, color, word, dict_entry);
    ASSERT_FALSE(dict_entry.empty());
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);
}
} // namespace

TEST(ImageColorTest, GetColor) {
    op::Op op;
    wstring color;
    op.GetColor(10, 10, color);
    wcout << L"Color at (10,10): " << color << endl;
    EXPECT_EQ(color.length(), 6u) << "Color string should be 6 hex characters";
}

TEST(ImageColorTest, CmpColor) {
    op::Op op;
    wstring color;
    op.GetColor(10, 10, color);
    ASSERT_EQ(color.length(), 6u);

    long ret;
    op.CmpColor(10, 10, color.c_str(), 0.9, &ret);
    EXPECT_TRUE(ret) << "CmpColor should match the same color just read";
}

TEST(ImageColorTest, GetColorAndCmpColorWorkAtBottomRightEdge) {
    op::Op op;
    long ret = 0;
    const int width = 4;
    const int height = 4;
    auto pixels = MakePixels(width, height, 0xff, 0xff, 0xff);
    PaintPixel(pixels, width, width - 1, height - 1, 0x12, 0x34, 0x56);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring color;
    op.GetColor(width - 1, height - 1, color);
    EXPECT_EQ(color, L"563412");

    op.CmpColor(width - 1, height - 1, L"563412", 1.0, &ret);
    EXPECT_EQ(ret, 1);
}

TEST(ImageColorTest, CmpColorUsesSimilarityAndExplicitDelta) {
    op::Op op;
    long ret = 0;
    const int width = 32;
    const int height = 32;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto idx = static_cast<size_t>(y * width + x) * 4;
            pixels[idx + 0] = 0x20;
            pixels[idx + 1] = 0x30;
            pixels[idx + 2] = 0x40;
        }
    }
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    op.CmpColor(10, 10, L"433222", 1.0, &ret);
    EXPECT_EQ(ret, 0) << "Different colors should not match at exact similarity";

    op.CmpColor(10, 10, L"433222", 0.98, &ret);
    EXPECT_EQ(ret, 1) << "Similarity should provide an implicit color tolerance";

    op.CmpColor(10, 10, L"433222-030202", 1.0, &ret);
    EXPECT_EQ(ret, 1) << "Explicit delta in the color string should still be honored";
}

TEST(ImageColorTest, FindStrIgnoresEmptyAlternatives) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);
    UseSingleWordDict(op, width, height, L"000000-000000", L"A", ret);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"|A", L"000000-000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);

    wstring results;
    op.FindStrEx(0, 0, width, height, L"|A", L"000000-000000", 1.0, results);
    EXPECT_EQ(results, L"1,1,1");
}

TEST(ImageColorTest, FindStrMapsMatchesInsideMultiCharacterDictWords) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);
    UseSingleWordDict(op, width, height, L"000000-000000", L"AB", ret);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"B", L"000000-000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);

    wstring results;
    op.FindStrEx(0, 0, width, height, L"A|B", L"000000-000000", 1.0, results);
    EXPECT_EQ(results, L"0,1,1|1,1,1");
}

TEST(ImageColorTest, FindColor) {
    op::Op op;
    wstring color;
    op.GetColor(10, 10, color);
    ASSERT_EQ(color.length(), 6u);

    long x, y, ret;
    op.FindColor(0, 0, 100, 100, color.c_str(), 0.9, 0, &x, &y, &ret);
    cout << "FindColor ret: " << ret << " at " << x << "," << y << endl;
}

TEST(ImageColorTest, FindColorUsesSimilarity) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto idx = static_cast<size_t>(y * width + x) * 4;
            pixels[idx + 0] = 0x00;
            pixels[idx + 1] = 0x00;
            pixels[idx + 2] = 0x00;
        }
    }

    const auto marker_idx = static_cast<size_t>(8 * width + 7) * 4;
    pixels[marker_idx + 0] = 0x20;
    pixels[marker_idx + 1] = 0x30;
    pixels[marker_idx + 2] = 0x40;

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindColor(0, 0, width, height, L"433222", 1.0, 0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);

    op.FindColor(0, 0, width, height, L"433222", 0.98, 0, &x, &y, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(x, 7);
    EXPECT_EQ(y, 8);
}

TEST(ImageColorTest, FindColorReturnsFirstPointBeforeColorPriority) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;
    auto pixels = MakePixels(width, height);

    PaintPixel(pixels, width, 1, 1, 0x30, 0x20, 0x10);
    PaintPixel(pixels, width, 6, 6, 0x03, 0x02, 0x01);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindColor(0, 0, width, height, L"010203|102030", 1.0, 0, &x, &y, &ret);

    EXPECT_EQ(ret, 1);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, FindColorHonorsAllDirections) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    auto paint_marker = [&](int x, int y) {
        const auto idx = static_cast<size_t>(y * width + x) * 4;
        pixels[idx + 0] = 0x11;
        pixels[idx + 1] = 0x22;
        pixels[idx + 2] = 0x33;
        pixels[idx + 3] = 0xff;
    };
    paint_marker(1, 1);
    paint_marker(6, 1);
    paint_marker(3, 3);
    paint_marker(1, 6);
    paint_marker(6, 6);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    const vector<wstring> expected = {
        L"1,1|6,1|3,3|1,6|6,6", L"1,6|6,6|3,3|1,1|6,1", L"6,1|1,1|3,3|6,6|1,6",
        L"6,6|1,6|3,3|6,1|1,1", L"3,3|1,1|6,1|1,6|6,6", L"1,1|1,6|3,3|6,1|6,6",
        L"6,1|6,6|3,3|1,1|1,6", L"1,6|1,1|3,3|6,6|6,1", L"6,6|6,1|3,3|1,6|1,1",
    };
    const vector<long> expected_x = {1, 1, 6, 6, 3, 1, 6, 1, 6};
    const vector<long> expected_y = {1, 6, 1, 6, 3, 1, 1, 6, 6};

    for (long dir = 0; dir <= 8; ++dir) {
        long x = -1;
        long y = -1;
        op.FindColor(0, 0, width, height, L"332211", 1.0, dir, &x, &y, &ret);
        ASSERT_EQ(ret, 1) << "dir=" << dir;
        EXPECT_EQ(x, expected_x[dir]) << "dir=" << dir;
        EXPECT_EQ(y, expected_y[dir]) << "dir=" << dir;

        wstring results;
        op.FindColorEx(0, 0, width, height, L"332211", 1.0, dir, results);
        EXPECT_EQ(results, expected[dir]) << "dir=" << dir;
    }
}

TEST(ImageColorTest, FindMultiColorHonorsAllDirections) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    auto paint = [&](int x, int y, uchar b, uchar g, uchar r) {
        const auto idx = static_cast<size_t>(y * width + x) * 4;
        pixels[idx + 0] = b;
        pixels[idx + 1] = g;
        pixels[idx + 2] = r;
        pixels[idx + 3] = 0xff;
    };
    auto paint_marker = [&](int x, int y) {
        paint(x - 1, y, 0x44, 0x55, 0x66);
        paint(x, y, 0x11, 0x22, 0x33);
    };
    paint_marker(1, 1);
    paint_marker(6, 1);
    paint_marker(3, 3);
    paint_marker(1, 6);
    paint_marker(6, 6);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    const vector<wstring> expected = {
        L"1,1|6,1|3,3|1,6|6,6", L"1,6|6,6|3,3|1,1|6,1", L"6,1|1,1|3,3|6,6|1,6",
        L"6,6|1,6|3,3|6,1|1,1", L"3,3|1,1|6,1|1,6|6,6", L"1,1|1,6|3,3|6,1|6,6",
        L"6,1|6,6|3,3|1,1|1,6", L"1,6|1,1|3,3|6,6|6,1", L"6,6|6,1|3,3|1,6|1,1",
    };
    const vector<long> expected_x = {1, 1, 6, 6, 3, 1, 6, 1, 6};
    const vector<long> expected_y = {1, 6, 1, 6, 3, 1, 1, 6, 6};

    for (long dir = 0; dir <= 8; ++dir) {
        long x = -1;
        long y = -1;
        op.FindMultiColor(0, 0, width, height, L"332211", L"-1|0|665544", 1.0, dir, &x, &y, &ret);
        ASSERT_EQ(ret, 1) << "dir=" << dir;
        EXPECT_EQ(x, expected_x[dir]) << "dir=" << dir;
        EXPECT_EQ(y, expected_y[dir]) << "dir=" << dir;

        wstring results;
        op.FindMultiColorEx(0, 0, width, height, L"332211", L"-1|0|665544", 1.0, dir, results);
        EXPECT_EQ(results, expected[dir]) << "dir=" << dir;
    }
}

// I3：畸形偏移段（坐标不可解析 / 缺颜色）必须被跳过而不是中断或注入垃圾偏移，
// 且多次调用结果确定一致。
TEST(ImageColorTest, FindMultiColorSkipsMalformedOffsetSegments) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    auto paint = [&](int x, int y, uchar b, uchar g, uchar r) {
        const auto idx = static_cast<size_t>(y * width + x) * 4;
        pixels[idx + 0] = b;
        pixels[idx + 1] = g;
        pixels[idx + 2] = r;
        pixels[idx + 3] = 0xff;
    };
    // 与 FindMultiColorHonorsAllDirections 相同的标记：主色(1,1)，偏移色在(0,1)。
    paint(1, 1, 0x11, 0x22, 0x33);
    paint(0, 1, 0x44, 0x55, 0x66);

    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    // 基线：单个合法偏移段。
    auto run = [&](const wchar_t *offsets, long &x, long &y) {
        op.FindMultiColor(0, 0, width, height, L"332211", offsets, 1.0, 0, &x, &y, &ret);
        return ret;
    };

    long bx = -1, by = -1;
    ASSERT_EQ(run(L"-1|0|665544", bx, by), 1);
    EXPECT_EQ(bx, 1);
    EXPECT_EQ(by, 1);

    // 坐标不可解析的段被跳过，合法段仍生效；结果与基线一致。
    for (int i = 0; i < 3; ++i) {
        long x = -1, y = -1;
        ASSERT_EQ(run(L"a|b|665544,-1|0|665544", x, y), 1) << "iter=" << i;
        EXPECT_EQ(x, bx) << "iter=" << i;
        EXPECT_EQ(y, by) << "iter=" << i;
    }

    // 缺颜色描述的段（旧实现会 break，吞掉后面的合法段）被跳过，合法段仍生效。
    for (int i = 0; i < 3; ++i) {
        long x = -1, y = -1;
        ASSERT_EQ(run(L"1|1|,-1|0|665544", x, y), 1) << "iter=" << i;
        EXPECT_EQ(x, bx) << "iter=" << i;
        EXPECT_EQ(y, by) << "iter=" << i;
    }
}

// I2：模板文件缺失/不可解析时 FindPic 不得崩溃，且返回"未找到"而非"找到"。
// （缺失原因经 setlog 落 __op.log，由调用方开 op.set_show_error_msg(2) 查看。）
TEST(ImageColorTest, FindPicWithMissingTemplateDoesNotMatch) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = 0, y = 0;
    op.FindPic(0, 0, width, height, L"definitely_missing_template_xyz.bmp", L"000000", 0.8, 0, &x, &y, &ret);
    EXPECT_NE(ret, 1) << "missing template must not report a match";

    wstring results = L"sentinel";
    op.FindPicEx(0, 0, width, height, L"definitely_missing_template_xyz.bmp", L"000000", 0.8, 0, results);
    EXPECT_TRUE(results.empty());
}

// I1：直调图像入口对异常/非法输入兜底——进程不终止，出参保持失败值。
TEST(ImageColorTest, DirectImageEntriesFailSoft) {
    op::Op op;
    long ret = 1; // 故意预置非零，验证失败路径会重置
    op.LoadPic(L"definitely_missing_pic_xyz.bmp", &ret);
    EXPECT_EQ(ret, 0);

    ret = 1;
    long w = 7, h = 7;
    op.GetPicSize(L"definitely_missing_pic_xyz.bmp", &w, &h, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(w, 0L);
    EXPECT_EQ(h, 0L);

    // 从磁盘缺失文件做 OCR：返回空串而不是抛异常终止进程。
    wstring text = L"sentinel";
    op.OcrFromFile(L"definitely_missing_pic_xyz.bmp", L"ffffff", 0.9, text);
    EXPECT_TRUE(text.empty());
}

TEST(ImageColorTest, FindPicHonorsDirection) {
    op::Op op;
    long ret = 0;
    const int width = 32;
    const int height = 32;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    auto paint = [&](int x, int y, uchar b, uchar g, uchar r) {
        const auto idx = static_cast<size_t>(y * width + x) * 4;
        pixels[idx + 0] = b;
        pixels[idx + 1] = g;
        pixels[idx + 2] = r;
        pixels[idx + 3] = 0xff;
    };
    auto paint_marker = [&](int left, int top) {
        paint(left, top, 0x11, 0x22, 0x33);
        paint(left + 1, top, 0x44, 0x55, 0x66);
        paint(left, top + 1, 0x77, 0x88, 0x99);
        paint(left + 1, top + 1, 0xaa, 0xbb, 0xcc);
    };

    paint_marker(2, 3);
    paint_marker(24, 3);
    paint_marker(15, 15);
    paint_marker(2, 25);
    paint_marker(24, 25);
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    vector<uchar> tpl(static_cast<size_t>(2) * 2 * 4, 0xff);
    auto write_tpl = [&](int x, int y, uchar b, uchar g, uchar r) {
        const auto idx = static_cast<size_t>(y * 2 + x) * 4;
        tpl[idx + 0] = b;
        tpl[idx + 1] = g;
        tpl[idx + 2] = r;
        tpl[idx + 3] = 0xff;
    };
    write_tpl(0, 0, 0x11, 0x22, 0x33);
    write_tpl(1, 0, 0x44, 0x55, 0x66);
    write_tpl(0, 1, 0x77, 0x88, 0x99);
    write_tpl(1, 1, 0xaa, 0xbb, 0xcc);
    auto tpl_bmp = BuildBmp32TopDown(2, 2, tpl);
    op.LoadMemPic(L"findpic_dir_marker", tpl_bmp.data(), static_cast<long>(tpl_bmp.size()), &ret);
    ASSERT_EQ(ret, 1);

    const vector<wstring> expected = {
        L"0,2,3|0,24,3|0,15,15|0,2,25|0,24,25", L"0,2,25|0,24,25|0,15,15|0,2,3|0,24,3",
        L"0,24,3|0,2,3|0,15,15|0,24,25|0,2,25", L"0,24,25|0,2,25|0,15,15|0,24,3|0,2,3",
        L"0,15,15|0,24,25|0,24,3|0,2,25|0,2,3", L"0,2,3|0,2,25|0,15,15|0,24,3|0,24,25",
        L"0,24,3|0,24,25|0,15,15|0,2,3|0,2,25", L"0,2,25|0,2,3|0,15,15|0,24,25|0,24,3",
        L"0,24,25|0,24,3|0,15,15|0,2,25|0,2,3",
    };
    const vector<long> expected_x = {2, 2, 24, 24, 15, 2, 24, 2, 24};
    const vector<long> expected_y = {3, 25, 3, 25, 15, 3, 3, 25, 25};

    auto named_results = [](const wstring &id_results) {
        wstring named;
        size_t start = 0;
        while (start < id_results.size()) {
            const size_t end = id_results.find(L'|', start);
            const wstring item = id_results.substr(start, end == wstring::npos ? wstring::npos : end - start);
            const size_t comma = item.find(L',');
            named += L"findpic_dir_marker" + item.substr(comma) + L"|";
            if (end == wstring::npos)
                break;
            start = end + 1;
        }
        if (!named.empty())
            named.pop_back();
        return named;
    };

    for (long dir = 0; dir <= 8; ++dir) {
        long x = -1;
        long y = -1;
        op.FindPic(0, 0, width, height, L"findpic_dir_marker", L"000000", 1.0, dir, &x, &y, &ret);
        ASSERT_EQ(ret, 0) << "dir=" << dir;
        EXPECT_EQ(x, expected_x[dir]) << "dir=" << dir;
        EXPECT_EQ(y, expected_y[dir]) << "dir=" << dir;

        wstring results;
        op.FindPicEx(0, 0, width, height, L"findpic_dir_marker", L"000000", 1.0, dir, results);
        EXPECT_EQ(results, expected[dir]) << "dir=" << dir;

        op.FindPicExS(0, 0, width, height, L"findpic_dir_marker", L"000000", 1.0, dir, results);
        EXPECT_EQ(results, named_results(expected[dir])) << "dir=" << dir;
    }
}

TEST(ImageColorTest, FindPicReturnsMinusOneWhenTemplateIsMissing) {
    op::Op op;
    long ret = 0;
    auto pixels = MakePixels(8, 8);
    SetMemBmp(op, 8, 8, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = 123;
    long y = 456;
    op.FindPic(0, 0, 8, 8, L"missing_findpic_template", L"000000", 1.0, 0, &x, &y, &ret);

    EXPECT_EQ(ret, -1);
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);

    int cx = 123;
    int cy = 456;
    EXPECT_EQ(OpFindPic(nullptr, 0, 0, 8, 8, L"missing_findpic_template", L"000000", 1.0, 0, &cx, &cy), -1);
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cy, -1);
}

TEST(ImageColorTest, SharedPicCacheIsGlobalAcrossObjects) {
    op::Op loader;
    op::Op matcher;
    long ret = 0;
    const int width = 12;
    const int height = 10;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    PaintPixel(pixels, width, 5, 4, 0x21, 0x43, 0x65);
    PaintPixel(pixels, width, 6, 4, 0x32, 0x54, 0x76);
    PaintPixel(pixels, width, 5, 5, 0x43, 0x65, 0x87);
    PaintPixel(pixels, width, 6, 5, 0x54, 0x76, 0x98);

    auto screen_bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(screen_bmp.data());
    matcher.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    vector<uchar> tpl(static_cast<size_t>(2) * 2 * 4, 0xff);
    PaintPixel(tpl, 2, 0, 0, 0x21, 0x43, 0x65);
    PaintPixel(tpl, 2, 1, 0, 0x32, 0x54, 0x76);
    PaintPixel(tpl, 2, 0, 1, 0x43, 0x65, 0x87);
    PaintPixel(tpl, 2, 1, 1, 0x54, 0x76, 0x98);
    auto tpl_bmp = BuildBmp32TopDown(2, 2, tpl);

    const std::wstring mem_name = L"shared_pic_cache_mem_template";
    loader.LoadMemPic(mem_name.c_str(), tpl_bmp.data(), static_cast<long>(tpl_bmp.size()), &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    matcher.FindPic(0, 0, width, height, mem_name.c_str(), L"000000", 1.0, 0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 5);
    EXPECT_EQ(y, 4);

    auto replaced_tpl = tpl;
    PaintPixel(replaced_tpl, 2, 0, 0, 0xff, 0x00, 0x00);
    auto replaced_tpl_bmp = BuildBmp32TopDown(2, 2, replaced_tpl);
    loader.LoadMemPic(mem_name.c_str(), replaced_tpl_bmp.data(), static_cast<long>(replaced_tpl_bmp.size()), &ret);
    ASSERT_EQ(ret, 1);

    x = -1;
    y = -1;
    matcher.FindPic(0, 0, width, height, mem_name.c_str(), L"000000", 1.0, 0, &x, &y, &ret);
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);

    const std::wstring file_path = test_support::GetTempBmpPath(L"op_shared_loadpic_template.bmp");
    {
        std::ofstream out(std::filesystem::path(file_path), std::ios::binary);
        ASSERT_TRUE(out);
        out.write(reinterpret_cast<const char *>(tpl_bmp.data()), static_cast<std::streamsize>(tpl_bmp.size()));
        ASSERT_TRUE(out);
    }

    loader.LoadPic(file_path.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    long pic_w = 0;
    long pic_h = 0;
    matcher.GetPicSize(file_path.c_str(), &pic_w, &pic_h, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(pic_w, 2);
    EXPECT_EQ(pic_h, 2);

    x = -1;
    y = -1;
    matcher.FindPic(0, 0, width, height, file_path.c_str(), L"000000", 1.0, 0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 5);
    EXPECT_EQ(y, 4);

    matcher.FreePic(mem_name.c_str(), &ret);
    EXPECT_EQ(ret, 1);
    matcher.FreePic(file_path.c_str(), &ret);
    EXPECT_EQ(ret, 1);
    std::filesystem::remove(std::filesystem::path(file_path));
}

TEST(ImageColorTest, MissingPicSizeClearsOutputValues) {
    op::Op op;
    long width = 123;
    long height = 456;
    long ret = 99;

    op.GetPicSize(L"__missing_pic_size_template__.bmp", &width, &height, &ret);

    EXPECT_EQ(ret, 0);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);

    int c_width = 123;
    int c_height = 456;
    EXPECT_EQ(OpGetPicSize(nullptr, L"__missing_pic_size_template__.bmp", &c_width, &c_height), 0);
    EXPECT_EQ(c_width, 0);
    EXPECT_EQ(c_height, 0);
}

TEST(ImageColorTest, FindPicTransparentOddPointsCountsCenterMismatchOnce) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    auto paint_screen = [&](int x, int y, uchar b, uchar g, uchar r) { PaintPixel(pixels, width, x, y, b, g, r); };

    const vector<pair<int, int>> points = {
        {1, 1}, {4, 1}, {2, 2}, {5, 2}, {3, 3}, {1, 4}, {4, 4}, {2, 5}, {5, 5}, {3, 6}, {5, 6},
    };
    const int left = 4;
    const int top = 3;
    for (auto [x, y] : points)
        paint_screen(left + x, top + y, 0x10, 0x20, 0x30);
    paint_screen(left + 1, top + 4, 0xe0, 0xd0, 0xc0);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    vector<uchar> tpl(static_cast<size_t>(7) * 7 * 4, 0x00);
    auto paint_tpl = [&](int x, int y, uchar b, uchar g, uchar r, uchar a = 0xff) {
        const auto idx = static_cast<size_t>(y * 7 + x) * 4;
        tpl[idx + 0] = b;
        tpl[idx + 1] = g;
        tpl[idx + 2] = r;
        tpl[idx + 3] = a;
    };
    for (auto [x, y] : points)
        paint_tpl(x, y, 0x10, 0x20, 0x30);
    auto tpl_bmp = BuildBmp32TopDown(7, 7, tpl);
    op.LoadMemPic(L"findpic_transparent_odd", tpl_bmp.data(), static_cast<long>(tpl_bmp.size()), &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindPic(0, 0, width, height, L"findpic_transparent_odd", L"000000", 0.8, 0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, left);
    EXPECT_EQ(y, top);
}

TEST(ImageColorTest, FetchWordUsesProvidedWordName) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);

    auto paint = [&](int x, int y) {
        const auto idx = static_cast<size_t>(y * width + x) * 4;
        pixels[idx + 0] = 0x00;
        pixels[idx + 1] = 0x00;
        pixels[idx + 2] = 0x00;
        pixels[idx + 3] = 0xff;
    };

    // A small glyph-like block so FetchWord has foreground pixels to crop.
    paint(2, 1);
    paint(1, 2);
    paint(3, 2);
    paint(1, 3);
    paint(2, 3);
    paint(3, 3);
    paint(1, 4);
    paint(3, 4);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring word_data;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", word_data);

    wcout << L"FetchWord output: " << word_data << endl;
    EXPECT_FALSE(word_data.empty());
    EXPECT_EQ(word_data.rfind(L"A$", 0), 0u) << "FetchWord should preserve the provided word name";

    op.FetchWord(1, 1, 4, 5, L"000000-000000", L"B", word_data);
    wcout << L"FetchWord offset output: " << word_data << endl;
    EXPECT_EQ(word_data, L"B$4,3,8$5E0E") << "FetchWord should use local coordinates after capturing an offset rect";
}

TEST(ImageColorTest, FetchWordReturnsEmptyForBlankRegion) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring word_data;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"Blank", word_data);

    EXPECT_TRUE(word_data.empty()) << "FetchWord should not emit a dictionary entry for a blank region";
}

TEST(ImageColorTest, FetchWordExUsesSimilarity) {
    op::Op op;
    long ret = 0;
    const int width = 5;
    const int height = 6;
    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring strict_entry;
    op.FetchWordEx(0, 0, width, height, L"000000", 1.0, L"A", strict_entry);
    EXPECT_TRUE(strict_entry.empty()) << "Strict matching should not treat a gray glyph as pure black";

    wstring tolerant_entry;
    op.FetchWordEx(0, 0, width, height, L"000000", 0.9, L"A", tolerant_entry);
    EXPECT_EQ(tolerant_entry.rfind(L"A$", 0), 0u);
}

TEST(ImageColorTest, ExtractWordRectsCutsScaledGlyphs) {
    op::Op op;
    long ret = 0;
    const int width = 24;
    const int height = 12;
    auto pixels = MakePixels(width, height);
    PaintScaledGlyphA(pixels, width, 0, 0, 2, 0x00, 0x00, 0x00);
    PaintScaledGlyphA(pixels, width, 12, 0, 2, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring rects;
    op.ExtractWordRects(0, 0, width, height, L"000000-000000", 1.0, 2, rects);

    EXPECT_EQ(rects, L"2,2,8,10|14,2,20,10");
}

TEST(ImageColorTest, ExtractWordRectsExFiltersNoiseAndAddsPadding) {
    op::Op op;
    long ret = 0;
    const int width = 24;
    const int height = 12;
    auto pixels = MakePixels(width, height);
    PaintScaledGlyphA(pixels, width, 0, 0, 2, 0x00, 0x00, 0x00);
    PaintScaledGlyphA(pixels, width, 12, 0, 2, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 23, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring rects;
    op.ExtractWordRectsEx(0, 0, width, height, L"000000-000000", 1.0, 3, 4, 1, rects);

    EXPECT_EQ(rects, L"1,1,9,11|13,1,21,11");
}

TEST(ImageColorTest, FetchWordsBuildsMultiplePointEntries) {
    op::Op op;
    long ret = 0;
    const int width = 24;
    const int height = 12;
    auto pixels = MakePixels(width, height);
    PaintScaledGlyphA(pixels, width, 0, 0, 2, 0x19, 0x19, 0x19);
    PaintScaledGlyphA(pixels, width, 12, 0, 2, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_text;
    op.FetchWords(0, 0, width, height, L"000000", 0.9, L"AB", 2, dict_text);

    EXPECT_EQ(dict_text.find(L"A$"), 0u);
    EXPECT_NE(dict_text.find(L"\nB$"), wstring::npos);
    EXPECT_EQ(count(dict_text.begin(), dict_text.end(), L'\n'), 1);

    wstring mismatch;
    op.FetchWords(0, 0, width, height, L"000000", 0.9, L"A", 2, mismatch);
    EXPECT_TRUE(mismatch.empty()) << "Word count mismatch should not silently create a partial dictionary";
}

TEST(ImageColorTest, FetchWordsExFiltersNoiseBeforeBuildingEntries) {
    op::Op op;
    long ret = 0;
    const int width = 24;
    const int height = 12;
    auto pixels = MakePixels(width, height);
    PaintScaledGlyphA(pixels, width, 0, 0, 2, 0x19, 0x19, 0x19);
    PaintScaledGlyphA(pixels, width, 12, 0, 2, 0x19, 0x19, 0x19);
    PaintPixel(pixels, width, 23, 0, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_text;
    op.FetchWordsEx(0, 0, width, height, L"000000", 0.9, L"AB", 3, 4, 1, dict_text);

    EXPECT_EQ(dict_text.find(L"A$"), 0u);
    EXPECT_NE(dict_text.find(L"\nB$"), wstring::npos);
    EXPECT_EQ(count(dict_text.begin(), dict_text.end(), L'\n'), 1);

    wstring mismatch;
    op.FetchWordsEx(0, 0, width, height, L"000000", 0.9, L"A", 3, 4, 1, mismatch);
    EXPECT_TRUE(mismatch.empty()) << "Filtered rect count and word count must still match";
}

TEST(ImageColorTest, DotMatrixCApiEntrypointsWork) {
    CApiHandle api;
    ASSERT_NE(api.handle, nullptr);

    const int width = 24;
    const int height = 12;
    auto pixels = MakePixels(width, height);
    PaintScaledGlyphA(pixels, width, 0, 0, 2, 0x19, 0x19, 0x19);
    PaintScaledGlyphA(pixels, width, 12, 0, 2, 0x19, 0x19, 0x19);
    PaintPixel(pixels, width, 23, 0, 0x19, 0x19, 0x19);
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    const wstring mode = L"mem:" + PtrToWString(bmp.data());
    ASSERT_EQ(OpSetDisplayInput(api.handle, mode.c_str()), 1);

    const wstring rects =
        OpExtractWordRectsEx(api.handle, 0, 0, width, height, L"000000", 0.9, 3, 4, 1);
    EXPECT_EQ(rects, L"1,1,9,11|13,1,21,11");

    const wstring dict_text = OpFetchWordsEx(api.handle, 0, 0, width, height, L"000000", 0.9, L"AB", 3, 4, 1);
    EXPECT_EQ(dict_text.find(L"A$"), 0u);
    EXPECT_NE(dict_text.find(L"\nB$"), wstring::npos);
    EXPECT_EQ(count(dict_text.begin(), dict_text.end(), L'\n'), 1);

    const wstring dict_by_rects =
        OpFetchWordsByRects(api.handle, 0, 0, width, height, L"000000", 0.9, L"AB", L"2,2,8,10|14,2,20,10");
    EXPECT_EQ(dict_by_rects.find(L"A$"), 0u);
    EXPECT_NE(dict_by_rects.find(L"\nB$"), wstring::npos);

    const wstring word_entry = OpFetchWordEx(api.handle, 0, 0, 10, height, L"000000", 0.9, L"A");
    EXPECT_EQ(word_entry.rfind(L"A$", 0), 0u);

    int valid_count = 0;
    const wstring normalized =
        OpNormalizeWordDict(api.handle, (dict_text + L"\r\nbad-entry\n" + word_entry).c_str(), &valid_count);
    EXPECT_EQ(valid_count, 3);
    EXPECT_EQ(normalized, dict_text + L"\n" + word_entry);

    int renamed_count = 0;
    const wstring renamed = OpRenameWordDict(api.handle, normalized.c_str(), L"XYZ", &renamed_count);
    EXPECT_EQ(renamed_count, 3);
    EXPECT_EQ(renamed.find(L"X$"), 0u);
    EXPECT_NE(renamed.find(L"\nY$"), wstring::npos);
    EXPECT_NE(renamed.find(L"\nZ$"), wstring::npos);

    EXPECT_EQ(OpSetBinaryPreprocess(api.handle, 3, 0, 2, 0), 1);
    int preprocess_mode = 0;
    int isolated_threshold = -1;
    int min_component_area = 0;
    int bridge_gap = 0;
    EXPECT_EQ(
        OpGetBinaryPreprocess(api.handle, &preprocess_mode, &isolated_threshold, &min_component_area, &bridge_gap), 1);
    EXPECT_EQ(preprocess_mode, 3);
    EXPECT_EQ(isolated_threshold, 0);
    EXPECT_EQ(min_component_area, 2);
    EXPECT_EQ(bridge_gap, 0);
}

TEST(ImageColorTest, FetchWordsByRectsUsesGivenRects) {
    op::Op op;
    long ret = 0;
    const int width = 24;
    const int height = 12;
    auto pixels = MakePixels(width, height);
    PaintScaledGlyphA(pixels, width, 0, 0, 2, 0x19, 0x19, 0x19);
    PaintScaledGlyphA(pixels, width, 12, 0, 2, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_text;
    op.FetchWordsByRects(0, 0, width, height, L"000000", 0.9, L"AB", L"2,2,8,10|14,2,20,10", dict_text);

    EXPECT_EQ(dict_text.find(L"A$"), 0u);
    EXPECT_NE(dict_text.find(L"\nB$"), wstring::npos);
    EXPECT_EQ(count(dict_text.begin(), dict_text.end(), L'\n'), 1);

    wstring mismatch;
    op.FetchWordsByRects(0, 0, width, height, L"000000", 0.9, L"A", L"2,2,8,10|14,2,20,10", mismatch);
    EXPECT_TRUE(mismatch.empty()) << "Rect count and word count must match";

    wstring out_of_range;
    op.FetchWordsByRects(0, 0, width, height, L"000000", 0.9, L"A", L"2,2,99,10", out_of_range);
    EXPECT_TRUE(out_of_range.empty()) << "Rects outside the capture region should be rejected";
}

TEST(ImageColorTest, GetBinaryPreviewShowsBinarizedRegion) {
    op::Op op;
    long ret = 0;
    const int width = 5;
    const int height = 6;
    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring preview;
    op.GetBinaryPreview(0, 0, width, height, L"000000", 0.9, preview, &ret);

    EXPECT_EQ(ret, 8);
    EXPECT_EQ(preview, L"5,6\n.....\n..#..\n.#.#.\n.###.\n.#.#.\n.....");
}

TEST(ImageColorTest, BinaryPreprocessIsDisabledByDefault) {
    op::Op op;
    long ret = 0;
    const int width = 6;
    const int height = 4;
    auto pixels = MakePixels(width, height);
    PaintPixel(pixels, width, 5, 0, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 2, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 2, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    // Explicitly disable preprocessing: the "disabled" contract must hold
    // regardless of the library default (which is now mode=1 — remove isolated points).
    op.SetBinaryPreprocess(0, 0, 0, 0, &ret);
    ASSERT_EQ(ret, 1);

    wstring preview;
    op.GetBinaryPreview(0, 0, width, height, L"000000", 1.0, preview, &ret);

    EXPECT_EQ(ret, 5);
    EXPECT_EQ(preview, L"6,4\n.....#\n.#.#..\n.#.#..\n......");
}

TEST(ImageColorTest, BinaryPreprocessRemovesIsolatedPointsByDefault) {
    op::Op op;
    long ret = 0;
    const int width = 6;
    const int height = 4;
    auto pixels = MakePixels(width, height);
    PaintPixel(pixels, width, 5, 0, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 2, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 2, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    // Default preprocessing (mode=1) removes the fully-isolated single point at
    // (5,0) but must NOT touch the two connected 2x2 components.
    wstring preview;
    op.GetBinaryPreview(0, 0, width, height, L"000000", 1.0, preview, &ret);

    EXPECT_EQ(ret, 4);
    EXPECT_EQ(preview, L"6,4\n......\n.#.#..\n.#.#..\n......");
}

TEST(ImageColorTest, BinaryPreprocessRemovesNoiseAndBridgesOnePixelGaps) {
    op::Op op;
    long ret = 0;
    op.SetBinaryPreprocess(3, 0, 2, 1, &ret);
    ASSERT_EQ(ret, 1);

    long mode = 0;
    long isolated_threshold = -1;
    long min_component_area = 0;
    long bridge_gap = 0;
    op.GetBinaryPreprocess(&mode, &isolated_threshold, &min_component_area, &bridge_gap, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(mode, 3);
    EXPECT_EQ(isolated_threshold, 0);
    EXPECT_EQ(min_component_area, 2);
    EXPECT_EQ(bridge_gap, 1);

    const int width = 6;
    const int height = 4;
    auto pixels = MakePixels(width, height);
    PaintPixel(pixels, width, 5, 0, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 2, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 2, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring preview;
    op.GetBinaryPreview(0, 0, width, height, L"000000", 1.0, preview, &ret);

    EXPECT_EQ(ret, 6);
    EXPECT_EQ(preview, L"6,4\n......\n.###..\n.###..\n......");
}

TEST(ImageColorTest, BinaryPreprocessBridgeGapZeroDoesNotBridge) {
    op::Op op;
    long ret = 0;
    op.SetBinaryPreprocess(3, 0, 1, 0, &ret);
    ASSERT_EQ(ret, 1);

    const int width = 5;
    const int height = 4;
    auto pixels = MakePixels(width, height);
    PaintPixel(pixels, width, 1, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 1, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 1, 2, 0x00, 0x00, 0x00);
    PaintPixel(pixels, width, 3, 2, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring preview;
    op.GetBinaryPreview(0, 0, width, height, L"000000", 1.0, preview, &ret);

    EXPECT_EQ(ret, 4);
    EXPECT_EQ(preview, L"5,4\n.....\n.#.#.\n.#.#.\n.....");
}

TEST(ImageColorTest, BinaryPreprocessDoesNotChangeColorBlockSearch) {
    op::Op op;
    long ret = 0;
    op.SetBinaryPreprocess(1, 0, 2, 0, &ret);
    ASSERT_EQ(ret, 1);

    const int width = 6;
    const int height = 4;
    auto pixels = MakePixels(width, height);
    PaintPixel(pixels, width, 5, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindColorBlock(0, 0, width, height, L"000000", 1.0, 1, 1, 1, &x, &y, &ret);

    EXPECT_EQ(ret, 1);
    EXPECT_EQ(x, 5);
    EXPECT_EQ(y, 0);
}

TEST(ImageColorTest, ColorSearchFailurePathsClearOutputs) {
    op::Op op;
    long ret = 0;
    auto pixels = MakePixels(6, 4);
    SetMemBmp(op, 6, 4, pixels, ret);
    ASSERT_EQ(ret, 1);

    long count = 123;
    op.GetColorNum(0, 0, 6, 4, L"000000", 1.0, &count);
    EXPECT_EQ(count, 0);

    long x = 123;
    long y = 456;
    ret = 99;
    op.FindColorBlock(0, 0, 6, 4, L"000000", 1.0, 1, 1, 1, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);

    int cx = 123;
    int cy = 456;
    EXPECT_EQ(OpFindColorBlock(nullptr, 0, 0, 6, 4, L"000000", 1.0, 1, 1, 1, &cx, &cy), 0);
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cy, -1);
}

TEST(ImageColorTest, FindColorBlockExClearsResultAndRejectsInvalidSize) {
    op::Op op;
    long ret = 0;
    const int width = 6;
    const int height = 4;
    auto pixels = MakePixels(width, height);
    PaintPixel(pixels, width, 2, 1, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring results = L"old";
    op.FindColorBlockEx(0, 0, width, height, L"000000", 1.0, 1, 1, 1, results);
    EXPECT_EQ(results, L"2,1");

    op.FindColorBlockEx(0, 0, width, height, L"000000", 1.0, 1, 0, 1, results);
    EXPECT_TRUE(results.empty());
}

TEST(ImageColorTest, GetWordPreviewValidatesAndShowsPointEntry) {
    op::Op op;
    long ret = 0;
    const int width = 5;
    const int height = 6;
    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", dict_entry);
    ASSERT_FALSE(dict_entry.empty());

    wstring preview;
    op.GetWordPreview(dict_entry.c_str(), preview, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(preview, L"A,3,4,8\n.#.\n#.#\n###\n#.#");

    op.GetWordPreview(L"bad-entry", preview, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(preview.empty());

    wstring report;
    op.CheckWordDict((dict_entry + L"\nbad-entry").c_str(), report, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(report, L"0,1,A,3,4,8,67|1,0,invalid");
}

TEST(ImageColorTest, NormalizeWordDictKeepsOnlyValidEntries) {
    op::Op op;
    long ret = 0;
    const int width = 5;
    const int height = 6;
    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", dict_entry);
    ASSERT_FALSE(dict_entry.empty());

    wstring normalized;
    op.NormalizeWordDict((L"\r\n" + dict_entry + L"\r\nbad-entry\n" + dict_entry + L"\n").c_str(), normalized, &ret);

    EXPECT_EQ(ret, 2);
    EXPECT_EQ(normalized, dict_entry + L"\n" + dict_entry);
}

TEST(ImageColorTest, RenameWordDictUpdatesValidEntryNames) {
    op::Op op;
    long ret = 0;
    const int width = 5;
    const int height = 6;
    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", dict_entry);
    ASSERT_FALSE(dict_entry.empty());

    wstring renamed;
    op.RenameWordDict((dict_entry + L"\ninvalid\n" + dict_entry).c_str(), L"BC", renamed, &ret);

    EXPECT_EQ(ret, 2);
    EXPECT_EQ(renamed, L"B" + dict_entry.substr(1) + L"\nC" + dict_entry.substr(1));

    wstring mismatch;
    op.RenameWordDict(dict_entry.c_str(), L"BC", mismatch, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(mismatch.empty());
}

TEST(ImageColorTest, AddDictSupportsDmPointTextFormat) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 14;

    auto pixels = MakePixels(width, height);
    for (auto [x, y] :
         {pair{2, 3}, pair{1, 4}, pair{3, 4}, pair{1, 5}, pair{2, 5}, pair{3, 5}, pair{1, 6}, pair{3, 6},
          pair{1, 7}, pair{3, 7}}) {
        PaintPixel(pixels, width, x, y, 0x00, 0x00, 0x00);
    }
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, L"1E0500780$A$0.0.10$11", &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, SetDictDoesNotImportOpTextDictFiles) {
    const auto path = std::filesystem::temp_directory_path() / L"op_setdict_op_text_dict.txt";
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "A$4,3,8$5E0E\n";
    }

    op::Op op;
    long ret = 0;
    op.SetDict(0, path.c_str(), &ret);
    EXPECT_EQ(ret, 0);

    long count = -1;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 0);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ImageColorTest, SetDictSupportsDmTextDictFiles) {
    const auto path = std::filesystem::temp_directory_path() / L"op_setdict_dm_text_dict.txt";
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "1E0500780$A$0.0.10$11\n";
    }

    op::Op op;
    long ret = 0;
    op.SetDict(0, path.c_str(), &ret);
    EXPECT_EQ(ret, 1);

    long count = -1;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 1);

    std::wstring converted;
    op.GetDict(0, 0, converted);
    EXPECT_EQ(converted, L"A$11,3,10$78A0001E00");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ImageColorTest, SetDictIsSharedAcrossObjects) {
    const auto path = std::filesystem::temp_directory_path() / L"op_setdict_global_dm_text_dict.txt";
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "1E0500780$A$0.0.10$11\n";
    }

    op::Op loader;
    op::Op matcher;
    long ret = 0;
    loader.SetDict(3, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    long count = 0;
    matcher.GetDictCount(3, &count);
    EXPECT_EQ(count, 1);

    std::wstring converted;
    matcher.GetDict(3, 0, converted);
    EXPECT_EQ(converted, L"A$11,3,10$78A0001E00");

    const int width = 8;
    const int height = 14;
    auto pixels = MakePixels(width, height);
    for (auto [x, y] :
         {pair{2, 3}, pair{1, 4}, pair{3, 4}, pair{1, 5}, pair{2, 5}, pair{3, 5}, pair{1, 6}, pair{3, 6},
          pair{1, 7}, pair{3, 7}}) {
        PaintPixel(pixels, width, x, y, 0x00, 0x00, 0x00);
    }
    SetMemBmp(matcher, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);
    matcher.UseDict(3, &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    matcher.FindStr(0, 0, width, height, L"A", L"000000-000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ImageColorTest, SetMemDictSupportsDmPointTextFormat) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 14;

    auto pixels = MakePixels(width, height);
    for (auto [x, y] :
         {pair{2, 3}, pair{1, 4}, pair{3, 4}, pair{1, 5}, pair{2, 5}, pair{3, 5}, pair{1, 6}, pair{3, 6},
          pair{1, 7}, pair{3, 7}}) {
        PaintPixel(pixels, width, x, y, 0x00, 0x00, 0x00);
    }
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    const char *legacy = "1E0500780$A$0.0.10$11\n";
    op.SetMemDict(0, reinterpret_cast<const wchar_t *>(legacy), static_cast<long>(strlen(legacy)), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, SetMemDictSupportsOpTextEntryFormat) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    const char *op_text = "A$4,3,8$5E0E\n";
    op.SetMemDict(0, reinterpret_cast<const wchar_t *>(op_text), static_cast<long>(strlen(op_text)), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, SetMemDictUpdatesGlobalDictForAllObjects) {
    const auto path = std::filesystem::temp_directory_path() / L"op_setmemdict_global_update.txt";
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "1E0500780$A$0.0.10$11\n";
    }

    op::Op loader;
    op::Op memory_loader;
    op::Op global_user;
    long ret = 0;
    loader.SetDict(4, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    const char *memory_text = "B$4,3,8$5E0E\n";
    memory_loader.SetMemDict(4, memory_text, static_cast<long>(strlen(memory_text)),
                            &ret);
    ASSERT_EQ(ret, 1);

    std::wstring converted;
    memory_loader.GetDict(4, 0, converted);
    EXPECT_EQ(converted, L"B$4,3,8$5E0E");

    global_user.GetDict(4, 0, converted);
    EXPECT_EQ(converted, L"B$4,3,8$5E0E");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ImageColorTest, SetMemDictSupportsBinaryDictFileBytes) {
    const auto path = std::filesystem::temp_directory_path() / L"op_setmemdict_binary_bytes.dict";

    op::Op writer;
    op::Op loader;
    op::Op reader;
    long ret = 0;
    writer.ClearDict(6, &ret);
    ASSERT_EQ(ret, 1);
    writer.AddDict(6, L"A$4,3,8$5E0E", &ret);
    ASSERT_EQ(ret, 1);
    writer.SaveDict(6, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());

    loader.SetMemDict(6, bytes.data(), static_cast<long>(bytes.size()), &ret);
    ASSERT_EQ(ret, 1);

    std::wstring converted;
    reader.GetDict(6, 0, converted);
    EXPECT_EQ(converted, L"A$4,3,8$5E0E");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ImageColorTest, DictSlotSupportsZeroToNinetyNine) {
    op::Op op;
    long ret = 0;
    const char *dict_text = "A$4,3,8$5E0E\n";

    op.SetMemDict(99, dict_text, static_cast<long>(strlen(dict_text)), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(99, &ret);
    EXPECT_EQ(ret, 1);

    long count = 0;
    op.GetDictCount(99, &count);
    EXPECT_EQ(count, 1);

    op.UseDict(100, &ret);
    EXPECT_EQ(ret, 0);
    op.SetMemDict(100, dict_text, static_cast<long>(strlen(dict_text)), &ret);
    EXPECT_EQ(ret, 0);
}

TEST(ImageColorTest, AddDictRejectsInvalidOpTextEntry) {
    op::Op op;
    long ret = 0;

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    const wchar_t *invalid_entries[] = {
        L"A$4,3,8$5E",
        L"A$4,3,8$5E0Z",
        L"A$4,3,13$5E0E",
        L"A$0,3,8$5E0E",
    };

    for (const auto entry : invalid_entries) {
        op.AddDict(0, entry, &ret);
        EXPECT_EQ(ret, 0);
    }

    long count = -1;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 0);
}

TEST(ImageColorTest, SetMemDictRejectsInvalidOpTextEntry) {
    op::Op op;
    long ret = 0;
    const char *op_text = "A$4,3,8$5E\n";

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    op.SetMemDict(0, op_text, static_cast<long>(strlen(op_text)), &ret);
    EXPECT_EQ(ret, 0);

    long count = -1;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 0);
}

TEST(ImageColorTest, AddDictSupportsProvidedOpTextEntryFormats) {
    op::Op op;
    long ret = 0;
    const wchar_t *ci = L"此$13,15,107$0010FFE37F00F4FFFE1F048320FFEFFFFF7F188801193C8307";
    const wchar_t *diannao =
        L"电脑$13,29,224$"
        L"FC87FF9008124122FEFFFF2791249244F24FFEC9FF011800FEFFFF3712FEFFFF4FFEE81FCD2A4FE7C97ED12CF24FFE01";

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, ci, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, diannao, &ret);
    ASSERT_EQ(ret, 1);

    long count = 0;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 2);

    std::wstring converted;
    op.GetDict(0, 0, converted);
    EXPECT_EQ(converted, ci);
    op.GetDict(0, 1, converted);
    EXPECT_EQ(converted, diannao);
}

TEST(ImageColorTest, AddDictSupportsProvidedDmMultiCharFormat) {
    op::Op op;
    long ret = 0;
    const wchar_t *legacy = L"1FE000007FF08010420840FFE08020080206000001FF24448891122FFE48891122244488FF001000003FFC488"
                            L"91FFC0013F2844928E33C2CC4089F900$"
                            L"此电脑$1.0.202$13";

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, legacy, &ret);
    EXPECT_EQ(ret, 1);

    long count = 0;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 1);

    std::wstring converted;
    op.GetDict(0, 0, converted);
    EXPECT_FALSE(converted.empty());
    EXPECT_EQ(converted.rfind(L"此电脑$11,44,", 0), 0u);
}

TEST(ImageColorTest, AddDictSupportsProvidedDmFeishuFormat) {
    op::Op op;
    long ret = 0;
    const wchar_t *dm = L"80100200400801002007F80CE1404810840801C0000811022044088111FFC440881102237420F21C$"
                        L"飞书$0.0.88$13";

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dm, &ret);
    EXPECT_EQ(ret, 1);

    long count = 0;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 1);

    std::wstring converted;
    op.GetDict(0, 0, converted);
    EXPECT_FALSE(converted.empty());
    EXPECT_EQ(converted.rfind(L"飞书$11,29,", 0), 0u);
}

TEST(ImageColorTest, AddDictSupportsProvidedDmTextFormats) {
    struct DmCase {
        const wchar_t *entry;
        const wchar_t *converted_prefix;
    };
    const DmCase cases[] = {
        {L"8010020440881102204408FFF0220440881102204408$玉$0.1.62$16", L"玉$11,16,"},
        {L"0801002004008010FFF0400801803F840C8290C23040080100$龙$0.0.78$17", L"龙$11,18,"},
        {L"C019FF64EC9D93B2764ED9FFFB2764EC9D93B277FE00$更$0.1.131$17", L"更$11,16,"},
        {L"012624FC989F1E624DC989012000FF90C2104210620F410$新$0.0.101$18", L"新$11,17,"},
        {L"0083904208410E21846094D08E104208410821C430841C8390020$安$0.0.93$19", L"安$11,19,"},
        {L"0FE12024048091F2124248490921242484FE800$卓$3.1.91$18", L"卓$11,14,"},
        {L"0400811F9800000001A066F8F81202404809013E3FE7048080100$设$0.0.98$18", L"设$11,19,"},
        {L"0640982298570BD14949392324448C93925A514E298700C018$备$0.0.114$18", L"备$11,18,"},
        {L"40180300600C0180300600C0180300600C0180300600C0180300600C01800$一$0.0.43$2", L"一$11,22,"},
        {L"0200C0388F83B0E670DE18C30C60CC0E10E20C01C1FF3FE000007FFFFE$剑$0.0.188$21", L"剑$11,21,"},
        {L"008019839831801000483D3E2F84B08610C3FFFFF8610C2184308610020000000301E0FC7FBE06000007F8FF00000000000000FFCFF8"
         L"00000000000FF1FE$诛仙$0.0.365$22",
         L"诛仙$11,45,"},
        {L"401803786FCC3F81F00600E01FFFFFF00C0180F07E7EDF180300200$平$1.1.164$21", L"平$11,20,"},
        {L"0180703C3F0FC0B80700E01C0387FFFFF1C2380700E01C0380700E0080$生$0.1.180$21", L"生$11,21,"},
        {L"1FE3FC03FFFFFE3E0701F07FFFFF04618C31BFFFFE38C318630C7FF7FE$烟$0.0.274$21", L"烟$11,21,"},
        {L"4018FF3FE78CF19F337667CC7983FFFFFFF99A33666ECCF98F30E70CFF8FC$雨$0.0.244$21", L"雨$11,22,"},
        {L"00660CE19E31E61CC180180700E1FFFFF7FE0E01C038FFFFFDFF8380700E0080000000000000000000000000000C0381B9F7FE7F8FE0"
         L"FF07E01C0000000000000000000000000000000000738F78E78C700600C1FFFFFBFF1CE39C7187C1F8FF3FE3FF1FE3FC7F8EE1800000"
         L"0000000000EE1DE39E71CE09FF3FE7FC3F00601EFFFFFFFF03E039C73CE3FE3FC0E01C$进入游戏$0.1.1035$24",
         L"进入游戏$11,104,"},
    };

    for (const auto &item : cases) {
        op::Op op;
        long ret = 0;

        op.ClearDict(0, &ret);
        ASSERT_EQ(ret, 1);
        op.AddDict(0, item.entry, &ret);
        EXPECT_EQ(ret, 1);

        long count = 0;
        op.GetDictCount(0, &count);
        EXPECT_EQ(count, 1);

        std::wstring converted;
        op.GetDict(0, 0, converted);
        EXPECT_FALSE(converted.empty());
        EXPECT_EQ(converted.rfind(item.converted_prefix, 0), 0u);
    }
}

TEST(ImageColorTest, FindStrUsesChannelColorToleranceForBinaryText) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto make_pixels = [&](bool add_blue_noise) {
        vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
        auto paint = [&](int x, int y, uchar b, uchar g, uchar r) {
            const auto idx = static_cast<size_t>(y * width + x) * 4;
            pixels[idx + 0] = b;
            pixels[idx + 1] = g;
            pixels[idx + 2] = r;
            pixels[idx + 3] = 0xff;
        };
        for (auto [x, y] :
             {pair{2, 1}, pair{1, 2}, pair{3, 2}, pair{1, 3}, pair{2, 3}, pair{3, 3}, pair{1, 4}, pair{3, 4}}) {
            paint(x, y, 0x00, 0x00, 0x00);
        }
        if (add_blue_noise)
            paint(2, 2, 0xff, 0x00, 0x00);
        return pixels;
    };

    auto clean_bmp = BuildBmp32TopDown(width, height, make_pixels(false));
    wstring mode = L"mem:" + PtrToWString(clean_bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-303030", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto noisy_bmp = BuildBmp32TopDown(width, height, make_pixels(true));
    mode = L"mem:" + PtrToWString(noisy_bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-303030", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0) << "Blue noise must not match the black text tolerance";
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, FindStrKeepsAntialiasedTextWithinTolerance) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto clean_pixels = MakePixels(width, height);
    PaintGlyphA(clean_pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, clean_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto antialiased_pixels = MakePixels(width, height);
    PaintGlyphA(antialiased_pixels, width, 0, 0, 0x22, 0x22, 0x22);
    SetMemBmp(op, width, height, antialiased_pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-303030", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, FindStrSupportsColoredTextWithChannelTolerance) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto clean_pixels = MakePixels(width, height);
    PaintGlyphA(clean_pixels, width, 0, 0, 0x00, 0x00, 0xff);
    SetMemBmp(op, width, height, clean_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"FF0000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto noisy_pixels = MakePixels(width, height);
    PaintGlyphA(noisy_pixels, width, 0, 0, 0x10, 0x12, 0xe8);
    PaintPixel(noisy_pixels, width, 2, 2, 0x00, 0x80, 0x00);
    SetMemBmp(op, width, height, noisy_pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FF0000-202020", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0) << "Same-gray green noise must not match red text tolerance";
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, FindStrSupportsMultipleForegroundColors) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto paint_mixed_glyph = [&](vector<uchar> &pixels, uchar red_b, uchar red_g, uchar red_r, uchar black) {
        PaintPixel(pixels, width, 2, 1, red_b, red_g, red_r);
        PaintPixel(pixels, width, 1, 2, black, black, black);
        PaintPixel(pixels, width, 3, 2, red_b, red_g, red_r);
        PaintPixel(pixels, width, 1, 3, black, black, black);
        PaintPixel(pixels, width, 2, 3, red_b, red_g, red_r);
        PaintPixel(pixels, width, 3, 3, black, black, black);
        PaintPixel(pixels, width, 1, 4, red_b, red_g, red_r);
        PaintPixel(pixels, width, 3, 4, black, black, black);
    };

    auto clean_pixels = MakePixels(width, height);
    paint_mixed_glyph(clean_pixels, 0x00, 0x00, 0xff, 0x00);
    SetMemBmp(op, width, height, clean_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000|FF0000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto tolerant_pixels = MakePixels(width, height);
    paint_mixed_glyph(tolerant_pixels, 0x08, 0x10, 0xf0, 0x18);
    SetMemBmp(op, width, height, tolerant_pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-202020|FF0000-202020", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, LocalOcrApisUseBinaryColorTolerance) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 8;

    auto dict_pixels = MakePixels(8, height);
    PaintGlyphA(dict_pixels, 8, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, 8, height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, 8, height, L"000000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x20, 0x20, 0x20);
    PaintGlyphA(pixels, width, 8, 0, 0x20, 0x20, 0x20);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring str;
    op.Ocr(0, 0, width, height, L"000000-303030", 1.0, str);
    EXPECT_EQ(str, L"AA");

    op.OcrEx(0, 0, width, height, L"000000-303030", 1.0, str);
    EXPECT_EQ(str, L"1,1,A|9,1,A");

    op.FindStrEx(0, 0, width, height, L"A", L"000000-303030", 1.0, str);
    EXPECT_EQ(str, L"0,1,1|0,9,1");
}

TEST(ImageColorTest, PointTextUsesSimilarityAsImplicitColorTolerance) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 8;

    auto dict_pixels = MakePixels(8, height);
    PaintGlyphA(dict_pixels, 8, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, 8, height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, 8, height, L"000000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x19, 0x19, 0x19);
    PaintGlyphA(pixels, width, 8, 0, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, -1) << "Exact similarity keeps color matching exact when no explicit delta is provided";
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);

    op.FindStr(0, 0, width, height, L"A", L"000000", 0.9, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);

    wstring str;
    op.Ocr(0, 0, width, height, L"000000", 0.9, str);
    EXPECT_EQ(str, L"AA");

    op.OcrEx(0, 0, width, height, L"000000", 0.9, str);
    EXPECT_EQ(str, L"1,1,A|9,1,A");

    op.FindStrEx(0, 0, width, height, L"A", L"000000", 0.9, str);
    EXPECT_EQ(str, L"0,1,1|0,9,1");
}

TEST(ImageColorTest, PointTextExplicitZeroDeltaOverridesSimilarityTolerance) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto dict_pixels = MakePixels(width, height);
    PaintGlyphA(dict_pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x19, 0x19, 0x19);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"000000-000000", 0.9, &x, &y, &ret);
    EXPECT_EQ(ret, -1) << "Explicit -000000 keeps point-text color matching exact";
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);
}

TEST(ImageColorTest, FetchWordTreatsMultipleBackgroundColorsAsBackground) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto pixels = MakePixels(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x) {
            PaintPixel(pixels, width, x, y, 0xee, 0xee, 0xee);
        }
    }
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring word_data;
    op.FetchWord(0, 0, width, height, L"@FFFFFF-000000|EEEEEE-000000", L"Blank", word_data);

    EXPECT_TRUE(word_data.empty()) << "All pixels are declared background colors, so no word should be extracted";
}

TEST(ImageColorTest, FindStrSupportsAutoBackgroundBinarization) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    auto clean_pixels = MakePixels(width, height);
    PaintGlyphA(clean_pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, clean_pixels, ret);
    ASSERT_EQ(ret, 1);

    wstring dict_entry;
    op.FetchWord(0, 0, width, height, L"000000-000000", L"A", dict_entry);
    ASSERT_EQ(dict_entry, L"A$4,3,8$5E0E");
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, dict_entry.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    op.UseDict(0, &ret);
    ASSERT_EQ(ret, 1);

    auto pixels = MakePixels(width, height);
    PaintGlyphA(pixels, width, 0, 0, 0x00, 0x00, 0x00);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 1);
    EXPECT_EQ(y, 1);
}

TEST(ImageColorTest, GameHudFindsBrightTextOnDynamicBackground) {
    op::Op op;
    long ret = 0;
    const int dict_width = 8;
    const int dict_height = 8;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x40, 0x40, 0x40);
    PaintGlyphA(dict_pixels, dict_width, 0, 0, 0xff, 0xff, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"FFFFFF-000000", L"A", ret));

    const int width = 32;
    const int height = 18;
    auto pixels = MakePixels(width, height, 0x40, 0x50, 0x60);
    PaintGlyphA(pixels, width, 12, 6, 0xf2, 0xf5, 0xff);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FFFFFF-202020", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 13);
    EXPECT_EQ(y, 7);
}

TEST(ImageColorTest, GameHudFindsOutlinedTextWithMultipleForegroundColors) {
    op::Op op;
    long ret = 0;
    const int dict_width = 10;
    const int dict_height = 10;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x50, 0x60, 0x70);
    PaintOutlinedGlyphA(dict_pixels, dict_width, 2, 2, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"FFFFFF-000000|000000-000000", L"A", ret));

    const int width = 36;
    const int height = 20;
    auto pixels = MakePixels(width, height);
    PaintGameBackground(pixels, width, height);
    PaintOutlinedGlyphA(pixels, width, 14, 5, 0x08, 0x08, 0x08, 0xee, 0xf0, 0xff);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FFFFFF-202020|000000-202020", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 14);
    EXPECT_EQ(y, 5);
}

TEST(ImageColorTest, GameHudIgnoresGlowAroundText) {
    op::Op op;
    long ret = 0;
    const int dict_width = 8;
    const int dict_height = 8;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x40, 0x40, 0x40);
    PaintGlyphA(dict_pixels, dict_width, 0, 0, 0x60, 0xe0, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"FFE060-000000", L"A", ret));

    const int width = 28;
    const int height = 16;
    auto pixels = MakePixels(width, height);
    PaintGameBackground(pixels, width, height);
    PaintOutlinedGlyphA(pixels, width, 10, 4, 0x20, 0x60, 0xa0, 0x70, 0xd4, 0xf4);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FFE060-303030", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 11);
    EXPECT_EQ(y, 5);
}

TEST(ImageColorTest, GameHudFindsGradientDamageTextWithinTolerance) {
    op::Op op;
    long ret = 0;
    const int dict_width = 8;
    const int dict_height = 8;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x40, 0x40, 0x40);
    PaintGlyphA(dict_pixels, dict_width, 0, 0, 0x00, 0xaa, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"FFAA00-000000", L"A", ret));

    const int width = 30;
    const int height = 18;
    auto pixels = MakePixels(width, height);
    PaintGameBackground(pixels, width, height);
    const vector<pair<int, int>> points = {{2, 1}, {1, 2}, {3, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {3, 4}};
    int idx = 0;
    for (auto [x, y] : points) {
        const auto b = static_cast<uchar>((idx % 3) * 0x10);
        const auto g = static_cast<uchar>(0x92 + (idx % 4) * 0x12);
        const auto r = static_cast<uchar>(0xe8 + (idx % 2) * 0x10);
        PaintPixel(pixels, width, x + 13, y + 6, b, g, r);
        ++idx;
    }
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FFAA00-305030", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 14);
    EXPECT_EQ(y, 7);
}

TEST(ImageColorTest, GameHudFindsScaledUiTextWhenDictionaryMatchesScale) {
    op::Op op;
    long ret = 0;
    const int dict_width = 14;
    const int dict_height = 14;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x40, 0x40, 0x40);
    PaintScaledGlyphA(dict_pixels, dict_width, 1, 1, 2, 0xff, 0xff, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"FFFFFF-000000", L"A", ret));

    const int width = 40;
    const int height = 22;
    auto pixels = MakePixels(width, height);
    PaintGameBackground(pixels, width, height);
    PaintScaledGlyphA(pixels, width, 16, 6, 2, 0xf0, 0xf0, 0xff);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FFFFFF-202020", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 18);
    EXPECT_EQ(y, 8);
}

TEST(ImageColorTest, GameHudScaleMismatchDoesNotMatchUnscaledDictionary) {
    op::Op op;
    long ret = 0;
    const int dict_width = 8;
    const int dict_height = 8;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x40, 0x40, 0x40);
    PaintGlyphA(dict_pixels, dict_width, 0, 0, 0xff, 0xff, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"FFFFFF-000000", L"A", ret));

    const int width = 40;
    const int height = 22;
    auto pixels = MakePixels(width, height);
    PaintGameBackground(pixels, width, height);
    PaintScaledGlyphA(pixels, width, 16, 6, 2, 0xf0, 0xf0, 0xff);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"FFFFFF-202020", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, -1) << "Point-matrix matching requires a dictionary captured at the same UI scale";
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);
}

TEST(ImageColorTest, GameHudBackgroundModeSupportsTwoTonePanels) {
    op::Op op;
    long ret = 0;
    const int dict_width = 12;
    const int dict_height = 10;

    auto dict_pixels = MakePixels(dict_width, dict_height, 0x30, 0x40, 0x50);
    for (int y = 0; y < dict_height; ++y) {
        for (int x = dict_width / 2; x < dict_width; ++x) {
            PaintPixel(dict_pixels, dict_width, x, y, 0x38, 0x48, 0x58);
        }
    }
    PaintGlyphA(dict_pixels, dict_width, 3, 2, 0xff, 0xff, 0xff);
    SetMemBmp(op, dict_width, dict_height, dict_pixels, ret);
    ASSERT_EQ(ret, 1);
    ASSERT_NO_FATAL_FAILURE(UseSingleWordDict(op, dict_width, dict_height, L"@504030-080808|584838-080808", L"A", ret));

    const int width = 34;
    const int height = 18;
    auto pixels = MakePixels(width, height, 0x34, 0x44, 0x54);
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x) {
            PaintPixel(pixels, width, x, y, 0x3a, 0x4a, 0x5a);
        }
    }
    PaintGlyphA(pixels, width, 14, 5, 0xf0, 0xf0, 0xff);
    SetMemBmp(op, width, height, pixels, ret);
    ASSERT_EQ(ret, 1);

    long x = -1;
    long y = -1;
    op.FindStr(0, 0, width, height, L"A", L"@504030-080808|584838-080808", 1.0, &x, &y, &ret);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(x, 15);
    EXPECT_EQ(y, 6);
}

TEST(ImageColorTest, SetDisplayInputMemBmpPointer) {
    op::Op op;
    long ret = 0;
    const int width = 32;
    const int height = 32;
    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = static_cast<size_t>(y * width + x) * 4;
            pixels[idx + 0] = 0x01;
            pixels[idx + 1] = 0x02;
            pixels[idx + 2] = 0x03;
            pixels[idx + 3] = 0xff;
        }
    }
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring color;
    op.GetColor(10, 10, color);
    EXPECT_EQ(color, L"030201");
}

TEST(ImageColorTest, SetDisplayInputMemRawBgr) {
    op::Op op;
    long ret = 0;
    const int width = 32;
    const int height = 32;
    vector<uchar> raw_bgr(static_cast<size_t>(width) * height * 3, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = static_cast<size_t>(y * width + x) * 3;
            raw_bgr[idx + 0] = 0x28;
            raw_bgr[idx + 1] = 0x32;
            raw_bgr[idx + 2] = 0x3c;
        }
    }
    wstring mode = L"mem:" + PtrToWString(raw_bgr.data()) + L",32,32,bgr";

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring color;
    op.GetColor(10, 10, color);
    EXPECT_EQ(color, L"3C3228");
}

TEST(ImageColorTest, SetDisplayInputMemRawHexPointer) {
    op::Op op;
    long ret = 0;
    const int width = 32;
    const int height = 32;
    vector<uchar> raw_bgra(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = static_cast<size_t>(y * width + x) * 4;
            raw_bgra[idx + 0] = 0x11;
            raw_bgra[idx + 1] = 0x22;
            raw_bgra[idx + 2] = 0x33;
            raw_bgra[idx + 3] = 0xff;
        }
    }
    wstring mode = L"mem:" + PtrToWString(raw_bgra.data(), true) + L",32,32,bgra";

    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring color;
    op.GetColor(10, 10, color);
    EXPECT_EQ(color, L"332211");
}
TEST(ImageColorTest, SetDisplayInputMemBareRawPointerFailsWithoutChangingCurrentInput) {
    op::Op op;
    long ret = 0;
    const int width = 8;
    const int height = 8;

    vector<uchar> pixels(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = static_cast<size_t>(y * width + x) * 4;
            pixels[idx + 0] = 0x01;
            pixels[idx + 1] = 0x02;
            pixels[idx + 2] = 0x03;
            pixels[idx + 3] = 0xff;
        }
    }
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring valid_mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(valid_mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    vector<uchar> raw_bgr(static_cast<size_t>(width) * height * 3, 0x7f);
    wstring invalid_mode = L"mem:" + PtrToWString(raw_bgr.data());
    op.SetDisplayInput(invalid_mode.c_str(), &ret);
    EXPECT_EQ(ret, 0) << "Bare raw pointers must include width/height/format metadata";

    wstring color;
    op.GetColor(1, 1, color);
    EXPECT_EQ(color, L"030201") << "Failed mem mode parsing should not clobber the previous display input";
}

// 回归：GdiCapture 每帧重算客户区偏移（修复 dx_/dy_ 绑定后不随 resize 刷新的边界 bug）。
// 绑定后改变窗口尺寸/边框，验证 resize 后截图不崩溃且仍返回有效颜色（旧逻辑会用陈旧边框宽度错位）。
TEST(ImageColorTest, GdiCaptureRefreshesClientOffsetAfterResize) {
    test_support::ColorPulseWindow window;
    ASSERT_TRUE(window.Create(false)) << "failed to create test window";

    op::Op op;
    long ret = 0;
    op.BindWindow((long)(intptr_t)window.hwnd, L"normal", L"windows", L"windows", 0, &ret);
    if (ret != 1)
        GTEST_SKIP() << "normal capture unavailable on current environment";

    // 基线：客户区坐标取色应成功。
    std::wstring color_before;
    op.GetColor(60, 60, color_before);
    ASSERT_EQ(color_before.length(), 6u) << "baseline GetColor should return 6 hex chars";

    // 改变窗口尺寸，触发此前绑定计算的边框宽度过时场景。
    ::SetWindowPos(window.hwnd, nullptr, 0, 0, 500, 400, SWP_NOMOVE | SWP_NOZORDER);
    // 让窗口过程处理 WM_SIZE，刷新客户区几何。
    MSG msg = {};
    for (int i = 0; i < 20 && ::PeekMessageW(&msg, window.hwnd, WM_SIZE, WM_SIZE, PM_REMOVE); ++i) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // resize 后再取色：每帧重算 dx_/dy_，应不崩溃且坐标有效。
    std::wstring color_after;
    op.GetColor(60, 60, color_after);
    EXPECT_EQ(color_after.length(), 6u) << "resize 后 normal 模式取色应仍成功（dx_/dy_ 每帧重算）";

    long unbind = 0;
    op.UnBindWindow(&unbind);
}

// 回归：绑定后把窗口放大到明显超过绑定时刻客户区尺寸，整窗 Capture 必须按新尺寸截取。
// 旧缺陷：GdiCapture 未 override refreshMetrics()，RectConvert 每次把请求区域钳到绑定时刻的
// get_width()/get_height() 上限 —— 绑定 220x180 后放大，整窗 Capture 只截到 220x180，
// 新区域内 GetColor 越界返回 000000（真机冒烟 smoke_real.py A 组暴露）。
TEST(ImageColorTest, GdiCaptureRefreshesFrameSizeAfterEnlarge) {
    test_support::ColorPulseWindow window;
    ASSERT_TRUE(window.Create(false)) << "failed to create test window";

    op::Op op;
    long ret = 0;
    op.BindWindow((long)(intptr_t)window.hwnd, L"normal", L"windows", L"windows", 0, &ret);
    if (ret != 1)
        GTEST_SKIP() << "normal capture unavailable on current environment";

    // 绑定时刻客户区尺寸。
    long w0 = 0, h0 = 0, ok0 = 0;
    op.GetClientSize((long)(intptr_t)window.hwnd, &w0, &h0, &ok0);
    ASSERT_EQ(ok0, 1);
    ASSERT_GT(w0, 0);
    ASSERT_GT(h0, 0);

    // 放大窗口（外框尺寸增量，客户区必然超出绑定时刻尺寸）。
    ::SetWindowPos(window.hwnd, nullptr, 0, 0, static_cast<int>(w0 + 200), static_cast<int>(h0 + 200),
                   SWP_NOMOVE | SWP_NOZORDER);
    MSG msg = {};
    for (int i = 0; i < 40 && ::PeekMessageW(&msg, window.hwnd, WM_SIZE, WM_SIZE, PM_REMOVE); ++i) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    // 强制重绘放大后新增区域（窗口过程 FillRect 全客户区，填当前色）。
    ::InvalidateRect(window.hwnd, nullptr, TRUE);
    ::UpdateWindow(window.hwnd);

    long w1 = 0, h1 = 0, ok1 = 0;
    op.GetClientSize((long)(intptr_t)window.hwnd, &w1, &h1, &ok1);
    ASSERT_EQ(ok1, 1);
    ASSERT_GT(w1, w0) << "test requires enlarged width beyond bind-time size";
    ASSERT_GT(h1, h0) << "test requires enlarged height beyond bind-time size";

    // 1) 放大后整窗 Capture 到文件：文件宽高必须等于新客户区尺寸（旧逻辑被钳成旧尺寸）。
    const std::wstring cap_file = test_support::GetTempBmpPath(L"op_gdi_enlarge_capture.bmp");
    DeleteFileW(cap_file.c_str());
    long cap_ret = 0;
    op.Capture(0, 0, w1, h1, cap_file.c_str(), &cap_ret);
    EXPECT_EQ(cap_ret, 1) << "enlarged whole-window capture should succeed";
    if (cap_ret == 1) {
        long fw = 0, fh = 0;
        FILE *f = nullptr;
        if (_wfopen_s(&f, cap_file.c_str(), L"rb") == 0 && f) {
            unsigned char hdr[26] = {0};
            fread(hdr, 1, sizeof(hdr), f);
            fclose(f);
            if (hdr[0] == 'B' && hdr[1] == 'M') {
                memcpy(&fw, hdr + 18, 4);
                memcpy(&fh, hdr + 22, 4);
            }
        }
        EXPECT_EQ(fw, w1) << "capture file width should follow enlarged client width";
        EXPECT_EQ(fh, h1) << "capture file height should follow enlarged client height";
        DeleteFileW(cap_file.c_str());
    }

    // 2) 新客户区内、绑定时刻尺寸之外的点取色应有效：旧逻辑越界返回黑/失败。
    std::wstring color_far;
    op.GetColor(w1 - 20, h1 - 20, color_far);
    EXPECT_EQ(color_far.length(), 6u) << "GetColor inside enlarged area should succeed";
    EXPECT_NE(color_far, L"000000") << "GetColor inside enlarged area should not be frame-miss black";

    long unbind = 0;
    op.UnBindWindow(&unbind);
}

// 回归：直接驱动 BindingSession::requestCapture 的 pic/mem 路径，验证改动 A 的越界防御。
// 走 op::Op 门面时 RectConvert 会在到达 requestCapture 前钳制越界坐标，故必须直连 requestCapture。
TEST(ImageColorTest, RequestCapturePicMemBoundsCheck) {
    const int width = 32;
    const int height = 32;
    vector<uchar> raw_bgra(static_cast<size_t>(width) * height * 4, 0xff);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = static_cast<size_t>(y * width + x) * 4;
            raw_bgra[idx + 0] = static_cast<uchar>(x);      // B = x
            raw_bgra[idx + 1] = static_cast<uchar>(y);      // G = y
            raw_bgra[idx + 2] = static_cast<uchar>(x ^ y);  // R = x^y
            raw_bgra[idx + 3] = 0xff;                       // A
        }
    }
    const wstring mode = L"mem:" + PtrToWString(raw_bgra.data(), true) + L",32,32,bgra";
    unsigned char px[4] = {};

    // 合法全幅子区：成功且拷到 (0,0) 像素。
    ASSERT_EQ(OpRequestCaptureForTest(mode.c_str(), 0, 0, 16, 16, px), 1);
    EXPECT_EQ(px[0], 0);
    EXPECT_EQ(px[1], 0);
    EXPECT_EQ(px[2], 0);
    EXPECT_EQ(px[3], 0xff);

    // 合法偏移子区 (4,8)：拷到 _pic 像素(4,8)，验证 x1/y1 偏移正确。
    ASSERT_EQ(OpRequestCaptureForTest(mode.c_str(), 4, 8, 4, 4, px), 1);
    EXPECT_EQ(px[0], 4);
    EXPECT_EQ(px[1], 8);
    EXPECT_EQ(px[2], static_cast<uchar>(4 ^ 8));
    EXPECT_EQ(px[3], 0xff);

    // 越界变体：全部应被防御检查拒绝（返回 0，不崩溃、不越界 memcpy）。
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), -1, 0, 16, 16, px), 0) << "x1<0 must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 0, -1, 16, 16, px), 0) << "y1<0 must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 0, 0, 40, 16, px), 0) << "x1+w>pw must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 0, 0, 16, 40, px), 0) << "y1+h>ph must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 20, 20, 16, 16, px), 0) << "20+16>32 overflow must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 0, 0, 0, 16, px), 0) << "w<=0 must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 0, 0, 16, 0, px), 0) << "h<=0 must be rejected";
    EXPECT_EQ(OpRequestCaptureForTest(mode.c_str(), 1000, 1000, 1, 1, px), 0) << "far OOB must be rejected";

    // mem_mode 设置失败应返回 <0（不触及 requestCapture）。
    EXPECT_LT(OpRequestCaptureForTest(L"garbage", 0, 0, 1, 1, px), 0) << "invalid mem_mode must fail setup";
}

// FindLineEx：在内存位图上画一条水平线，验证扩展版输出的“直线上点数”可作为可信度使用。
// FindLine 本身无阈值、即便图中没有直线也必定返回一条“最强”直线，调用方无从判断真伪；
// 扩展版把霍夫累加器峰值一并输出，本用例钉住该语义，且确认旧接口结果不被改变。
TEST(ImageColorTest, FindLineExReportsPointCountAsConfidence) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;

    // 全黑背景 + 第 8 行整行白色（长度 16 的水平线）
    auto pixels = MakePixels(width, height, 0x00, 0x00, 0x00);
    for (int x = 0; x < width; ++x)
        PaintPixel(pixels, width, x, 8, 0xff, 0xff, 0xff);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring line;
    long point_count = -1;
    op.FindLineEx(0, 0, width, height, L"ffffff", 1.0, line, &point_count);

    // 每个前景点对每个角度只投一票，累加器峰值不可能超过前景点数；16 点共线故峰值恰为 16。
    EXPECT_EQ(point_count, 16) << "point count must equal the number of collinear pixels";
    ASSERT_FALSE(line.empty()) << "line descriptor must not be empty";

    // 只做区间断言：90 度附近存在多个等票的累加器桶，具体落在哪个桶依赖浮点取整，
    // 断言精确字符串会让用例变脆。水平线对应法线角度约 90 度、到区域左上角距离约 8。
    const auto comma = line.find(L',');
    ASSERT_NE(comma, wstring::npos) << "line must be formatted as \"angle,distance\"";
    const long angle = stol(line.substr(0, comma));
    const long distance = stol(line.substr(comma + 1));
    EXPECT_GE(angle, 85);
    EXPECT_LE(angle, 95);
    EXPECT_GE(distance, 7);
    EXPECT_LE(distance, 8);

    // 回归保护：扩展版不得改变旧接口在相同入参下的结果。
    wstring legacy;
    op.FindLine(0, 0, width, height, L"ffffff", 1.0, legacy);
    EXPECT_EQ(legacy, line) << "FindLineEx must not change FindLine's existing result";
}

// 无任何匹配颜色的点时，点数必须为 0 —— 这正是调用方判定“本次结果不可信”的依据。
TEST(ImageColorTest, FindLineExReturnsZeroCountWhenNoForeground) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;

    auto pixels = MakePixels(width, height, 0x00, 0x00, 0x00);
    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring line;
    long point_count = -1;
    op.FindLineEx(0, 0, width, height, L"ffffff", 1.0, line, &point_count);

    EXPECT_EQ(point_count, 0) << "no foreground pixel must yield zero confidence";
}

// FindLineExS：min_points 阈值必须压制幻觉线——短线索产生的低峰值不再返回直线描述。
// （单个孤立点会被默认去噪直接删除，故用 3 像素短线验证阈值语义本身。）
TEST(ImageColorTest, FindLineExSThresholdSuppressesHallucination) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;

    // 全黑背景 + 3 像素水平短线：峰值 = 3，低于常规直线但非孤立点（去噪保留）
    auto pixels = MakePixels(width, height, 0x00, 0x00, 0x00);
    for (int x = 7; x <= 9; ++x)
        PaintPixel(pixels, width, x, 8, 0xff, 0xff, 0xff);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring line;
    long peak = -1;
    op.FindLineExS(0, 0, width, height, L"ffffff", 1.0, 5, line, &peak);
    EXPECT_EQ(peak, 3) << "3 collinear pixels vote at most 3 per accumulator bucket";
    EXPECT_TRUE(line.empty()) << "peak below min_points must yield empty line descriptor";

    // 阈值降到 3：同一画面应恢复返回直线，且峰值真实回传
    op.FindLineExS(0, 0, width, height, L"ffffff", 1.0, 3, line, &peak);
    EXPECT_EQ(peak, 3);
    ASSERT_FALSE(line.empty());
}

// FindLineExS：16 点共线场景下行为与 FindLineEx 一致（峰值 16），阈值 100 时置空。
TEST(ImageColorTest, FindLineExSMatchesFindLineExOnRealLine) {
    op::Op op;
    long ret = 0;
    const int width = 16;
    const int height = 16;

    auto pixels = MakePixels(width, height, 0x00, 0x00, 0x00);
    for (int x = 0; x < width; ++x)
        PaintPixel(pixels, width, x, 8, 0xff, 0xff, 0xff);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring line;
    long peak = -1;
    op.FindLineExS(0, 0, width, height, L"ffffff", 1.0, 0, line, &peak);
    EXPECT_EQ(peak, 16);
    ASSERT_FALSE(line.empty());

    op.FindLineExS(0, 0, width, height, L"ffffff", 1.0, 100, line, &peak);
    EXPECT_EQ(peak, 16) << "peak is still reported for calibration even below threshold";
    EXPECT_TRUE(line.empty());
}

// FindColorBlockExS：mode=0 与 FindColorBlockEx 完全一致（兼容）；mode=1 把重合窗口聚成单一块。
TEST(ImageColorTest, FindColorBlockExSClustersOverlappingWindows) {
    op::Op op;
    long ret = 0;
    const int width = 10;
    const int height = 10;

    // 黑色背景 + (1,1) 起 6x6 白色实心块；4x4 滑动窗口会产生 7x7=49 个重合命中
    auto pixels = MakePixels(width, height, 0x00, 0x00, 0x00);
    for (int y = 1; y <= 6; ++y)
        for (int x = 1; x <= 6; ++x)
            PaintPixel(pixels, width, x, y, 0xff, 0xff, 0xff);

    auto bmp = BuildBmp32TopDown(width, height, pixels);
    wstring mode = L"mem:" + PtrToWString(bmp.data());
    op.SetDisplayInput(mode.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    wstring raw_legacy;
    op.FindColorBlockEx(0, 0, width, height, L"ffffff", 1.0, 1, 4, 4, raw_legacy);

    wstring raw_new;
    op.FindColorBlockExS(0, 0, width, height, L"ffffff", 1.0, 1, 4, 4, 0, raw_new);
    EXPECT_EQ(raw_new, raw_legacy) << "mode=0 must be byte-identical to FindColorBlockEx";
    ASSERT_FALSE(raw_new.empty());

    wstring clustered;
    op.FindColorBlockExS(0, 0, width, height, L"ffffff", 1.0, 1, 4, 4, 1, clustered);
    EXPECT_EQ(clustered, L"0,0") << "49 overlapping windows must merge into a single block origin";
}
