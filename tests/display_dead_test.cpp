// 卡屏检测自测（P2 第二十四批，2026-10-16）：OpIsDisplayDead。
// 静态窗口 → 连续帧哈希全同 → 返回 1（卡死）；
// 动画窗口 → 帧间有变化 → 返回 0（存活）。
// 关键约束：IsDisplayDead 内部 Sleep 期间主线程无法泵消息，所以动画窗口必须建在
// 专用工作线程上跑自己的消息循环（WM_TIMER 换色 + WM_PAINT 重绘都在工作线程完成）。

#include "test_support.h"

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <atomic>
#include <thread>

using test_support::ColorPulseWindow;

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 在工作线程上创建 ColorPulseWindow 并泵消息，直到 Stop。
struct WorkerWindow {
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    std::thread th;

    void Start(bool animate) {
        th = std::thread([this, animate] {
            ColorPulseWindow window;
            window.Create(animate);
            hwnd.store(window.hwnd);
            ready.store(true);
            MSG msg = {};
            while (!stop.load()) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            DestroyWindow(window.hwnd);
            window.hwnd = nullptr;
        });
        while (!ready.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    void Stop() {
        stop.store(true);
        if (th.joinable())
            th.join();
    }

    ~WorkerWindow() {
        Stop();
    }
};

int BindGdi(op_handle handle, HWND hwnd) {
    return OpBindWindow(handle, reinterpret_cast<intptr_t>(hwnd), L"gdi", L"windows", L"windows", 0);
}

TEST(DisplayDeadTest, StaticWindowReportsDead) {
    WorkerWindow worker;
    worker.Start(false);
    ASSERT_TRUE(IsWindow(worker.hwnd.load()));
    // 等工作线程完成首帧绘制：否则第一帧可能是未绘制的黑底、后续帧才是红色，
    // 帧间哈希不一致会被误判为"存活"（全量回归高负载下尤甚）。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CApiHandle api;
    ASSERT_EQ(BindGdi(api.handle, worker.hwnd.load()), 1);
    // 静态画面：3 帧 x 80ms 全同 → 卡死
    EXPECT_EQ(OpIsDisplayDead(api.handle, 10, 10, 120, 100, 3, 80), 1);
    EXPECT_EQ(OpUnBindWindow(api.handle), 1);
}

TEST(DisplayDeadTest, AnimatedWindowReportsAlive) {
    WorkerWindow worker;
    worker.Start(true); // 定时器每 120ms 轮换红绿蓝
    ASSERT_TRUE(IsWindow(worker.hwnd.load()));
    // 等工作线程先泵起来、首帧绘制完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CApiHandle api;
    ASSERT_EQ(BindGdi(api.handle, worker.hwnd.load()), 1);
    // 4 帧 x 150ms ≈ 450ms 跨度 ≥ 3 次换色，必捕捉到帧间变化 → 存活
    EXPECT_EQ(OpIsDisplayDead(api.handle, 10, 10, 120, 100, 4, 150), 0);
    EXPECT_EQ(OpUnBindWindow(api.handle), 1);
}

TEST(DisplayDeadTest, FailureCasesReturnNotDead) {
    CApiHandle api;
    WorkerWindow worker;
    worker.Start(false);
    ASSERT_TRUE(IsWindow(worker.hwnd.load()));
    ASSERT_EQ(BindGdi(api.handle, worker.hwnd.load()), 1);
    // 非法区域（x2<=x1，截图为空）→ 保守返回 0
    EXPECT_EQ(OpIsDisplayDead(api.handle, 100, 10, 50, 100, 2, 50), 0);
    // 空句柄 → call_ret 兜底 0
    EXPECT_EQ(OpIsDisplayDead(nullptr, 10, 10, 120, 100, 2, 50), 0);
    EXPECT_EQ(OpUnBindWindow(api.handle), 1);
}

} // namespace
