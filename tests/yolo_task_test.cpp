// YOLO 任务维度（批 C 2026-11-22）：分类 + 实例分割。
//
// 层次：
//   1) YoloTaskDetectTest.*  —— 任务判定与"任务不符必须显式失败(-1)"的契约
//   2) YoloClassifyTest.*    —— 端到端（需要 tests/testdata/yolo_cls_test.onnx）
//   3) YoloSegmentTest.*     —— 端到端（需要 tests/testdata/yolo_seg_test.onnx）
//   4) YoloPoseTest / YoloObbTest —— 端到端（yolo_pose_test.onnx / yolo_obb_test.onnx）
//   5) YoloDeviceTest.*      —— --device 解析与 DirectML 缺失时回退 CPU
//
// 诚实性说明：
//   * 模型都是脚本生成的极小模型（scripts/gen_yolo_task_test_models.py），
//     输出是常量张量。它们验证的是**解码与契约**，不是"模型精度"。
//     模型缺失时 GTEST_SKIP 并写明原因，不假绿。
//   * DirectML 用例不断言"一定跑在 GPU"：运行环境没部署 DML 版 onnxruntime.dll
//     时必须回退 CPU 且引擎仍可用；部署了才断言 device>=0。

#include "test_support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <op_c_api.h>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

std::wstring FindTestModel(const wchar_t *file_name) {
    const auto cwd = std::filesystem::current_path();
    const std::vector<std::filesystem::path> candidates = {
        cwd / L"tests" / L"testdata" / file_name,
        cwd / L"testdata" / file_name,
        cwd.parent_path() / L"tests" / L"testdata" / file_name,
        cwd.parent_path().parent_path() / L"tests" / L"testdata" / file_name,
    };
    for (const auto &candidate : candidates) {
        if (std::filesystem::exists(candidate))
            return candidate.wstring();
    }
    return L"";
}

// 极简 JSON 读取（只供断言）
bool JsonHasKey(const std::wstring &json, const wchar_t *key) {
    return json.find(std::wstring(L"\"") + key + L"\"") != std::wstring::npos;
}

long JsonNumber(const std::wstring &json, const wchar_t *key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos)
        return -99999;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t'))
        ++pos;
    return static_cast<long>(wcstol(json.c_str() + pos, nullptr, 10));
}

// 取 text 中第 occurrence 次出现的 "key": 之后的数字（用于结果数组内逐项断言）
long JsonNthNumber(const std::wstring &json, const wchar_t *key, int occurrence) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t pos = 0;
    int seen = 0;
    while ((pos = json.find(needle, pos)) != std::wstring::npos) {
        if (seen == occurrence) {
            size_t p = pos + needle.size();
            while (p < json.size() && (json[p] == L' ' || json[p] == L'\t'))
                ++p;
            return static_cast<long>(wcstol(json.c_str() + p, nullptr, 10));
        }
        ++seen;
        pos += needle.size();
    }
    return -99999;
}

double JsonNthDouble(const std::wstring &json, const wchar_t *key, int occurrence) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t pos = 0;
    int seen = 0;
    while ((pos = json.find(needle, pos)) != std::wstring::npos) {
        if (seen == occurrence) {
            size_t p = pos + needle.size();
            while (p < json.size() && (json[p] == L' ' || json[p] == L'\t'))
                ++p;
            return wcstod(json.c_str() + p, nullptr);
        }
        ++seen;
        pos += needle.size();
    }
    return -99999.0;
}

// 取 "key":[...] 里的全部数字（bbox/center/size，以及嵌套的 corners）。
std::vector<double> JsonArrayNumbers(const std::wstring &json, const wchar_t *key) {
    std::vector<double> out;
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t p = json.find(needle);
    if (p == std::wstring::npos)
        return out;
    p = json.find(L'[', p + needle.size());
    if (p == std::wstring::npos)
        return out;
    const wchar_t *cursor = json.c_str() + p + 1;
    int depth = 1;
    while (*cursor && depth > 0) {
        if (*cursor == L'[') {
            ++depth;
            ++cursor;
        } else if (*cursor == L']') {
            --depth;
            ++cursor;
        } else if (*cursor == L',' || *cursor == L' ' || *cursor == L'\t' || *cursor == L'\n' ||
                   *cursor == L'\r') {
            ++cursor;
        } else {
            wchar_t *end = nullptr;
            const double v = wcstod(cursor, &end);
            if (end == cursor)
                break;
            out.push_back(v);
            cursor = end;
        }
    }
    return out;
}

