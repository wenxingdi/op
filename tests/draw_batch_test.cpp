// 绘制补齐（批 B 2026-11-22）。
//
// 测什么 / 不测什么（诚实说明）：
//   * **能测**：接口契约（返回值、未绑定窗口时显式失败、图元计数、清层语义）。
//     这些是可判定的，也是实际最容易坏的地方（比如"未绑定时静默画到屏幕"）。
//   * **不测**：像素级"画出来长什么样"。叠加层是独立分层窗口，不在本进程的截图链里，
//     要在无头/CI 环境里验证像素就得依赖特定 DPI/桌面会话，判据不稳。
//     真正的可视化验收按 CLAUDE.md 的约定走人工（截图目视）。
//   * 因此本文件**不声称验证了视觉正确性**，只验证"调用链与状态语义正确"。

#include "test_support.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <fstream>
#include <iterator>
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

} // namespace

// 屏幕层：加图元 -> 计数增加 -> 清屏 -> 归零。
// 这条链验证"图元真的进了渲染队列"，而不是接口返回 1 就完事。
TEST(DrawBatchTest, ScreenShapesAccumulateAndClear) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int screen_count = -1, bind_count = -1;
    EXPECT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    const int base = screen_count;

    EXPECT_EQ(1, OpDrawCircle(h.handle, 100, 100, 40, L"FF0000", 2));
    EXPECT_EQ(1, OpDrawRect(h.handle, 10, 10, 60, 60, L"00FF00", 1));
    EXPECT_EQ(1, OpDrawLine(h.handle, 0, 0, 50, 50, L"0000FF", 1));

    ASSERT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    EXPECT_EQ(base + 3, screen_count) << "三个图元应全部进入队列";

    EXPECT_EQ(1, OpDrawClear(h.handle));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    EXPECT_EQ(0, screen_count) << "DrawClear 之后屏幕层应为空";
}

// 非法颜色必须**显式失败**，且不往队列里塞图元。
// 反例：如果实现"解析失败就画白色"，调用方永远发现不了自己写错了颜色。
TEST(DrawBatchTest, InvalidColorIsRejectedWithoutEnqueue) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    OpDrawClear(h.handle);

    int screen_count = -1, bind_count = -1;
    EXPECT_EQ(0, OpDrawCircle(h.handle, 10, 10, 5, L"zzzzzz", 1)) << "颜色非法应返回 0";
    EXPECT_EQ(0, OpDrawCircle(h.handle, 10, 10, 5, L"", 1)) << "空颜色应返回 0";
    EXPECT_EQ(0, OpDrawCircle(h.handle, 10, 10, 5, L"FFFFFFF", 1)) << "超过 6 位应返回 0";

    ASSERT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    EXPECT_EQ(0, screen_count) << "非法颜色不应留下图元";
}

// 未绑定窗口时，*Ex 系列必须返回 0 并明确失败。
// 这是本批最重要的语义：静默画到屏幕会被误判成"坐标系搞错了"，排查成本极高。
TEST(DrawBatchTest, BindShapesFailExplicitlyWhenNotBound) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    EXPECT_EQ(0, OpDrawRectEx(h.handle, 10, 10, 50, 50, 1, 1)) << "未绑定应失败";
    EXPECT_EQ(0, OpDrawCircleEx(h.handle, 10, 10, 20, 1)) << "未绑定应失败";
    EXPECT_EQ(0, OpDrawTextEx(h.handle, 10, 10, L"probe", 14)) << "未绑定应失败";

    int screen_count = -1, bind_count = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    EXPECT_EQ(0, bind_count) << "未绑定时绑定层不应有图元";
}

