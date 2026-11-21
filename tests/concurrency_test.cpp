// 并发安全基线（2026-11-21）。
//
// 为什么需要：DLL 里 OCR 服务 / 图像句柄注册表 / 模板缓存都有锁，锁的设计看起来是对的
// （OCR 推理拿 shared_ptr 快照、不全局串行；图像注册表用 shared_mutex），
// 但**没有任何用例真的多线程调过同一套 API** —— 宿主（GMAJ）恰恰是多窗口多线程，
// 这是最贴近真实用法的空白。
//
// 三条路径分开测，因为它们共享的东西不同：
//   1) 多句柄并发：线程各自 OpCreate，验证全局资源（缓存/注册表）不被踩坏
//   2) 同句柄并发：多线程共用一个 op_handle，验证 handle 内部状态
//   3) 同引擎并发：多线程共用同一个 OCR 引擎实例（会话是共享的）
//
// 已知且有意为之：`OcrService.h` 的 profile 注释写明"并发时数值对应任意一次调用，
// 最后写入者胜"（加锁采样会污染计时）。所以本文件**不断言 profile 数值**，
// 只断言"不崩 + 结果正确"。

#include "test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <op_c_api.h>

namespace {

bool SkipOcr() {
    return !test_support::GetEnvString(L"OP_SKIP_OCR_TESTS").empty();
}

// 每线程独立句柄的并发：颜色取值
TEST(ConcurrencyTest, MultiHandleColorReadInParallel) {
    constexpr int kThreads = 8;
    constexpr int kRounds = 20;
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&errors, t]() {
            op_handle h = OpCreate();
            if (!h) {
                ++errors;
                return;
            }
            // 每线程一个独立颜色，避免"全都读同一个值"掩盖串扰
            const unsigned char r = static_cast<unsigned char>(10 + t * 20);
            std::vector<unsigned char> px(static_cast<size_t>(16) * 16 * 4, 255);
            // BGRA 顺序：R 在 [2]，所以 RRGGBB 的高两位就是 r
            for (size_t i = 0; i < px.size(); i += 4) {
                px[i + 0] = 0;
                px[i + 1] = 0;
                px[i + 2] = r;
                px[i + 3] = 255;
            }
            for (int round = 0; round < kRounds; ++round) {
                const wchar_t *handle_name = OpLoadImageMem(h, reinterpret_cast<const char *>(px.data()), 16, 16,
                                                            0, L"bgra");
                if (!handle_name || handle_name[0] == L'\0') {
                    ++errors;
                    break;
                }
                const std::wstring name(handle_name);
                // 读回颜色：RGBA -> 期望 RRGGBB 前缀与本线程颜色一致
                const wchar_t *hex = OpGetColorFromMem(h, name.c_str(), 8, 8);
                if (!hex) {
                    ++errors;
                    OpFreeImage(h, name.c_str());
                    break;
                }
                const std::wstring got(hex);
                wchar_t want[8] = {0};
                ::swprintf_s(want, L"%02X", static_cast<unsigned>(r));
                if (got.size() < 2 || got.compare(0, 2, want) != 0)
                    ++errors;
                OpFreeImage(h, name.c_str());
            }
            OpDestroy(h);
        });
    }
    for (auto &w : workers)
        w.join();
    EXPECT_EQ(0, errors.load()) << "多句柄并发读色出现错误（结果错乱或句柄分配冲突）";
}

// 同一句柄并发注册/释放图像：验证注册表（shared_mutex 保护）不丢不重
TEST(ConcurrencyTest, SharedHandleImageRegistryNoLeak) {
    constexpr int kThreads = 4;
    constexpr int kRounds = 15;
    op_handle shared = OpCreate();
    ASSERT_NE(nullptr, shared);
    // 注册表是进程级全局的（前面用例可能还留着句柄），所以断言用**增量基线**
    const int baseline = OpGetImageCount(shared);
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&errors, shared, t]() {
            const int w = 8 + t;
            std::vector<unsigned char> px(static_cast<size_t>(w) * w * 4, 128);
            for (int round = 0; round < kRounds; ++round) {
                const wchar_t *name = OpLoadImageMem(shared, reinterpret_cast<const char *>(px.data()), w, w, 0,
                                                     L"bgra");
                if (!name || !name[0]) {
                    ++errors;
                    break;
                }
                const std::wstring hn(name);
                int width = 0, height = 0, channels = 0;
                if (OpGetImageSize(shared, hn.c_str(), &width, &height, &channels) != 1 || width != w ||
                    height != w)
                    ++errors;
                if (OpFreeImage(shared, hn.c_str()) != 1)
                    ++errors;
            }
        });
    }
    for (auto &w : workers)
        w.join();

    EXPECT_EQ(0, errors.load()) << "同句柄并发注册/释放出现错误";
    EXPECT_EQ(baseline, OpGetImageCount(shared))
        << "并发注册/释放后必须回到基线（只增不减 = 泄漏）";
    OpDestroy(shared);
}

