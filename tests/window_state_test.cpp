#include "test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <windows.h>

#include <imm.h>

using test_support::SendStringWindow;

namespace {

// 顶层可交互窗口（带 WS_EX_TOPMOST，用于验证 SetWindowTransparent 不清扩展样式）。
struct TopmostWindow {
    HWND hwnd = nullptr;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool Create() {
        const wchar_t *cls = L"op_test_topmost_cls";
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        if (::GetClassInfoW(wc.hInstance, cls, &wc) == 0) {
            wc.lpfnWndProc = WndProc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = cls;
            if (::RegisterClassW(&wc) == 0)
                return false;
        }
        hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls, L"op_test_topmost",
                                 WS_OVERLAPPEDWINDOW, 100, 100, 320, 240, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr)
            return false;
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);
        return true;
    }

    ~TopmostWindow() {
        if (hwnd != nullptr)
            ::DestroyWindow(hwnd);
    }
};

} // namespace

// W1 判别力用例：调透明前窗口是置顶的，调一次透明后 WS_EX_TOPMOST 必须仍在。
// 旧实现 SetWindowLong(GWL_EXSTYLE, 0x80001) 会清掉 TOPMOST/TOOLWINDOW，此用例钉死修复。
TEST(WindowStateTest, SetWindowTransparentPreservesTopmostStyle) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());

    const auto before = ::GetWindowLongPtrW(wnd.hwnd, GWL_EXSTYLE);
    ASSERT_TRUE((before & WS_EX_TOPMOST) != 0);

    long ret = 0;
    op.SetWindowTransparent(reinterpret_cast<LONG_PTR>(wnd.hwnd), 200, &ret);
    EXPECT_EQ(ret, 1);

    const auto after = ::GetWindowLongPtrW(wnd.hwnd, GWL_EXSTYLE);
    EXPECT_TRUE((after & WS_EX_TOPMOST) != 0) << "SetWindowTransparent cleared WS_EX_TOPMOST";
    EXPECT_TRUE((after & WS_EX_TOOLWINDOW) != 0) << "SetWindowTransparent cleared WS_EX_TOOLWINDOW";

    // 透明度本身生效：LWA_ALPHA 且 alpha == 200
    COLORREF key = 0;
    BYTE alpha = 0;
    DWORD flags = 0;
    ASSERT_TRUE(::GetLayeredWindowAttributes(wnd.hwnd, &key, &alpha, &flags) != FALSE);
    EXPECT_TRUE((flags & LWA_ALPHA) != 0);
    EXPECT_EQ(alpha, 200);
}

TEST(WindowStateTest, SetWindowTransparentRejectsInvalidHwnd) {
    op::Op op;
    long ret = 1;
    op.SetWindowTransparent(0x7fff, 128, &ret);
    EXPECT_EQ(ret, 0);
}

// GetWindowState 常用 flag 组合（自建置顶可见窗口，全可确定性断言）。
TEST(WindowStateTest, GetWindowStateFlagsOnTopmostWindow) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());
    const auto hwnd = reinterpret_cast<LONG_PTR>(wnd.hwnd);

    long ret = 0;
    op.GetWindowState(hwnd, 0, &ret);
    EXPECT_EQ(ret, 1); // 存在
    op.GetWindowState(hwnd, 2, &ret);
    EXPECT_EQ(ret, 1); // 可见
    op.GetWindowState(hwnd, 4, &ret);
    EXPECT_EQ(ret, 0); // 未最大化
    op.GetWindowState(hwnd, 5, &ret);
    EXPECT_EQ(ret, 1); // 置顶
    op.GetWindowState(hwnd, 7, &ret);
    EXPECT_EQ(ret, 1); // 可用（未禁用）
}

