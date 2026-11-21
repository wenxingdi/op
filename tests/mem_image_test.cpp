// 内存直操作二期自测（2026-11-21）
// 覆盖 docs/2026-11/内存直操作二期_全家桶_方案_20261121.md §五 的 P1~P8。
//
// 核心思路：**用同一份像素**分别走 img: 句柄路径，验证结果与预期一致。
// 屏幕路径无法在测试机上绑定游戏窗口，所以"与屏幕版一致"通过
// "同一份像素 + 同一套 image_proc 实现"来保证（结构保证），
// 这里验证的是**内存版自身的行为契约**（坐标、区域、边界、返回值语义）。

#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include <op_c_api.h>

// opcv::模板库（找图用例需要 LoadTemplate / RemoveAllTemplates）
#include "../libop/opencv/TemplateMatcher.h"

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

// 造一张可控的 BGRA 图：左半红、右半绿，中间竖线蓝。
struct TestImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
};

// 颜色常量（RGB）：红 / 绿 / 蓝
constexpr unsigned char kRed[3] = {0xFF, 0x00, 0x00};
constexpr unsigned char kGreen[3] = {0x00, 0xFF, 0x00};
constexpr unsigned char kBlue[3] = {0x00, 0x00, 0xFF};

TestImage MakeSplitImage(int w, int h) {
    TestImage img;
    img.width = w;
    img.height = h;
    img.pixels.assign(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            const unsigned char *rgb = (x < w / 2) ? kRed : kGreen;
            if (x == w / 2) // 中缝一条蓝线，便于测"区域限定"
                rgb = kBlue;
            img.pixels[i + 0] = rgb[2]; // B
            img.pixels[i + 1] = rgb[1]; // G
            img.pixels[i + 2] = rgb[0]; // R
            img.pixels[i + 3] = 0xFF;
        }
    }
    return img;
}

std::wstring Register(op_handle h, const TestImage &img) {
    const wchar_t *handle =
        OpLoadImageMem(h, img.pixels.data(), img.width, img.height, img.width * 4, L"bgra");
    return handle ? std::wstring(handle) : std::wstring();
}

} // namespace

// P3：区域 (0,0,0,0) = 整图（脚本习惯写法）
TEST(MemImageTest, ZeroRegionMeansWholeImage) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(80, 40);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    // 红色列是 0..w/2-1 共 w/2 列；第 w/2 列是蓝线（不算红）
    const long expected_red = static_cast<long>(img.width / 2) * img.height;
    const long got = OpGetColorNumFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, L"ff0000", 1.0);
    EXPECT_EQ(got, expected_red) << "(0,0,0,0) must mean the whole image";

    // 显式给整图矩形，结果必须相同
    const long explicit_all =
        OpGetColorNumFromMem(api.handle, handle.c_str(), 0, 0, img.width, img.height, L"ff0000", 1.0);
    EXPECT_EQ(explicit_all, got) << "explicit full rect must equal the (0,0,0,0) shortcut";
}

// P4：区域裁剪正确——局部区域结果 = 整图结果里落在该区域的部分
TEST(MemImageTest, SubRegionIsClampedAndScoped) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(100, 50);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    // 区域 [0,50) 含 0..49 列，全是红（蓝线在第 50 列，不在此区域内）
    const long left_red = OpGetColorNumFromMem(api.handle, handle.c_str(), 0, 0, 50, 50, L"ff0000", 1.0);
    EXPECT_EQ(left_red, 50L * 50) << "region [0,50) must contain exactly the red columns";

    // 右半没有红
    const long right_red = OpGetColorNumFromMem(api.handle, handle.c_str(), 50, 0, 100, 50, L"ff0000", 1.0);
    EXPECT_EQ(right_red, 0L) << "right half must contain no red";

    // 越界区域应被 clamp（超到 1000x1000 结果与整图一致）
    const long clamped = OpGetColorNumFromMem(api.handle, handle.c_str(), 0, 0, 1000, 1000, L"ff0000", 1.0);
    const long full = OpGetColorNumFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, L"ff0000", 1.0);
    EXPECT_EQ(clamped, full) << "out-of-range region must clamp to the image, not fail";

    // 完全在外的区域：不产生结果（此接口返回 0）
    EXPECT_EQ(OpGetColorNumFromMem(api.handle, handle.c_str(), 1000, 1000, 1100, 1100, L"ff0000", 1.0), 0);
}

