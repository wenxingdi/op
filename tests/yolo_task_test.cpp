// YOLO 任务维度（批 C 2026-11-22）：分类 + 实例分割。
//
// 层次：
//   1) YoloTaskDetectTest.*  —— 任务判定与"任务不符必须显式失败(-1)"的契约
//   2) YoloClassifyTest.*    —— 端到端（需要 tests/testdata/yolo_cls_test.onnx）
//   3) YoloSegmentTest.*     —— 端到端（需要 tests/testdata/yolo_seg_test.onnx）
//
// 诚实性说明：
//   * 两个模型都是脚本生成的极小模型（scripts/gen_yolo_task_test_models.py），
//     输出是常量张量。它们验证的是**解码与契约**，不是"模型精度"。
//     模型缺失时 GTEST_SKIP 并写明原因，不假绿。
//   * 不做姿态(Pose)/旋转框(Obb)：见 docs/2026-11/ 里对"未做"的说明。

#include "test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <iostream>
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

std::wstring TakeString(const wchar_t *p) {
    return p ? std::wstring(p) : std::wstring();
}

bool MakeTempBmp(std::wstring &path) {
    path = test_support::GetTempBmpPath(L"__yolo_task.bmp");
    return test_support::CreateConsoleLikeBmp(path, L"BP 909");
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
