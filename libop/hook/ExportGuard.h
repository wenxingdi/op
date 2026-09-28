#pragma once

#include "../base/Utils.h"
#include <exception>
#include <utility>

namespace op::hook {

// 注入侧导出的统一异常护栏。
//
// 适用范围：随 hook DLL 注入到**目标进程**（游戏）内、由宿主通过
// blackbone::MakeRemoteFunction 远程调用的那批 __stdcall 导出（见 HookExport.cpp）。
//
// 为什么必须有：C++ 异常穿出 __stdcall/extern "C" 边界不会变成错误码，而是 std::terminate
// —— 直接崩掉目标进程。用户侧看到的是"点一下绑定，游戏就没了"。
// 宿主侧的 C API（libop/c_api/op_c_api.cpp）早就有 call_int / call_string 等同类护栏，
// 注入侧此前是零护栏；H14 的普查把这块补上。
//
// 用法：
//     long __stdcall Foo(HWND hwnd) {
//         return guarded<long>("Foo", 0, [&]() -> long { ... });
//     }
//
// 注意：被包装的函数体里若会调用 FreeLibraryAndExitThread（它不返回），
// 放进来是安全的（线程终止不会走 catch），但绝不能在持有互斥体时调用，见 HookExport.cpp。
template <typename Ret, typename Func>
Ret guarded(const char *name, Ret fallback, Func &&func) {
    try {
        return std::forward<Func>(func)();
    } catch (const std::exception &e) {
        setlog("hook export %s threw: %s", name, e.what());
        return fallback;
    } catch (...) {
        setlog("hook export %s threw unknown exception", name);
        return fallback;
    }
}

} // namespace op::hook