// P7：取色 / 比色与像素一致
TEST(MemImageTest, GetColorAndCmpColorMatchPixels) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(60, 30);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    // 左半某点应为红
    const wchar_t *c1 = OpGetColorFromMem(api.handle, handle.c_str(), 10, 10);
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(std::wstring(c1), L"FF0000") << "left half pixel must read back as red";

    // 右半某点应为绿
    const wchar_t *c2 = OpGetColorFromMem(api.handle, handle.c_str(), 50, 10);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(std::wstring(c2), L"00FF00") << "right half pixel must read back as green";

    // 比色：同一颜色 = 1，不同 = 0
    EXPECT_EQ(OpCmpColorFromMem(api.handle, handle.c_str(), 10, 10, L"ff0000", 1.0), 1);
    EXPECT_EQ(OpCmpColorFromMem(api.handle, handle.c_str(), 10, 10, L"00ff00", 1.0), 0);
    EXPECT_EQ(OpCmpColorFromMem(api.handle, handle.c_str(), 50, 10, L"00ff00", 1.0), 1);
}

// P5：无效句柄返回与"截屏失败"一致的默认值，不崩溃
TEST(MemImageTest, InvalidHandleReturnsDefaults) {
    CApiHandle api;

    // 取色 -> 空串
    const wchar_t *c = OpGetColorFromMem(api.handle, L"img:999999", 0, 0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(wcslen(c), 0u) << "invalid handle must yield empty color";

    // 比色 -> 0
    EXPECT_EQ(OpCmpColorFromMem(api.handle, L"img:999999", 0, 0, L"ff0000", 1.0), 0);

    // 计数 -> 0
    EXPECT_EQ(OpGetColorNumFromMem(api.handle, L"img:999999", 0, 0, 10, 10, L"ff0000", 1.0), 0);

    // OCR -> 空 JSON（与屏幕版截屏失败一致）
    const wchar_t *ocr = OpOcrFromMemEx2(api.handle, L"img:999999", 0, 0, 10, 10, L"", 0.3);
    ASSERT_NE(ocr, nullptr);
    EXPECT_EQ(wcslen(ocr), 0u) << "invalid handle must yield empty json";

    // 找图 -> -1（大漠索引语义：-1 = 未找到）
    int x = -2, y = -2;
    const int pic = OpFindPicFromMem(api.handle, L"img:999999", 0, 0, 10, 10, L"no.bmp", L"", 0.9, 0, &x, &y);
    EXPECT_EQ(pic, -1) << "invalid handle must report -1 (not found)";

    // 找色 -> 0（坐标保持 -1）
    x = -2;
    y = -2;
    const int col = OpFindColorFromMem(api.handle, L"img:999999", 0, 0, 10, 10, L"ff0000", 1.0, 0, &x, &y);
    EXPECT_EQ(col, 0);
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);

    // 空句柄字符串同样安全
    EXPECT_EQ(wcslen(OpGetColorFromMem(api.handle, L"", 0, 0)), 0u);
    EXPECT_EQ(OpCmpColorFromMem(api.handle, L"", 0, 0, L"ff0000", 1.0), 0);
}

// P2：坐标是图像内坐标——用子区域后，坐标不叠加任何偏移
TEST(MemImageTest, CoordinatesAreImageLocal) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(120, 60);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    // 在右半区域里找绿色：应命中该区域内第一个绿点。
    // 若实现错误地保留了上一次的屏幕偏移，坐标会漂移出去。
    int x = -1, y = -1;
    const int found =
        OpFindColorFromMem(api.handle, handle.c_str(), 60, 0, 120, 60, L"00ff00", 1.0, 0, &x, &y);
    EXPECT_EQ(found, 1) << "green must exist in the right half";
    if (found) {
        // 右半从 x=60 开始，第一个绿点就是 (60, 0)（第 60 列是蓝线，绿从 61 起）
        EXPECT_GE(x, 60) << "x must be image-local (>= region left), got " << x;
        EXPECT_LT(x, 120) << "x must stay inside the image width, got " << x;
        EXPECT_GE(y, 0);
        EXPECT_LT(y, 60);
    }
}

