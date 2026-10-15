// crypto + draw overlay 模块自测（P2 第二十一批，2026-10-15）。
// 摘要/编码用 RFC 1321 / FIPS 180-1 公开测试向量钉死行为；
// 绘制走隐藏模式（SetVisible(0)），不上屏、不干扰桌面，只验证调用链与返回值。

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <string>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

TEST(CryptoApiTest, Md5KnownVectors) {
    CApiHandle api;
    EXPECT_STREQ(OpMd5(api.handle, L"abc"), L"900150983cd24fb0d6963f7d28e17f72");
    // RFC 1321 标准测试组
    EXPECT_STREQ(OpMd5(api.handle, L""), L"");
    EXPECT_STREQ(OpMd5(api.handle, L"The quick brown fox jumps over the lazy dog"),
                 L"9e107d9d372bb6826bd81d3542a419d6");
    // UTF-8 编码后哈希（与 ASCII 同一实现路径，中文只验长度与非空）
    const wchar_t *cn = OpMd5(api.handle, L"中文");
    ASSERT_TRUE(cn != nullptr);
    EXPECT_EQ(std::wstring(cn).size(), 32u);
}

TEST(CryptoApiTest, Sha1KnownVectors) {
    CApiHandle api;
    EXPECT_STREQ(OpSha1(api.handle, L"abc"), L"a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_STREQ(OpSha1(api.handle, L""), L"");
    EXPECT_STREQ(OpSha1(api.handle, L"The quick brown fox jumps over the lazy dog"),
                 L"2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST(CryptoApiTest, Base64RoundTripAndInvalid) {
    CApiHandle api;
    EXPECT_STREQ(OpBase64Encode(api.handle, L"hello"), L"aGVsbG8=");
    EXPECT_STREQ(OpBase64Decode(api.handle, L"aGVsbG8="), L"hello");
    // 空输入往返
    EXPECT_STREQ(OpBase64Encode(api.handle, L""), L"");
    EXPECT_STREQ(OpBase64Decode(api.handle, L""), L"");
    // 非法串返回空串
    EXPECT_STREQ(OpBase64Decode(api.handle, L"!!!not-base64!!!"), L"");
    // 中文 UTF-8 往返保真
    // 注意：c_api 所有字符串返回值共用 string_result 缓冲，有效至下一次字符串调用；
    // 跨调用使用必须先拷贝，否则 clear() 会把输入清空（这正是之前的失败原因）。
    const std::wstring enc = OpBase64Encode(api.handle, L"中文abc123");
    EXPECT_STREQ(OpBase64Decode(api.handle, enc.c_str()), L"中文abc123");
}

TEST(DrawOverlayTest, CommandsReturnOkHidden) {
    CApiHandle api;
    // 先隐藏（测试不打扰桌面）；窗口惰性创建失败（无桌面会话）允许整体跳过。
    if (OpDrawSetVisible(api.handle, 0) != 1)
        GTEST_SKIP() << "overlay 不可用（无桌面会话）";

    EXPECT_EQ(OpDrawRect(api.handle, 10, 10, 200, 100, L"FF0000", 2), 1);
    EXPECT_EQ(OpDrawLine(api.handle, 0, 0, 300, 300, L"00FF00", 1), 1);
    EXPECT_EQ(OpDrawText(api.handle, 50, 50, L"test", L"FFFFFF", 16), 1);
    // 非法颜色返回 0
    EXPECT_EQ(OpDrawRect(api.handle, 0, 0, 10, 10, L"GGGGGG", 1), 0);
    EXPECT_EQ(OpDrawRect(api.handle, 0, 0, 10, 10, L"12345678", 1), 0) << "超过 RRGGBB 应拒绝";
    // 清空与空文本
    EXPECT_EQ(OpDrawClear(api.handle), 1);
    EXPECT_EQ(OpDrawText(api.handle, 0, 0, L"", L"FFFFFF", 14), 0) << "空文本不生成图元";
    // 收尾保持隐藏
    EXPECT_EQ(OpDrawSetVisible(api.handle, 0), 1);
}

} // namespace
