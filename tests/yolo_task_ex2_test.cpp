// YOLO 任务族 Ex2 过滤变体（2026-11-26）：classify / segment / pose / obb。
//
// 层次：
//   1) YoloTaskEx2CoreTest.* —— 纯核心单测（不需要模型）：直接喂 vyolo_*_t 给
//      YoloDetectCore.h 的过滤/构建器，覆盖并集语义、截断排序、total/returned/index、
//      旧输出结构不变（extra_fields=false 与 2026-11-22 版逐字节一致）。
//   2) YoloTaskEx2ApiTest.*  —— 端到端（需要 tests/testdata/yolo_{cls,seg,pose,obb}_test.onnx，
//      脚本 scripts/gen_yolo_task_test_models.py 生成的常量输出模型）。
//
// 诚实性说明：端到端模型输出是常量张量，验证的是**解码 + 过滤 + JSON 契约**，
// 不是"模型精度"。模型缺失时 GTEST_SKIP 并写明原因，不假绿。

#include "test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include <op/YoloDetectCore.h>
#include <op_c_api.h>

namespace {

// ---------- 公共小工具（与 yolo_task_test.cpp 同风格） ----------

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

std::wstring TakeString(const wchar_t *p) {
    return p ? std::wstring(p) : std::wstring();
}

bool JsonHasKey(const std::wstring &json, const wchar_t *key) {
    return json.find(std::wstring(L"\"") + key + L"\"") != std::wstring::npos;
}

// 第 occurrence 次出现的 "key": 之后的数字（结果数组内逐项断言用）
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

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 纯色 640x640 靶子：分割/姿态/旋转框模型的常量框在 640 输入空间的中心，
// 1:1 映射（scale=1、pad=0）下坐标断言最稳。
std::wstring WriteSolidBmp640(const wchar_t *file_name) {
    constexpr int w = 640, h = 640;
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

// ---------- 核心单测数据 ----------

using op::yolo::core::BuildClassifyJson;
using op::yolo::core::BuildObbJson;
using op::yolo::core::BuildPoseJson;
using op::yolo::core::BuildSegmentJson;
using op::yolo::core::CountKept;
using op::yolo::core::Options;
using op::yolo::core::ParseOptions;
using op::yolo::core::ClassifyClassLabel;
using op::yolo::core::ClassifyScore;
using op::yolo::core::SegClassLabel;
using op::yolo::core::SegScore;
using op::yolo::core::PoseClassLabel;
using op::yolo::core::PoseScore;
using op::yolo::core::ObBClassLabel;
using op::yolo::core::ObBScore;

op::yolo::vyolo_cls_t MakeClsItems() {
    // 模拟引擎输出（已按 score 降序）：charlie 0.71 / alpha 0.15 / delta 0.06
    op::yolo::vyolo_cls_t items;
    items.push_back({2, L"charlie", 0.71f});
    items.push_back({0, L"alpha", 0.15f});
    items.push_back({3, L"delta", 0.06f});
    return items;
}

op::yolo_rec_t MakeBox(int class_id, const wchar_t *label, float conf) {
    op::yolo_rec_t box;
    box.class_id = class_id;
    box.label = label;
    box.confidence = conf;
    box.left_top = {10, 20};
    box.right_bottom = {110, 220};
    return box;
}

} // namespace

// ==================== 核心单测（不需要模型） ====================

TEST(YoloTaskEx2CoreTest, ClassIdsFilterKeepsOnlyMatching) {
    Options opt;
    ASSERT_TRUE(ParseOptions(L"{\"class_ids\":[0]}", opt));
    const auto items = MakeClsItems();
    const std::wstring json = BuildClassifyJson(items, opt, /*extra_fields=*/true, 0);
    EXPECT_TRUE(JsonHasKey(json, L"total"));
    EXPECT_TRUE(JsonHasKey(json, L"index"));
    // total=3（过滤前），returned=1
    EXPECT_NE(std::wstring::npos, json.find(L"\"total\":3")) << json.c_str();
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":1")) << json.c_str();
    // 只留 alpha
    EXPECT_NE(std::wstring::npos, json.find(L"\"label\":\"alpha\""));
    EXPECT_EQ(std::wstring::npos, json.find(L"charlie"));
    EXPECT_EQ(1L, CountKept(items, opt, ClassifyClassLabel, ClassifyScore));
}