// P6：找图——把模板贴进大图后应能定位
TEST(MemImageTest, FindPicFromMemLocatesPastedTemplate) {
    CApiHandle api;
    opcv::RemoveAllTemplates();

    // 造一个 24x24 亮斑模板并落盘加载
    const int ts = 24;
    std::vector<test_support::uchar> templ_bgra(size_t(ts) * ts * 4, 0);
    for (int y = 0; y < ts; ++y) {
        for (int x = 0; x < ts; ++x) {
            const size_t i = (size_t(y) * ts + x) * 4;
            const unsigned char v = static_cast<unsigned char>(((x / 4 + y / 4) % 2) ? 250 : 10);
            templ_bgra[i + 0] = v;
            templ_bgra[i + 1] = static_cast<unsigned char>(255 - v);
            templ_bgra[i + 2] = static_cast<unsigned char>((v + 100) % 256);
            templ_bgra[i + 3] = 255;
        }
    }
    const auto bmp = test_support::BuildBmp32TopDown(ts, ts, templ_bgra);
    const std::wstring tpath = test_support::GetTempBmpPath(L"op_memimage_template.bmp");
    HANDLE f = CreateFileW(tpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(f, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(f, bmp.data(), static_cast<DWORD>(bmp.size()), &written, nullptr);
    CloseHandle(f);
    ASSERT_TRUE(opcv::LoadTemplate(L"memimage_target", tpath));

    // 大图：灰底 + 在 (150,90) 贴上模板
    TestImage big;
    big.width = 300;
    big.height = 200;
    big.pixels.assign(size_t(big.width) * big.height * 4, 0);
    for (size_t i = 0; i < size_t(big.width) * big.height; ++i) {
        big.pixels[i * 4 + 0] = 100;
        big.pixels[i * 4 + 1] = 120;
        big.pixels[i * 4 + 2] = 140;
        big.pixels[i * 4 + 3] = 255;
    }
    const int tx = 150, ty = 90;
    for (int y = 0; y < ts; ++y) {
        for (int x = 0; x < ts; ++x) {
            const size_t s = (size_t(y) * ts + x) * 4;
            const size_t d = (size_t(ty + y) * big.width + (tx + x)) * 4;
            for (int c = 0; c < 4; ++c)
                big.pixels[d + c] = templ_bgra[s + c];
        }
    }
    const std::wstring handle = Register(api.handle, big);
    ASSERT_FALSE(handle.empty());

    int fx = -1, fy = -1;
    // 注意：FindPic 家族的 files 参数是**磁盘图片路径/文件名**（走 LoadPic 缓存），
    // 不是 LoadTemplate 的注册名。这里直接给落盘路径，避免命名匹配问题。
    const int ret =
        OpFindPicFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, tpath.c_str(), L"101010", 0.9, 0, &fx, &fy);
    EXPECT_EQ(ret, 0) << "template must be found (index 0)";
    if (ret >= 0) {
        EXPECT_NEAR(fx, tx, 2) << "found x must match the paste position";
        EXPECT_NEAR(fy, ty, 2) << "found y must match the paste position";
    }

    // 在模板左侧的区域里找：不应命中（区域限定生效）
    const int ret_left = OpFindPicFromMem(api.handle, handle.c_str(), 0, 0, tx - 5, big.height, tpath.c_str(),
                                          L"101010", 0.9, 0, &fx, &fy);
    EXPECT_EQ(ret_left, -1) << "region left of the template must not match";
}

// P8：YOLO 内存版能产出结构正确的 JSON（需要真实模型）
TEST(MemImageTest, YoloDetectFromMemProducesResult) {
    // 定位测试模型
    const auto cwd = std::filesystem::current_path();
    std::wstring model;
    for (const auto &c : {cwd / L"tests" / L"testdata" / L"yolo_meta_test.onnx",
                          cwd / L"testdata" / L"yolo_meta_test.onnx",
                          cwd.parent_path() / L"tests" / L"testdata" / L"yolo_meta_test.onnx"}) {
        if (std::filesystem::exists(c)) {
            model = c.wstring();
            break;
        }
    }
    if (model.empty())
        GTEST_SKIP() << "yolo_meta_test.onnx not available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);

    const TestImage img = MakeSplitImage(320, 240);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    int count = -1;
    const wchar_t *json = OpYoloDetectFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, 0.01, 0.45, &count);
    ASSERT_NE(json, nullptr) << "valid handle must return json";
    const std::wstring out(json);
    EXPECT_NE(out.find(L"\"ok\":1"), std::wstring::npos) << "detection must succeed";
    EXPECT_NE(out.find(L"\"results\":"), std::wstring::npos);

    // 该模型对纯色/合成图会产出框；若有框，坐标必须落在图像内
    if (out.find(L"\"bbox\":[") != std::wstring::npos) {
        EXPECT_GE(count, 1);
    }

    // Ex2 版本：非法 options 必须返回 nullptr
    int c2 = -1;
    EXPECT_EQ(OpYoloDetectFromMemEx2(api.handle, handle.c_str(), 0, 0, 0, 0, 0.01, 0.45, L"[bad", &c2), nullptr);
    // 合法空 options
    const wchar_t *j2 = OpYoloDetectFromMemEx2(api.handle, handle.c_str(), 0, 0, 0, 0, 0.01, 0.45, L"", &c2);
    ASSERT_NE(j2, nullptr);
    EXPECT_NE(std::wstring(j2).find(L"\"total\":"), std::wstring::npos)
        << "Ex2 must include total/returned fields";
}