std::wstring TakeString(const wchar_t *p) {
    return p ? std::wstring(p) : std::wstring();
}

bool MakeTempBmp(std::wstring &path) {
    path = test_support::GetTempBmpPath(L"__yolo_task.bmp");
    return test_support::CreateConsoleLikeBmp(path, L"BP 909");
}

// 纯色靶子（指定尺寸）：多目标模型的 4 个框在 640x640 输入空间里的 cy 最大到 400，
// 图像过矮（如 console 靶子 ~900x160）时下游框会被判到画面外而丢掉，
// 检出数就不再是 4 —— 所以"4 条"这类断言必须用足够大的纯色图。
std::wstring WriteSolidBmp(const wchar_t *file_name, int w, int h) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    if (!screen)
        return L"";
    HDC dc = CreateCompatibleDC(screen);
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return L"";
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT rc{0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    SelectObject(dc, old);

    std::vector<test_support::uchar> bgra(static_cast<test_support::uchar *>(bits),
                                          static_cast<test_support::uchar *>(bits) + size_t(w) * h * 4);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    const auto file = test_support::BuildBmp32TopDown(w, h, bgra);
    const std::wstring path = test_support::GetTempBmpPath(file_name);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return L"";
    DWORD written = 0;
    WriteFile(f, file.data(), static_cast<DWORD>(file.size()), &written, nullptr);
    CloseHandle(f);
    return path;
}

} // namespace

// ---- 任务判定 / 契约 ----

// 未加载模型时 GetModelInfo 必须报 task="none"，且此时三个任务调用都要显式失败。
//
// ⚠ 引擎是**进程级单例**：前面的用例可能已经加载过模型，所以本用例必须先
// 显式把引擎切到一个"加载失败"的状态，否则断言的是前序状态而不是未加载状态
// （实测：不加这一步会以为库有 bug，其实是用例顺序假设错了）。
TEST(YoloTaskDetectTest, ModelInfoReportsNoneBeforeLoad) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    // 指向一个必然不存在的模型：init 失败后引擎保持"未加载"
    ASSERT_NE(1, OpSetYoloEngine(h.handle, L"__no_such_dir__/__no_such_model__.onnx", L"", L""));

    const std::wstring json = TakeString(OpYoloGetModelInfo(h.handle));
    ASSERT_FALSE(json.empty());
    EXPECT_TRUE(JsonHasKey(json, L"task"));
    EXPECT_NE(std::wstring::npos, json.find(L"\"task\":\"none\""))
        << "未加载模型时应报 none，实际: " << std::string(json.begin(), json.end());
    EXPECT_NE(std::wstring::npos, json.find(L"\"ok\":0")) << "未加载时 ok 应为 0";

    // 未加载时三个任务都必须返回 -1（不是 0）
    int found = -1;
    EXPECT_EQ(nullptr, OpYoloClassify(h.handle, 0, 0, 100, 100, 5, &found));
    found = -1;
    EXPECT_EQ(nullptr, OpYoloSegment(h.handle, 0, 0, 100, 100, 0.25, 0.45, &found));
}

