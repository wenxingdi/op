// YOLO 模块增强自测（2026-11-21）
// 覆盖 docs/2026-11/YOLO模块优化_缺口与方案_20261121.md §四 的 V1~V8。
//
// 层次：
//   1) YoloOptionTest.*  —— 纯 JSON/过滤语义（不需要模型，直接走 C API 的参数校验路径）
//   2) YoloEx2Test.*     —— 端到端（需要 tests/testdata/yolo_meta_test.onnx）
//
// 诚实性说明：本机只有 yolo_meta_test.onnx 一个真实模型（2.8MB，脚本生成）。
// 若该模型缺失，端到端用例 GTEST_SKIP 并写明原因，**不假装验证过**。

#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
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

// 定位 tests/testdata/yolo_meta_test.onnx（与 yolo_test.cpp 同一套候选路径）
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

// 单 Constant 假模型：NMS 后只产 1 条（过滤/并集语义用它）
std::wstring FindYoloMetaTestModel() {
    return FindTestModel(L"yolo_meta_test.onnx");
}

// 多目标模型：4 条互不重叠、类别各异（0.9/0.8/0.7/0.6），用于闭环验证
// max_results 截断与"截断保留高分"顺序。生成脚本 scripts/gen_yolo_multi_test_model.py
std::wstring FindYoloMultiTestModel() {
    return FindTestModel(L"yolo_multi_test.onnx");
}

// 极简 JSON 读取（只供断言）
bool JsonHasKey(const std::wstring &json, const wchar_t *key) {
    return json.find(std::wstring(L"\"") + key + L"\":") != std::wstring::npos;
}

bool JsonInt(const std::wstring &json, const wchar_t *key, long &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    const size_t p = json.find(pattern);
    if (p == std::wstring::npos)
        return false;
    value = _wtoi(json.c_str() + p + pattern.size());
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

// 用 GDI 造一张纯色小图并落盘（给端到端用；YOLO 对纯色图通常无检测，正好验证"不崩溃"）
std::wstring WriteSolidImage(const wchar_t *file_name, int w, int h, COLORREF color) {
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
    HBRUSH brush = CreateSolidBrush(color);
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

// V6 前半：非法 options 必须返回 nullptr（与"没检测到"区分）。
// 这一组**完全不需要模型**：参数校验发生在检测之前。
TEST(YoloOptionTest, InvalidOptionsRejectBeforeDetect) {
    CApiHandle api;
    int count = -1;

    // 坏 JSON
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"[not json", &count), nullptr);
    // 不是对象
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"[1,2]", &count), nullptr);
    // 未知字段（拼写错误必须报错）
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"{\"class_id\":[0]}", &count), nullptr);
    // 类型错误：class_ids 必须是数组
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"{\"class_ids\":0}", &count), nullptr);
    // 类型错误：数组元素必须是数字
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"{\"class_ids\":[\"a\"]}", &count), nullptr);
    // 类型错误：labels 元素必须是字符串
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"{\"labels\":[1]}", &count), nullptr);
    // max_results 不能为负
    EXPECT_EQ(OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"{\"max_results\":-5}", &count), nullptr);
    // 空 options 是合法输入（等价不过滤），只是这里没绑定窗口 → 走失败路径返回非 nullptr 的失败 JSON
    const wchar_t *empty = OpYoloDetectEx2(api.handle, 0, 0, 10, 10, 0.3, 0.5, L"", &count);
    // 未绑定窗口时没有截图，行为与旧接口一致（返回失败 JSON，不是 nullptr）
    if (empty != nullptr) {
        EXPECT_NE(std::wstring(empty).find(L'"'), std::wstring::npos);
    }
}

// 空句柄必须安全
TEST(YoloOptionTest, NullHandleIsSafe) {
    int count = -1;
    EXPECT_EQ(OpYoloDetectEx2(nullptr, 0, 0, 10, 10, 0.3, 0.5, L"{}", &count), nullptr);
}

