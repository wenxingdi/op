// 内存三件套 + 模块列表自测（P2 第二十二批，2026-10-15）。
// 全部走本进程（hwnd=0）：分配/保护/读写闭环可在任何桌面会话验证；
// 模块大小与列表用进程自身 exe 断言，不依赖外部目标。

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <Windows.h>

#include <cwchar>
#include <string>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 当前 exe 的模块基名（如 "op_test.exe"）。
std::wstring self_module_name() {
    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        return L"";
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }
    return base;
}

// 大写十六进制串 -> uintptr（测试辅助）。
uintptr_t hex_to_ptr(const wchar_t *hex) {
    if (!hex)
        return 0;
    uintptr_t v = 0;
    while (*hex) {
        const wchar_t c = *hex++;
        v <<= 4;
        if (c >= L'0' && c <= L'9')
            v |= static_cast<uintptr_t>(c - L'0');
        else if (c >= L'A' && c <= L'F')
            v |= static_cast<uintptr_t>(c - L'A' + 10);
        else if (c >= L'a' && c <= L'f')
            v |= static_cast<uintptr_t>(c - L'a' + 10);
        else
            return 0;
    }
    return v;
}

TEST(MemoryExtTest, GetModuleSizeSelfAndMissing) {
    CApiHandle api;
    const std::wstring self = self_module_name();
    ASSERT_FALSE(self.empty());
    // 自身模块大小：PE 映像 SizeOfImage，至少 64KB 量级
    const long size = OpGetModuleSize(api.handle, 0, self.c_str());
    EXPECT_GT(size, 0);
    // 不存在的模块返回 0
    EXPECT_EQ(OpGetModuleSize(api.handle, 0, L"no_such_module_xyz.dll"), 0);
    // 空串返回 0
    EXPECT_EQ(OpGetModuleSize(api.handle, 0, L""), 0);
}

TEST(MemoryExtTest, GetModuleInfoListContainsSelf) {
    CApiHandle api;
    const std::wstring json = OpGetModuleInfoList(api.handle, 0);
    ASSERT_FALSE(json.empty());
    EXPECT_NE(json.find(L"\"ok\":1"), std::wstring::npos);
    EXPECT_NE(json.find(L"\"modules\":["), std::wstring::npos);
    const std::wstring self = self_module_name();
    ASSERT_FALSE(self.empty());
    EXPECT_NE(json.find(self), std::wstring::npos) << "模块列表应包含自身 exe: " << self.c_str();
}

TEST(MemoryExtTest, VirtualAllocProtectWriteReadRoundTrip) {
    CApiHandle api;
    // 默认参数分配 4KB（MEM_COMMIT|MEM_RESERVE / PAGE_READWRITE）
    // 注意：返回值在共享 string_result 缓冲里，OpReadData 等 call_string 接口会先 clear
    // 它——跨调用使用前必须立即拷贝（与 crypto_draw_test 的 Base64 坑同源）。
    const std::wstring addr = OpVirtualAllocEx(api.handle, 0, L"", 4096, 0, 0);
    ASSERT_FALSE(addr.empty());
    const std::wstring addr_hex = addr;
    const uintptr_t addr_value = hex_to_ptr(addr_hex.c_str());
    ASSERT_NE(addr_value, 0u);
    EXPECT_EQ(addr_value % 4096, 0u) << "VirtualAllocEx 返回应按页对齐";

    // 默认可读写：写 "AB12CD" 三字节再读回
    EXPECT_EQ(OpWriteData(api.handle, 0, addr_hex.c_str(), L"AB12CD", 3), 1);
    const std::wstring read_back = OpReadData(api.handle, 0, addr_hex.c_str(), 3);
    EXPECT_EQ(read_back, L"AB12CD");

    // 改保护为 PAGE_NOACCESS(1)，原保护应为 PAGE_READWRITE(4)
    int old_protect = 0;
    EXPECT_EQ(OpVirtualProtectEx(api.handle, 0, addr_hex.c_str(), 4096, 1, &old_protect), 1);
    EXPECT_EQ(old_protect, 4);
    // 保护改成 NOACCESS 后 ReadData 应失败返回空串（读自己进程的 NOACCESS 会失败）
    EXPECT_EQ(std::wstring(OpReadData(api.handle, 0, addr_hex.c_str(), 3)), std::wstring());
    // 改回 READWRITE，读恢复
    EXPECT_EQ(OpVirtualProtectEx(api.handle, 0, addr_hex.c_str(), 4096, 4, &old_protect), 1);
    EXPECT_EQ(old_protect, 1);
    EXPECT_EQ(std::wstring(OpReadData(api.handle, 0, addr_hex.c_str(), 3)), L"AB12CD");

    // 非法表达式地址：VirtualProtectEx 失败、VirtualAllocEx 失败
    EXPECT_EQ(OpVirtualProtectEx(api.handle, 0, L"zzz-not-hex", 4096, 4, &old_protect), 0);
    EXPECT_EQ(std::wstring(OpVirtualAllocEx(api.handle, 0, L"zzz-not-hex", 4096, 0, 0)), std::wstring());
    // 非法窗口句柄：模块大小返回 0
    EXPECT_EQ(OpGetModuleSize(api.handle, 0x7FFFFFFE, self_module_name().c_str()), 0);
}

} // namespace