// DrawImage：不存在的图片不能崩，且 alpha 越界要被规整（不是拒绝）。
// 这里只断言"不崩 + 返回 1"，像素正确性走人工验收。
TEST(DrawBatchTest, DrawImageMissingFileDoesNotCrash) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    EXPECT_EQ(1, OpDrawImage(h.handle, 0, 0, L"__no_such_image__.png", 128));
    EXPECT_EQ(1, OpDrawImage(h.handle, 0, 0, L"__no_such_image__.png", 0));   // alpha=0 -> 视为 255
    EXPECT_EQ(1, OpDrawImage(h.handle, 0, 0, L"__no_such_image__.png", 999)); // 越界 -> 255
    OpDrawClear(h.handle);
}

// 两层互不干扰：清屏层不影响绑定层，反之亦然。
TEST(DrawBatchTest, ScreenAndBindLayersAreIndependent) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    OpDrawClear(h.handle);
    OpDrawClearBind(h.handle);

    OpDrawCircle(h.handle, 1, 1, 5, L"FFFFFF", 1);
    int screen_count = -1, bind_count = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    EXPECT_EQ(1, screen_count);
    EXPECT_EQ(0, bind_count);

    // 未绑定时 DrawClearBind 也不应清掉屏幕层
    OpDrawClearBind(h.handle);
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &screen_count, &bind_count));
    EXPECT_EQ(1, screen_count) << "DrawClearBind 不能清屏幕层";
    OpDrawClear(h.handle);
}

// ===================== 批 G（2026-11-23）：状态 / 填充 / 图片 =====================

namespace {

// 绑定层用例需要真实窗口（桌面窗口不可绑）。内容无关，只要客户区存在、能 ClientToScreen。
struct SimpleWindow {
    HWND hwnd = nullptr;

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        return ::DefWindowProcW(h, m, w, l);
    }

    bool Create(const wchar_t *cls_name, int client_w, int client_h) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls_name;
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
        ::RegisterClassExW(&wc);
        RECT rc{0, 0, client_w, client_h};
        ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
        hwnd = ::CreateWindowExW(0, cls_name, L"draw_probe", WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, 120, 120,
                                 rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
        if (!hwnd)
            return false;
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd);
        return true;
    }

    ~SimpleWindow() {
        if (hwnd)
            ::DestroyWindow(hwnd);
    }
};

std::vector<char> ReadFileBytes(const std::wstring &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

// 状态接口的入参校验：非法色必须显式失败且不改状态。
TEST(DrawBatchTest, SetColorAndFillStateValidation) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    EXPECT_EQ(0, OpDrawSetColor(h.handle, L"zzzzzz")) << "非法色应返回 0";
    EXPECT_EQ(0, OpDrawSetColor(h.handle, L"")) << "空色应返回 0";
    EXPECT_EQ(0, OpDrawSetColor(h.handle, L"FFFFFFF")) << "超 6 位应返回 0";
    EXPECT_EQ(1, OpDrawSetColor(h.handle, L"00FF00"));

    EXPECT_EQ(0, OpDrawSetFill(h.handle, 1, L"zzz")) << "非法填充色应返回 0";
    EXPECT_EQ(1, OpDrawSetFill(h.handle, 1, L"00FF00")) << "6 位 = 自动加 0x66 半透明";
    EXPECT_EQ(1, OpDrawSetFill(h.handle, 1, L"80FF0000")) << "8 位 = 显式 alpha";
    EXPECT_EQ(1, OpDrawSetFill(h.handle, 0, L"")) << "关填充不需要颜色";
    EXPECT_EQ(1, OpDrawSetFill(h.handle, 1, L"")) << "省略颜色 = 沿用当前色";

    // 状态是进程级的，恢复默认免得污染后面的用例
    OpDrawSetColor(h.handle, L"FFFFFF");
    OpDrawSetFill(h.handle, 1, L"66FF0000");
}

