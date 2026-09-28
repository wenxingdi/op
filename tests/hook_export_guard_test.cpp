// 注入侧导出护栏（H14）与"每会话一次"诊断开关（H20）的纯函数判据。
//
// 为什么单独测这两块：它们都跑在**别的进程**里（hook DLL 被注入进游戏进程，由宿主远程调用）。
// 真机用例只能覆盖到"绑定成功、像素正确"，覆盖不到异常路径与"第二次绑定"——
// 而恰恰是这两点决定了：游戏会不会被自己的异常打死、第二次绑定后还有没有诊断日志可看。

#include "test_support.h"

#include "../libop/hook/ExportGuard.h"
#include "../libop/hook/HookDiagnostics.h"

#include <stdexcept>

namespace {

// 反向验证锚点：fallback 必须与"默认构造值"不同。否则 "return fallback" 与 "return Ret{}"
// 两种实现给出同样的结果，断言就分不出对错（同 REFERENCE 里"用例必须能区分对错实现"）。
constexpr long kFallback = 7;
constexpr unsigned long long kFallbackUll = 99ull;

struct NonStdError {};

long ThrowStd() {
    throw std::runtime_error("boom");
}

long ThrowNonStd() {
    throw NonStdError{};
}

} // namespace

// 正常返回必须原样透传 —— 护栏不能顺手改语义。
TEST(HookExportGuardTest, PassesThroughReturnValue) {
    const long value = op::hook::guarded<long>("PassThrough", kFallback, []() -> long { return 42; });
    EXPECT_EQ(value, 42);
}

// std::exception 子类 -> 返回调用方指定的 fallback，且必须**不是**默认值 0。
TEST(HookExportGuardTest, StdExceptionReturnsConfiguredFallback) {
    const long value = op::hook::guarded<long>("StdThrow", kFallback, ThrowStd);
    EXPECT_EQ(value, kFallback) << "异常路径没有返回调用方指定的 fallback";
    EXPECT_NE(value, 0) << "fallback 退化成默认值，断言将无法区分 correct/incorrect 实现";
}

// 非 std::exception 同样要兜住 —— 这是最容易漏掉的一半：
// 只写 catch (const std::exception &) 时，抛自定义类型仍会 terminate 掉游戏进程。
TEST(HookExportGuardTest, NonStdExceptionReturnsConfiguredFallback) {
    const long value = op::hook::guarded<long>("NonStdThrow", kFallback, ThrowNonStd);
    EXPECT_EQ(value, kFallback) << "catch(...) 分支没有兜住非 std 异常（宿主侧护栏一直双兜）";
}

// 模板不能只对某一种返回类型正确。
TEST(HookExportGuardTest, WorksForOtherReturnTypes) {
    const unsigned long long ok = op::hook::guarded<unsigned long long>(
        "UllOk", kFallbackUll, []() -> unsigned long long { return 5; });
    EXPECT_EQ(ok, 5u);

    const unsigned long long bad = op::hook::guarded<unsigned long long>(
        "UllThrow", kFallbackUll, []() -> unsigned long long { throw std::runtime_error("boom"); });
    EXPECT_EQ(bad, kFallbackUll) << "unsigned long long 返回类型的异常路径没有走 fallback";
}

// ---------------------------------------------------------------- OncePerSession（H20）

TEST(HookOncePerSessionTest, ConsumesExactlyOnceUntilReset) {
    op::hook::OncePerSession once;
    EXPECT_TRUE(once.consume()) << "首次应放行（否则一条诊断日志都看不到）";
    EXPECT_FALSE(once.consume()) << "同一会话内第二次起必须静默，否则每帧刷日志";
    EXPECT_FALSE(once.consume());
    EXPECT_TRUE(once.used());
}

// H20 的核心：复位必须真的生效，否则第二次 Bind 起永远沉默。
TEST(HookOncePerSessionTest, ResetLetsTheNextSessionLogAgain) {
    op::hook::OncePerSession once;
    once.consume();

    once.reset();
    EXPECT_FALSE(once.used()) << "reset 没有清掉标志";
    EXPECT_TRUE(once.consume()) << "reset 之后必须能再次放行 —— reset 若为空实现（反向验证点）这里必然 FAIL";
    EXPECT_FALSE(once.consume());
}