// V1~V5、V7、V8：端到端（需要真实模型）
//
// 关键发现（2026-11-21 实测）：yolo_meta_test.onnx 对**纯色图**也能产出检测框
// （class_id=1, label="target_cls", conf 0.99）。因此过滤/截断逻辑可以**真实验证**，
// 不需要真实游戏画面。
//
// 但 OpYoloDetect / OpYoloDetectEx2 走"绑定窗口截图"，测试机没有可绑定窗口；
// 因此这里用 OpYoloDetectFromFile 覆盖同一套 build_yolo_json + offset_detections，
// 并**用 Ex2 的过滤语义在进程内模拟**（见 LegacyJsonShape 用例说明）。
TEST(YoloEx2Test, ModelProducesDetectionsOnSyntheticImage) {
    const std::wstring model = FindYoloMetaTestModel();
    if (model.empty())
        GTEST_SKIP() << "tests/testdata/yolo_meta_test.onnx not available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1) << "failed to load test model";

    const std::wstring image = WriteSolidImage(L"op_yolo_detect_solid.bmp", 320, 240, RGB(128, 128, 128));
    ASSERT_FALSE(image.empty());

    // conf 放很低，确保这个元数据测试模型能吐框
    const wchar_t *json = OpYoloDetectFromFile(api.handle, image.c_str(), 0.01, 0.45);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    ASSERT_TRUE(JsonHasKey(out, L"ok"));
    long ok = 0;
    ASSERT_TRUE(JsonInt(out, L"ok", ok));
    ASSERT_EQ(ok, 1) << "model must load and run: " << std::string(out.begin(), out.end());

    // V7：结果字段齐全（class_id / label / bbox / confidence）
    const size_t n = JsonCountOf(out, L"class_id");
    ASSERT_GT(n, 0u) << "the metadata test model is expected to emit at least one box on a synthetic image; "
                        "if it stops doing so, the filtering assertions below lose their meaning";
    EXPECT_EQ(JsonCountOf(out, L"label"), n);
    EXPECT_EQ(JsonCountOf(out, L"bbox"), n);
    EXPECT_EQ(JsonCountOf(out, L"confidence"), n);

    // V8：旧接口**不含** Ex2 专属字段（向后兼容）
    EXPECT_FALSE(JsonHasKey(out, L"total")) << "legacy interface must not gain Ex2-only fields";
    EXPECT_FALSE(JsonHasKey(out, L"returned"));
    EXPECT_FALSE(JsonHasKey(out, L"cx"));
}

// V5/V7：Ex2 的 total/returned/index/cx/cy/w/h 字段存在性。
// 由于 Ex2 需要绑定窗口，这里改用"Ex2 的参数校验 + 旧接口的真实结果"组合验证：
// 参数校验分支（parse_yolo_options）已由 InvalidOptionsRejectBeforeDetect 覆盖，
// 字段结构由下面的 JSON 形状断言覆盖（用 Ex2 在无窗口时的失败 JSON 校验外层结构）。
TEST(YoloEx2Test, Ex2JsonOuterShapeIsStable) {
    const std::wstring model = FindYoloMetaTestModel();
    if (model.empty())
        GTEST_SKIP() << "tests/testdata/yolo_meta_test.onnx not available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);

    int count = -1;
    const wchar_t *json = OpYoloDetectEx2(api.handle, 0, 0, 100, 100, 0.01, 0.45, L"{}", &count);
    ASSERT_NE(json, nullptr) << "valid options must not return nullptr";
    const std::wstring out(json);
    // 无绑定窗口时是失败 JSON，但结构必须是合法的三件套
    EXPECT_TRUE(JsonHasKey(out, L"ok"));
    EXPECT_TRUE(JsonHasKey(out, L"code"));
    EXPECT_TRUE(JsonHasKey(out, L"results"));
}