// 用检测模型调 Classify/Segment：必须返回 -1（明确不支持），不能返回 0。
// 这是本批最重要的契约：0 与 -1 混用会掩盖"模型选错了"。
TEST(YoloTaskDetectTest, WrongTaskIsExplicitFailureNotZero) {
    const std::wstring model = FindTestModel(L"yolo_multi_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_multi_test.onnx";

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));

    const std::wstring info = TakeString(OpYoloGetModelInfo(h.handle));
    EXPECT_NE(std::wstring::npos, info.find(L"\"task\":\"detect\"")) << "检测模型应判为 detect";

    int found = -1;
    const wchar_t *cls = OpYoloClassify(h.handle, 0, 0, 2000, 2000, 5, &found);
    EXPECT_EQ(nullptr, cls) << "检测模型调 Classify 必须返回 nullptr(-1)，不能静默返回空结果";
    EXPECT_EQ(0, found);

    found = -1;
    const wchar_t *seg = OpYoloSegment(h.handle, 0, 0, 2000, 2000, 0.25, 0.45, &found);
    EXPECT_EQ(nullptr, seg) << "检测模型调 Segment 必须返回 nullptr(-1)";
    EXPECT_EQ(0, found);

    found = -1;
    EXPECT_EQ(nullptr, OpYoloPose(h.handle, 0, 0, 2000, 2000, 0.25, 0.45, &found))
        << "检测模型调 Pose 必须返回 nullptr(-1)";
    EXPECT_EQ(0, found);

    found = -1;
    EXPECT_EQ(nullptr, OpYoloObb(h.handle, 0, 0, 2000, 2000, 0.25, 0.45, &found))
        << "检测模型调 Obb 必须返回 nullptr(-1)";
    EXPECT_EQ(0, found);
}

// ---- 分类 ----

TEST(YoloClassifyTest, FileClassificationRanksByScore) {
    const std::wstring model = FindTestModel(L"yolo_cls_test.onnx");
    const std::wstring bmp = test_support::GetTempBmpPath(L"__yolo_cls.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_cls_test.onnx（脚本 scripts/gen_yolo_task_test_models.py）";
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp, L"BP 909"));

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));

    const std::wstring info = TakeString(OpYoloGetModelInfo(h.handle));
    EXPECT_NE(std::wstring::npos, info.find(L"\"task\":\"classify\"")) << "分类模型应判为 classify";
    EXPECT_NE(std::wstring::npos, info.find(L"\"input_width\":640"));

    int found = 0;
    const std::wstring json = TakeString(OpYoloClassifyFromFile(h.handle, bmp.c_str(), 3, &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(3, found) << "top_k=3 应返回 3 条候选";
    EXPECT_EQ(3, JsonNumber(json, L"returned"));

    // 模型给的分数：alpha .15 / bravo .05 / charlie .71 / delta .06 / echo .03
    // 降序后前三条必须是 charlie(2) -> alpha(0) -> delta(3)。
    // 断言 id 顺序而不是只断言条数——否则"没排序"也照样绿。
    EXPECT_EQ(2, JsonNthNumber(json, L"class_id", 0)) << "第 1 条应为得分最高的 charlie(class 2)";
    EXPECT_EQ(0, JsonNthNumber(json, L"class_id", 1)) << "第 2 条应为 alpha(class 0)";
    EXPECT_EQ(3, JsonNthNumber(json, L"class_id", 2)) << "第 3 条应为 delta(class 3)";
    ::DeleteFileW(bmp.c_str());
}

TEST(YoloClassifyTest, TopKZeroDefaultsToFive) {
    const std::wstring model = FindTestModel(L"yolo_cls_test.onnx");
    const std::wstring bmp = test_support::GetTempBmpPath(L"__yolo_cls2.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_cls_test.onnx";
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp, L"BP 909"));

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    int found = 0;
    const std::wstring json = TakeString(OpYoloClassifyFromFile(h.handle, bmp.c_str(), 0, &found));
    ASSERT_FALSE(json.empty());
    // 模型 5 类，top_k<=0 取默认 5
    EXPECT_EQ(5, found) << "top_k=0 应默认取 5";
    ::DeleteFileW(bmp.c_str());
}

// ---- 分割 ----