// 绑定层：mode 0/1/2/非法值 都要能入队；DrawImageEx 缺文件显式失败且不留图元。
TEST(DrawBatchTest, BindLayerShapesAndImageWithRealWindow) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    SimpleWindow win;
    ASSERT_TRUE(win.Create(L"OpDrawProbeWindow", 200, 150)) << "创建窗口失败（GDI 环境问题）";
    if (OpBindWindow(h.handle, reinterpret_cast<intptr_t>(win.hwnd), L"gdi", L"windows", L"windows", 0) != 1)
        GTEST_SKIP() << "gdi 绑定失败（无桌面会话/前台被占），绑定层绘制跳过";

    int sc = -1, bc = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    const int base = bc;

    EXPECT_EQ(1, OpDrawRectEx(h.handle, 10, 10, 50, 30, 0, 1)) << "mode=0 纯填充";
    EXPECT_EQ(1, OpDrawRectEx(h.handle, 10, 10, 50, 30, 2, 2)) << "mode=2 填充+描边";
    EXPECT_EQ(1, OpDrawRectEx(h.handle, 10, 10, 50, 30, 3, 1)) << "非法 mode 按描边，仍成功";
    EXPECT_EQ(1, OpDrawCircleEx(h.handle, 20, 20, 10, 2));
    EXPECT_EQ(1, OpDrawTextEx(h.handle, 5, 5, L"probe", 14));

    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base + 5, bc) << "5 个 Ex 图元应全部进绑定层";

    // 缺文件：显式失败 + 不留图元（屏幕层是静默跳过，Ex 系列必须给明确反馈）
    EXPECT_EQ(0, OpDrawImageEx(h.handle, 0, 0, L"__no_such__.png", 255));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base + 5, bc) << "缺文件的 DrawImageEx 不应留下图元";

    const std::wstring bmp = test_support::GetTempBmpPath(L"__draw_probe.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp, L"ok"));
    EXPECT_EQ(1, OpDrawImageEx(h.handle, 0, 0, bmp.c_str(), 255));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base + 6, bc);

    OpDrawClearBind(h.handle);
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(0, bc) << "DrawClearBind 之后绑定层应为空";

    ::DeleteFileW(bmp.c_str());
    OpUnBindWindow(h.handle);
}

// 图片句柄上屏：像素被折进图元，句柄释放后已提交的图元仍然有效。
TEST(DrawBatchTest, DrawImageFromHandleCopiesPixels) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    OpDrawClear(h.handle);

    EXPECT_EQ(0, OpDrawImageFromHandle(h.handle, 0, 0, L"__no_such_handle__", 255)) << "无效句柄应返回 0";
    EXPECT_EQ(0, OpDrawImageFromHandle(h.handle, 0, 0, L"", 255)) << "空句柄应返回 0";

    const std::wstring bmp = test_support::GetTempBmpPath(L"__draw_handle.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(bmp, L"909"));
    const auto bytes = ReadFileBytes(bmp);
    ASSERT_FALSE(bytes.empty()) << "读 bmp 失败";

    const wchar_t *raw = OpLoadImageBmp(h.handle, bytes.data(), static_cast<int>(bytes.size()));
    ASSERT_NE(nullptr, raw);
    // C API 字符串返回值是共享缓冲，必须立刻拷走
    const std::wstring image_handle(raw);
    ASSERT_FALSE(image_handle.empty()) << "加载 bmp 失败";

    int sc = -1, bc = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    const int base = sc;
    EXPECT_EQ(1, OpDrawImageFromHandle(h.handle, 0, 0, image_handle.c_str(), 200));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base + 1, sc) << "图元应进屏幕层";

    // 释放句柄后图元仍在（像素已拷贝，渲染不碰注册表）
    OpFreeImage(h.handle, image_handle.c_str());
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base + 1, sc) << "释放图片句柄不应影响已提交的图元";

    OpDrawClear(h.handle);
    ::DeleteFileW(bmp.c_str());
}