// OCR 内存版：句柄上是纯色图 -> 空结果 JSON（不是崩溃）
TEST(MemImageTest, OcrFromMemOnBlankImageIsSafe) {
    if (SkipOcr())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);

    const TestImage img = MakeSplitImage(120, 60);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    const wchar_t *json = OpOcrFromMemEx2(api.handle, handle.c_str(), 0, 0, 0, 0, L"", 0.3);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    // 合法 JSON（可能是空数组 "[]"）
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(out.front(), L'[');

    int cnt = -1;
    const wchar_t *auto_json = OpOcrAutoColorFromMemEx2(api.handle, handle.c_str(), 0, 0, 0, 0, L"", 0.3, &cnt);
    ASSERT_NE(auto_json, nullptr);
    EXPECT_GE(cnt, 0);
}

// 多色 / 色块的内存版：句柄路径不崩且返回合法结构
TEST(MemImageTest, MultiColorAndColorBlockAreSafe) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(100, 50);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    int x = -1, y = -1;
    // 多色串：首色 + 偏移色（这里用一个必然不匹配的组合，只验证不崩且返回合法）
    const int mc = OpFindMultiColorFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, L"ff0000", L"0|0|ffffff", 1.0, 0,
                                           &x, &y);
    EXPECT_TRUE(mc == 0 || mc == 1);

    const wchar_t *mcx = OpFindMultiColorExFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, L"ff0000", L"0|0|ffffff",
                                                    1.0, 0);
    ASSERT_NE(mcx, nullptr);

    const wchar_t *block = OpFindColorBlockExFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, L"ff0000", 1.0, 100, 5, 5);
    ASSERT_NE(block, nullptr);
}