TEST(WindowStateTest, SetWindowStateMaximizeThenRestore) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());
    const auto hwnd = reinterpret_cast<LONG_PTR>(wnd.hwnd);

    long ret = 0;
    op.SetWindowState(hwnd, 4, &ret); // 最大化
    EXPECT_EQ(ret, 1);
    op.GetWindowState(hwnd, 4, &ret);
    EXPECT_EQ(ret, 1);

    op.SetWindowState(hwnd, 12, &ret); // 恢复并激活
    EXPECT_EQ(ret, 1);
    op.GetWindowState(hwnd, 4, &ret);
    EXPECT_EQ(ret, 0);
}

TEST(WindowStateTest, SetWindowStateEnableDisable) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());
    const auto hwnd = reinterpret_cast<LONG_PTR>(wnd.hwnd);

    long ret = 0;
    op.SetWindowState(hwnd, 10, &ret); // 禁止
    EXPECT_EQ(ret, 1);
    op.GetWindowState(hwnd, 7, &ret);
    EXPECT_EQ(ret, 0);

    op.SetWindowState(hwnd, 11, &ret); // 取消禁止
    EXPECT_EQ(ret, 1);
    op.GetWindowState(hwnd, 7, &ret);
    EXPECT_EQ(ret, 1);
}

// 剪贴板往返（SetClipboard 写 ANSI / GetClipboard 读回）。
TEST(WindowStateTest, ClipboardRoundTripAscii) {
    op::Op op;
    long ret = 0;
    op.SetClipboard(L"op_clip_42", &ret);
    ASSERT_EQ(ret, 1);

    std::wstring back;
    op.GetClipboard(back);
    EXPECT_EQ(back, L"op_clip_42");
}

// 层叠布局判别力：第 i 个窗口左上角 = 起点 + i*(gap_x, gap_y)。
TEST(WindowStateTest, LayoutWindowsCascadeStepsWindows) {
    op::Op op;
    TopmostWindow w0, w1, w2;
    ASSERT_TRUE(w0.Create());
    ASSERT_TRUE(w1.Create());
    ASSERT_TRUE(w2.Create());

    const int start_x = 200;
    const int start_y = 200;
    const int gap = 32;
    wchar_t list[128] = {};
    _snwprintf_s(list, _countof(list), _TRUNCATE, L"%lld|%lld|%lld",
                 static_cast<long long>(reinterpret_cast<intptr_t>(w0.hwnd)),
                 static_cast<long long>(reinterpret_cast<intptr_t>(w1.hwnd)),
                 static_cast<long long>(reinterpret_cast<intptr_t>(w2.hwnd)));

    long ret = 0;
    // layout_type=2 层叠，size_mode=0 保持原大小，anchor_mode=0 窗口外框
    op.LayoutWindows(list, 2, 1, start_x, start_y, gap, gap, 0, 0, 0, 0, &ret);
    EXPECT_EQ(ret, 1);

    const HWND hwnds[3] = {w0.hwnd, w1.hwnd, w2.hwnd};
    for (int i = 0; i < 3; ++i) {
        RECT rc = {};
        ASSERT_TRUE(::GetWindowRect(hwnds[i], &rc) != FALSE);
        EXPECT_NEAR(rc.left, start_x + i * gap, 2) << "window " << i << " x";
        EXPECT_NEAR(rc.top, start_y + i * gap, 2) << "window " << i << " y";
    }
}

// 句柄串含 0/负值必须整体拒绝（不能等排列到它才失败留下半应用状态）。
TEST(WindowStateTest, LayoutWindowsRejectsZeroHwnd) {
    op::Op op;
    long ret = 1;
    op.LayoutWindows(L"0|123456", 0, 2, 0, 0, 10, 10, 0, 0, 0, 0, &ret);
    EXPECT_EQ(ret, 0);
}

TEST(WindowStateTest, LayoutWindowsRejectsInvalidLayoutType) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());

    wchar_t list[64] = {};
    _snwprintf_s(list, _countof(list), _TRUNCATE, L"%lld",
                 static_cast<long long>(reinterpret_cast<intptr_t>(wnd.hwnd)));
    long ret = 1;
    op.LayoutWindows(list, 9, 2, 0, 0, 10, 10, 0, 0, 0, 0, &ret);
    EXPECT_EQ(ret, 0);
}