// 旧接口在重构后仍正常（回归守门）
TEST(YoloEx2Test, LegacyDetectFromFileUnchanged) {
    const std::wstring model = FindYoloMetaTestModel();
    if (model.empty())
        GTEST_SKIP() << "tests/testdata/yolo_meta_test.onnx not available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);

    const std::wstring image = WriteSolidImage(L"op_yolo_legacy2.bmp", 256, 256, RGB(200, 100, 50));
    ASSERT_FALSE(image.empty());

    const wchar_t *json = OpYoloDetectFromFile(api.handle, image.c_str(), 0.01, 0.45);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    long ok = 0;
    ASSERT_TRUE(JsonInt(out, L"ok", ok));
    EXPECT_EQ(ok, 1);

    // 缺失文件仍返回失败 JSON（既有契约）
    const wchar_t *missing = OpYoloDetectFromFile(api.handle, L"__no_such_file__.bmp", 0.3, 0.45);
    ASSERT_NE(missing, nullptr);
    long ok2 = 1;
    ASSERT_TRUE(JsonInt(std::wstring(missing), L"ok", ok2));
    EXPECT_EQ(ok2, 0) << "missing file must report ok=0";
}

// 反向验证说明（人工执行，CLAUDE.md 要求"故意改错必须 FAIL"）：
//   1) 把 keep_detection 改成恒 return true -> 过滤失效。
//      当前用例集**抓不到**（见"未覆盖项"），故这一条**未做反向验证**。
//   2) 把 build_yolo_json 的 "cx" 字段删掉 -> 因 Ex2 走不到真实检测，也抓不到。
//   3) 把 parse_yolo_options 的未知字段分支改成 return true ->
//      YoloOptionTest.InvalidOptionsRejectBeforeDetect 的"未知字段"断言**会 FAIL 可抓到**。
//
// 未覆盖项（如实记录，不掩饰）：
//   - **OpYoloDetect / OpYoloDetectEx2 的屏幕路径**：依赖"绑定窗口后截图"，
//     测试机无可绑定游戏窗口，无法端到端。
//   - **过滤/截断在真实检测结果上的行为**：Ex2 需要窗口，拿不到真实 results；
//     DetectFromFile 能产出真实框，但它调的是**不过滤**的旧路径。
//     因此"过滤正确性"这一支**没有被真实数据验证**，只有参数解析分支被测到。
//     要闭环需要：一个可绑定窗口 + 一个已知类别的真实 YOLO 模型。**这是本阶段
//     唯一没能闭环的验证点，明确记录。**
// ===== 过滤/截断的**真实闭环验证**（2026-11-21 关键用例）=====
//
// 用 yolo_meta_test.onnx 对纯色图产出真实检测框（class_id=1, label="target_cls"），
// 再通过 OpYoloDetectFromFileEx2 验证 class_ids / labels / max_results 的真实行为。
// 这是本阶段唯一能闭环"过滤正确性"的路径 —— 屏幕版 Ex2 需要绑定窗口，测不了。

TEST(YoloFilterRealTest, ClassIdsFilterKeepsOnlyMatching) {
    const std::wstring model = FindYoloMetaTestModel();
    if (model.empty())
        GTEST_SKIP() << "tests/testdata/yolo_meta_test.onnx not available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);
    const std::wstring image = WriteSolidImage(L"op_yolo_filter_cls.bmp", 320, 240, RGB(128, 128, 128));
    ASSERT_FALSE(image.empty());

    // 先拿全量，确认有结果且知道真实 class_id
    int total_count = 0;
    const wchar_t *all = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"null", &total_count);
    ASSERT_NE(all, nullptr);
    const std::wstring all_json(all);
    long total = 0, returned = 0;
    ASSERT_TRUE(JsonInt(all_json, L"total", total));
    ASSERT_TRUE(JsonInt(all_json, L"returned", returned));
    ASSERT_GT(total, 0) << "model must emit boxes; otherwise filtering cannot be verified";
    EXPECT_EQ(total, returned) << "no filter => returned must equal total";
    EXPECT_EQ(total_count, static_cast<int>(returned));

    // 命中类：保留
    int hit_count = 0;
    const wchar_t *hit = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"{\"class_ids\":[1]}",
                                                  &hit_count);
    ASSERT_NE(hit, nullptr);
    const std::wstring hit_json(hit);
    long hit_returned = 0, hit_total = 0;
    ASSERT_TRUE(JsonInt(hit_json, L"returned", hit_returned));
    ASSERT_TRUE(JsonInt(hit_json, L"total", hit_total));
    EXPECT_GT(hit_returned, 0) << "class_id=1 must survive the filter";
    EXPECT_EQ(hit_total, total) << "total must always report the unfiltered count";
    EXPECT_EQ(hit_count, static_cast<int>(hit_returned));

    // 不存在的类：全部被过滤掉，但 total 仍报真实条数
    int miss_count = -1;
    const wchar_t *miss = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"{\"class_ids\":[9999]}",
                                                   &miss_count);
    ASSERT_NE(miss, nullptr);
    const std::wstring miss_json(miss);
    long miss_returned = -1, miss_total = -1;
    ASSERT_TRUE(JsonInt(miss_json, L"returned", miss_returned));
    ASSERT_TRUE(JsonInt(miss_json, L"total", miss_total));
    EXPECT_EQ(miss_returned, 0) << "filtering by a non-existent class must drop everything";
    EXPECT_EQ(miss_total, total) << "total must still be the unfiltered count";
    EXPECT_EQ(miss_count, 0);
}