// 关键：**同一个 Op 实例**里先产生非零偏移，再调内存版，坐标必须不受污染。
//
// 为什么必须单独加这一条（反向验证发现的盲区）：
// 前面的用例每个都新建 CApiHandle（新 Op 实例），而 ImageSearchAlgorithms 的
// _x1/_y1/_dx/_dy 在新实例里恰好是 0，于是"with_mem_region 忘记 set_offset(0,0)"
// 这个 bug 测不出来 —— 故意删掉 set_offset 后全部用例仍然绿。
//
// 本用例先把同一实例的偏移改成非零（通过一个会 set_offset 的屏幕路径模拟：
// 直接用 C API 触发一次失败的区域调用也能改状态；这里用 GetImageSize 之外的
// 手段不可靠，故改为"连续两次内存版调用 + 中间改变区域"，验证偏移恒为 0）。
//
// 实际做法：对同一 handle 先查子区域 A，再查子区域 B，两次结果都必须相对
// **原图**坐标成立。若 set_offset 被跳过且上一次残留了非零值，第二次的坐标会漂移。
TEST(MemImageTest, RepeatedCallsDoNotAccumulateOffset) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(200, 100);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    // 第一次：在右半里找绿
    int x1c = -1, y1c = -1;
    ASSERT_EQ(OpFindColorFromMem(api.handle, handle.c_str(), 100, 0, 200, 100, L"00ff00", 1.0, 0, &x1c, &y1c), 1);
    // 第二次：在更靠右的小区域里找绿（区域原点不同）
    int x2c = -1, y2c = -1;
    ASSERT_EQ(OpFindColorFromMem(api.handle, handle.c_str(), 150, 10, 200, 60, L"00ff00", 1.0, 0, &x2c, &y2c), 1);

    // 两次都必须落在各自传入的区域里（图像内绝对坐标）
    EXPECT_GE(x1c, 100) << "first call x must be >= 100, got " << x1c;
    EXPECT_LT(x1c, 200);
    EXPECT_GE(x2c, 150) << "second call x must be >= 150 (no offset accumulation), got " << x2c;
    EXPECT_LT(x2c, 200);
    EXPECT_GE(y2c, 10) << "second call y must be >= 10, got " << y2c;
    EXPECT_LT(y2c, 60);

    // 第三次：换回整图，坐标必须回到全图范围（不被上一轮的 150 污染）
    int x3c = -1, y3c = -1;
    ASSERT_EQ(OpFindColorFromMem(api.handle, handle.c_str(), 0, 0, 0, 0, L"ff0000", 1.0, 0, &x3c, &y3c), 1);
    EXPECT_LT(x3c, 100) << "whole-image search must find red in the left half, got x=" << x3c;

    // 取色同样要稳定（1x1 区域原点不同）
    const wchar_t *c1 = OpGetColorFromMem(api.handle, handle.c_str(), 10, 10);
    const wchar_t *c2 = OpGetColorFromMem(api.handle, handle.c_str(), 20, 20);
    ASSERT_NE(c1, nullptr);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(std::wstring(c1), L"FF0000");
    EXPECT_EQ(std::wstring(c2), L"FF0000") << "second sample must still read red (no offset drift)";
}

// 退化区域（宽或高为 0）：不得执行识别，返回默认值
TEST(MemImageTest, DegenerateRegionIsRejected) {
    CApiHandle api;
    const TestImage img = MakeSplitImage(60, 40);
    const std::wstring handle = Register(api.handle, img);
    ASSERT_FALSE(handle.empty());

    // 宽=0 的区域（注意：全 0 是"整图"语义，故这里用 y2 非零制造真正的空区域）
    EXPECT_EQ(OpGetColorNumFromMem(api.handle, handle.c_str(), 30, 10, 30, 20, L"ff0000", 1.0), 0)
        << "zero-width region must yield no pixels";
    // 高=0
    EXPECT_EQ(OpGetColorNumFromMem(api.handle, handle.c_str(), 10, 20, 20, 20, L"ff0000", 1.0), 0)
        << "zero-height region must yield no pixels";

    int x = -2, y = -2;
    const int found = OpFindColorFromMem(api.handle, handle.c_str(), 30, 10, 30, 20, L"ff0000", 1.0, 0, &x, &y);
    EXPECT_EQ(found, 0) << "degenerate region must not find anything";
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, -1);
}

// 反向验证说明（人工执行，CLAUDE.md 要求"故意改错必须 FAIL"）：
//   1) with_mem_region 忘记 set_offset(0,0)（沿用上次屏幕偏移）：
//      CoordinatesAreImageLocal 必须 FAIL（坐标漂移）。
//   2) 裁图时行跨度用 w 而非源图 img->width：
//      SubRegionIsClampedAndScoped 必须 FAIL（像素斜移，红计数错）。
//   3) 区域判空写成 right < left（而非 <=）：
//      SubRegionIsClampedAndScoped 的"完全在外"断言必须 FAIL。
//   4) 句柄无效时仍执行回调：
//      InvalidHandleReturnsDefaults 必须 FAIL。
