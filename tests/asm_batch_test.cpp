// 汇编模块（批 D 2026-11-22；59 批 2026-11-23 补齐 Assemble/Disassemble）。
//
// 被测范围：AsmCall / Assemble / Disassemble，全部走 hwnd=0（当前进程）。
// **不测** AsmCall 的目标进程路径（需要真实第三方进程 + 管理员权限，属环境依赖项）。
//
// 诚实说明：本文件所有用例都走 hwnd=0。这是有意为之：
//   * 当前进程执行机器码不需要拉起第三方载体、不需要提权，判据完全确定；
//   * 目标进程路径用的是 BlackBone 的成熟 API，风险在权限而非逻辑，
//     用当前进程验证"机器码解析 + 执行 + 取返回值"这条链已足够。
//
// 判据设计：汇编/反汇编最容易出的错是**位数搞反**（32/64 模式）。这种错不会报错，
// 只会安静产出错码，所以每条相关用例都用"32 位能编、64 位编不过"或
// "同一字节串在两种模式下的助记符不同"来做**区分性**判据，而不是只断言"能跑通"。

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

// 取字符串字段的原值（不反转义：本文件断言的字段里没有需要转义的字符）
std::wstring JsonString(const std::wstring &json, const wchar_t *key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos)
        return L"<缺失>";
    pos += needle.size();
    const size_t end = json.find(L'"', pos);
    if (end == std::wstring::npos)
        return L"<未闭合>";
    return json.substr(pos, end - pos);
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

// Assemble：文本 -> 机器码。判据用**确定的字节**，不是"非空即可"。
TEST(AsmBatchTest, AssembleEmitsExactBytes) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    int ok = -1;
    const std::wstring json = Take(OpAssemble(h.handle, L"mov rax, 42", 64, &ok));
    ASSERT_EQ(1, ok) << "汇编失败: " << std::string(json.begin(), json.end());
    EXPECT_EQ(1, JsonNumber(json, L"ok"));
    EXPECT_EQ(7, JsonNumber(json, L"size"));
    EXPECT_EQ(1, JsonNumber(json, L"count"));
    // 48 C7 C0 2A 00 00 00 = mov rax, 0x2A（LLVM 选 C7 /0 imm32 形态）
    EXPECT_EQ(std::wstring(L"48 C7 C0 2A 00 00 00"), JsonString(json, L"hex"));
}

// ★ 数字字面量口径：裸数字必须是**十进制**，十六进制用 0x 前缀。
// 这条钉的是 keystone 0.9.2 的 radix bug（原实现无条件 setRadix(16)：`mov rax, 100`
// 会编成 0x100=256，而且没有任何语法能写十进制）。判据用两个**不同**的立即数，
// 若 radix 退回 16，第一条必红（100 -> 0x100）。
TEST(AsmBatchTest, AssembleNumberRadixIsDecimal) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok = -1;
    const std::wstring dec = Take(OpAssemble(h.handle, L"mov rax, 100", 64, &ok));
    ASSERT_EQ(1, ok) << std::string(dec.begin(), dec.end());
    EXPECT_EQ(std::wstring(L"48 C7 C0 64 00 00 00"), JsonString(dec, L"hex"))
        << "十进制 100 应编码为 0x64；若得到 00 01 说明裸数字被当成十六进制";

    ok = -1;
    const std::wstring hex = Take(OpAssemble(h.handle, L"mov rax, 0x100", 64, &ok));
    ASSERT_EQ(1, ok) << std::string(hex.begin(), hex.end());
    EXPECT_EQ(std::wstring(L"48 C7 C0 00 01 00 00"), JsonString(hex, L"hex"))
        << "0x100 应编码为 0x00000100";
}

