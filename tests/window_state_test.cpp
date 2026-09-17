#include "test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <windows.h>

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