TEST(YoloTaskEx2CoreTest, LabelsFilterAndUnionSemantics) {
    const auto items = MakeClsItems();
    // labels 精确匹配
    Options by_label;
    ASSERT_TRUE(ParseOptions(L"{\"labels\":[\"charlie\"]}", by_label));
    EXPECT_EQ(1L, CountKept(items, by_label, ClassifyClassLabel, ClassifyScore));
    // 并集：class_ids=[3]（delta）∪ labels=["charlie"] → 2 条
    Options uni;
    ASSERT_TRUE(ParseOptions(L"{\"class_ids\":[3],\"labels\":[\"charlie\"]}", uni));
    const std::wstring json = BuildClassifyJson(items, uni, /*extra_fields=*/true, 0);
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":2")) << json.c_str();
    // 交集语义会返回 0 条——这里正向证明是并集：charlie 与 delta 都出现
    EXPECT_NE(std::wstring::npos, json.find(L"charlie"));
    EXPECT_NE(std::wstring::npos, json.find(L"delta"));
}

TEST(YoloTaskEx2CoreTest, MaxResultsTruncatesByScoreDescending) {
    Options opt;
    ASSERT_TRUE(ParseOptions(L"{\"max_results\":2}", opt));
    const auto items = MakeClsItems();
    const std::wstring json = BuildClassifyJson(items, opt, /*extra_fields=*/true, 0);
    EXPECT_NE(std::wstring::npos, json.find(L"\"total\":3")) << json.c_str();
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":2")) << json.c_str();
    // 降序截断：保留 score 最高的 charlie 与 alpha，delta 被截掉
    EXPECT_NE(std::wstring::npos, json.find(L"charlie"));
    EXPECT_NE(std::wstring::npos, json.find(L"alpha"));
    EXPECT_EQ(std::wstring::npos, json.find(L"delta"));
    // index 保留原始下标：charlie=0、alpha=1
    EXPECT_EQ(0L, JsonNthNumber(json, L"index", 0));
    EXPECT_EQ(1L, JsonNthNumber(json, L"index", 1));
}

TEST(YoloTaskEx2CoreTest, InvalidOptionsRejectedExplicitly) {
    Options opt;
    EXPECT_FALSE(ParseOptions(L"{\"class_ids\":\"not-array\"}", opt)); // 类型错
    EXPECT_FALSE(ParseOptions(L"{\"max_results\":-1}", opt));          // 负数
    EXPECT_FALSE(ParseOptions(L"{\"unknown_field\":1}", opt));         // 未知字段
    EXPECT_FALSE(ParseOptions(L"{broken json", opt));                  // 语法错
    ASSERT_TRUE(ParseOptions(L"", opt));                               // 空串 = 默认
    ASSERT_TRUE(ParseOptions(L"{}", opt));                             // 空对象 = 默认
}

TEST(YoloTaskEx2CoreTest, LegacyModeOutputStructureUnchanged) {
    // extra_fields=false + 空 Options：输出与 2026-11-22 旧版逐字段一致。
    // classify 旧版**没有** total 字段，也不带 index。
    const auto items = MakeClsItems();
    const std::wstring json = BuildClassifyJson(items, Options{}, /*extra_fields=*/false, 0);
    EXPECT_FALSE(JsonHasKey(json, L"total")) << "旧 classify JSON 不应有 total";
    EXPECT_FALSE(JsonHasKey(json, L"index")) << "旧 JSON 不应有 index";
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":3")) << json.c_str();
}

