// 文件模块功能测试（P1 新增 2026-10-15）：Op*File* 全家在临时目录上的真实读写流程。
// 注意 c_api_null_handle_test.cpp 是生成物，勿在此补空句柄用例。

#include "op_c_api.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <string>
#include <thread>

namespace {

std::wstring make_temp_root() {
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    std::wstring root = temp;
    root += L"op_file_api_test_";
    wchar_t unique[MAX_PATH] = {};
    ::GetTempFileNameW(temp, L"opft", 0, unique);
    // GetTempFileName 会真的建出文件：删掉它，改作目录名
    ::DeleteFileW(unique);
    root = unique;
    return root;
}

class FileApiTest : public ::testing::Test {
  protected:
    void SetUp() override {
        handle = OpCreate();
        ASSERT_NE(handle, nullptr);
        root = make_temp_root();
        ASSERT_EQ(OpCreateFolder(handle, root.c_str()), 1);
    }
    void TearDown() override {
        if (handle) {
            OpDeleteFolder(handle, root.c_str());
            OpDestroy(handle);
        }
    }
    std::wstring path(const wchar_t *name) const {
        return root + L"\\" + name;
    }

    op_handle handle;
    std::wstring root;
};

TEST_F(FileApiTest, ExistsCreateDeleteFolder) {
    EXPECT_EQ(OpFileExists(handle, root.c_str()), 1);
    const std::wstring sub = path(L"a\\b\\c");
    EXPECT_EQ(OpCreateFolder(handle, sub.c_str()), 1) << "多级创建";
    EXPECT_EQ(OpFileExists(handle, sub.c_str()), 1);
    EXPECT_EQ(OpDeleteFolder(handle, sub.c_str()), 1) << "递归删除";
    EXPECT_EQ(OpFileExists(handle, sub.c_str()), 0);
    // 已存在目录再创建视为成功；同名文件视为失败
    EXPECT_EQ(OpCreateFolder(handle, root.c_str()), 1);
    const std::wstring as_file = path(L"notdir");
    ASSERT_EQ(OpWriteStringToFile(handle, as_file.c_str(), L"x"), 1);
    EXPECT_EQ(OpCreateFolder(handle, as_file.c_str()), 0);
    EXPECT_EQ(OpDeleteFolder(handle, as_file.c_str()), 0) << "文件不能按目录删";
    EXPECT_EQ(OpFileExists(handle, path(L"missing").c_str()), 0);
    EXPECT_EQ(OpFileExists(handle, L""), 0);
}

TEST_F(FileApiTest, WriteReadStringRoundTrip) {
    const std::wstring file = path(L"hello.txt");
    const wchar_t *content = L"第一行 ascii123\r\n第二行";
    ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), content), 1);
    const wchar_t *read = OpReadFileString(handle, file.c_str());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(std::wstring(read), content) << "UTF-8 带 BOM 往返应保持内容";
    // 覆盖写
    ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), L"覆盖"), 1);
    EXPECT_EQ(std::wstring(OpReadFileString(handle, file.c_str())), L"覆盖");
    // 读不存在的文件 → 空串，不崩溃
    EXPECT_EQ(std::wstring(OpReadFileString(handle, path(L"none.txt").c_str())), L"");
}

TEST_F(FileApiTest, CopyMoveDeleteFileAndSize) {
    const std::wstring src = path(L"src.txt");
    const std::wstring copied = path(L"copied.txt");
    const std::wstring moved = path(L"moved.txt");
    ASSERT_EQ(OpWriteStringToFile(handle, src.c_str(), L"12345"), 1);

    int ret = 0;
    EXPECT_EQ(OpGetFileSize(handle, src.c_str(), &ret), 8) << "BOM 3 字节 + 5 个 ASCII 字节";
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(OpGetFileSize(handle, path(L"none").c_str(), &ret), 0);
    EXPECT_EQ(ret, 0);

    ASSERT_EQ(OpCopyFile(handle, src.c_str(), copied.c_str()), 1);
    EXPECT_EQ(OpFileExists(handle, copied.c_str()), 1);
    ASSERT_EQ(OpMoveFile(handle, copied.c_str(), moved.c_str()), 1);
    EXPECT_EQ(OpFileExists(handle, copied.c_str()), 0);
    EXPECT_EQ(OpFileExists(handle, moved.c_str()), 1);
    EXPECT_EQ(OpDeleteFile(handle, moved.c_str()), 1);
    EXPECT_EQ(OpDeleteFile(handle, moved.c_str()), 0) << "重复删除应失败";
}

TEST_F(FileApiTest, GetFileListReturnsFullPaths) {
    ASSERT_EQ(OpWriteStringToFile(handle, path(L"a.log").c_str(), L"a"), 1);
    ASSERT_EQ(OpWriteStringToFile(handle, path(L"b.txt").c_str(), L"b"), 1);
    const std::wstring all = OpGetFileList(handle, root.c_str(), L"");
    EXPECT_NE(all.find(path(L"a.log")), std::wstring::npos) << all;
    EXPECT_NE(all.find(path(L"b.txt")), std::wstring::npos) << all;
    const std::wstring only_txt = OpGetFileList(handle, root.c_str(), L"*.txt");
    EXPECT_EQ(only_txt.find(L"a.log"), std::wstring::npos) << only_txt;
    EXPECT_NE(only_txt.find(L"b.txt"), std::wstring::npos) << only_txt;
    EXPECT_EQ(std::wstring(OpGetFileList(handle, path(L"nosuch").c_str(), L"")), L"");
}

