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

#include <string>

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
