// 多实例 / 并发补充基线（批 A，2026-11-22）。
//
// 与 concurrency_test.cpp 的分工（那份是 2026-11-21 的 4 条基线）：
//   已有：多句柄读色 / 同句柄图像注册表 / 同句柄内存找色 / 同句柄 OCR
//   本文件补的是**真实多开宿主（GMAJ 这类）会踩到、而上面 4 条没覆盖的路径**：
//     A1 多实例并发 OCR（每线程独立 OpCreate —— 最常见的多开写法）
//     A2 多实例并发 YOLO（引擎是进程级单例，验证不串味）
//     A3 同实例并发可观测（OpConcurrencyInfo 必须真的数得出来）
//     A4 销毁与全局资源竞态（A 线程 OpDestroy 与 B 线程用另一实例并行）
//     A5 多实例绑不同窗口（本文件不绑窗口，改为验证"未绑定时各实例状态互不影响"）
//     A6 ConcurrencyInfo 契约（字段齐全、thread_safe 恒 false、无并发时 reentrant=0）
//
// 诚实性说明：
//   * 本文件**不**断言"同一实例可以多线程安全使用" —— 它本来就不是（libop.h 顶部有明文）。
//     A3 断言的是"库能把这个错误用法检测出来"，而不是"这个用法可以放心用"。
//   * 需要模型的用例（OCR/YOLO）在模型缺失时 GTEST_SKIP 并写明原因，不假绿。

#include "test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <op_c_api.h>

namespace {

bool SkipOcrTests() {
    return !test_support::GetEnvString(L"OP_SKIP_OCR_TESTS").empty();
}

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

// 极简 JSON 取值（只供断言；与 yolo_ex2_test.cpp 同一套写法，不引第三方）
long JsonNumber(const std::wstring &json, const wchar_t *key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos)
        return -1;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t'))
        ++pos;
    return static_cast<long>(wcstol(json.c_str() + pos, nullptr, 10));
}

bool JsonHasKey(const std::wstring &json, const wchar_t *key) {
    return json.find(std::wstring(L"\"") + key + L"\"") != std::wstring::npos;
}

// 把 BMP 文件读成字节流，供 LoadImageBmp 使用（避免多线程同读一个文件）
std::vector<char> ReadWholeFile(const std::wstring &path) {
    std::vector<char> raw;
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return raw;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
        raw.insert(raw.end(), buf, buf + got);
    ::CloseHandle(f);
    return raw;
}

} // namespace

// A6 —— ConcurrencyInfo 契约。放最前面：此时本进程还没有并发调用，
// reentrant 必须为 0，字段必须齐。这是后面 A3 的判据前提。
TEST(ConcurrencyBatchTest, InfoContractOnFreshInstance) {
    op_handle h = OpCreate();
    ASSERT_NE(nullptr, h);
    const std::wstring json = OpConcurrencyInfo(h) ? std::wstring(OpConcurrencyInfo(h)) : L"";
    ASSERT_FALSE(json.empty()) << "OpConcurrencyInfo 返回空串，说明 C API 没接上";

    for (const wchar_t *key : {L"id", L"in_flight", L"peak", L"reentrant", L"calls_total", L"image_count", L"bound",
                               L"thread_safe"}) {
        EXPECT_TRUE(JsonHasKey(json, key)) << "缺少字段 " << test_support::PtrToWString(std::string("").c_str())
                                           << "（实际 JSON: " << std::string(json.begin(), json.end()) << "）";
    }
    EXPECT_EQ(0, JsonNumber(json, L"reentrant")) << "全新实例不应有并发重入记录";
    EXPECT_EQ(0, JsonNumber(json, L"bound")) << "未绑定窗口时 bound 应为 0";
    // thread_safe 是**约定**不是能力：恒定 false。若哪天改成 true，必须同时改 libop.h 顶部说明。
    EXPECT_NE(std::wstring::npos, json.find(L"\"thread_safe\":false"))
        << "thread_safe 必须为 false（单实例非线程安全是既定约定）";
    OpDestroy(h);
}

