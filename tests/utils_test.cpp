#include "test_support.h"

#include "../libop/base/Environment.h"
#include "../libop/base/ThreadPool.h"
#include "../libop/base/Utils.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace std;

TEST(UtilsTest, ThreadPool) {
    ThreadPool pool(4);
    auto fut = pool.enqueue([] { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

// S3 回归：ThreadPool(0) 旧实现无 worker，enqueue 的 future 永不 ready。
// 用 wait_for 而非 get —— 旧实现下判为 FAIL(wait_for 超时) 而非整个测试进程挂死。
TEST(UtilsTest, ThreadPoolZeroThreadsStillRuns) {
    ThreadPool pool(0);
    auto fut = pool.enqueue([] { return 7; });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(fut.get(), 7);
}

TEST(UtilsTest, RectDivideBlock) {
    op::rect_t rc(0, 0, 100, 100);
    vector<op::rect_t> blocks;
    rc.divideBlock(2, false, blocks);
    EXPECT_EQ(blocks.size(), 2u);
}

// S1 回归：count<=0 旧实现 NumberGen(height(), 0) 内 n/cnt 除零（release 直接崩）。
// 修复后入口早退并清空 blocks。
TEST(UtilsTest, RectDivideBlockZeroCountNoDivZero) {
    op::rect_t rc(0, 0, 100, 100);
    vector<op::rect_t> blocks;
    blocks.push_back(op::rect_t(1, 1, 2, 2)); // 预置脏数据，验证被清空
    rc.divideBlock(0, false, blocks);
    EXPECT_TRUE(blocks.empty());

    blocks.push_back(op::rect_t(1, 1, 2, 2));
    rc.divideBlock(-5, true, blocks);
    EXPECT_TRUE(blocks.empty());
}

// S1 回归：count>span 旧实现 blocks.size()==count 且尾部产生 0 尺寸空块。
// 修复后 clamp 到 span，块数==span 且每块非空。
TEST(UtilsTest, RectDivideBlockCountExceedsSpanClamped) {
    op::rect_t rc(0, 0, 10, 10);
    vector<op::rect_t> blocks;

    rc.divideBlock(20, false, blocks); // 横向 width()=10
    EXPECT_EQ(blocks.size(), 10u);
    for (const auto &b : blocks) {
        EXPECT_GT(b.width(), 0);
        EXPECT_GT(b.height(), 0);
    }

    rc.divideBlock(20, true, blocks); // 纵向 height()=10
    EXPECT_EQ(blocks.size(), 10u);
    for (const auto &b : blocks) {
        EXPECT_GT(b.height(), 0);
    }
}

// S2 回归：hex2bin 旧实现 'a'..'f' 走 c-'A'+10 得 42..47（错），仅认大写。
TEST(UtilsTest, Hex2BinAcceptsLowercase) {
    EXPECT_EQ(hex2bin('0'), 0);
    EXPECT_EQ(hex2bin('9'), 9);
    EXPECT_EQ(hex2bin('A'), 10);
    EXPECT_EQ(hex2bin('F'), 15);
    EXPECT_EQ(hex2bin('a'), 10);
    EXPECT_EQ(hex2bin('f'), 15);
}

// 拟人抖动回归：样本必须全部落在 [基准*(1-p), 基准*(1+p)] 内，且不能退化为恒定值
// （旧实现无此函数，"恒定延时"回归的判别点就是样本集大小必须 >1）。
TEST(UtilsTest, JitteredDelayStaysWithinBoundsAndVaries) {
    std::set<long> samples;
    for (int i = 0; i < 200; ++i) {
        const long v = jittered_delay_ms(100, 40);
        EXPECT_GE(v, 60);
        EXPECT_LE(v, 140);
        samples.insert(v);
    }
    EXPECT_GT(samples.size(), 1u);
}

// 下限 1ms：按下/弹起间隔过短可能被系统合并为一次点击；base<=0 保持无延时语义。
TEST(UtilsTest, JitteredDelayFloorAndZeroBase) {
    EXPECT_EQ(jittered_delay_ms(0, 40), 0);
    EXPECT_EQ(jittered_delay_ms(-5, 40), 0);
    for (int i = 0; i < 100; ++i)
        EXPECT_GE(jittered_delay_ms(1, 40), 1);
}

// 进程级播种至多一次。注意 OpEnvironment/前面的用例可能已创建过 Op 抢先播种，
// 故只断言"不存在两次都成功"，不假设本用例一定拿到首次。
TEST(UtilsTest, SeedProcessRandomSeedsAtMostOnce) {
    const bool first = SeedProcessRandom();
    const bool second = SeedProcessRandom();
    EXPECT_FALSE(first && second);
}

// B1 回归：setlog(宽字符版) 旧实现把「已格式化完成的文本」再当 format 回调窄字符版，
// 后者 va_start 后 vsprintf_s 读取不存在的可变参数（UB）。
// 判别特征（关键）：旧实现会对展开结果二次解析 —— 「val=%d pct=50%」里的 %d 被
// 替换成栈垃圾数字；新实现各格式化一次即落盘，字面 "val=%d pct=50%" 原样保留。
// 注：断言不能只看返回值 —— 旧实现同样返回 1，必须比对日志内容才具判别力。
// 测试数据全 ASCII，规避源码字面量与 _ws2string(CP_ACP) 的编码差异。
TEST(UtilsTest, SetLogWideFormatDoesNotReexpand) {
    const char *kLog = "__op.log";
    std::remove(kLog);

    const int saved_mode = RuntimeEnvironment::m_showErrorMsg;
    RuntimeEnvironment::m_showErrorMsg = 2; // 2 = 写 __op.log
    EXPECT_EQ(setlog(L"val=%s pct=%d%%", L"%d", 50), 1); // 展开为「val=%d pct=50%」
    RuntimeEnvironment::m_showErrorMsg = saved_mode;

    std::ifstream fin(kLog, std::ios::binary);
    ASSERT_TRUE(fin.is_open()) << "setlog(mode=2) 未产出 __op.log";
    const std::string content((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    fin.close();
    std::remove(kLog);

    EXPECT_NE(content.find("val=%d pct=50%"), std::string::npos) << "实际日志内容: " << content;
}