// 同一句柄并发内存找色：读路径并发（缓存是 shared_mutex）
TEST(ConcurrencyTest, SharedHandleFindColorFromMemParallel) {
    op_handle shared = OpCreate();
    ASSERT_NE(nullptr, shared);
    ASSERT_EQ(1, OpSetPath(shared, L"."));

    const int w = 40, h = 40;
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4, 0);
    // 中心 4x4 放一块纯红（RGB F00000 -> BGRA 00 00 F0 FF）
    for (int y = 18; y < 22; ++y)
        for (int x = 18; x < 22; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            px[i + 0] = 0x00;
            px[i + 1] = 0x00;
            px[i + 2] = 0xF0;
            px[i + 3] = 0xFF;
        }
    const wchar_t *name = OpLoadImageMem(shared, reinterpret_cast<const char *>(px.data()), w, h, 0, L"bgra");
    ASSERT_NE(nullptr, name);
    ASSERT_NE(L'\0', name[0]);
    const std::wstring image(name);

    constexpr int kThreads = 6;
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&errors, shared, &image]() {
            for (int round = 0; round < 10; ++round) {
                int x = -1, y = -1;
                const int found = OpFindColorFromMem(shared, image.c_str(), 0, 0, 0, 0, L"F00000", 0.95, 0, &x, &y);
                if (found != 1 || x != 18 || y != 18)
                    ++errors;
            }
        });
    }
    for (auto &w : workers)
        w.join();
    EXPECT_EQ(0, errors.load()) << "并发热路径（内存找色）结果必须是确定值 (18,18)";
    OpFreeImage(shared, image.c_str());
    OpDestroy(shared);
}

// 并发共用同一个 OCR 引擎实例（会话共享 + 在途推理快照）
TEST(ConcurrencyTest, SharedHandleOcrInParallel) {
    if (SkipOcr())
        GTEST_SKIP() << "OP_SKIP_OCR_TESTS 已设置";

    const std::wstring path = test_support::GetTempBmpPath(L"__concurrency_ocr.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(path, L"BP 909"));

    op_handle shared = OpCreate();
    ASSERT_NE(nullptr, shared);
    std::wstring image;
    // 加载到内存句柄，避免多线程同时读文件
    {
        HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(INVALID_HANDLE_VALUE, f);
        std::vector<char> raw;
        char buf[4096];
        DWORD got = 0;
        while (::ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
            raw.insert(raw.end(), buf, buf + got);
        ::CloseHandle(f);
        ASSERT_GT(raw.size(), 100u);
        const wchar_t *name = OpLoadImageBmp(shared, raw.data(), static_cast<int>(raw.size()));
        ASSERT_NE(nullptr, name);
        ASSERT_NE(L'\0', name[0]);
        image = name;
    }

    // 前序用例可能把全局引擎切成带 --charset 白名单的实例（进程级单例），
    // 这里显式切回"全字典 v6"，保证本用例测的是并发而不是前序残留状态。
    ASSERT_EQ(1, OpSetOcrEngine(shared, L"onnx", L"", L""));
    {
        const wchar_t *probe = OpOcrFromMemEx2(shared, image.c_str(), 0, 0, 0, 0, L"", 0.9);
        if (!probe || !probe[0])
            GTEST_SKIP() << "串行 OCR 自检即为空（模型/引擎状态问题），并发无意义";
    }

    constexpr int kThreads = 2;   // 2 路：够触发共享会话，又不把回归拖长
    constexpr int kRounds = 2;
    std::atomic<int> errors{0};
    std::atomic<int> non_empty{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&errors, &non_empty, &image, shared, t]() {
            for (int round = 0; round < kRounds; ++round) {
                // 内存版：屏幕版 OcrEx2 未绑定窗口时恒返回空串，测不出引擎并发
                const wchar_t *json = OpOcrFromMemEx2(shared, image.c_str(), 0, 0, 0, 0, L"", 0.9);
                if (!json) {
                    ++errors;
                    continue;
                }
                const std::wstring out(json);
                if (out.empty())
                    ++errors;
                else
                    ++non_empty;
            }
        });
    }
    for (auto &w : workers)
        w.join();
    EXPECT_EQ(0, errors.load()) << "并发 OCR 返回了空/失败结果";
    EXPECT_GT(non_empty.load(), 0) << "并发 OCR 至少要有一路成功";
    OpDestroy(shared);
    ::DeleteFileW(path.c_str());
}

} // namespace
