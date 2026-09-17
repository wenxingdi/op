#include "test_support.h"

#include <Windows.h>

#include <cstdio>

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new test_support::OpEnvironment);
    const int rc = RUN_ALL_TESTS();
    // 测试进程若创建过可见且抢焦点的窗口，Windows 会把本机 IME（如搜狗
    // SogouPY.ime + TextInputFramework）载入进程；这类第三方 IME 在
    // ExitProcess 的 DLL 分离阶段可能永久卡死主线程——表现为全部用例已出
    // 结果、总结已打印，但 op_test.exe 进程不退出（tasklist 只剩 1 个线程
    // 停在 UserRequest 等待）。结果已落盘，此处主动自决退出，绕开第三方
    // IME 的 detach 挂起。生产宿主（脚本/工具）若自身无焦点窗口则不受影响。
    std::fflush(stdout);
    std::fflush(stderr);
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(rc));
    return rc;
}