// ★ 位数区分性判据：`mov rax, 42` 在 32 位模式**非法**（没有 64 位寄存器）。
// 若实现把 arch 忽略/写死成一种模式，这条必红。
TEST(AsmBatchTest, AssembleRespectsArchWidth) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok64 = -1;
    const std::wstring r64 = Take(OpAssemble(h.handle, L"mov rax, 42", 64, &ok64));
    EXPECT_EQ(1, ok64) << "64 位模式应汇编成功";
    EXPECT_EQ(1, JsonNumber(r64, L"ok"));

    int ok32 = -1;
    const std::wstring r32 = Take(OpAssemble(h.handle, L"mov rax, 42", 32, &ok32));
    EXPECT_EQ(0, ok32) << "32 位模式编 mov rax 必须失败（否则 arch 被忽略了）";
    EXPECT_EQ(0, JsonNumber(r32, L"ok"));
    EXPECT_NE(std::wstring::npos, r32.find(L"error")) << "失败必须带 error 文本";

    // 反向：32 位合法、64 位非法的形态（push eax）
    int ok32b = -1;
    const std::wstring r32b = Take(OpAssemble(h.handle, L"push eax", 32, &ok32b));
    EXPECT_EQ(1, ok32b) << "32 位模式编 push eax 应成功";
    EXPECT_EQ(std::wstring(L"50"), JsonString(r32b, L"hex"));
}

// ★ 端到端：assemble 出来的 hex 直接喂 asm_call 执行 —— 两个接口的格式必须同构。
// 这条同时是"汇编结果真的是可执行码"的最强证据（内容错了返回值就不可能对）。
TEST(AsmBatchTest, AssembleThenAsmCallRunsTheCode) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok = -1;
    const std::wstring asm_json = Take(OpAssemble(h.handle, L"mov rax, 7; ret", 64, &ok));
    ASSERT_EQ(1, ok) << "汇编失败: " << std::string(asm_json.begin(), asm_json.end());
    const std::wstring hex = JsonString(asm_json, L"hex");
    ASSERT_NE(std::wstring(L"<缺失>"), hex);

    int run_ok = -1;
    const std::wstring run_json = Take(OpAsmCall(h.handle, 0, hex.c_str(), 5000, &run_ok));
    EXPECT_EQ(1, run_ok) << "执行失败: " << std::string(run_json.begin(), run_json.end());
    EXPECT_EQ(7, JsonNumber(run_json, L"ret")) << "汇编出的码执行结果应为 7";
}

// Disassemble：固定字节 -> 固定助记符。count=1 时只解一条（顺带验证 count 生效）。
TEST(AsmBatchTest, DisassembleDecodesKnownBytes) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    int ok = -1;
    // B8 2A 00 00 00 = mov eax, 0x2A ; C3 = ret
    const std::wstring json = Take(OpDisassemble(h.handle, L"B8 2A 00 00 00 C3", L"401000", 0, 32, &ok));
    ASSERT_EQ(1, ok) << "反汇编失败: " << std::string(json.begin(), json.end());
    EXPECT_EQ(1, JsonNumber(json, L"ok"));
    EXPECT_EQ(2, JsonNumber(json, L"count")) << "6 字节应解出 2 条";
    EXPECT_EQ(32, JsonNumber(json, L"mode"));
    EXPECT_NE(std::wstring::npos, json.find(L"\"mnemonic\":\"mov\""));
    EXPECT_NE(std::wstring::npos, json.find(L"\"op_str\":\"eax, 0x2a\""));
    EXPECT_NE(std::wstring::npos, json.find(L"\"mnemonic\":\"ret\""));
    // 首条地址应等于传入的 401000
    EXPECT_NE(std::wstring::npos, json.find(L"\"addr\":\"401000\""));

    int ok1 = -1;
    const std::wstring one = Take(OpDisassemble(h.handle, L"B8 2A 00 00 00 C3", L"401000", 1, 32, &ok1));
    EXPECT_EQ(1, ok1);
    EXPECT_EQ(1, JsonNumber(one, L"count")) << "count=1 必须只解一条";
}

// ★ 位数区分性判据：0x50 在 32 位是 push eax，64 位是 push rax。
// 若实现把 arch 忽略，两次结果的 op_str 会一样 → 这条红。
TEST(AsmBatchTest, DisassembleArchChangesOperandWidth) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok32 = -1;
    const std::wstring r32 = Take(OpDisassemble(h.handle, L"50", L"0", 1, 32, &ok32));
    ASSERT_EQ(1, ok32);
    EXPECT_EQ(std::wstring(L"eax"), JsonString(r32, L"op_str")) << "32 位下 0x50 是 push eax";

    int ok64 = -1;
    const std::wstring r64 = Take(OpDisassemble(h.handle, L"50", L"0", 1, 64, &ok64));
    ASSERT_EQ(1, ok64);
    EXPECT_EQ(std::wstring(L"rax"), JsonString(r64, L"op_str")) << "64 位下 0x50 是 push rax";
    EXPECT_EQ(64, JsonNumber(r64, L"mode"));
}

