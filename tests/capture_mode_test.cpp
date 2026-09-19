#include "test_support.h"

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
