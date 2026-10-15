// 文件模块功能测试（P1 新增 2026-10-15）：Op*File* 全家在临时目录上的真实读写流程。
// 注意 c_api_null_handle_test.cpp 是生成物，勿在此补空句柄用例。

#include "op_c_api.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <string>

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

} // namespace
