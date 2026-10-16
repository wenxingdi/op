// OCR 预热自测（P2 第二十六批，2026-10-16）：OpOcrWarmup。
// 引擎为内置 ONNX（模型内嵌于 op_x64.dll），OCR 测试按惯例先 SetOcrEngine("onnx") 显式初始化。
// 预热语义：触发懒加载 + 一次小图推理，幂等；返回 1 引擎就绪、0 不可用。

#include "test_support.h"

#include <gtest/gtest.h>

#include <op_c_api.h>

namespace {

using test_support::GetEnvString;

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

TEST(OcrWarmupTest, ExplicitEngineWarmupSucceedsAndIsIdempotent) {
    if (!GetEnvString(L"OP_SKIP_OCR_TESTS").empty())
        GTEST_SKIP() << "OCR tests are disabled by OP_SKIP_OCR_TESTS.";

    CApiHandle api;
    ASSERT_EQ(OpSetOcrEngine(api.handle, L"onnx", L"", L""), 1);
    // 首次：完成懒加载 + 一次推理
    EXPECT_EQ(OpOcrWarmup(api.handle), 1);
    // 幂等：引擎已就绪，第二次直接成功
    EXPECT_EQ(OpOcrWarmup(api.handle), 1);
}

TEST(OcrWarmupTest, NullHandleReturnsZero) {
    EXPECT_EQ(OpOcrWarmup(nullptr), 0);
}

} // namespace