TEST(YoloTaskEx2CoreTest, SegmentPoseObbFilterOnBoxAndFlatFields) {
    // segment/pose 的过滤键在内嵌 box 上；obb 是扁平字段。三者共用同一套并集语义。
    op::yolo::vyolo_seg_t segs;
    segs.push_back({MakeBox(0, L"blob", 0.9f), {{0, 0}, {5, 5}}, 25.0f});
    segs.push_back({MakeBox(5, L"rock", 0.5f), {}, 0.0f});

    Options keep_blob;
    ASSERT_TRUE(ParseOptions(L"{\"labels\":[\"blob\"]}", keep_blob));
    EXPECT_EQ(1L, CountKept(segs, keep_blob, SegClassLabel, SegScore));
    const std::wstring seg_json = BuildSegmentJson(segs, keep_blob, /*extra_fields=*/true, 0);
    EXPECT_NE(std::wstring::npos, seg_json.find(L"\"total\":2")) << seg_json.c_str();
    EXPECT_NE(std::wstring::npos, seg_json.find(L"\"returned\":1")) << seg_json.c_str();
    EXPECT_NE(std::wstring::npos, seg_json.find(L"\"label\":\"blob\""));
    EXPECT_TRUE(JsonHasKey(seg_json, L"cx")) << "segment Ex2 应输出 cx/cy/w/h";

    op::yolo::vyolo_pose_t poses;
    poses.push_back({MakeBox(0, L"person", 0.9f), {{320.0f, 320.0f, 0.9f}}});
    const std::wstring pose_json = BuildPoseJson(poses, keep_blob, /*extra_fields=*/true, 0);
    EXPECT_NE(std::wstring::npos, pose_json.find(L"\"returned\":0")) << pose_json.c_str();
    Options keep_person;
    ASSERT_TRUE(ParseOptions(L"{\"class_ids\":[0]}", keep_person));
    EXPECT_EQ(1L, CountKept(poses, keep_person, PoseClassLabel, PoseScore));

    op::yolo::vyolo_obb_t obbs;
    obbs.push_back({0, L"ship", 0.9f, 320, 320, 160, 80, 0.1f, {240, 280}, {400, 360}, {}});
    Options no_ship;
    ASSERT_TRUE(ParseOptions(L"{\"labels\":[\"truck\"]}", no_ship));
    EXPECT_EQ(0L, CountKept(obbs, no_ship, ObBClassLabel, ObBScore));
    const std::wstring obb_json = BuildObbJson(obbs, no_ship, /*extra_fields=*/true, 0);
    EXPECT_NE(std::wstring::npos, obb_json.find(L"\"total\":1")) << obb_json.c_str();
    EXPECT_NE(std::wstring::npos, obb_json.find(L"\"returned\":0")) << obb_json.c_str();
}

// ==================== 端到端（常量输出模型） ====================

