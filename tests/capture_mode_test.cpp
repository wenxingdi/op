#include "test_support.h"

#include <atomic>
#include <thread>

using test_support::ColorPulseWindow;

namespace {

void PumpMessagesFor(int milliseconds) {
    const auto deadline = GetTickCount64() + milliseconds;
    MSG msg = {};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

// 屏幕坐标处最上层的根窗口（用于自检遮挡场景是否真的成立）。
HWND RootWindowAt(POINT pt) {
    HWND hit = WindowFromPoint(pt);
    if (!hit)
        return nullptr;
    HWND root = GetAncestor(hit, GA_ROOT);
    return root ? root : hit;
}

} // namespace

class CaptureModeTest : public ::testing::Test {};

// gdi / gdi2 / dx2 三条 GDI 家族通道都必须取到窗口真实内容，且随窗口重绘实时更新。
//
// 判别力说明：截图接口对“取不到内容”是**静默**的 —— 绑定与截图都返回 1、BMP 尺寸与字节数全正常，
// 只是内容整幅全黑。此前 gdi 走 PrintWindow(flags=0)，对 DirectComposition 合成的窗口
// （UWP / Chromium 系：系统计算器、钉钉之外的 Edge/Electron 壳等）就恒定返回全黑图。
// 因此这里必须断言**像素颜色**，只断言返回值等于没断言。
TEST_F(CaptureModeTest, GdiFamilyCapturesWindowContentForAllModes) {
    ColorPulseWindow window;
    ASSERT_TRUE(window.Create(false));
    PumpMessagesFor(200);

    for (const wchar_t *display : {L"gdi", L"gdi2", L"dx2"}) {
        op::Op op;
        long ret = 0;
        op.SetShowErrorMsg(3, &ret);

        ret = 0;
        op.BindWindow((long)(intptr_t)window.hwnd, display, L"windows", L"windows", 0, &ret);
        ASSERT_EQ(ret, 1) << display;

        window.SetColor(RGB(255, 0, 0));
        PumpMessagesFor(200);
        std::wstring color;
        op.GetColor(60, 60, color);
        EXPECT_EQ(color, L"FF0000") << display << " 取到的不是窗口内容（全黑/错图）";

        // 重绘后必须取到新内容：证明拿到的是实时帧而非缓存帧
        window.SetColor(RGB(0, 255, 0));
        PumpMessagesFor(200);
        op.GetColor(60, 60, color);
        EXPECT_EQ(color, L"00FF00") << display;

        long unbind_ret = 0;
        op.UnBindWindow(&unbind_ret);
        EXPECT_EQ(unbind_ret, 1) << display;
        PumpMessagesFor(100);
    }

    DestroyWindow(window.hwnd);
    window.hwnd = nullptr;
    PumpMessagesFor(200);
}

// gdi 取的是窗口自身内容：窗口被完全遮住时仍必须取到自己的画面（穿透遮挡、不依赖 Z 序）。
// 对照组：用同尺寸的遮挡窗口盖满目标窗口并以蓝色填充，若实现退化成“屏幕所见”就会读到 0000FF。
TEST_F(CaptureModeTest, GdiCaptureIgnoresOcclusion) {
    ColorPulseWindow target;
    ASSERT_TRUE(target.Create(false));
    target.SetColor(RGB(255, 0, 0));
    PumpMessagesFor(200);

    RECT rc = {};
    ASSERT_TRUE(GetWindowRect(target.hwnd, &rc) != FALSE);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    ColorPulseWindow occluder;
    ASSERT_TRUE(occluder.Create(false));
    occluder.SetColor(RGB(0, 0, 255));
    ASSERT_TRUE(SetWindowPos(occluder.hwnd, HWND_TOPMOST, rc.left, rc.top, width, height,
                             SWP_SHOWWINDOW | SWP_NOACTIVATE) != FALSE);
    PumpMessagesFor(300);

    // 自检：遮挡必须真的成立，否则本用例没有判别力
    const POINT center = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    ASSERT_NE(RootWindowAt(center), target.hwnd) << "遮挡未生效，用例失效";

    op::Op op;
    long ret = 0;
    op.SetShowErrorMsg(3, &ret);

    ret = 0;
    op.BindWindow((long)(intptr_t)target.hwnd, L"gdi", L"windows", L"windows", 0, &ret);
    ASSERT_EQ(ret, 1);

    std::wstring color;
    op.GetColor(60, 60, color);
    EXPECT_EQ(color, L"FF0000") << "gdi 读到遮挡物颜色，说明退化为“屏幕所见”";

    long unbind_ret = 0;
    op.UnBindWindow(&unbind_ret);
    EXPECT_EQ(unbind_ret, 1);

    DestroyWindow(occluder.hwnd);
    occluder.hwnd = nullptr;
    DestroyWindow(target.hwnd);
    target.hwnd = nullptr;
    PumpMessagesFor(200);
}

// ---------------- 绑定微调批：GetFPS / DownCpu ----------------

TEST_F(CaptureModeTest, GetFpsReturnsZeroWhenUnbound) {
    op::Op op;
    long ret = -1;
    op.GetFPS(&ret);
    EXPECT_EQ(ret, 0);
}

// GetFPS 判别力：GDI 后端 frameId 随每次抓帧递增。worker 线程以 ~50Hz 抓帧时,
// 1 秒采样窗口内 frameId 必须有可观增量。恒返回 0 / 恒返回 60 的错误实现都会 FAIL。
TEST_F(CaptureModeTest, GetFpsReflectsCaptureRateOnGdi) {
    ColorPulseWindow window;
    ASSERT_TRUE(window.Create(false));
    PumpMessagesFor(100);

    op::Op op;
    long ret = 0;
    op.BindWindow((long)(intptr_t)window.hwnd, L"gdi", L"windows", L"windows", 0, &ret);
    ASSERT_EQ(ret, 1);

    // 预热一次抓帧:GDI 后端的 FrameInfo 在首次截图前为空(hwnd=0),先初始化再采样
    std::wstring prime;
    op.GetColor(10, 10, prime);

    std::atomic<bool> stop{false};
    std::thread worker([&]() {
        std::wstring color;
        while (!stop.load()) {
            op.GetColor(10, 10, color);
            Sleep(20);
        }
    });

    op.GetFPS(&ret);
    stop.store(true);
    worker.join();

    EXPECT_GE(ret, 5) << "采样窗口内抓帧频率应可测(恒 0 实现 FAIL)";
    EXPECT_LE(ret, 200) << "FPS 超出合理上界(疑似返回了毫秒数等错误量纲)";

    op.UnBindWindow(&ret);
    DestroyWindow(window.hwnd);
    window.hwnd = nullptr;
    PumpMessagesFor(100);
}

// DownCpu 判别力:未降载时 5 次小窗截图总耗时远小于 150ms;rate=30 后每次截图强制延时 30ms,
// 总耗时必须 >= 150ms。恒不延时 / 恒延时的错误实现必有一条 FAIL。
TEST_F(CaptureModeTest, DownCpuAddsDelayAfterEachCapture) {
    ColorPulseWindow window;
    ASSERT_TRUE(window.Create(false));
    PumpMessagesFor(100);

    op::Op op;
    long ret = 0;
    op.BindWindow((long)(intptr_t)window.hwnd, L"gdi", L"windows", L"windows", 0, &ret);
    ASSERT_EQ(ret, 1);

    auto capture5 = [&]() -> long long {
        std::wstring color;
        const auto t0 = GetTickCount64();
        for (int i = 0; i < 5; i++)
            op.GetColor(10, 10, color);
        return static_cast<long long>(GetTickCount64() - t0);
    };

    const auto baseline_ms = capture5();
    EXPECT_LT(baseline_ms, 150) << "前置条件:未降载时 5 次截图应远快于 150ms,实际 " << baseline_ms << "ms";

    op.DownCpu(0, 30, &ret);
    ASSERT_EQ(ret, 1);
    const auto slowed_ms = capture5();
    EXPECT_GE(slowed_ms, 150) << "降载 rate=30 后 5 次截图应至少延时 150ms,实际 " << slowed_ms << "ms";

    // 复位为 0,避免会话残留影响后续用例
    op.DownCpu(0, 0, &ret);
    ASSERT_EQ(ret, 1);
    const auto reset_ms = capture5();
    EXPECT_LT(reset_ms, 150) << "rate=0 复位后延时应消失,实际 " << reset_ms << "ms";

    op.UnBindWindow(&ret);
    DestroyWindow(window.hwnd);
    window.hwnd = nullptr;
}

TEST_F(CaptureModeTest, DownCpuValidatesTypeAndClampsRate) {
    op::Op op;
    long ret = 1;
    op.DownCpu(2, 10, &ret);
    EXPECT_EQ(ret, 0) << "非法 type=2 应拒绝";
    ret = 1;
    op.DownCpu(-1, 10, &ret);
    EXPECT_EQ(ret, 0) << "非法 type=-1 应拒绝";
    op.DownCpu(1, 500, &ret);
    EXPECT_EQ(ret, 1) << "rate 超界应钳制而非拒绝";
}