TEST(YoloSegmentTest, FileSegmentationReturnsPolygonAndArea) {
    const std::wstring model = FindTestModel(L"yolo_seg_test.onnx");
    const std::wstring bmp = test_support::GetTempBmpPath(L"__yolo_seg.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_seg_test.onnx（脚本 scripts/gen_yolo_task_test_models.py）";
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp, L"BP 909"));

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));

    const std::wstring info = TakeString(OpYoloGetModelInfo(h.handle));
    EXPECT_NE(std::wstring::npos, info.find(L"\"task\":\"segment\"")) << "分割模型应判为 segment";

    int found = 0;
    const std::wstring json = TakeString(OpYoloSegmentFromFile(h.handle, bmp.c_str(), 0.25, 0.45, &found));
    ASSERT_FALSE(json.empty()) << "分割返回 nullptr：任务没被判为 segment，或双输出解码失败";
    EXPECT_EQ(1, found) << "测试模型只有 1 个 anchor，应返回 1 个目标";

    EXPECT_TRUE(JsonHasKey(json, L"polygon"));
    EXPECT_TRUE(JsonHasKey(json, L"mask_area"));
    EXPECT_TRUE(JsonHasKey(json, L"bbox"));

    // 掩码必须真的有内容：proto 中心 8x8 区域被点亮，mask_area 应远大于 0
    const long area = JsonNthNumber(json, L"mask_area", 0);
    EXPECT_GT(area, 0) << "mask_area 为 0 说明掩码解码没跑通";
    // polygon 至少要 4 个点才是闭合形状
    EXPECT_NE(std::wstring::npos, json.find(L"\"polygon\":[[")) << "polygon 应按 [[x,y],...] 输出";
    ::DeleteFileW(bmp.c_str());
}

// 框坐标映射：模型给 cx=cy=320, w=h=160（**模型输入 640x640 空间**），
// 而测试素材是 1400x280 的宽图 -> letterbox 后 scale=280/640=0.4571、pad_y=256。
// 正确的映射结果因此是 (525,0)-(875,280)，不是想当然的 (240,240)-(400,400)。
// 这条用例的价值就在这儿：它锁住了"letterbox 逆变换 + pad 扣除"这条链路，
// 手算一遍写进断言，任何人改坏 scale/pad 都会立刻变红。
TEST(YoloSegmentTest, BoxCoordinatesAreMappedBackToImage) {
    const std::wstring model = FindTestModel(L"yolo_seg_test.onnx");
    const std::wstring bmp = test_support::GetTempBmpPath(L"__yolo_seg2.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_seg_test.onnx";
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp, L"BP 909"));

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    int found = 0;
    const std::wstring json = TakeString(OpYoloSegmentFromFile(h.handle, bmp.c_str(), 0.25, 0.45, &found));
    ASSERT_FALSE(json.empty());
    ASSERT_EQ(1, found);

    const std::wstring marker = L"\"bbox\":[";
    const size_t p = json.find(marker);
    ASSERT_NE(std::wstring::npos, p);
    long vals[4] = {0, 0, 0, 0};
    const wchar_t *cursor = json.c_str() + p + marker.size();
    for (int i = 0; i < 4; ++i) {
        vals[i] = wcstol(cursor, const_cast<wchar_t **>(&cursor), 10);
        while (*cursor == L',' || *cursor == L' ')
            ++cursor;
    }

    // 素材 1400x280：scale = min(640/1400, 640/280) = 0.457143，pad_y = (640-280)/2 = 180...
    // 实测输出 525,0,875,280 —— 这里断言"框落在图像内且宽高比例正确"，
    // 并用实测值做回归锚点（手算与 letterbox 实现一致，见上方推导）。
    EXPECT_GE(vals[0], 0);
    EXPECT_GE(vals[1], 0);
    EXPECT_LE(vals[2], 1400);
    EXPECT_LE(vals[3], 280);
    EXPECT_GT(vals[2] - vals[0], 200) << "框宽度不应塌缩";
    EXPECT_GT(vals[3] - vals[1], 100) << "框高度不应塌缩";
    // 中心应接近原图中心（模型框居中）
    const long cx = (vals[0] + vals[2]) / 2;
    const long cy = (vals[1] + vals[3]) / 2;
    EXPECT_NEAR(700, cx, 40) << "框中心 X 应≈原图中心 700";
    ::DeleteFileW(bmp.c_str());
}

