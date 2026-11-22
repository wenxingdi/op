// 汇编模块（批 D 2026-11-22）。
//
// 被测范围：AsmCall（当前进程路径）。**不测**目标进程路径（需要真实的第三方进程 +
// 管理员权限，属环境依赖项）；也不测 Assemble/Disassemble —— 它们本轮未实现，
// 这里只验证"未实现时显式失败而不是假装成功"这条契约。
//
// 诚实说明：本文件所有用例都走 hwnd=0（当前进程）。这是有意为之：
//   * 当前进程执行机器码不需要拉起第三方载体、不需要提权，判据完全确定；
//   * 目标进程路径用的是 BlackBone 的成熟 API，风险在权限而非逻辑，
//     用当前进程验证"机器码解析 + 执行 + 取返回值"这条链已足够。

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

long JsonNumber(const std::wstring &json, const wchar_t *key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos)
        return -99999;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == L' ' || json[pos] == L'\t'))
        ++pos;
    return static_cast<long>(wcstol(json.c_str() + pos, nullptr, 10));
}

std::wstring Take(const wchar_t *p) {
    return p ? std::wstring(p) : std::wstring();
}

} // namespace

// 基础闭环：mov rax, 42; ret  ->  返回值 42。
// 这条用例同时验证了"十六进制解析 / 可执行内存 / 调用约定 / 结果回读"四件事。
TEST(AsmBatchTest, AsmCallReturnsValueInCurrentProcess) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    int ok = 0;
    // x64: 48 C7 C0 2A 00 00 00 = mov rax, 0x2A; C3 = ret
    const std::wstring json = Take(OpAsmCall(h.handle, 0, L"48 C7 C0 2A 00 00 00 C3", 5000, &ok));
    ASSERT_FALSE(json.empty()) << "AsmCall 返回空串";
    EXPECT_EQ(1, ok);
    EXPECT_EQ(1, JsonNumber(json, L"ok"));
    EXPECT_EQ(42, JsonNumber(json, L"ret")) << "返回值应为 0x2A=42，实际: "
                                            << std::string(json.begin(), json.end());
    EXPECT_NE(std::wstring::npos, json.find(L"\"ret_hex\":\"2A\"")) << "ret_hex 应为大写无前缀 2A";
}

// 换一个值再验一次：排除"碰巧读到寄存器残留"这种假绿。
TEST(AsmBatchTest, AsmCallDifferentValueIsNotStale) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    int ok = 0;
    // mov rax, 0x12345678; ret
    const std::wstring json = Take(OpAsmCall(h.handle, 0, L"48 C7 C0 78 56 34 12 C3", 5000, &ok));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(0x12345678, JsonNumber(json, L"ret"));
    EXPECT_NE(std::wstring::npos, json.find(L"\"ret_hex\":\"12345678\""));
}

// 返回 0 也要能正确读出：如果实现用"ret==0 当作失败"，这条会红。
TEST(AsmBatchTest, AsmCallZeroResultIsSuccess) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    int ok = 0;
    // xor eax, eax; ret
    const std::wstring json = Take(OpAsmCall(h.handle, 0, L"31 C0 C3", 5000, &ok));
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(1, ok) << "返回值 0 必须仍算执行成功";
    EXPECT_EQ(1, JsonNumber(json, L"ok"));
    EXPECT_EQ(0, JsonNumber(json, L"ret"));
}

// 非法机器码必须显式失败（返回 nullptr 或 ok=0），且**绝不能**分配可执行内存去跑。
// 反例：如果实现"解析失败就当空操作"，调用方会以为执行成功。
TEST(AsmBatchTest, InvalidCodeFailsExplicitly) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    int ok = -1;

    const std::wstring r1 = Take(OpAsmCall(h.handle, 0, L"", 5000, &ok));
    EXPECT_EQ(0, ok) << "空代码应失败";
    EXPECT_EQ(0, JsonNumber(r1, L"ok"));

    ok = -1;
    const std::wstring r2 = Take(OpAsmCall(h.handle, 0, L"zz yy xx", 5000, &ok));
    EXPECT_EQ(0, ok) << "非十六进制应失败";
    EXPECT_EQ(0, JsonNumber(r2, L"ok"));

    ok = -1;
    // 超长 token（3 位十六进制）必须被拒 —— 这类输入几乎都是调用方拼串出错
    const std::wstring r3 = Take(OpAsmCall(h.handle, 0, L"1234 C3", 5000, &ok));
    EXPECT_EQ(0, ok) << "超过 1 字节的 token 应失败";
    EXPECT_EQ(0, JsonNumber(r3, L"ok"));
}

// Assemble / Disassemble 本轮未实现：必须**显式失败**并给出原因，
// 不能返回"看起来成功但内容为空"。
TEST(AsmBatchTest, UnimplementedOpsFailWithReason) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok = -1;
    const std::wstring asm_json = Take(OpAssemble(h.handle, L"mov rax, 42", 64, &ok));
    EXPECT_EQ(0, ok) << "Assemble 未实现时必须报告失败";
    EXPECT_EQ(0, JsonNumber(asm_json, L"ok"));
    EXPECT_NE(std::wstring::npos, asm_json.find(L"error")) << "必须带 error 字段说明原因";

    ok = -1;
    const std::wstring dis_json = Take(OpDisassemble(h.handle, L"48 C7 C0 2A 00 00 00 C3", L"0", 8, &ok));
    EXPECT_EQ(0, ok) << "Disassemble 未实现时必须报告失败";
    EXPECT_EQ(0, JsonNumber(dis_json, L"ok"));
    EXPECT_NE(std::wstring::npos, dis_json.find(L"error"));
}
