// JSON 族自测（P3 第一批，2026-10-16）：OpJson* 11 接口。
// 设计：无句柄路径直取式，路径 "a.b[0].c" 兼容 "a.b.0"，空路径=根。
// 失败约定：取值类 0/空串，写类（Set/Append/Delete）返回空串。
// 注意：c_api 字符串返回共用 string_result 缓冲，结果须立即拷贝（此处直接构造 wstring）。

#include "test_support.h"

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

// 取字符串并立即拷贝（规避共享缓冲被下一次调用覆盖）。
std::wstring GetStr(op_handle h, const wchar_t *json, const wchar_t *path) {
    const wchar_t *p = OpJsonGetString(h, json, path);
    return p ? p : L"";
}

TEST(JsonValueTest, IsValidAcceptsAndRejects) {
    CApiHandle api;
    EXPECT_EQ(OpJsonIsValid(api.handle, L"{\"a\":1}"), 1);
    EXPECT_EQ(OpJsonIsValid(api.handle, L"[1,2,3]"), 1);
    EXPECT_EQ(OpJsonIsValid(api.handle, L"  \"str\"  "), 1);
    EXPECT_EQ(OpJsonIsValid(api.handle, L"null"), 1);
    EXPECT_EQ(OpJsonIsValid(api.handle, L"-12.5e3"), 1);
    EXPECT_EQ(OpJsonIsValid(api.handle, L""), 0);
    EXPECT_EQ(OpJsonIsValid(api.handle, L"{\"a\":1}extra"), 0); // 顶层尾随内容
    EXPECT_EQ(OpJsonIsValid(api.handle, L"{\"a\":}"), 0);
    EXPECT_EQ(OpJsonIsValid(api.handle, L"{\"a\":1"), 0); // 未闭合
    EXPECT_EQ(OpJsonIsValid(api.handle, L"[1,]"), 0);     // 尾逗号
    EXPECT_EQ(OpJsonIsValid(api.handle, L"01"), 0);       // 前导零
    EXPECT_EQ(OpJsonIsValid(api.handle, L"\"\\q\""), 0);  // 非法转义
    EXPECT_EQ(OpJsonIsValid(api.handle, nullptr), 0);
}

TEST(JsonValueTest, GetTypedValuesByPath) {
    CApiHandle api;
    const wchar_t *json = L"{\"user\":{\"name\":\"张三\",\"age\":30,\"vip\":true,"
                          L"\"tags\":[\"a\",\"b\"],\"scores\":[95.5,88]}}";
    EXPECT_EQ(GetStr(api.handle, json, L"user.name"), L"张三");
    double num = 0.0;
    EXPECT_EQ(OpJsonGetNumber(api.handle, json, L"user.age", &num), 1);
    EXPECT_DOUBLE_EQ(num, 30.0);
    EXPECT_EQ(OpJsonGetNumber(api.handle, json, L"user.scores[0]", &num), 1);
    EXPECT_DOUBLE_EQ(num, 95.5);
    int b = -1;
    EXPECT_EQ(OpJsonGetBool(api.handle, json, L"user.vip", &b), 1);
    EXPECT_EQ(b, 1);
    // 数组下标两种写法等价
    EXPECT_EQ(GetStr(api.handle, json, L"user.tags[1]"), L"b");
    EXPECT_EQ(GetStr(api.handle, json, L"user.tags.1"), L"b");
    // 类型不符 / 不存在 → 失败
    EXPECT_STREQ(OpJsonGetString(api.handle, json, L"user.age"), L"");
    EXPECT_EQ(OpJsonGetNumber(api.handle, json, L"user.missing", &num), 0);
    EXPECT_EQ(OpJsonGetBool(api.handle, json, L"user.name", &b), 0);
}

TEST(JsonValueTest, UnicodeEscapeAndEmojiRoundTrip) {
    CApiHandle api;
    // \u4e2d\u6587 = 中文；emoji 😀 经代理对 \ud83d\ude00
    const wchar_t *json = L"{\"s\":\"\\u4e2d\\u6587 \\ud83d\\ude00\"}";
    const std::wstring expected = L"中文 " L"\U0001F600";
    EXPECT_EQ(GetStr(api.handle, json, L"s"), expected);
    // 写回再校验仍是合法 JSON
    const std::wstring new_json = OpJsonSetString(api.handle, json, L"s", expected.c_str());
    EXPECT_EQ(OpJsonIsValid(api.handle, new_json.c_str()), 1);
    EXPECT_EQ(GetStr(api.handle, new_json.c_str(), L"s"), expected);
}

TEST(JsonValueTest, GetSizeAndGetKeys) {
    CApiHandle api;
    const wchar_t *json = L"{\"arr\":[1,2,3],\"obj\":{\"x\":1,\"y\":2},\"n\":5}";
    EXPECT_EQ(OpJsonGetSize(api.handle, json, L"arr"), 3);
    EXPECT_EQ(OpJsonGetSize(api.handle, json, L"obj"), 2);
    EXPECT_EQ(OpJsonGetSize(api.handle, json, L""), 3); // 根对象键数
    EXPECT_EQ(OpJsonGetSize(api.handle, json, L"n"), -1); // 非集合
    EXPECT_EQ(OpJsonGetSize(api.handle, json, L"none"), -1);
    // GetKeys 返回 JSON 数组文本
    const std::wstring keys = OpJsonGetKeys(api.handle, json, L"obj");
    EXPECT_EQ(keys, L"[\"x\",\"y\"]");
    EXPECT_STREQ(OpJsonGetKeys(api.handle, json, L"arr"), L""); // 非对象
}

