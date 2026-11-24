#include "test_support.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

// DPI 跨进程端到端用例的子进程模式（定义在 dpi_context_test.cpp）。
// 必须拆进程：SetThreadDpiAwarenessContext 只能降级、不能把 unaware 线程提升为
// aware，而「aware 宿主 + unaware 目标窗口」这个组合要求宿主与目标处于不同进程
// （窗口的 awareness 由创建它的线程决定），同进程造不出有判别力的条件。
namespace op_dpi_e2e {
int run_target(const char *info_path);
} // namespace op_dpi_e2e

namespace {

// 主测试进程是 **DPI unaware**（实测 thread_ctx=UNAWARE、IsProcessDPIAware=0）。
// 这里刻意**不**做进程级 DPI 感知提升：
//   · user32.SetProcessDpiAwarenessContext 在本机（Win11 24H2 / build 26100）对
//     PER_MONITOR_AWARE / PER_MONITOR_AWARE_V2 直接返回 E_INVALIDARG(87)；
//   · shcore.dll 已不再导出该函数；
//   · 进程级 PMv2 只能靠 manifest 声明。
// 线程级 SetThreadDpiAwarenessContext(PMv2) 仍可生效（Win10 1607+ 允许线程比进程更 aware），
// DPI 用例据此在各自作用域内把线程切到 PMv2，模拟 OPTool 这类 PerMonitorV2 宿主。

} // namespace

int main(int argc, char **argv) {
    // 子进程模式必须在任何 DPI API 之前分流：DPI 不感知目标进程绝不能碰
    // SetProcessDPIAware / SetProcessDpiAwarenessContext，否则窗口就不再是不感知的。
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--dpi-target=", 13) == 0) {
            return op_dpi_e2e::run_target(argv[i] + 13);
        }
    }

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