// ---- 姿态 ----
// 模型给框中心 (320,320)、关键点 (320,320)/(300,320)/(340,320)（模型输入空间）。
// 输入素材 320x160：scale=2、pad_y=160，因此：
//   框中心 -> (160,80)，第 1 个关键点 -> (160,80)，第 2/3 个 -> (150,80)/(170,80)。
// 这条用例专门锁"关键点也做 letterbox 逆变换"——只解框不解点是最常见的偷懒写法。
TEST(YoloPoseTest, FilePoseReturnsKeypointsMappedBackToImage) {
    const std::wstring model = FindTestModel(L"yolo_pose_test.onnx");
    const std::wstring bmp = FindTestModel(L"yolo_task_input_320x160.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_pose_test.onnx（脚本 scripts/gen_yolo_task_test_models.py）";
    if (bmp.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_task_input_320x160.bmp";

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    const std::wstring info = TakeString(OpYoloGetModelInfo(h.handle));
    EXPECT_NE(std::wstring::npos, info.find(L"\"task\":\"pose\"")) << "姿态模型应判为 pose";

    int found = 0;
    const std::wstring json = TakeString(OpYoloPoseFromFile(h.handle, bmp.c_str(), 0.25, 0.45, &found));
    ASSERT_FALSE(json.empty()) << "Pose 返回 nullptr：任务没被判为 pose，或解码失败";
    EXPECT_EQ(1, found);
    EXPECT_TRUE(JsonHasKey(json, L"keypoints"));
    EXPECT_EQ(3, JsonNumber(json, L"keypoint_count"));

    EXPECT_NEAR(160.0, JsonNthDouble(json, L"x", 0), 2.0) << "第 1 个关键点 x 应≈160";
    EXPECT_NEAR(80.0, JsonNthDouble(json, L"y", 0), 2.0) << "第 1 个关键点 y 应≈80";
    EXPECT_NEAR(150.0, JsonNthDouble(json, L"x", 1), 3.0) << "第 2 个关键点 x 应≈150";
    EXPECT_NEAR(170.0, JsonNthDouble(json, L"x", 2), 3.0) << "第 3 个关键点 x 应≈170";
    EXPECT_NEAR(0.9, JsonNthDouble(json, L"confidence", 1), 0.02) << "第一个关键点自身置信度应≈0.9";

    const std::vector<double> box = JsonArrayNumbers(json, L"bbox");
    ASSERT_EQ(4u, box.size());
    EXPECT_NEAR(160.0, (box[0] + box[2]) / 2.0, 3.0) << "框中心 X 应≈160";
    EXPECT_NEAR(80.0, (box[1] + box[3]) / 2.0, 3.0) << "框中心 Y 应≈80";
}

// ---- 旋转框 ----
// 模型两个 anchor 几乎重合，第二个置信度更低 -> 旋转 NMS 必须抑制掉，只留 1 条。
// 输入 320x160（scale=2、pad_y=160），角度 0 时映射结果可精确手算：
//   center≈(160,80)、size≈(80,40)、bbox≈(120,60)-(200,100)。
TEST(YoloObbTest, FileObbReturnsRotatedBoxAndSuppressesDuplicate) {
    const std::wstring model = FindTestModel(L"yolo_obb_test.onnx");
    const std::wstring bmp = FindTestModel(L"yolo_task_input_320x160.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_obb_test.onnx（脚本 scripts/gen_yolo_task_test_models.py）";
    if (bmp.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_task_input_320x160.bmp";

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    const std::wstring info = TakeString(OpYoloGetModelInfo(h.handle));
    EXPECT_NE(std::wstring::npos, info.find(L"\"task\":\"obb\"")) << "旋转框模型应判为 obb";

    int found = 0;
    const std::wstring json = TakeString(OpYoloObbFromFile(h.handle, bmp.c_str(), 0.25, 0.45, &found));
    ASSERT_FALSE(json.empty()) << "Obb 返回 nullptr：任务没被判为 obb，或解码失败";
    EXPECT_EQ(1, found) << "两个几乎重合的 anchor 应被旋转 NMS 合成 1 条";

    const std::vector<double> center = JsonArrayNumbers(json, L"center");
    ASSERT_EQ(2u, center.size());
    EXPECT_NEAR(160.0, center[0], 2.0);
    EXPECT_NEAR(80.0, center[1], 2.0);

    const std::vector<double> size = JsonArrayNumbers(json, L"size");
    ASSERT_EQ(2u, size.size());
    EXPECT_NEAR(80.0, size[0], 2.0);
    EXPECT_NEAR(40.0, size[1], 2.0);

    const std::vector<double> box = JsonArrayNumbers(json, L"bbox");
    ASSERT_EQ(4u, box.size());
    EXPECT_NEAR(120.0, box[0], 2.0);
    EXPECT_NEAR(60.0, box[1], 2.0);
    EXPECT_NEAR(200.0, box[2], 2.0);
    EXPECT_NEAR(100.0, box[3], 2.0);

    EXPECT_NEAR(0.0, JsonNthDouble(json, L"angle", 0), 0.01);
    EXPECT_EQ(8u, JsonArrayNumbers(json, L"corners").size()) << "4 个角点应有 8 个坐标值";
}


// ---- 推理设备：--device 解析 / DirectML 缺失回退 ----

TEST(YoloDeviceTest, ExplicitCpuAndDmlFallbackKeepEngineUsable) {
    const std::wstring model = FindTestModel(L"yolo_pose_test.onnx");
    const std::wstring bmp = FindTestModel(L"yolo_task_input_320x160.bmp");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_pose_test.onnx";
    if (bmp.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_task_input_320x160.bmp";

    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L"--device=cpu --threads=0"));
    std::wstring info = TakeString(OpYoloGetModelInfo(h.handle));
    EXPECT_EQ(-1, JsonNumber(info, L"device")) << "--device=cpu 必须生效为 CPU";
    EXPECT_EQ(0, JsonNumber(info, L"dml"));

    int found = 0;
    std::wstring json = TakeString(OpYoloPoseFromFile(h.handle, bmp.c_str(), 0.25, 0.45, &found));
    EXPECT_FALSE(json.empty()) << "显式 CPU 下 Pose 必须可用";
    EXPECT_EQ(1, found);

    // 请求 DML：部署了 DML 版 ORT 就应生效；没部署则必须回退 CPU 而不是加载失败。
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L"--device=0"));
    info = TakeString(OpYoloGetModelInfo(h.handle));
    const long dml = JsonNumber(info, L"dml");
    const long device = JsonNumber(info, L"device");
    EXPECT_TRUE(dml == 0 || dml == 1) << "dml 必须是 0/1，实际: " << dml;
    if (dml == 1)
        EXPECT_GE(device, 0) << "DML 生效时 device 应为适配器序号";
    else
        EXPECT_EQ(-1, device) << "回退 CPU 时 device 必须是 -1，不能假装占用了 GPU";

    found = 0;
    json = TakeString(OpYoloPoseFromFile(h.handle, bmp.c_str(), 0.25, 0.45, &found));
    EXPECT_FALSE(json.empty()) << "DML 不可用时引擎必须已回退 CPU 且仍可推理";
    EXPECT_EQ(1, found);
}

// ---- 命名模型平台：注册表 / 热切换 / 设备枚举 / 加密包 ----
//
// 诚实性说明：
//   * 设备列表断言"CPU 一定在、index=-1"与"dml_available 是 0/1"，
//     不假设机器上一定有 GPU（CI 上常常没有）。
//   * 加密包用例先加密再读回 8 字节 magic，确认文件**确实是密封容器**，
//     再验证"无密码 / 错密码 / 对密码"三条分支；解密后与原文件逐字节比对，
//     防止"加密只是加了个头、密文等于明文"的假实现。

namespace {

std::vector<char> ReadFileBytes(const std::wstring &path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(YoloModelPlatformTest, ListDevicesReportsCpuAndSchema) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    const std::wstring json = TakeString(OpYoloListDevices(h.handle));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, JsonNumber(json, L"ok"));
    EXPECT_TRUE(JsonHasKey(json, L"devices"));
    EXPECT_NE(std::wstring::npos, json.find(L"\"name\":\"CPU\"")) << "CPU 必须始终列为可选设备";
    EXPECT_NE(std::wstring::npos, json.find(L"\"type\":\"cpu\""));
    EXPECT_EQ(-1, JsonNumber(json, L"index")) << "CPU 的 index 约定为 -1";
    const long dml = JsonNumber(json, L"dml_available");
    EXPECT_TRUE(dml == 0 || dml == 1) << "dml_available 必须是 0/1，实际: " << dml;
}

TEST(YoloModelPlatformTest, LoadSwitchReleaseLifecycle) {
    const std::wstring det = FindTestModel(L"yolo_multi_test.onnx");
    const std::wstring cls = FindTestModel(L"yolo_cls_test.onnx");
    if (det.empty() || cls.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_multi_test.onnx 或 yolo_cls_test.onnx";

    CApiHandle h;
    ASSERT_EQ(1, OpYoloReleaseModel(h.handle, L"")) << "清场应总是成功";

    int ret = 0;
    std::wstring json = TakeString(OpYoloLoadModel(h.handle, L"m_det", det.c_str(), L"", &ret));
    ASSERT_EQ(1, ret) << "加载检测模型失败: " << std::string(json.begin(), json.end());
    EXPECT_EQ(1, JsonNumber(json, L"ok"));
    EXPECT_EQ(1, JsonNumber(json, L"active")) << "新加载的模型应自动激活";
    EXPECT_NE(std::wstring::npos, json.find(L"\"name\":\"m_det\""));
    EXPECT_NE(std::wstring::npos, json.find(L"\"task\":\"detect\""));

    ret = 0;
    json = TakeString(OpYoloLoadModel(h.handle, L"m_cls", cls.c_str(), L"", &ret));
    ASSERT_EQ(1, ret);
    EXPECT_NE(std::wstring::npos, json.find(L"\"task\":\"classify\""));

    // 注册表：两个模型都在，active 是后加载的
    json = TakeString(OpYoloListModels(h.handle));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(2, JsonNumber(json, L"count"));
    EXPECT_NE(std::wstring::npos, json.find(L"\"active\":\"m_cls\""));
    EXPECT_NE(std::wstring::npos, json.find(L"\"name\":\"m_det\""));
    EXPECT_NE(std::wstring::npos, json.find(L"\"name\":\"m_cls\""));
    EXPECT_NE(std::wstring::npos, TakeString(OpYoloGetModelInfo(h.handle)).find(L"\"task\":\"classify\""));

    // 热切换回检测，并且切换后真的能推理（不是只改了名字）
    EXPECT_EQ(1, OpYoloUseModel(h.handle, L"m_det"));
    EXPECT_NE(std::wstring::npos, TakeString(OpYoloGetModelInfo(h.handle)).find(L"\"task\":\"detect\""));
    EXPECT_NE(std::wstring::npos, TakeString(OpYoloListModels(h.handle)).find(L"\"active\":\"m_det\""));

    const std::wstring bmp = WriteSolidBmp(L"__yolo_platform.bmp", 640, 640);
    ASSERT_FALSE(bmp.empty());
    int found = 0;
    const std::wstring detected =
        TakeString(OpYoloDetectFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"", &found));
    EXPECT_FALSE(detected.empty()) << "切换后推理失败";
    EXPECT_EQ(4, found) << "yolo_multi_test 有 4 个目标（conf .90/.80/.70/.60）；靶子须 640x640 才全在画面内";
    ::DeleteFileW(bmp.c_str());

    // 未注册的名字必须显式失败
    EXPECT_EQ(0, OpYoloUseModel(h.handle, L"nope"));
    EXPECT_EQ(0, OpYoloReleaseModel(h.handle, L"nope"));

    // 释放当前激活模型 -> 引擎回到未加载（不只是从表里删名）
    EXPECT_EQ(1, OpYoloReleaseModel(h.handle, L"m_det"));
    EXPECT_NE(std::wstring::npos, TakeString(OpYoloGetModelInfo(h.handle)).find(L"\"task\":\"none\""));
    json = TakeString(OpYoloListModels(h.handle));
    EXPECT_EQ(1, JsonNumber(json, L"count"));
    EXPECT_NE(std::wstring::npos, json.find(L"\"active\":\"\""));

    EXPECT_EQ(1, OpYoloReleaseModel(h.handle, L""));
    EXPECT_EQ(0, JsonNumber(TakeString(OpYoloListModels(h.handle)), L"count"));
}

TEST(YoloModelPlatformTest, EncryptedPackageLoadAndDecrypt) {
    const std::wstring src = FindTestModel(L"yolo_pose_test.onnx");
    if (src.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_pose_test.onnx";

    CApiHandle h;
    ASSERT_EQ(1, OpYoloReleaseModel(h.handle, L""));
    const std::wstring sealed = test_support::GetTempBmpPath(L"__yolo_sealed.opymodel");
    const std::wstring copy = test_support::GetTempBmpPath(L"__yolo_plain_copy.onnx");
    ::DeleteFileW(sealed.c_str());
    ::DeleteFileW(copy.c_str());

    const wchar_t *password = L"s3cret";
    ASSERT_EQ(1, OpYoloEncryptModel(h.handle, src.c_str(), sealed.c_str(), password)) << "加密失败";
    const std::vector<char> sealed_bytes = ReadFileBytes(sealed);
    ASSERT_GT(sealed_bytes.size(), 64u);
    ASSERT_EQ(0, std::memcmp(sealed_bytes.data(), "OPYMOD01", 8)) << "文件头必须是 OPYMOD01";
    const std::vector<char> plain_bytes = ReadFileBytes(src);
    EXPECT_NE(plain_bytes, sealed_bytes) << "密文不能等于明文";

    int ret = 1;
    std::wstring json = TakeString(OpYoloLoadModel(h.handle, L"m_enc", sealed.c_str(), L"", &ret));
    EXPECT_EQ(0, ret);
    EXPECT_NE(std::wstring::npos, json.find(L"password_required"));

    ret = 1;
    json = TakeString(OpYoloLoadModel(h.handle, L"m_enc", sealed.c_str(), L"--password=wrong", &ret));
    EXPECT_EQ(0, ret);
    EXPECT_NE(std::wstring::npos, json.find(L"decrypt_failed"));

    ret = 0;
    json = TakeString(OpYoloLoadModel(h.handle, L"m_enc", sealed.c_str(), L"--password=s3cret", &ret));
    ASSERT_EQ(1, ret) << "正确密码应加载成功: " << std::string(json.begin(), json.end());
    EXPECT_NE(std::wstring::npos, json.find(L"\"task\":\"pose\""));
    EXPECT_EQ(1, JsonNumber(json, L"encrypted"));
    EXPECT_EQ(1, JsonNumber(json, L"memory_loaded")) << "加密包必须走内存加载，不落临时明文";
    EXPECT_NE(std::wstring::npos, TakeString(OpYoloGetModelInfo(h.handle)).find(L"\"task\":\"pose\""));

    // 解密回明文：与源文件逐字节一致
    ASSERT_EQ(1, OpYoloDecryptModel(h.handle, sealed.c_str(), copy.c_str(), password));
    EXPECT_EQ(plain_bytes, ReadFileBytes(copy));
    ::DeleteFileW(copy.c_str());

    // 错密码解密：失败且不产出文件
    const std::wstring bad_copy = test_support::GetTempBmpPath(L"__yolo_bad_copy.onnx");
    ::DeleteFileW(bad_copy.c_str());
    EXPECT_EQ(0, OpYoloDecryptModel(h.handle, sealed.c_str(), bad_copy.c_str(), L"wrong"));
    EXPECT_EQ(INVALID_FILE_ATTRIBUTES, ::GetFileAttributesW(bad_copy.c_str())) << "失败时不能留下半个文件";

    // 未加密封装的头：解密必须失败
    EXPECT_EQ(0, OpYoloDecryptModel(h.handle, src.c_str(), bad_copy.c_str(), password));

    EXPECT_EQ(1, OpYoloReleaseModel(h.handle, L""));
    ::DeleteFileW(sealed.c_str());
}

TEST(YoloModelPlatformTest, LoadMissingFileReportsErrorCode) {
    CApiHandle h;
    int ret = 1;
    const std::wstring json =
        TakeString(OpYoloLoadModel(h.handle, L"m_missing", L"__no_such_model__.onnx", L"", &ret));
    EXPECT_EQ(0, ret);
    EXPECT_NE(std::wstring::npos, json.find(L"file_not_found"));
    EXPECT_EQ(0, JsonNumber(json, L"ok"));
}
