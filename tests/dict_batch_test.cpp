// 字库模块修复验证（第六十一批 2026-11-23）。
//
// 覆盖四个此前实际会丢数据的场景：
//   1. 条数超过 int16 上限（32767）后「保存报成功、读回 0 条」
//   2. 空名条目覆盖已有名字，随后被 write_dict 当空名剔除
//   3. AddDict 多行传入时整批失败（无批量能力，也无原子性）
//   4. 无单条删除，改错一条只能 ClearDict 整槽重来
//
// 断言口径：全部走公开接口（Op / C API），不碰 Dictionary 内部结构；
// 唯一的「内部」读取是 .dict 文件头的前两个字节（版本号），因为版本选择本身是契约。

#include "test_support.h"

#include "libop.h"
#include "op_c_api.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// 8x11 = 88 bit 的唯一点阵：前 4 字节放 id 的乘法散列（奇数乘法在 2^32 上是双射，
// 所以不同 id 必然得到不同点阵），后 7 字节用另一条散列填充保证 bit_cnt 有变化。
std::wstring MakeUniqueEntry(int id, const wchar_t *name) {
    constexpr int kW = 8;
    constexpr int kH = 11;
    constexpr size_t kBytes = (kW * kH + 7) / 8; // 11

    const uint32_t seed = static_cast<uint32_t>(id) * 2654435761u;
    std::vector<uint8_t> data(kBytes, 0);
    for (size_t i = 0; i < kBytes; ++i) {
        if (i < 4) {
            data[i] = static_cast<uint8_t>((seed >> (8 * i)) & 0xFF);
        } else {
            const uint32_t chunk = (seed + 0x9E3779B9u * static_cast<uint32_t>(i)) ^ (0x85EBCA6Bu * (i + 1));
            data[i] = static_cast<uint8_t>(chunk & 0xFF);
        }
    }

    int bits = 0;
    for (uint8_t byte : data) {
        for (int bit = 0; bit < 8; ++bit)
            bits += (byte >> bit) & 1;
    }
    if (bits == 0) { // id=0 的极端情况：至少留一位，否则不是合法字条
        data[0] = 0x01;
        bits = 1;
    }

    wchar_t hex[kBytes * 2 + 1] = {0};
    for (size_t i = 0; i < kBytes; ++i)
        swprintf_s(hex + i * 2, 3, L"%02X", data[i]);

    return std::wstring(name) + L"$" + std::to_wstring(kH) + L"," + std::to_wstring(kW) + L"," +
           std::to_wstring(bits) + L"$" + hex;
}

// 读 .dict 文件头的版本号（前 2 字节 int16）。版本选择是格式契约，属于可断言的外观。
int ReadDictFileVersion(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return -1;
    int16_t ver = -1;
    if (!file.read(reinterpret_cast<char *>(&ver), sizeof(ver)))
        return -1;
    return ver;
}

std::filesystem::path TempDictPath(const wchar_t *name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

// 多行一次性入库：返回成功处理的行数，且三条都真的进去了
TEST(DictBatchTest, AddDictAcceptsMultipleLines) {
    op::Op op;
    long ret = 0;

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    const std::wstring batch = MakeUniqueEntry(1, L"A") + L"\n" + MakeUniqueEntry(2, L"B") + L"\n" +
                               MakeUniqueEntry(3, L"C");
    op.AddDict(0, batch.c_str(), &ret);
    EXPECT_EQ(ret, 3) << "多行应返回成功处理的行数";

    long count = 0;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 3);
}

// 整批原子：第二行格式错时，第一行也不能进库（旧实现逐条调用会留下半成功状态）
TEST(DictBatchTest, AddDictRejectsWholeBatchWhenAnyLineInvalid) {
    op::Op op;
    long ret = 0;

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    const std::wstring batch =
        MakeUniqueEntry(11, L"A") + L"\n" + L"坏行没有分隔符" + L"\n" + MakeUniqueEntry(13, L"C");
    op.AddDict(0, batch.c_str(), &ret);
    EXPECT_EQ(ret, 0) << "整批应失败";

    long count = -1;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 0) << "失败时不能有半批数据入库";
}