// W3：SendPaste 与 SendString 一致地把字符投递到焦点子控件（编辑框）。
TEST(WindowStateTest, SendPasteDeliversToFocusedChildEdit) {
    op::Op op;
    SendStringWindow wnd;
    ASSERT_TRUE(wnd.Create()) << "failed to create test windows";

    long ret = 0;
    op.SetClipboard(L"paste9", &ret);
    ASSERT_EQ(ret, 1);

    op.SendPaste(reinterpret_cast<LONG_PTR>(wnd.parent), &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(wnd.GetEditText(), L"paste9");
}

// ---------------- 绑定微调批：窗口几何锁 / DisableMinMax / SetIme ----------------

// 等待期间持续泵消息:SetWindowPos 会同步向目标窗口发 WM_WINDOWPOSCHANGED,
// 守护线程的回弹需要目标窗口消息循环存活才能落地(与真实游戏窗口一致;纯 Sleep 会延迟回弹)。
namespace {
void PumpGeoLockWait(int milliseconds) {
    const auto deadline = GetTickCount64() + milliseconds;
    MSG msg = {};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}
} // namespace

// 位置锁判别力：锁定后外部 MoveWindow 必须被拉回锚点；解锁后再移动必须能生效（证明锁真的关了，
// 防止实现退化成"永远拉回"或"从不拉回"两种恒真错误）。
TEST(WindowStateTest, LockWindowPositionPullsBackExternalMove) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());

    RECT base = {};
    ASSERT_TRUE(GetWindowRect(wnd.hwnd, &base) != FALSE);

    long ret = 0;
    op.LockWindowPosition(reinterpret_cast<LONG_PTR>(wnd.hwnd), 1, &ret);
    ASSERT_EQ(ret, 1);

    const int base_w = base.right - base.left;
    const int base_h = base.bottom - base.top;
    ASSERT_TRUE(MoveWindow(wnd.hwnd, base.left + 150, base.top + 100, base_w, base_h, FALSE) != FALSE);
    PumpGeoLockWait(300); // 守护线程 50ms 轮询,留足回弹时间

    RECT now = {};
    ASSERT_TRUE(GetWindowRect(wnd.hwnd, &now) != FALSE);
    EXPECT_NEAR(now.left, base.left, 2) << "位置锁未回弹外部移动";
    EXPECT_NEAR(now.top, base.top, 2) << "位置锁未回弹外部移动";

    op.LockWindowPosition(reinterpret_cast<LONG_PTR>(wnd.hwnd), 0, &ret);
    ASSERT_EQ(ret, 1);
    ASSERT_TRUE(MoveWindow(wnd.hwnd, base.left + 150, base.top + 100, base_w, base_h, FALSE) != FALSE);
    PumpGeoLockWait(200); // 解锁后不允许再被拉回
    ASSERT_TRUE(GetWindowRect(wnd.hwnd, &now) != FALSE);
    EXPECT_NEAR(now.left, base.left + 150, 2) << "解锁后位置仍被拉回,锁未真正关闭";
}