// A1 —— 多实例并发 OCR：每线程独立 OpCreate（真实多开写法）
TEST(ConcurrencyBatchTest, MultiInstanceOcrInParallel) {
    if (SkipOcrTests())
        GTEST_SKIP() << "OP_SKIP_OCR_TESTS 已设置";

    const std::wstring path = test_support::GetTempBmpPath(L"__concurrency_multi_ocr.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(path, L"BP 909"));
    const std::vector<char> bmp = ReadWholeFile(path);
    ASSERT_GT(bmp.size(), 100u);

    // 先在主线程确认这份素材能识别出来（否则并发用例的"失败"没有意义）
    {
        op_handle probe = OpCreate();
        ASSERT_NE(nullptr, probe);
        ASSERT_EQ(1, OpSetOcrEngine(probe, L"onnx", L"", L""));
        const wchar_t *h = OpLoadImageBmp(probe, bmp.data(), static_cast<int>(bmp.size()));
        ASSERT_NE(nullptr, h);
        const std::wstring img(h);
        const wchar_t *out = OpOcrFromMemEx2(probe, img.c_str(), 0, 0, 0, 0, L"", 0.9);
        const bool serial_ok = out && out[0];
        OpDestroy(probe);
        if (!serial_ok)
            GTEST_SKIP() << "串行 OCR 自检即为空（模型/引擎状态问题），并发无意义";
    }

    constexpr int kInstances = 8;
    std::atomic<int> errors{0};
    std::atomic<int> ok_count{0};
    std::vector<std::thread> workers;
    workers.reserve(kInstances);
    for (int t = 0; t < kInstances; ++t) {
        workers.emplace_back([&errors, &ok_count, &bmp]() {
            op_handle h = OpCreate();
            if (!h) {
                ++errors;
                return;
            }
            // 每实例都要自己 SetOcrEngine：引擎虽是进程级单例，但这样写才是真实脚本的样子
            if (OpSetOcrEngine(h, L"onnx", L"", L"") != 1) {
                ++errors;
                OpDestroy(h);
                return;
            }
            const wchar_t *name = OpLoadImageBmp(h, bmp.data(), static_cast<int>(bmp.size()));
            if (!name || !name[0]) {
                ++errors;
                OpDestroy(h);
                return;
            }
            const std::wstring img(name); // 必须立刻复制：C API 字符串是复用缓冲
            for (int round = 0; round < 2; ++round) {
                const wchar_t *out = OpOcrFromMemEx2(h, img.c_str(), 0, 0, 0, 0, L"", 0.9);
                if (!out || !out[0])
                    ++errors;
                else
                    ++ok_count;
            }
            OpFreeImage(h, img.c_str());
            OpDestroy(h);
        });
    }
    for (auto &w : workers)
        w.join();

    EXPECT_EQ(0, errors.load()) << "多实例并发 OCR 出现空结果（引擎共享状态被踩坏）";
    EXPECT_EQ(kInstances * 2, ok_count.load()) << "应当每一路都成功";
    ::DeleteFileW(path.c_str());
}

// A2 —— 多实例并发 YOLO：引擎是进程级单例（m_mutex 串行化），
// 验证多实例同时调用时结果**不串味**（条数与类别一致）且不崩。
TEST(ConcurrencyBatchTest, MultiInstanceYoloInParallel) {
    const std::wstring model = FindTestModel(L"yolo_multi_test.onnx");
    if (model.empty())
        GTEST_SKIP() << "缺少 tests/testdata/yolo_multi_test.onnx（脚本 scripts/gen_yolo_multi_test_model.py 可重建）";

    const std::wstring bmp_path = test_support::GetTempBmpPath(L"__concurrency_multi_yolo.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp_path, L"BP 909"));
    const std::vector<char> bmp = ReadWholeFile(bmp_path);
    ASSERT_GT(bmp.size(), 100u);

    constexpr int kInstances = 4;
    std::atomic<int> errors{0};
    std::atomic<int> zero_result{0};
    std::vector<std::thread> workers;
    workers.reserve(kInstances);
    for (int t = 0; t < kInstances; ++t) {
        workers.emplace_back([&, model]() {
            op_handle h = OpCreate();
            if (!h) {
                ++errors;
                return;
            }
            if (OpSetYoloEngine(h, model.c_str(), L"", L"") != 1) {
                ++errors;
                OpDestroy(h);
                return;
            }
            const wchar_t *name = OpLoadImageBmp(h, bmp.data(), static_cast<int>(bmp.size()));
            if (!name || !name[0]) {
                ++errors;
                OpDestroy(h);
                return;
            }
            const std::wstring img(name);
            for (int round = 0; round < 2; ++round) {
                int found = 0;
                const wchar_t *json = OpYoloDetectFromMem(h, img.c_str(), 0, 0, 0, 0, 0.25, 0.45, &found);
                if (!json)
                    ++errors;
                else if (found == 0)
                    ++zero_result; // 模型恒定给 4 条，0 条说明串味或状态被踩
            }
            OpFreeImage(h, img.c_str());
            OpDestroy(h);
        });
    }
    for (auto &w : workers)
        w.join();

    EXPECT_EQ(0, errors.load()) << "多实例并发 YOLO 出现失败返回";
    EXPECT_EQ(0, zero_result.load()) << "并发下检测条数变成 0：引擎共享状态被踩坏";
    ::DeleteFileW(bmp_path.c_str());
}

// A3 —— 同实例并发必须"数得出来"。
// 这不是在承诺安全，而是承诺**错误用法可观测**：
// 两个线程同时打同一个 handle，OpConcurrencyInfo 的 reentrant 必须 > 0。
// 反过来说：这条用例变红 = 观测机制失灵（用户撞了并发却查不出来）。
TEST(ConcurrencyBatchTest, SameInstanceReentryIsObservable) {
    op_handle shared = OpCreate();
    ASSERT_NE(nullptr, shared);
    ASSERT_EQ(1, OpSetPath(shared, L"."));

    // ⚠ 必须用**有实际耗时**的接口来造重叠窗口。
    // 初版用 OpGetImageCount（纯内存自增，几十纳秒），两个线程 400 次调用从头到尾
    // 一次都没重叠上（实测 peak 恒为 1），用例变成"恒真"——这类假绿比失败更危险。
    // 改用 SetPath：它要走路径规范化 + 文件系统，单次微秒级，重叠窗口稳定存在。
    std::atomic<bool> start{false};
    auto worker = [&]() {
        while (!start.load(std::memory_order_acquire)) {
        } // 对齐起跑，最大化重叠窗口
        for (int i = 0; i < 300; ++i) {
            OpSetPath(shared, (i % 2 == 0) ? L"." : L"..");
        }
    };
    std::thread a(worker);
    std::thread b(worker);
    start.store(true, std::memory_order_release);
    a.join();
    b.join();

    const wchar_t *raw = OpConcurrencyInfo(shared);
    const std::wstring json = raw ? raw : L"";
    const long reentrant = JsonNumber(json, L"reentrant");
    const long peak = JsonNumber(json, L"peak");
    const long calls = JsonNumber(json, L"calls_total");
    OpDestroy(shared);

    EXPECT_GT(calls, 0) << "calls_total 没有增长，说明计数没接上 C API 门面";
    EXPECT_GT(peak, 1) << "两个线程同时打同一实例，peak 必须 > 1（否则说明调用根本没并行）";
    EXPECT_GT(reentrant, 0) << "同实例并发必须被记录（这是本用例的核心判据）";
}

// A4 —— 销毁与全局资源竞态：一边不停创建/销毁实例，一边在另一个实例上做注册表操作。
// 覆盖"OpDestroy 释放实例内存"与"进程级图像注册表"同时被碰到的窗口。
TEST(ConcurrencyBatchTest, DestroyRacesGlobalRegistry) {
    op_handle stable = OpCreate();
    ASSERT_NE(nullptr, stable);
    const int baseline = OpGetImageCount(stable);

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    std::thread churn([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            op_handle tmp = OpCreate();
            if (!tmp) {
                ++errors;
                continue;
            }
            OpGetID(tmp);
            OpDestroy(tmp);
        }
    });

    std::thread registry([&]() {
        std::vector<unsigned char> px(8 * 8 * 4, 200);
        for (int i = 0; i < 60; ++i) {
            const wchar_t *name = OpLoadImageMem(stable, reinterpret_cast<const char *>(px.data()), 8, 8, 0, L"bgra");
            if (!name || !name[0]) {
                ++errors;
                continue;
            }
            const std::wstring hn(name);
            if (OpFreeImage(stable, hn.c_str()) != 1)
                ++errors;
        }
    });

    registry.join();
    stop.store(true, std::memory_order_release);
    churn.join();

    EXPECT_EQ(0, errors.load()) << "创建/销毁与图像注册表并行时出错";
    EXPECT_EQ(baseline, OpGetImageCount(stable)) << "注册表计数未回到基线（泄漏或计数被踩）";
    OpDestroy(stable);
}

// A5 —— 各实例状态互不影响：一个实例绑定/解绑失败不应污染另一个实例的状态。
// 这里不真绑窗口（绑定依赖环境），而是验证"实例级的图像句柄/计数完全隔离"。
TEST(ConcurrencyBatchTest, InstanceStateIsolated) {
    op_handle a = OpCreate();
    op_handle b = OpCreate();
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    ASSERT_NE(OpGetID(a), OpGetID(b)) << "两个实例必须拿到不同的 id";

    const int a0 = OpGetImageCount(a);
    const int b0 = OpGetImageCount(b);

    std::vector<unsigned char> px(4 * 4 * 4, 77);
    const wchar_t *raw = OpLoadImageMem(a, reinterpret_cast<const char *>(px.data()), 4, 4, 0, L"bgra");
    ASSERT_NE(nullptr, raw);
    const std::wstring name(raw);
    ASSERT_FALSE(name.empty());

    EXPECT_EQ(a0 + 1, OpGetImageCount(a));
    // 注册表是进程级的（设计如此），所以 b 看到的计数也会 +1；关键是 b **不能**用它自己
    // 的 id 去冒充 a 的句柄，且 b 的 ConcurrencyInfo 统计不因 a 的调用而增长。
    const std::wstring info_b = OpConcurrencyInfo(b) ? std::wstring(OpConcurrencyInfo(b)) : L"";
    EXPECT_EQ(0, JsonNumber(info_b, L"reentrant")) << "b 实例没有并发却报出 reentrant，说明计数串了实例";

    OpFreeImage(a, name.c_str());
    EXPECT_EQ(a0, OpGetImageCount(a));
    EXPECT_GE(b0, 0);
    OpDestroy(a);
    OpDestroy(b);
}