// 单条删除：只删掉匹配的那一条，其余保留；删不存在的点阵返回 0
TEST(DictBatchTest, RemoveDictDeletesOnlyMatchedEntry) {
    op::Op op;
    long ret = 0;

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    const std::wstring a = MakeUniqueEntry(21, L"A");
    const std::wstring b = MakeUniqueEntry(22, L"B");
    const std::wstring c = MakeUniqueEntry(23, L"C");
    op.AddDict(0, (a + L"\n" + b + L"\n" + c).c_str(), &ret);
    ASSERT_EQ(ret, 3);

    op.RemoveDict(0, b.c_str(), &ret);
    EXPECT_EQ(ret, 1) << "应删掉一条";

    long count = 0;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 2);

    // 被删的点阵再删一次必须返回 0（真的没了，而不是「删了但没生效」）
    op.RemoveDict(0, b.c_str(), &ret);
    EXPECT_EQ(ret, 0);

    // 剩下的两条必须还在：用名字核对，避免「删错了一条」也能通过条数断言
    std::wstring all;
    for (long i = 0; i < 2; ++i) {
        std::wstring one;
        op.GetDict(0, i, one);
        all += one.substr(0, 1);
    }
    EXPECT_NE(all.find(L"A"), std::wstring::npos) << "A 不该被删";
    EXPECT_NE(all.find(L"C"), std::wstring::npos) << "C 不该被删";
    EXPECT_EQ(all.find(L"B"), std::wstring::npos) << "B 应已删除";
}

// C API 导出层同样能删（防止 7 层链只通到 C++ 内部）
TEST(DictBatchTest, RemoveDictWorksThroughCApi) {
    op_handle handle = OpCreate();
    ASSERT_NE(handle, nullptr);

    const std::wstring entry = MakeUniqueEntry(61, L"A");
    OpClearDict(handle, 0);
    EXPECT_EQ(OpAddDict(handle, 0, entry.c_str()), 1);
    EXPECT_EQ(OpGetDictCount(handle, 0), 1);

    EXPECT_EQ(OpRemoveDict(handle, 0, entry.c_str()), 1);
    EXPECT_EQ(OpGetDictCount(handle, 0), 0);

    // 再删一次：不存在
    EXPECT_EQ(OpRemoveDict(handle, 0, entry.c_str()), 0);

    OpDestroy(handle);
}

// 空名不得擦掉已有标注：先 A 后空名（同点阵），名字必须还是 A
TEST(DictBatchTest, EmptyNameDoesNotEraseExistingName) {
    op::Op op;
    long ret = 0;

    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    const std::wstring named = MakeUniqueEntry(31, L"A");
    op.AddDict(0, named.c_str(), &ret);
    ASSERT_EQ(ret, 1);

    // 同点阵、空名
    const std::wstring unnamed = MakeUniqueEntry(31, L"");
    op.AddDict(0, unnamed.c_str(), &ret);
    EXPECT_EQ(ret, 1) << "同点阵再传空名仍是合法输入";

    long count = 0;
    op.GetDictCount(0, &count);
    EXPECT_EQ(count, 1) << "点阵相同应合并为一条";

    std::wstring stored;
    op.GetDict(0, 0, stored);
    EXPECT_EQ(stored.substr(0, 1), L"A") << "空名不能把已有名字擦掉";
}

// 全部为空名时保存必须失败：否则会出现「SaveDict 报成功、文件里 0 条」
TEST(DictBatchTest, SaveDictFailsWhenEveryNameIsEmpty) {
    const auto path = TempDictPath(L"op_dict_all_empty_name.dict");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    op::Op op;
    long ret = 0;
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    op.AddDict(0, MakeUniqueEntry(41, L"").c_str(), &ret);
    ASSERT_EQ(ret, 1);

    long count = 0;
    op.GetDictCount(0, &count);
    ASSERT_EQ(count, 1) << "内存里确实有一条（只是没名字）";

    op.SaveDict(0, path.c_str(), &ret);
    EXPECT_EQ(ret, 0) << "一条都写不出去时必须报失败，不能假装成功";

    std::filesystem::remove(path, ec);
}