TEST(YoloTaskEx2ApiTest, ClassifyFilterByClassIdAndLabel) {
    const std::wstring model = FindTestModel(L"yolo_cls_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_cls_test.onnx（脚本 scripts/gen_yolo_task_test_models.py）";
    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));

    const std::wstring bmp = WriteSolidBmp640(L"__yolo_cls_ex2.bmp");
    ASSERT_FALSE(bmp.empty());

    // 空 options：5 条全量（cls 模型 5 类），total=returned=5
    int found = -1;
    std::wstring json = TakeString(OpYoloClassifyFromFileEx2(h.handle, bmp.c_str(), 5, L"", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(5, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"total\":5")) << json.c_str();
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":5")) << json.c_str();
    // 引擎按 score 降序：第 0 条是 charlie（0.71）
    EXPECT_EQ(2L, JsonNthNumber(json, L"class_id", 0)) << json.c_str();

    // class_ids=[2]：只留 charlie
    found = -1;
    json = TakeString(OpYoloClassifyFromFileEx2(h.handle, bmp.c_str(), 5, L"{\"class_ids\":[2]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"label\":\"charlie\""));
    EXPECT_NE(std::wstring::npos, json.find(L"\"total\":5")) << "total 恒为过滤前条数";
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":1")) << json.c_str();

    // labels=["alpha"]：只留 alpha
    found = -1;
    json = TakeString(OpYoloClassifyFromFileEx2(h.handle, bmp.c_str(), 5, L"{\"labels\":[\"alpha\"]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"class_id\":0"));

    // max_results=2：score 降序前 2 = charlie + alpha
    found = -1;
    json = TakeString(OpYoloClassifyFromFileEx2(h.handle, bmp.c_str(), 5, L"{\"max_results\":2}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(2, found);
    EXPECT_EQ(2L, JsonNthNumber(json, L"class_id", 0)) << json.c_str();
    EXPECT_EQ(0L, JsonNthNumber(json, L"class_id", 1)) << json.c_str();

    // 非法 options：nullptr（与"没结果"区分）
    found = -1;
    EXPECT_EQ(nullptr, OpYoloClassifyFromFileEx2(h.handle, bmp.c_str(), 5, L"{\"bogus\":1}", &found));
}

TEST(YoloTaskEx2ApiTest, ClassifyEmptyOptionsMatchesLegacyContent) {
    // 旧接口与 Ex2 空 options 必须识别同一批条目（旧结构无 total/index）。
    const std::wstring model = FindTestModel(L"yolo_cls_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_cls_test.onnx";
    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    const std::wstring bmp = WriteSolidBmp640(L"__yolo_cls_ex2_parity.bmp");
    ASSERT_FALSE(bmp.empty());

    int legacy_found = -1;
    const std::wstring legacy = TakeString(OpYoloClassifyFromFile(h.handle, bmp.c_str(), 5, &legacy_found));
    int ex2_found = -1;
    const std::wstring ex2 = TakeString(OpYoloClassifyFromFileEx2(h.handle, bmp.c_str(), 5, L"", &ex2_found));
    ASSERT_FALSE(legacy.empty());
    ASSERT_FALSE(ex2.empty());
    EXPECT_EQ(legacy_found, ex2_found);
    EXPECT_FALSE(JsonHasKey(legacy, L"total")) << "旧 classify JSON 不应有 total";
    for (int i = 0; i < legacy_found; ++i) {
        EXPECT_EQ(JsonNthNumber(legacy, L"class_id", i), JsonNthNumber(ex2, L"class_id", i))
            << "第 " << i << " 条 class_id 应一致";
    }
}

TEST(YoloTaskEx2ApiTest, SegmentFilterKeepsOrDropsAll) {
    const std::wstring model = FindTestModel(L"yolo_seg_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_seg_test.onnx";
    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    const std::wstring bmp = WriteSolidBmp640(L"__yolo_seg_ex2.bmp");
    ASSERT_FALSE(bmp.empty());

    // class_ids=[0]：命中唯一目标
    int found = -1;
    std::wstring json = TakeString(
        OpYoloSegmentFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"class_ids\":[0]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"label\":\"blob\""));
    EXPECT_TRUE(JsonHasKey(json, L"polygon"));
    EXPECT_TRUE(JsonHasKey(json, L"cx")) << "segment Ex2 应输出 cx/cy/w/h";

    // class_ids=[7]：全滤掉——返回合法 JSON、returned=0（不是 nullptr）
    found = -1;
    json = TakeString(
        OpYoloSegmentFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"class_ids\":[7]}", &found));
    ASSERT_FALSE(json.empty()) << "全滤掉应返回合法 JSON，不是 nullptr";
    EXPECT_EQ(0, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"total\":1")) << json.c_str();
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":0")) << json.c_str();

    // 非法 options：nullptr
    found = -1;
    EXPECT_EQ(nullptr, OpYoloSegmentFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{bad", &found));
}

TEST(YoloTaskEx2ApiTest, PoseFilterAndKeypointsSurvive) {
    const std::wstring model = FindTestModel(L"yolo_pose_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_pose_test.onnx";
    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    const std::wstring bmp = WriteSolidBmp640(L"__yolo_pose_ex2.bmp");
    ASSERT_FALSE(bmp.empty());

    int found = -1;
    std::wstring json =
        TakeString(OpYoloPoseFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"labels\":[\"person\"]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, found);
    EXPECT_TRUE(JsonHasKey(json, L"keypoints"));
    EXPECT_EQ(3L, JsonNthNumber(json, L"keypoint_count", 0)) << "pose 模型 kpt_shape=[3,3]";
    EXPECT_TRUE(JsonHasKey(json, L"cx"));

    // 不存在的类：合法 JSON + returned=0
    found = -1;
    json = TakeString(OpYoloPoseFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"class_ids\":[9]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(0, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"returned\":0")) << json.c_str();
}

TEST(YoloTaskEx2ApiTest, ObBFilterAndAngleFieldsSurvive) {
    const std::wstring model = FindTestModel(L"yolo_obb_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_obb_test.onnx";
    CApiHandle h;
    ASSERT_EQ(1, OpSetYoloEngine(h.handle, model.c_str(), L"", L""));
    const std::wstring bmp = WriteSolidBmp640(L"__yolo_obb_ex2.bmp");
    ASSERT_FALSE(bmp.empty());

    // obb 模型两个 anchor 几乎重合，NMS 后只留 1 条（ship, conf 0.9）
    int found = -1;
    std::wstring json = TakeString(
        OpYoloObbFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"class_ids\":[0]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"label\":\"ship\""));
    EXPECT_TRUE(JsonHasKey(json, L"angle"));
    EXPECT_TRUE(JsonHasKey(json, L"corners"));

    // 并集语义：class_ids=[3] ∪ labels=["ship"] → 仍命中 ship
    found = -1;
    json = TakeString(
        OpYoloObbFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"class_ids\":[3],\"labels\":[\"ship\"]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, found);

    // 全滤掉：合法 JSON + returned=0
    found = -1;
    json = TakeString(OpYoloObbFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"{\"labels\":[\"truck\"]}", &found));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(0, found);
    EXPECT_NE(std::wstring::npos, json.find(L"\"total\":1")) << json.c_str();

    // 非法 options：nullptr
    found = -1;
    EXPECT_EQ(nullptr, OpYoloObbFromFileEx2(h.handle, bmp.c_str(), 0.25, 0.45, L"[]", &found));
}
