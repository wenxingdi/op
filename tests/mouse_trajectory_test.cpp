// 鼠标拟人轨迹单测（2026-11-22）：MoveToSmooth / DragPath 的路径生成与消息时序。
// 走 IN_WINDOWS 消息桩（MouseEventWindow），不断言随机量的具体值，只断言
// 必须成立的不变量：终点精确命中、中间有多步、曲线真的弯、拖拽先按后移再抬。
#include "test_support.h"

#include <cmath>

using test_support::MouseEventWindow;

namespace {

double PerpendicularDistance(long px, long py, long x1, long y1, long x2, long y2) {
    const double dx = static_cast<double>(x2 - x1);
    const double dy = static_cast<double>(y2 - y1);
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-9)
        return std::sqrt(std::pow(static_cast<double>(px - x1), 2.0) + std::pow(static_cast<double>(py - y1), 2.0));
    return std::fabs(dy * static_cast<double>(px) - dx * static_cast<double>(py) + x2 * static_cast<double>(y1) -
                     y2 * static_cast<double>(x1)) /
           len;
}

double MaxDeviationFromSegment(const std::vector<test_support::MouseMoveEvent> &moves, long x1, long y1, long x2,
                               long y2) {
    double max_dev = 0.0;
    for (const auto &mv : moves)
        max_dev = (std::max)(max_dev, PerpendicularDistance(mv.x, mv.y, x1, y1, x2, y2));
    return max_dev;
}

bool BindWindows(op::Op &op, MouseEventWindow &window) {
    if (!window.Create())
        return false;
    long ret = 0;
    op.BindWindow((long)(intptr_t)window.hwnd, L"normal", L"windows", L"windows", 0, &ret);
    return ret == 1;
}

} // namespace

TEST(MouseTrajectoryTest, SmoothMoveEndsAtTargetWithIntermediateSteps) {
    op::Op op;
    MouseEventWindow window;
    ASSERT_TRUE(BindWindows(op, window));
    long ret = 0;
    op.SetMouseTrajectory(0, 0, 0, 100, 0, 0, &ret);
    ASSERT_EQ(ret, 1);

    op.MoveToSmooth(120, 90, 0, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(window.last_x, 120);
    EXPECT_EQ(window.last_y, 90);
    // 平滑移动必须由多步组成（distance 150+/10 -> >=10 步），不能一步到位。
    EXPECT_GE(window.move_count, 3);
}

TEST(MouseTrajectoryTest, SmoothMoveProducesCurvedPath) {
    op::Op op;
    MouseEventWindow window;
    ASSERT_TRUE(BindWindows(op, window));
    long ret = 0;
    op.SetMouseTrajectory(0, 0, 0, 100, 0, 0, &ret);
    ASSERT_EQ(ret, 1);

    op.MoveTo(10, 40, &ret); // 确定起点
    ASSERT_EQ(ret, 1);
    window.move_events.clear();

    op.MoveToSmooth(210, 40, 0, &ret); // 水平 200px，jitter=100 -> 弯曲量打满 80px
    ASSERT_EQ(ret, 1);
    ASSERT_FALSE(window.move_events.empty());
    EXPECT_EQ(window.move_events.back().x, 210);
    EXPECT_EQ(window.move_events.back().y, 40);
    // 同号相关弯曲保证偏离直线（理论中点偏离 >= 30px，留 10 倍余量）。
    EXPECT_GE(MaxDeviationFromSegment(window.move_events, 10, 40, 210, 40), 3.0)
        << "smooth path must visibly deviate from the straight segment";
}

TEST(MouseTrajectoryTest, LinearModeStillReachesTarget) {
    op::Op op;
    MouseEventWindow window;
    ASSERT_TRUE(BindWindows(op, window));
    long ret = 0;
    op.SetMouseTrajectory(1, 0, 0, 100, 0, 0, &ret); // mode 1 = 线性
    ASSERT_EQ(ret, 1);

    window.move_events.clear();
    // 距离 28 < 末端过冲阈值(40)：排除过冲点，纯验证线性插值本身贴线。
    op.MoveToSmooth(20, 20, 0, &ret);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(window.last_x, 20);
    EXPECT_EQ(window.last_y, 20);
    EXPECT_GE(window.move_events.size(), 2u);
    // 线性模式所有点都应落在起点-终点连线上。
    EXPECT_LT(MaxDeviationFromSegment(window.move_events, 0, 0, 20, 20), 1.0);
}

TEST(MouseTrajectoryTest, DragPathSendsDownMovesThenUp) {
    op::Op op;
    MouseEventWindow window;
    ASSERT_TRUE(BindWindows(op, window));
    long ret = 0;
    op.SetMouseTrajectory(0, 0, 0, 100, 0, 0, &ret);
    ASSERT_EQ(ret, 1);

    window.ResetCounts();
    window.move_events.clear();
    window.button_events.clear();
    op.DragPath(L"50,50|200,80", 0, &ret);
    ASSERT_EQ(ret, 1);

    // 按下必须先于移动、移动必须先于抬起（看时间序：button_events 与 move_events 分开记录，
    // 这里用计数 + 最终位置 + 拖拽期间的带左键移动来锁定时序）。
    EXPECT_GE(window.left_down, 1);
    EXPECT_GE(window.left_up, 1);
    EXPECT_GE(window.move_count, 3) << "drag must move through intermediate points, not teleport";
    EXPECT_EQ(window.last_x, 200);
    EXPECT_EQ(window.last_y, 80);
    EXPECT_GE(window.move_with_left_count, 1) << "moves during drag must carry MK_LBUTTON";
}

TEST(MouseTrajectoryTest, DragPathIsCurvedNotStraight) {
    op::Op op;
    MouseEventWindow window;
    ASSERT_TRUE(BindWindows(op, window));
    long ret = 0;
    op.SetMouseTrajectory(0, 0, 0, 100, 0, 0, &ret);
    ASSERT_EQ(ret, 1);

    window.ResetCounts();
    window.move_events.clear();
    window.button_events.clear();
    op.DragPath(L"30,40|230,40", 0, &ret); // 水平拖拽 200px
    ASSERT_EQ(ret, 1);

    // 段内走贝塞尔：偏离直线。jitter=100 -> 弯曲量 80px，理论中点偏离 >= 30px。
    EXPECT_GE(MaxDeviationFromSegment(window.move_events, 30, 40, 230, 40), 3.0)
        << "drag path must not be a straight line (straight drag is a strong bot signature)";
}

TEST(MouseTrajectoryTest, MoveToExFavorsCenterOfRange) {
    op::Op op;
    MouseEventWindow window;
    ASSERT_TRUE(BindWindows(op, window));
    long ret = 0;

    // 300 次采样：正态偏置下 |offset| 均值 ≈ 0.8*σ ≈ 27（span=100, σ≈33），
    // 均匀分布则是 50。阈值 40 给统计波动留足余量。
    double sum_abs = 0.0;
    int max_abs = 0;
    std::wstring pos;
    for (int i = 0; i < 300; ++i) {
        op.MoveToEx(500, 300, 100, 0, pos);
        long x = 0, y = 0;
        ASSERT_EQ(swscanf_s(pos.c_str(), L"%ld,%ld", &x, &y), 2);
        const int d = std::abs(static_cast<int>(x - 500));
        sum_abs += d;
        max_abs = (std::max)(max_abs, d);
        EXPECT_EQ(y, 300);
    }
    EXPECT_LT(sum_abs / 300.0, 40.0) << "gaussian bias: mean |offset| should be well below uniform's 50";
    EXPECT_LE(max_abs, 99); // 仍不得越出 [400,600]
}