// 条数突破 int16 上限：这是旧实现的硬故障（GetDictCount 返负数、SaveDict 成功但读回 0 条）
TEST(DictBatchTest, LargeDictionarySurvivesInt16Overflow) {
    constexpr int kTotal = 33000; // > 32767，旧实现必挂
    const auto path = TempDictPath(L"op_dict_large.dict");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    op::Op writer;
    long ret = 0;
    writer.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    // 一次多行传入（顺带复用批量路径，避免 33000 次 API 调用的开销）
    std::wstring batch;
    batch.reserve(static_cast<size_t>(kTotal) * 48);
    for (int i = 0; i < kTotal; ++i) {
        if (i)
            batch.push_back(L'\n');
        batch += MakeUniqueEntry(i, L"X");
    }
    writer.AddDict(0, batch.c_str(), &ret);
    ASSERT_EQ(ret, kTotal) << "全部 33000 行都应被接受";

    long count = -1;
    writer.GetDictCount(0, &count);
    EXPECT_EQ(count, kTotal) << "内存条数不能因 int16 溢出而变负";

    writer.SaveDict(0, path.c_str(), &ret);
    ASSERT_EQ(ret, 1) << "大字库必须能保存";
    ASSERT_EQ(ReadDictFileVersion(path), 2) << "超 32767 条应写 v2（真实条数用 int32）";

    // 读回：条数一致，且抽查的点阵确实在这份文件里
    op::Op reader;
    reader.SetDict(1, path.c_str(), &ret);
    ASSERT_EQ(ret, 1) << "v2 文件必须能读回";

    long read_count = 0;
    reader.GetDictCount(1, &read_count);
    EXPECT_EQ(read_count, kTotal);

    // 抽查存在性：RemoveDict 按点阵唯一匹配，正好当「这条在不在」的探针。
    // 比按索引取串可靠 —— 读回时会 sort_dict()，索引顺序和写入顺序不同。
    op::Op probe;
    probe.SetDict(3, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    for (int id : {0, kTotal / 2, kTotal - 1}) {
        long found = -1;
        probe.RemoveDict(3, MakeUniqueEntry(id, L"").c_str(), &found);
        EXPECT_EQ(found, 1) << "id=" << id << " 的点阵应存在于读回的字库中";
    }

    std::filesystem::remove(path, ec);
}

// v2 文件走内存入口也要能读：脚本常见做法是「读 .dict 字节 → SetMemDict」，
// 这条路径走的是 detect_memory_format + read_binary_dict_memory，与读文件不是同一段代码。
TEST(DictBatchTest, SetMemDictAcceptsExtendedV2Bytes) {
    constexpr int kTotal = 33000;
    const auto path = TempDictPath(L"op_dict_large_mem.dict");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    op::Op writer;
    long ret = 0;
    writer.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);

    std::wstring batch;
    batch.reserve(static_cast<size_t>(kTotal) * 48);
    for (int i = 0; i < kTotal; ++i) {
        if (i)
            batch.push_back(L'\n');
        batch += MakeUniqueEntry(i, L"X");
    }
    writer.AddDict(0, batch.c_str(), &ret);
    ASSERT_EQ(ret, kTotal);
    writer.SaveDict(0, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    ASSERT_EQ(ReadDictFileVersion(path), 2);

    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());

    op::Op via_memory;
    via_memory.SetMemDict(7, bytes.data(), static_cast<long>(bytes.size()), &ret);
    ASSERT_EQ(ret, 1) << "内存入口必须能识别 v2";

    long count = 0;
    via_memory.GetDictCount(7, &count);
    EXPECT_EQ(count, kTotal);

    std::filesystem::remove(path, ec);
}

// 小字库仍写 v1：这条保证「新版 DLL 存的文件，旧版 DLL 也能读」，即零回滚风险
TEST(DictBatchTest, SmallDictionaryStaysOnCompactFormat) {
    const auto path = TempDictPath(L"op_dict_small_v1.dict");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    op::Op op;
    long ret = 0;
    op.ClearDict(0, &ret);
    ASSERT_EQ(ret, 1);
    op.AddDict(0, (MakeUniqueEntry(51, L"A") + L"\n" + MakeUniqueEntry(52, L"B")).c_str(), &ret);
    ASSERT_EQ(ret, 2);

    op.SaveDict(0, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    EXPECT_EQ(ReadDictFileVersion(path), 1) << "未超限时必须保持 v1，不能无条件升版";

    // 读回同样正常
    op::Op reader;
    reader.SetDict(2, path.c_str(), &ret);
    ASSERT_EQ(ret, 1);
    long count = 0;
    reader.GetDictCount(2, &count);
    EXPECT_EQ(count, 2);

    std::filesystem::remove(path, ec);
}