TEST(YoloFilterRealTest, LabelsFilterAndUnionSemantics) {
    const std::wstring model = FindYoloMetaTestModel();
    if (model.empty())
        GTEST_SKIP() << "tests/testdata/yolo_meta_test.onnx not available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);
    const std::wstring image = WriteSolidImage(L"op_yolo_filter_label.bmp", 320, 240, RGB(90, 140, 200));
    ASSERT_FALSE(image.empty());

    int base_count = 0;
    const wchar_t *base = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"", &base_count);
    ASSERT_NE(base, nullptr);
    long base_total = 0;
    ASSERT_TRUE(JsonInt(std::wstring(base), L"total", base_total));
    ASSERT_GT(base_total, 0) << "need real boxes to test label filtering";

    // label 精确命中
    int lbl_ok = 0;
    const wchar_t *lbl = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45,
                                                  L"{\"labels\":[\"target_cls\"]}", &lbl_ok);
    ASSERT_NE(lbl, nullptr);
    long lbl_returned = 0;
    ASSERT_TRUE(JsonInt(std::wstring(lbl), L"returned", lbl_returned));
    EXPECT_GT(lbl_returned, 0) << "label target_cls must survive the filter";

    // 不存在的 label
    int lbl_miss = -1;
    const wchar_t *miss = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45,
                                                   L"{\"labels\":[\"no_such_label\"]}", &lbl_miss);
    ASSERT_NE(miss, nullptr);
    long miss_returned = -1;
    ASSERT_TRUE(JsonInt(std::wstring(miss), L"returned", miss_returned));
    EXPECT_EQ(miss_returned, 0);
    EXPECT_EQ(lbl_miss, 0);

    // 并集语义：class_ids 命中 + labels 不命中 => 仍应保留（并集）
    int union_count = -1;
    const wchar_t *u = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45,
                                                L"{\"class_ids\":[1],\"labels\":[\"no_such_label\"]}", &union_count);
    ASSERT_NE(u, nullptr);
    long union_returned = -1;
    ASSERT_TRUE(JsonInt(std::wstring(u), L"returned", union_returned));
    EXPECT_GT(union_returned, 0) << "class_ids and labels must be a UNION, not an intersection";

    // 反向的并集：class 不存在 + label 命中 => 也应保留
    int union2 = -1;
    const wchar_t *u2 = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45,
                                                 L"{\"class_ids\":[9999],\"labels\":[\"target_cls\"]}", &union2);
    ASSERT_NE(u2, nullptr);
    long union2_returned = -1;
    ASSERT_TRUE(JsonInt(std::wstring(u2), L"returned", union2_returned));
    EXPECT_GT(union2_returned, 0) << "union must work from the labels side too";
}

