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

//-------------------- SystemMisc: 系统杂项 + 拟人化随机概率 (2026-09-23 新增) --------------------

// 屏幕三要素必须与 Win32 权威值一致（可判别"返回硬编码 1920x1080"的错误实现）
TEST(SystemMiscTest, ScreenMetricsMatchWin32) {
    op::Op op;
    long w = 0, h = 0, depth = 0;
    op.GetScreenWidth(&w);
    op.GetScreenHeight(&h);
    op.GetScreenDepth(&depth);
    EXPECT_EQ(w, static_cast<long>(::GetSystemMetrics(SM_CXSCREEN)));
    EXPECT_EQ(h, static_cast<long>(::GetSystemMetrics(SM_CYSCREEN)));
    HDC dc = ::GetDC(nullptr);
    ASSERT_NE(dc, nullptr);
    EXPECT_EQ(depth, static_cast<long>(::GetDeviceCaps(dc, BITSPIXEL)));
    ::ReleaseDC(nullptr, dc);
}

TEST(SystemMiscTest, DpiIsPositive) {
    op::Op op;
    long dpi = 0;
    op.GetDPI(&dpi);
    EXPECT_GE(dpi, 96); // 96=100%, 系统 DPI 不可能低于 96
}

// 格式 "yyyy-MM-dd HH:mm:ss" 逐位校验（可判别"返回时间戳数字"的错误实现）
TEST(SystemMiscTest, TimeFormatFixedWidth) {
    op::Op op;
    std::wstring t;
    op.GetTime(t);
    ASSERT_EQ(t.size(), 19u) << "实际: " << t;
    const auto is_digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    for (size_t i = 0; i < 19; ++i) {
        if (i == 4 || i == 7)
            EXPECT_EQ(t[i], L'-');
        else if (i == 10)
            EXPECT_EQ(t[i], L' ');
        else if (i == 13 || i == 16)
            EXPECT_EQ(t[i], L':');
        else
            EXPECT_TRUE(is_digit(t[i])) << "pos " << i;
    }
    EXPECT_EQ(t.substr(0, 2), L"20"); // 21 世纪
}

// Beep 只是 ::Beep 薄封装：无声卡环境可合法返回 0，故只钉"返回值域 + 不崩溃"
TEST(SystemMiscTest, BeepReturnsBoolean) {
    op::Op op;
    long ret = -1;
    op.Beep(800, 10, &ret);
    EXPECT_TRUE(ret == 0 || ret == 1);
}

// 随机整数：闭区间界 + 样本必须多变（可判别"恒定返回 min"的退化实现）
TEST(SystemMiscTest, RandomNumberBoundsAndVariation) {
    op::Op op;
    std::set<long> samples;
    for (int i = 0; i < 500; ++i) {
        long v = 0;
        op.GetRandomNumber(10, 20, &v);
        EXPECT_GE(v, 10);
        EXPECT_LE(v, 20);
        samples.insert(v);
    }
    EXPECT_GT(samples.size(), 1u);
}

// 随机浮点：界 + 多变 + 逆序参数自动纠正（min>max 时交换）
TEST(SystemMiscTest, RandomDoubleBoundsSwappedArgs) {
    op::Op op;
    std::set<long> quant;
    for (int i = 0; i < 500; ++i) {
        double v = 0.0;
        op.GetRandomDouble(0.5, 2.5, &v);
        EXPECT_GE(v, 0.5);
        EXPECT_LE(v, 2.5);
        quant.insert(static_cast<long>(v * 1000)); // 量化去重，避免 double 直比永远不同
    }
    EXPECT_GT(quant.size(), 1u);
    for (int i = 0; i < 100; ++i) {
        double v = 0.0;
        op.GetRandomDouble(2.5, 0.5, &v); // 逆序
        EXPECT_GE(v, 0.5);
        EXPECT_LE(v, 2.5);
    }
}

TEST(SystemMiscTest, GaiLuEdgeCases) {
    op::Op op;
    long r = -1;
    op.GaiLu(0, &r);
    EXPECT_EQ(r, 0); // p<=0 恒 0
    op.GaiLu(-7, &r);
    EXPECT_EQ(r, 0);
    for (int i = 0; i < 20; ++i) {
        op.GaiLu(1, &r);
        EXPECT_EQ(r, 1); // p==1 恒 1
    }
}

// 命中率统计判别：p=2 时 4000 次试验命中须落在二项分布 3σ 宽区间内
// （恒定返回 1 → 4000 次全中；恒定返回 0 → 0 次中，均必然 FAIL）
TEST(SystemMiscTest, GaiLuHitRateApproximatelyOneOverP) {
    op::Op op;
    int hits = 0;
    const int trials = 4000;
    long r = 0;
    for (int i = 0; i < trials; ++i) {
        op.GaiLu(2, &r);
        hits += static_cast<int>(r);
    }
    EXPECT_GT(hits, 1500);
    EXPECT_LT(hits, 2500);
}

// 机器码 = 注册表 MachineGuid，36 字符 GUID 形如 8-4-4-4-12 全十六进制
TEST(SystemMiscTest, MachineCodeIsGuidFormat) {
    op::Op op;
    std::wstring mc;
    op.GetMachineCode(mc);
    ASSERT_EQ(mc.size(), 36u) << "实际: " << mc;
    const auto is_hex = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    };
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            EXPECT_EQ(mc[i], L'-');
        else
            EXPECT_TRUE(is_hex(mc[i])) << "pos " << i << " char " << static_cast<int>(mc[i]);
    }
}

// 管理员检测结果只钉值域（沙箱/真机权限不同，不能钉具体值）
TEST(SystemMiscTest, IsElevatedReturnsBoolean) {
    op::Op op;
    long r = -1;
    op.IsElevated(&r);
    EXPECT_TRUE(r == 0 || r == 1);
}