// assemble -> disassemble 往返：文本进来、同样的文本出去。
TEST(AsmBatchTest, AssembleDisassembleRoundTrip) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok = -1;
    const std::wstring asm_json = Take(OpAssemble(h.handle, L"xor eax, eax; ret", 32, &ok));
    ASSERT_EQ(1, ok) << std::string(asm_json.begin(), asm_json.end());
    const std::wstring hex = JsonString(asm_json, L"hex");

    int dok = -1;
    const std::wstring dis = Take(OpDisassemble(h.handle, hex.c_str(), L"0", 0, 32, &dok));
    ASSERT_EQ(1, dok) << std::string(dis.begin(), dis.end());
    EXPECT_EQ(2, JsonNumber(dis, L"count"));
    EXPECT_NE(std::wstring::npos, dis.find(L"\"mnemonic\":\"xor\""));
    EXPECT_NE(std::wstring::npos, dis.find(L"\"mnemonic\":\"ret\""));
}

// 非法输入必须显式失败：空串 / 非十六进制 / 非法 arch / 非法 address。
// 反例：若实现把 arch 非法值默默当 64 处理，调用方永远发现不了自己传错了。
TEST(AsmBatchTest, InvalidInputsFailExplicitly) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);

    int ok = -1;
    const std::wstring e1 = Take(OpAssemble(h.handle, L"", 64, &ok));
    EXPECT_EQ(0, ok) << "空汇编文本应失败";
    EXPECT_EQ(0, JsonNumber(e1, L"ok"));

    ok = -1;
    const std::wstring e2 = Take(OpAssemble(h.handle, L"this is not asm", 64, &ok));
    EXPECT_EQ(0, ok) << "无法汇编的文本应失败";
    EXPECT_EQ(0, JsonNumber(e2, L"ok"));
    EXPECT_NE(std::wstring::npos, e2.find(L"error"));

    ok = -1;
    const std::wstring e3 = Take(OpAssemble(h.handle, L"mov rax, 42", 33, &ok));
    EXPECT_EQ(0, ok) << "arch=33 必须被拒，不能默认成某一种位数";
    EXPECT_EQ(0, JsonNumber(e3, L"ok"));

    ok = -1;
    const std::wstring d1 = Take(OpDisassemble(h.handle, L"", L"0", 0, 64, &ok));
    EXPECT_EQ(0, ok) << "空字节串应失败";

    ok = -1;
    const std::wstring d2 = Take(OpDisassemble(h.handle, L"zz", L"0", 0, 64, &ok));
    EXPECT_EQ(0, ok) << "非十六进制应失败";

    ok = -1;
    const std::wstring d3 = Take(OpDisassemble(h.handle, L"90", L"not-an-addr", 0, 64, &ok));
    EXPECT_EQ(0, ok) << "非法 address 应失败";
    EXPECT_NE(std::wstring::npos, d3.find(L"error"));

    ok = -1;
    const std::wstring d4 = Take(OpDisassemble(h.handle, L"90", L"0", 0, 33, &ok));
    EXPECT_EQ(0, ok) << "arch=33 必须被拒";
}

// 连续调用不泄漏、不互相污染：keystone/capstone 句柄必须每次成对释放。
// 判据：同一输入重复 200 次结果完全一致（句柄复用/泄漏导致的串味会先在这里显形）。
TEST(AsmBatchTest, RepeatedCallsAreStable) {
    CApiHandle h;
    ASSERT_NE(nullptr, h.handle);
    std::wstring first;
    for (int i = 0; i < 200; ++i) {
        int ok = -1;
        const std::wstring json = Take(OpAssemble(h.handle, L"mov rax, 42; ret", 64, &ok));
        ASSERT_EQ(1, ok) << "第 " << i << " 次汇编失败";
        const std::wstring hex = JsonString(json, L"hex");
        if (i == 0)
            first = hex;
        else
            ASSERT_EQ(first, hex) << "第 " << i << " 次结果与首次不一致";
    }
    EXPECT_EQ(std::wstring(L"48 C7 C0 2A 00 00 00 C3"), first);
}