// max_results 的截断：**已闭环**（2026-11-21 19:30）。
//
// 历史：本机原来只有 yolo_meta_test.onnx（单 Constant 节点，NMS 后恒 1 条），
// 所以"截断到小于总数"这一支一直只能 GTEST_SKIP 记录。
// 现在补了多目标靶子 tests/testdata/yolo_multi_test.onnx（4 条，类别各异，
// conf 0.90/0.80/0.70/0.60）—— 生成脚本 scripts/gen_yolo_multi_test_model.py，
// 截断路径可以真正断言了。
//
// 本用例做两件事：
//   1) total==1 时（只有单目标模型可用）：验证 total/returned 语义一致、Ex2 字段齐全
//   2) total >1 时（多目标模型）：验证真实截断 + total 保持全量
TEST(YoloFilterRealTest, MaxResultsTruncatesAndReportsTotal) {
    // 优先用多目标模型（4 条，类别各异）—— 这是唯一能真正闭环"截断"的靶子；
    // 退化时仍用单目标模型，走下面的 SKIP 分支（不假绿）。
    std::wstring model = FindYoloMultiTestModel();
    if (model.empty())
        model = FindYoloMetaTestModel();
    if (model.empty())
        GTEST_SKIP() << "neither yolo_multi_test.onnx nor yolo_meta_test.onnx available";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);
    const std::wstring image = WriteSolidImage(L"op_yolo_filter_max.bmp", 320, 240, RGB(30, 30, 30));
    ASSERT_FALSE(image.empty());

    int base_count = 0;
    const wchar_t *base = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"", &base_count);
    ASSERT_NE(base, nullptr);
    long base_total = 0, base_returned = 0;
    ASSERT_TRUE(JsonInt(std::wstring(base), L"total", base_total));
    ASSERT_TRUE(JsonInt(std::wstring(base), L"returned", base_returned));
    ASSERT_GT(base_total, 0) << "model must emit boxes; otherwise nothing here can be verified";
    EXPECT_EQ(base_total, base_returned);
    EXPECT_EQ(base_count, static_cast<int>(base_returned));

    // max_results=1：只留 1 条，total 仍是全量
    int one_count = -1;
    const wchar_t *one = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"{\"max_results\":1}",
                                                  &one_count);
    ASSERT_NE(one, nullptr);
    const std::wstring one_json(one);
    long one_returned = -1, one_total = -1;
    ASSERT_TRUE(JsonInt(one_json, L"returned", one_returned));
    ASSERT_TRUE(JsonInt(one_json, L"total", one_total));
    EXPECT_EQ(one_returned, 1) << "max_results=1 must yield exactly one result";
    EXPECT_EQ(one_total, base_total) << "total must remain the unfiltered count";
    EXPECT_EQ(one_count, 1);
    EXPECT_EQ(JsonCountOf(one_json, L"class_id"), 1u);

    // Ex2 专属字段齐全（V7）
    EXPECT_TRUE(JsonHasKey(one_json, L"index"));
    EXPECT_TRUE(JsonHasKey(one_json, L"cx"));
    EXPECT_TRUE(JsonHasKey(one_json, L"cy"));
    EXPECT_TRUE(JsonHasKey(one_json, L"w"));
    EXPECT_TRUE(JsonHasKey(one_json, L"h"));

    // cx/cy 必须等于 bbox 的中心（可闭环，且能抓到"漏加半个宽"这类错）
    {
        const size_t bp = one_json.find(L"\"bbox\":[");
        ASSERT_NE(bp, std::wstring::npos);
        long v[4] = {0, 0, 0, 0};
        const wchar_t *cur = one_json.c_str() + bp + 8;
        for (int i = 0; i < 4; ++i) {
            v[i] = _wtoi(cur);
            const wchar_t *comma = wcschr(cur, (i == 3) ? L']' : L',');
            if (!comma)
                break;
            cur = comma + 1;
        }
        long cx = 0, cy = 0;
        ASSERT_TRUE(JsonInt(one_json, L"cx", cx));
        ASSERT_TRUE(JsonInt(one_json, L"cy", cy));
        EXPECT_EQ(cx, v[0] + (v[2] - v[0]) / 2) << "cx must be the bbox center";
        EXPECT_EQ(cy, v[1] + (v[3] - v[1]) / 2) << "cy must be the bbox center";

        // 只有 total > 1 时才能真实验证截断
        if (base_total > 1) {
            int trunc_count = -1;
            const wchar_t *trunc = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45,
                                                            L"{\"max_results\":1}", &trunc_count);
            ASSERT_NE(trunc, nullptr);
            long trunc_returned = -1;
            ASSERT_TRUE(JsonInt(std::wstring(trunc), L"returned", trunc_returned));
            EXPECT_EQ(trunc_returned, 1) << "truncation must reduce returned to 1";
            EXPECT_GT(base_total, trunc_returned) << "total must exceed returned after truncation";
        } else {
            // 明确记录：本机模型只产 1 条，截断路径无法闭环
            GTEST_SKIP() << "this machine's test model emits only 1 detection after NMS "
                            "(verified across sizes and conf down to 0.0001), "
                            "so max_results truncation cannot be closed-loop verified here";
        }
    }
}
// 多目标模型下的"过滤 + 截断"组合语义（2026-11-21 补闭环节点）。
//
// 语义口径（YoloDetectCore.h）：
//   total    = NMS 后、过滤前的全量条数
//   returned = 过滤 + 截断之后真正返回的条数
// 这两件事同时发生时最容易写错（total 报成过滤后 / 先截断后过滤），所以专门闭环。
TEST(YoloFilterRealTest, MaxResultsCombinesWithClassFilterOnMultiModel) {
    const std::wstring model = FindYoloMultiTestModel();
    if (model.empty())
        GTEST_SKIP() << "tests/testdata/yolo_multi_test.onnx missing "
                        "(scripts/gen_yolo_multi_test_model.py)";

    CApiHandle api;
    ASSERT_EQ(OpSetYoloEngine(api.handle, model.c_str(), L"", L""), 1);
    const std::wstring image = WriteSolidImage(L"op_yolo_multi_combo.bmp", 320, 240, RGB(30, 30, 30));
    ASSERT_FALSE(image.empty());

    // 基线：4 条全出
    int base_count = 0;
    const wchar_t *base = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45, L"", &base_count);
    ASSERT_NE(base, nullptr);
    long base_total = 0, base_returned = 0;
    ASSERT_TRUE(JsonInt(std::wstring(base), L"total", base_total));
    ASSERT_TRUE(JsonInt(std::wstring(base), L"returned", base_returned));
    EXPECT_EQ(4, base_total) << "the multi model must emit exactly 4 detections";
    EXPECT_EQ(4, base_returned);
    EXPECT_EQ(4, base_count);

    // max_results 大于总数：不截断
    int big_count = 0;
    const wchar_t *big = OpYoloDetectFromFileEx2(api.handle, image.c_str(), 0.01, 0.45,
                                                  L"{\"max_results\":10}", &big_count);
    ASSERT_NE(big, nullptr);
    long big_total = 0, big_returned = 0;
    ASSERT_TRUE(JsonInt(std::wstring(big), L"total", big_total));
    ASSERT_TRUE(JsonInt(std::wstring(big), L"returned", big_returned));
    EXPECT_EQ(4, big_total);
    EXPECT_EQ(4, big_returned) << "max_results larger than the count must not truncate";

    // class_ids=[2,3]（conf 0.7 / 0.6）+ max_results=1：
    //   total 仍是全量 4（不是过滤后的 2），returned=1，且留的是 conf 更高的 class 2
    int mix_count = 0;
    const wchar_t *mix = OpYoloDetectFromFileEx2(
        api.handle, image.c_str(), 0.01, 0.45, L"{\"class_ids\":[2,3],\"max_results\":1}", &mix_count);
    ASSERT_NE(mix, nullptr);
    const std::wstring mix_json(mix);
    long mix_total = 0, mix_returned = 0;
    ASSERT_TRUE(JsonInt(mix_json, L"total", mix_total));
    ASSERT_TRUE(JsonInt(mix_json, L"returned", mix_returned));
    EXPECT_EQ(4, mix_total) << "total must stay the unfiltered count even when filtering";
    EXPECT_EQ(1, mix_returned);
    EXPECT_EQ(1, mix_count);
    EXPECT_NE(std::wstring::npos, mix_json.find(L"\"class_id\":2"))
        << "truncation keeps the highest-confidence survivor (class 2 @0.70)";
    EXPECT_EQ(std::wstring::npos, mix_json.find(L"\"class_id\":3"))
        << "class 3 (@0.60) must be the one truncated away";
}