TEST_F(FileApiTest, PathParsingHelpers) {
    const std::wstring full = path(L"data.ini");
    EXPECT_EQ(std::wstring(OpGetFileName(handle, full.c_str(), 0)), L"data.ini");
    EXPECT_EQ(std::wstring(OpGetFileName(handle, full.c_str(), 1)), L"data");
    EXPECT_EQ(std::wstring(OpGetFileName(handle, full.c_str(), 2)), L".ini");
    EXPECT_EQ(std::wstring(OpGetFileName(handle, full.c_str(), 3)), root);
    // 无扩展名 / 点文件
    EXPECT_EQ(std::wstring(OpGetFileName(handle, L"C:\\a\\Makefile", 1)), L"Makefile");
    EXPECT_EQ(std::wstring(OpGetFileName(handle, L"C:\\a\\.gitignore", 2)), L"");
    // 斜杠路径与绝对化
    const std::wstring abs = OpToAbsolutePath(handle, L"./x/../y.txt");
    ASSERT_FALSE(abs.empty());
    EXPECT_EQ(abs.back(), L't');
    EXPECT_EQ(std::wstring(OpGetFileName(handle, abs.c_str(), 0)), L"y.txt");
}

TEST_F(FileApiTest, AbsolutePathOfEmptyReturnsEmpty) {
    EXPECT_EQ(std::wstring(OpToAbsolutePath(handle, L"")), L"");
    EXPECT_EQ(std::wstring(OpGetFileName(handle, L"", 0)), L"");
}

// ===== 原子写语义（2026-11-23）=====
// 旧实现直接 CREATE_ALWAYS 截断目标文件：写到一半失败会毁掉原文件，且读方在写法窗口内
// 拿不到完整内容。新实现走「同目录 tmp → FlushFileBuffers → MoveFileEx 替换」。
namespace {

// 绕开 C API 的共享返回缓冲，用原生 Win32 读，才能在多线程里做并发读判据。
bool read_raw(const std::wstring &path, std::string &out) {
    out.clear();
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    char buf[4096];
    DWORD n = 0;
    while (::ReadFile(f, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    ::CloseHandle(f);
    return true;
}

} // namespace

TEST_F(FileApiTest, WriteLeavesNoTempFile) {
    const std::wstring file = path(L"plain.txt");
    ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), L"abc"), 1);
    ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), L"defg"), 1);
    EXPECT_EQ(OpFileExists(handle, (file + L".optmp").c_str()), 0) << "临时件必须被清掉";
    const std::wstring list = OpGetFileList(handle, root.c_str(), L"*");
    EXPECT_EQ(list.find(L".optmp"), std::wstring::npos) << list;
}

TEST_F(FileApiTest, ReadOnlyTargetFailsWithoutCorruptingOriginal) {
    const std::wstring file = path(L"ro.txt");
    ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), L"keep"), 1);
    ASSERT_TRUE(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    EXPECT_EQ(OpWriteStringToFile(handle, file.c_str(), L"lost"), 0) << "只读目标必须失败";
    ASSERT_TRUE(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
    EXPECT_EQ(std::wstring(OpReadFileString(handle, file.c_str())), L"keep") << "失败不能毁掉原文件";
    EXPECT_EQ(OpFileExists(handle, (file + L".optmp").c_str()), 0);
}

TEST_F(FileApiTest, ConcurrentReadNeverSeesTornOrMissingFile) {
    const std::wstring file = path(L"atomic.txt");
    const std::wstring a(200000, L'A');
    const std::wstring b(200000, L'B');
    ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), a.c_str()), 1);
    const size_t expect = 3 + 200000; // BOM + 全 ASCII 单字节

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> torn{0};
    std::atomic<int> unreadable{0};
    std::thread reader([&] {
        std::string raw;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!read_raw(file, raw)) {
                ++unreadable;
                continue;
            }
            ++reads;
            if (raw.size() != expect)
                ++torn;
            // 读者留 1ms 间隔：零间隔紧循环是饱和攻击（实测替换失败率 32%），既不是真实
            // 场景（编辑器/日志查看器都有间隔），也会让"替换失败"淹没"读到半截"这个真判据。
            ::Sleep(1);
        }
    });
    for (int i = 0; i < 40; ++i) {
        const std::wstring &src = (i % 2 == 0) ? a : b;
        ASSERT_EQ(OpWriteStringToFile(handle, file.c_str(), src.c_str()), 1);
    }
    stop.store(true);
    reader.join();

    EXPECT_GT(reads.load(), 0) << "读线程一次都没读到，用例无判别力";
    EXPECT_EQ(torn.load(), 0) << "读到半截文件 " << torn.load() << " 次";
    EXPECT_EQ(unreadable.load(), 0) << "写入期间目标文件不可读 " << unreadable.load() << " 次"
                                    << "（旧实现独占写会导致此值 > 0）";
    const std::wstring final_text = OpReadFileString(handle, file.c_str());
    EXPECT_EQ(final_text.size(), b.size()) << "尾部长度不符";
    EXPECT_TRUE(final_text == b) << "最后一次写的是 b（i=39 为奇数）";
}

} // namespace