// G5：图元上限。脚本死循环绘制时内存不能无限涨。
TEST(DrawBatchTest, ElementCapDropsOldestInsteadOfGrowingForever) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    OpDrawClear(h.handle);

    if (OpDrawLine(h.handle, 0, 0, 10, 10, L"FFFFFF", 1) != 1)
        GTEST_SKIP() << "叠加层不可用（无桌面会话），上限用例跳过";

    for (int i = 0; i < 4200; ++i)
        OpDrawLine(h.handle, 0, 0, i % 200, 100, L"FFFFFF", 1);

    int sc = -1, bc = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_LE(sc, 4096) << "上限 4096：超出部分必须裁掉";
    EXPECT_GE(sc, 4000) << "只裁超出部分，不该一刀切清空";
    OpDrawClear(h.handle);
}

// ===================== 批 G2（2026-11-23）：标签替换 =====================

// 同标签重画 = 就地替换（不累积）；不同标签互相独立；DrawRemoveTag 返回删除个数。
TEST(DrawBatchTest, TagReplacesInsteadOfAccumulating) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    OpDrawClear(h.handle);

    if (OpDrawRectTag(h.handle, L"probe_a", 0, 0, 10, 10, L"FF0000", 1) != 1)
        GTEST_SKIP() << "叠加层不可用（无桌面会话），标签用例跳过";

    int sc = -1, bc = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(1, sc);

    // 同一 tag 重画 3 次：仍然只有 1 条
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(1, OpDrawRectTag(h.handle, L"probe_a", i, i, i + 10, i + 10, L"00FF00", 1));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(1, sc) << "同标签必须替换而不是累积";

    EXPECT_EQ(1, OpDrawRectTag(h.handle, L"probe_b", 20, 20, 30, 30, L"0000FF", 1));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(2, sc) << "不同标签各自保留";

    EXPECT_EQ(0, OpDrawRectTag(h.handle, L"", 0, 0, 1, 1, L"FFFFFF", 1)) << "空标签应返回 0";
    EXPECT_EQ(0, OpDrawRectTag(h.handle, L"probe_b", 0, 0, 1, 1, L"zzz", 1)) << "非法色应返回 0";

    EXPECT_EQ(1, OpDrawRemoveTag(h.handle, L"probe_a"));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(1, sc) << "只该删掉 probe_a";
    EXPECT_EQ(1, OpDrawRemoveTag(h.handle, L"probe_b"));
    EXPECT_EQ(0, OpDrawRemoveTag(h.handle, L"probe_b")) << "已删过的标签返回 0";
    EXPECT_EQ(0, OpDrawRemoveTag(h.handle, L"")) << "空标签返回 0";
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(0, sc);
}

// 绑定层标签：未绑定显式失败；绑定后同 tag 替换、只留一条。
TEST(DrawBatchTest, BindTagReplaceWithRealWindow) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    EXPECT_EQ(0, OpDrawRectExTag(h.handle, L"t", 0, 0, 10, 10, 1, 1)) << "未绑定应显式失败";

    SimpleWindow win;
    ASSERT_TRUE(win.Create(L"OpDrawProbeWindow2", 200, 150)) << "创建窗口失败（GDI 环境问题）";
    if (OpBindWindow(h.handle, reinterpret_cast<intptr_t>(win.hwnd), L"gdi", L"windows", L"windows", 0) != 1)
        GTEST_SKIP() << "gdi 绑定失败，绑定层标签用例跳过";

    int sc = -1, bc = -1;
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    const int base = bc;

    EXPECT_EQ(1, OpDrawRectExTag(h.handle, L"t", 0, 0, 20, 20, 2, 1));
    EXPECT_EQ(1, OpDrawRectExTag(h.handle, L"t", 5, 5, 20, 20, 0, 1));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base + 1, bc) << "同标签重画只该留一条";

    EXPECT_EQ(1, OpDrawRemoveTag(h.handle, L"t"));
    ASSERT_EQ(1, OpDrawGetCount(h.handle, &sc, &bc));
    EXPECT_EQ(base, bc);

    OpUnBindWindow(h.handle);
}