TEST(WindowStateTest, LockWindowSizePullsBackExternalResize) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());

    RECT base = {};
    ASSERT_TRUE(GetWindowRect(wnd.hwnd, &base) != FALSE);
    const int base_w = base.right - base.left;
    const int base_h = base.bottom - base.top;

    long ret = 0;
    op.LockWindowSize(reinterpret_cast<LONG_PTR>(wnd.hwnd), 1, &ret);
    ASSERT_EQ(ret, 1);

    ASSERT_TRUE(SetWindowPos(wnd.hwnd, nullptr, 0, 0, 640, 480,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
    PumpGeoLockWait(300);

    RECT now = {};
    ASSERT_TRUE(GetWindowRect(wnd.hwnd, &now) != FALSE);
    EXPECT_NEAR(now.right - now.left, base_w, 2) << "尺寸锁未回弹外部缩放";
    EXPECT_NEAR(now.bottom - now.top, base_h, 2) << "尺寸锁未回弹外部缩放";

    op.LockWindowSize(reinterpret_cast<LONG_PTR>(wnd.hwnd), 0, &ret);
    ASSERT_EQ(ret, 1);
}

TEST(WindowStateTest, DisableMinMaxRemovesAndRestoresStyleBits) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());
    constexpr LONG_PTR kMask = WS_MAXIMIZEBOX | WS_MINIMIZEBOX;

    const auto orig = GetWindowLongPtrW(wnd.hwnd, GWL_STYLE);
    ASSERT_TRUE((orig & kMask) != 0) << "前置条件:测试窗口应自带最大最小化按钮";

    long ret = 0;
    op.DisableMinMax(reinterpret_cast<LONG_PTR>(wnd.hwnd), 1, &ret);
    ASSERT_EQ(ret, 1);
    const auto disabled = GetWindowLongPtrW(wnd.hwnd, GWL_STYLE);
    EXPECT_TRUE((disabled & kMask) == 0) << "DisableMinMax 未移除 WS_MAXIMIZEBOX/WS_MINIMIZEBOX";

    op.DisableMinMax(reinterpret_cast<LONG_PTR>(wnd.hwnd), 0, &ret);
    ASSERT_EQ(ret, 1);
    const auto restored = GetWindowLongPtrW(wnd.hwnd, GWL_STYLE);
    EXPECT_EQ(restored, orig) << "恢复时未还原锁定前原始样式";
}

TEST(WindowStateTest, SetImeTogglesOpenStatus) {
    op::Op op;
    TopmostWindow wnd;
    ASSERT_TRUE(wnd.Create());

    // imm32 动态加载读取状态(测试工程未链 imm32.lib)
    HMODULE imm = LoadLibraryW(L"imm32.dll");
    ASSERT_TRUE(imm != nullptr);
    auto get_ctx = reinterpret_cast<HIMC(WINAPI *)(HWND)>(GetProcAddress(imm, "ImmGetContext"));
    auto get_open = reinterpret_cast<BOOL(WINAPI *)(HIMC)>(GetProcAddress(imm, "ImmGetOpenStatus"));
    auto release = reinterpret_cast<BOOL(WINAPI *)(HWND, HIMC)>(GetProcAddress(imm, "ImmReleaseContext"));
    ASSERT_TRUE(get_ctx && get_open && release);

    long ret = 0;
    op.SetIme(reinterpret_cast<LONG_PTR>(wnd.hwnd), 0, &ret);
    ASSERT_EQ(ret, 1);
    HIMC himc = get_ctx(wnd.hwnd);
    ASSERT_TRUE(himc != nullptr);
    EXPECT_EQ(get_open(himc), 0) << "SetIme(0) 后输入法仍处打开状态";
    release(wnd.hwnd, himc);

    op.SetIme(reinterpret_cast<LONG_PTR>(wnd.hwnd), 1, &ret);
    ASSERT_EQ(ret, 1);
    himc = get_ctx(wnd.hwnd);
    ASSERT_TRUE(himc != nullptr);
    EXPECT_EQ(get_open(himc), 1) << "SetIme(1) 后输入法未恢复打开";
    release(wnd.hwnd, himc);

    FreeLibrary(imm);
}

TEST(WindowStateTest, BindingTuneFunctionsRejectInvalidHwnd) {
    op::Op op;
    long ret = 1;
    op.LockWindowPosition(0, 1, &ret);
    EXPECT_EQ(ret, 0);
    ret = 1;
    op.LockWindowSize(0, 1, &ret);
    EXPECT_EQ(ret, 0);
    ret = 1;
    op.DisableMinMax(0, 1, &ret);
    EXPECT_EQ(ret, 0);
    ret = 1;
    op.SetIme(0, 1, &ret);
    EXPECT_EQ(ret, 0);
}
