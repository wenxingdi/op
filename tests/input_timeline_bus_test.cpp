// 时间轴层（批 E 2026-11-22）：输入事件总线**跨模块唯一性**的判据。
//
// ## 这条判据为什么必须存在
//
// 总线最初写成函数内静态变量（`static InputEventBus bus;`）——那是"每个模块一份"。
// 测试进程同时载入 op_com.dll + op_c_api_x64.dll，而 op_test.exe 又直接编进了
// WinMouse.cpp / InputEventBus.cpp，于是进程里出现了三份单例：
//   * 录制钩子在 **exe 那份**总线里跑；
//   * `OpInputPlayback` 走 op_c_api.dll 操作 **另一份**；
//   * 表现：录制看得见事件，**STOP 永远看不见**（2s 间隔照走完）。
//   发布环境里是同一个错：COM 入口与 C API 入口各录各的。
//
// ## 判据
//
// `OpInputBusIdentity` 直接返回总线内部状态的指针。三个位置各取一次：
//   1. exe 自己直接编进来的那份（`op::input::InputEventBus::instance()`）；
//   2. 走 op_c_api_x64.dll 的导出；
//   两者必须指向同一块内存。
//
// 用指针比较而不是"从 COM 读录制路径"：后者依赖 tlb/DISPID 注册，一旦 COM 侧
// 未注册就整条判据 SKIP，等于没测。指针比较在任何时候都成立，且直接命中根因。

#include "test_support.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <op_c_api.h>

// exe 侧那份（本文件所在目标直接编进了 InputEventBus.cpp）
#include "../libop/input/InputEventBus.h"

TEST(InputTimelineBusTest, BusIsSingleInstanceAcrossModules) {
    // 1) 本模块（op_test.exe）看到的总线
    const void *local = op::input::InputEventBus::instance().identity();
    ASSERT_NE(nullptr, local);

    // 2) op_c_api_x64.dll 看到的总线
    op_handle handle = OpCreate();
    ASSERT_NE(nullptr, handle);
    const void *via_capi = OpInputBusIdentity(handle);
    OpDestroy(handle);

    ASSERT_NE(nullptr, via_capi) << "C API 侧取不到总线标识";
    EXPECT_EQ(local, via_capi)
        << "两个模块拿到的总线必须是同一个实例。不同 ⇒ 进程里存在多份 InputEventBus 单例，"
           "表现为录制/回放/监听各看各的（本次实测就是 STOP 看不见）。"
        << "\n  exe      侧: " << local << "\n  op_c_api 侧: " << via_capi;
}