TEST(JsonValueTest, SetCreatesIntermediatePathAndEscapes) {
    CApiHandle api;
    // 空文本从零建文档
    const std::wstring j1 = OpJsonSetString(api.handle, L"", L"a.b[1].c", L"v");
    EXPECT_EQ(j1, L"{\"a\":{\"b\":[null,{\"c\":\"v\"}]}}");
    // 已有文档上覆盖 + 字符串转义（引号/换行/反斜杠）
    const std::wstring j2 =
        OpJsonSetString(api.handle, L"{\"k\":\"old\"}", L"k", L"he said \"hi\"\n\\end");
    EXPECT_EQ(OpJsonIsValid(api.handle, j2.c_str()), 1);
    EXPECT_EQ(GetStr(api.handle, j2.c_str(), L"k"), L"he said \"hi\"\n\\end");
    // 类型转换：字符串路径写入数值/布尔
    const std::wstring j3 = OpJsonSetNumber(api.handle, L"{\"k\":\"old\"}", L"k", 2.5);
    EXPECT_EQ(j3, L"{\"k\":2.5}");
    double num = 0.0;
    EXPECT_EQ(OpJsonGetNumber(api.handle, j3.c_str(), L"k", &num), 1);
    const std::wstring j4 = OpJsonSetBool(api.handle, j3.c_str(), L"k", 0);
    EXPECT_EQ(j4, L"{\"k\":false}");
    // 整数按整数序列化（42.0 不输出成 42.000000）
    EXPECT_STREQ(OpJsonSetNumber(api.handle, L"{}", L"n", 42.0), L"{\"n\":42}");
}

TEST(JsonValueTest, AppendBuildsArrayAndValidatesElement) {
    CApiHandle api;
    const std::wstring j1 = OpJsonAppend(api.handle, L"", L"items", L"1");
    EXPECT_EQ(j1, L"{\"items\":[1]}");
    const std::wstring j2 = OpJsonAppend(api.handle, j1.c_str(), L"items", L"{\"x\":true}");
    EXPECT_EQ(j2, L"{\"items\":[1,{\"x\":true}]}");
    EXPECT_EQ(OpJsonGetSize(api.handle, j2.c_str(), L"items"), 2);
    // 元素非法 → 空串
    EXPECT_STREQ(OpJsonAppend(api.handle, j2.c_str(), L"items", L"{bad"), L"");
    // 目标是标量 → 空串
    EXPECT_STREQ(OpJsonAppend(api.handle, L"{\"n\":1}", L"n", L"1"), L"");
}

TEST(JsonValueTest, DeleteKeyAndArrayElement) {
    CApiHandle api;
    EXPECT_STREQ(OpJsonDelete(api.handle, L"{\"a\":1,\"b\":2}", L"a"), L"{\"b\":2}");
    EXPECT_STREQ(OpJsonDelete(api.handle, L"[1,2,3]", L"[1]"), L"[1,3]");
    EXPECT_STREQ(OpJsonDelete(api.handle, L"[1,2,3]", L"1"), L"[1,3]"); // 数字键兼容
    // 不存在 → 空串
    EXPECT_STREQ(OpJsonDelete(api.handle, L"{\"a\":1}", L"z"), L"");
    // 删除后原数组下标语义变化，再删越界
    EXPECT_STREQ(OpJsonDelete(api.handle, L"[1]", L"[5]"), L"");
}

TEST(JsonValueTest, NullHandleReturnsFailure) {
    EXPECT_EQ(OpJsonIsValid(nullptr, L"{}"), 0);
    EXPECT_STREQ(OpJsonGetString(nullptr, L"{}", L""), L"");
    double num = 1.0;
    EXPECT_EQ(OpJsonGetNumber(nullptr, L"{}", L"", &num), 0);
    int b = 1;
    EXPECT_EQ(OpJsonGetBool(nullptr, L"{}", L"", &b), 0);
    EXPECT_EQ(OpJsonGetSize(nullptr, L"{}", L""), 0); // 空句柄统一 0（项目 null-handle 惯例），-1 仅有效句柄下的"非集合"
    EXPECT_STREQ(OpJsonGetKeys(nullptr, L"{}", L""), L"");
    EXPECT_STREQ(OpJsonSetString(nullptr, L"{}", L"", L"x"), L"");
    EXPECT_STREQ(OpJsonSetNumber(nullptr, L"{}", L"", 1.0), L"");
    EXPECT_STREQ(OpJsonSetBool(nullptr, L"{}", L"", 1), L"");
    EXPECT_STREQ(OpJsonAppend(nullptr, L"{}", L"", L"1"), L"");
    EXPECT_STREQ(OpJsonDelete(nullptr, L"{}", L""), L"");
}

} // namespace
