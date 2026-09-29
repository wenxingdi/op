// RunApp(mode=1) 工作目录推导测试
//
// 背景（2026-09-28 蜀门真机全量测阶段 3 暴露的真缺陷）：
// 旧实现 `pos = cmd.find(".exe")` 后**向前**找 `\\` / `/`，找不到时**不重置 pos**，
// 于是 `curr_dir = cmd.substr(0, pos)` 把 `"notepad.exe"` 截成 **`"notepad"`** 当
// `lpCurrentDirectory` 传给 `CreateProcessW` → ERROR_DIRECTORY(267) → pid=0。
// 即 **mode=1 + 裸文件名必失败**，而 mode=0 / 全路径 + mode=1 都正常。
//
// 目录推导已抽成 header-only 纯函数 `libop/window/RunAppPath.h`（零 Windows 依赖），
// 本文件先用边界用例钉住它，再用一条端到端（真起进程）证明修复对宿主可见。
#include <gtest/gtest.h>

#include <op_c_api.h>

#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "../libop/window/RunAppPath.h"

namespace {

using op::runapp::ExtractAppDirectory;

// ---------------------------------------------------------------- 边界用例（纯函数）

// 核心不变式：**裸文件名不得编造目录**（旧实现在这里产出 "notepad" → ERROR_DIRECTORY）。
TEST(RunAppPathTest, BareFileNameYieldsNoDirectory) {
    EXPECT_TRUE(ExtractAppDirectory(L"notepad.exe").empty());
    EXPECT_TRUE(ExtractAppDirectory(L"NOTEPAD.EXE").empty());
    EXPECT_TRUE(ExtractAppDirectory(L"cmd.exe /c echo hi").empty());
}

TEST(RunAppPathTest, FullPathYieldsDirectory) {
    EXPECT_EQ(ExtractAppDirectory(L"C:\\Windows\\System32\\notepad.exe"), L"C:\\Windows\\System32");
    EXPECT_EQ(ExtractAppDirectory(L"D:\\games\\client.exe"), L"D:\\games");
    // 正斜杠也要认（CreateProcessW 接受，用户也会这么写）
    EXPECT_EQ(ExtractAppDirectory(L"D:/games/client.exe"), L"D:/games");
}

TEST(RunAppPathTest, DriveRootGetsSeparatorAppended) {
    // 旧实现会得到 "C:"（= C 盘**当前目录**，不是根目录）；这里显式补成 "C:\"
    EXPECT_EQ(ExtractAppDirectory(L"C:\\a.exe"), L"C:\\");
    EXPECT_EQ(ExtractAppDirectory(L"c:\\A.EXE"), L"c:\\");
}

TEST(RunAppPathTest, LeadingSeparatorYieldsRoot) {
    // 旧实现循环条件 `i >= 1` 把索引 0 的分隔符漏掉 ⇒ 落到"未找到"分支
    EXPECT_EQ(ExtractAppDirectory(L"\\notepad.exe"), L"\\");
    EXPECT_EQ(ExtractAppDirectory(L"/usr/bin/app.exe"), L"/usr/bin");
}

TEST(RunAppPathTest, QuotedPathWithSpaces) {
    // 带引号 + 空格是 cmd 最常见的形态；目录不得把前导引号吃进去
    EXPECT_EQ(ExtractAppDirectory(L"\"C:\\Program Files\\App\\app.exe\" -x"), L"C:\\Program Files\\App");
    EXPECT_EQ(ExtractAppDirectory(L"  \"C:\\a b\\c.exe\""), L"C:\\a b");
}

TEST(RunAppPathTest, UnquotedPathWithSpacesAndParentheses) {
    // 无引号带空格路径（蜀门真机 2026-09-29 复现形态）：
    // `D:\Program Files (x86)\shumen\game.exe` → 目录必须含空格与括号，不得截断
    EXPECT_EQ(ExtractAppDirectory(L"D:\\Program Files (x86)\\shumen\\game.exe"),
              L"D:\\Program Files (x86)\\shumen");
}

TEST(RunAppPathTest, DegenerateInputsYieldNoDirectory) {
    EXPECT_TRUE(ExtractAppDirectory(L"").empty());
    EXPECT_TRUE(ExtractAppDirectory(L"   ").empty());
    EXPECT_TRUE(ExtractAppDirectory(L"just-a-name").empty());
}

TEST(RunAppPathTest, FallsBackToFirstWhitespaceWhenNoExeExtension) {
    // 非 .exe（如 .bat）：退化为"第一个空白之前"当路径，再取目录
    EXPECT_EQ(ExtractAppDirectory(L"D:\\tools\\run.bat"), L"D:\\tools");
    EXPECT_EQ(ExtractAppDirectory(L"D:\\tools\\run.bat arg1"), L"D:\\tools");
}

// ---------------------------------------------------------------- 端到端（真起进程）

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

bool process_alive(std::uint32_t pid) {
    if (pid == 0)
        return false;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    DWORD code = 0;
    const bool running = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return running;
}

void kill_notepad() {
    ::system("taskkill /IM notepad.exe /F >nul 2>&1");
}

} // namespace

// 修复可见性：mode=1 + **裸文件名** 必须能起进程（旧实现恒 pid=0）。
TEST(RunAppPathTest, EndToEnd_BareFileNameWithMode1StartsProcess) {
    kill_notepad();
    ::Sleep(300);

    CApiHandle api;
    ASSERT_NE(api.handle, nullptr);

    std::uint32_t pid = 0;
    const int ret = OpRunApp(api.handle, L"notepad.exe", 1, &pid);
    EXPECT_NE(ret, 0) << "run_app('notepad.exe', mode=1) 应成功（旧实现恒失败，报错 ERROR_DIRECTORY）";
    EXPECT_GT(pid, 0u);
    EXPECT_TRUE(process_alive(pid)) << "返回的 pid 应指向一个存活进程";

    kill_notepad();
    ::Sleep(300);

    // 对照：mode=0 同样可用（本就正常，防回归）
    std::uint32_t pid0 = 0;
    const int ret0 = OpRunApp(api.handle, L"notepad.exe", 0, &pid0);
    EXPECT_NE(ret0, 0);
    EXPECT_GT(pid0, 0u);
    kill_notepad();
}

// 全路径 + mode=1 本就正常，钉住防回归
TEST(RunAppPathTest, EndToEnd_FullPathWithMode1StartsProcess) {
    kill_notepad();
    ::Sleep(300);

    CApiHandle api;
    ASSERT_NE(api.handle, nullptr);

    wchar_t windir[MAX_PATH] = {0};
    ASSERT_GT(::GetSystemDirectoryW(windir, MAX_PATH), 0u);
    const std::wstring exe = std::wstring(windir) + L"\\notepad.exe";

    std::uint32_t pid = 0;
    const int ret = OpRunApp(api.handle, exe.c_str(), 1, &pid);
    EXPECT_NE(ret, 0);
    EXPECT_GT(pid, 0u);
    kill_notepad();
}

} // namespace
