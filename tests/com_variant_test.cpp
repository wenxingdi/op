// COM late-binding 出参封送（ComVariant.h）自测。
//
// 为什么测：COM 是易语言 / VBScript / PowerShell 的真实入口，动态调用（IDispatch）
// 传进来的出参形态与 C++ 直接调用完全不同 —— PowerShell `[ref]` 给的是
// VT_BYREF|VT_I4 包装，oleaut 把**包装本身**交给服务端，因此必须写穿指针而不是
// 覆写包装；标准 VT_BYREF|VT_VARIANT 则要先解一层。09-19 连续两个 late-binding
// 出参 bug（`79aede7`、`8da94da`）都出在这里，而此前这些判据住在 OpAutomation.cpp
// 的匿名命名空间里，测试链接不进去 → 长期零覆盖。

#include "com/ComVariant.h"

#include <gtest/gtest.h>

namespace {

using op::com::InLong;
using op::com::OutLong;
using op::com::RunCvRetOnly;
using op::com::SetOutValue;

VARIANT MakeI4(LONG value) {
    VARIANT v;
    v.vt = VT_I4;
    v.lVal = value;
    return v;
}

}  // namespace

// ---- InLong ----

TEST(ComVariantTest, InLongReadsPlainI4) {
    VARIANT v = MakeI4(42);
    EXPECT_EQ(InLong(&v), 42);
}

TEST(ComVariantTest, InLongReadsThroughByrefI4Wrapper) {
    LONG slot = 77;
    VARIANT v;
    v.vt = VT_BYREF | VT_I4;
    v.plVal = &slot;
    EXPECT_EQ(InLong(&v), 77);
}

TEST(ComVariantTest, InLongByrefI4WithNullPointerIsZero) {
    VARIANT v;
    v.vt = VT_BYREF | VT_I4;
    v.plVal = nullptr;
    EXPECT_EQ(InLong(&v), 0);
}

TEST(ComVariantTest, InLongUnwrapsByrefVariant) {
    LONG slot = 5;
    VARIANT inner;
    inner.vt = VT_BYREF | VT_I4;
    inner.plVal = &slot;
    VARIANT outer;
    outer.vt = VT_BYREF | VT_VARIANT;
    outer.pvarVal = &inner;
    EXPECT_EQ(InLong(&outer), 5);
}

TEST(ComVariantTest, InLongByrefVariantHoldingI4) {
    VARIANT inner = MakeI4(9);
    VARIANT outer;
    outer.vt = VT_BYREF | VT_VARIANT;
    outer.pvarVal = &inner;
    EXPECT_EQ(InLong(&outer), 9);
}

TEST(ComVariantTest, InLongRejectsUnsupportedPayload) {
    // BYREF|BSTR 与 BYREF|VARIANT(BSTR) 都不是 LONG，一律 0，不得读垃圾
    BSTR text = nullptr;
    VARIANT byref_bstr;
    byref_bstr.vt = VT_BYREF | VT_BSTR;
    byref_bstr.pbstrVal = &text;
    EXPECT_EQ(InLong(&byref_bstr), 0);

    VARIANT inner;
    inner.vt = VT_BSTR;
    inner.bstrVal = nullptr;
    VARIANT outer;
    outer.vt = VT_BYREF | VT_VARIANT;
    outer.pvarVal = &inner;
    EXPECT_EQ(InLong(&outer), 0);
}

TEST(ComVariantTest, InLongNullIsZero) {
    EXPECT_EQ(InLong(nullptr), 0);
}

// ---- OutLong ----

TEST(ComVariantTest, OutLongWritesThroughByrefI4Wrapper) {
    LONG slot = 0;
    VARIANT v;
    v.vt = VT_BYREF | VT_I4;
    v.plVal = &slot;

    OutLong(&v, 123);

    EXPECT_EQ(slot, 123) << "必须写穿包装，而不是覆写包装本身";
    EXPECT_EQ(v.vt, VT_BYREF | VT_I4) << "包装形态不得被破坏";
    EXPECT_EQ(v.plVal, &slot) << "包装指针不得被覆写";
}

TEST(ComVariantTest, OutLongWritesInnerOfByrefVariant) {
    VARIANT inner;
    inner.vt = VT_EMPTY;
    inner.lVal = 0;
    VARIANT outer;
    outer.vt = VT_BYREF | VT_VARIANT;
    outer.pvarVal = &inner;

    OutLong(&outer, 88);

    EXPECT_EQ(outer.vt, VT_BYREF | VT_VARIANT);
    EXPECT_EQ(inner.vt, VT_I4);
    EXPECT_EQ(inner.lVal, 88);
}

TEST(ComVariantTest, OutLongLeavesUnsupportedByrefUntouched) {
    BSTR text = nullptr;
    VARIANT v;
    v.vt = VT_BYREF | VT_BSTR;
    v.pbstrVal = &text;

    OutLong(&v, 55);

    EXPECT_EQ(v.vt, VT_BYREF | VT_BSTR) << "不支持的 byref 载荷必须原样保留";
    EXPECT_EQ(v.pbstrVal, &text);
}

TEST(ComVariantTest, OutLongFillsPlainVariant) {
    VARIANT v;
    v.vt = VT_EMPTY;
    v.lVal = 0;

    OutLong(&v, 7);

    EXPECT_EQ(v.vt, VT_I4);
    EXPECT_EQ(v.lVal, 7);
}

TEST(ComVariantTest, OutLongNullIsNoOp) {
    OutLong(nullptr, 1);  // 只要求不崩
}

// ---- SetOutValue / RunCvRetOnly ----

TEST(ComVariantTest, SetOutValueRejectsNull) {
    EXPECT_EQ(SetOutValue(static_cast<LONG *>(nullptr), 1L), E_POINTER);
}

TEST(ComVariantTest, SetOutValueWritesAndReturnsOk) {
    LONG out = -1;
    EXPECT_EQ(SetOutValue(&out, 3L), S_OK);
    EXPECT_EQ(out, 3);
}

TEST(ComVariantTest, RunCvRetOnlyRejectsNullAndStartsFromZero) {
    EXPECT_EQ(RunCvRetOnly(nullptr, [](LONG *) {}), E_POINTER);

    LONG ret = -1;
    bool called = false;
    LONG seen = -1;
    EXPECT_EQ(RunCvRetOnly(&ret,
                           [&](LONG *r) {
                               called = true;
                               seen = *r;
                               *r = 1;
                           }),
              S_OK);
    EXPECT_TRUE(called);
    EXPECT_EQ(seen, 0) << "进入回调前必须已置 0（失败也是 0）";
    EXPECT_EQ(ret, 1);
}
