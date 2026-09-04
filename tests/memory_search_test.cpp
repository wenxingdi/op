// 内存特征码搜索 / 模块基址 测试（本进程自测，免注入、沙箱可跑）
#include <gtest/gtest.h>

#include <op_c_api.h>

#include <Windows.h>
#include <psapi.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

// 数据段特征码靶子（volatile 防优化消除；用运行时实际地址校验命中）
volatile unsigned char kMagic[] = {0x0F, 0x1F, 0x84, 0x00, 0xAA, 0x55, 0xC3, 0x90,
                                   0x11, 0x22, 0x33, 0x44, 0xDE, 0xAD, 0xBE, 0xEF};

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

struct ModuleRange {
    uintptr_t base = 0;
    size_t size = 0;
};

ModuleRange ExeRange() {
    ModuleRange r;
    const HMODULE exe = ::GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (exe && ::GetModuleInformation(::GetCurrentProcess(), exe, &mi, sizeof(mi))) {
        r.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        r.size = mi.SizeOfImage;
    }
    return r;
}

std::wstring HexRange(const ModuleRange &r) {
    std::wstringstream ss;
    ss << std::uppercase << std::hex << r.base << L"-" << (r.base + r.size - 1);
    return ss.str();
}

std::vector<uintptr_t> ParseHits(const wchar_t *s) {
    std::vector<uintptr_t> hits;
    if (!s || !*s)
        return hits;
    std::wstring text(s);
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t next = text.find(L'|', pos);
        const std::wstring token = text.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        if (!token.empty())
            hits.push_back(static_cast<uintptr_t>(wcstoull(token.c_str(), nullptr, 16)));
        if (next == std::wstring::npos)
            break;
        pos = next + 1;
    }
    return hits;
}

bool CoversMagic(const std::vector<uintptr_t> &hits) {
    const uintptr_t want = reinterpret_cast<uintptr_t>(const_cast<unsigned char *>(kMagic));
    for (const uintptr_t h : hits) {
        if (h <= want && want < h + sizeof(kMagic))
            return true;
    }
    return false;
}

TEST(MemorySearchTest, FindDataHitsMagicInModuleRange) {
    CApiHandle api;
    const ModuleRange mr = ExeRange();
    ASSERT_NE(mr.base, static_cast<uintptr_t>(0));

    const wchar_t *res = OpFindDataEx(api.handle, 0, HexRange(mr).c_str(),
                                      L"0F1F8400AA55C39011223344DEADBEEF", 1, 0);
    ASSERT_NE(res, nullptr);
    const std::vector<uintptr_t> hits = ParseHits(res);
    ASSERT_FALSE(hits.empty()) << "exact pattern found no hit in exe range";
    EXPECT_TRUE(CoversMagic(hits));
}

TEST(MemorySearchTest, FindDataWildcardMatches) {
    CApiHandle api;
    const ModuleRange mr = ExeRange();
    ASSERT_NE(mr.base, static_cast<uintptr_t>(0));

    const wchar_t *res = OpFindDataEx(api.handle, 0, HexRange(mr).c_str(),
                                      L"0F1F????AA55C390????????DEADBEEF", 1, 0);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(CoversMagic(ParseHits(res)));
}

TEST(MemorySearchTest, FindDataRespectsResultCap) {
    CApiHandle api;
    const ModuleRange mr = ExeRange();
    ASSERT_NE(mr.base, static_cast<uintptr_t>(0));

    // 常见字节 0x00 必然大量命中；count=3 必须截断到 <=3
    const wchar_t *res = OpFindDataEx(api.handle, 0, HexRange(mr).c_str(), L"00", 1, 3);
    ASSERT_NE(res, nullptr);
    EXPECT_LE(ParseHits(res).size(), 3u);
}

TEST(MemorySearchTest, FindDataInvalidInputReturnsEmpty) {
    CApiHandle api;
    // 奇数长度 / 非十六进制字符 → 空
    EXPECT_STREQ(OpFindData(api.handle, 0, L"", L"0F1F8"), L"");
    EXPECT_STREQ(OpFindData(api.handle, 0, L"", L"ZZZZ"), L"");
    // 全通配无意义 → 空
    EXPECT_STREQ(OpFindData(api.handle, 0, L"", L"????"), L"");
    // 坏 range（无连字符）→ 空
    EXPECT_STREQ(OpFindData(api.handle, 0, L"xyz", L"00"), L"");
}

TEST(MemorySearchTest, GetModuleBaseAddrReturnsHexForKnownModule) {
    CApiHandle api;
    const wchar_t *base = OpGetModuleBaseAddr(api.handle, 0, L"kernel32.dll");
    ASSERT_NE(base, nullptr);
    ASSERT_STRNE(base, L"");
    const uintptr_t addr = wcstoull(base, nullptr, 16);
    EXPECT_NE(addr, static_cast<uintptr_t>(0));

    const HMODULE h = ::GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(addr, reinterpret_cast<uintptr_t>(h));
}

TEST(MemorySearchTest, GetModuleBaseAddrMissingModuleReturnsEmpty) {
    CApiHandle api;
    EXPECT_STREQ(OpGetModuleBaseAddr(api.handle, 0, L"no_such_module_xyz.dll"), L"");
}

} // namespace
