#pragma once

// blackbone 依赖层：按导出名解析后建立远程函数对象。
// 纯解析逻辑在 HookExportName.h（零 blackbone，可单测），这里只做接线。

#include "HookExportName.h"
#include "../base/Environment.h"

#include "BlackBone/Process/Process.h"
#include "BlackBone/Process/RPC/RemoteFunction.hpp"

#include <string>

namespace op::hook {

// ============================================================================
// 【2026-09-29 P0 修复】跨位数远程调用约定错误（cdecl stub 调 stdcall 函数）
//
// 症状：x64 宿主绑 32 位目标 dx 注入，SetDisplayHook 远程调用返回后目标进程
//   0xC0000005（蜀门 client.exe、32 位记事本均稳定复现；事件日志 + 目标进程侧
//   __op.log 实证：注入✓ 导出解析✓，崩在远程 stub 收尾）。
//
// 根因：blackbone 的 RemoteFunction<R(__stdcall*)(Args...)> 特化只在 USE32
//   （32 位 blackbone 构建）下编译（RemoteFunction.hpp: "Under AMD64 these will
//   be same declarations as __cdecl, so compilation will fail"）。x64 宿主上
//   __stdcall 被归一化，我们的函数指针类型落进 __cdecl 特化 →
//   RemoteFunctionBase<cc_cdecl,...> → 跨 WoW64 时 AsmHelper32 生成 cdecl stub
//   调 stdcall 导出 → 每参 4 字节栈漂移 → stub 后续代码踩坏栈 → 崩。
//   x64 目标不受影响（AsmHelper64::GenCall 忽略约定参数，x64 仅一种调用约定）。
//   同源症状：SetDllDirectoryW（1 参）跨 Wow64 "取结果抛 unknown exception" 的
//   既有噪音记录，即 4 字节漂移的可存活变体。
//   先例：x86_host_probe.c 探针 cdecl 调 stdcall 栈漂移（2026-09-29 同日）。
//
// 修法：绕过 RemoteFunction<T> 特化体系，统一直接实例化
//   RemoteFunctionBase<cc_stdcall, R, Args...>：
//   - 注入侧导出全部 stdcall（x86 带 @N 修饰 / x64 同语义）
//   - x64 目标：AsmHelper64 忽略 conv，cc_stdcall ≡ cc_cdecl，行为不变
//   - WoW64 目标：AsmHelper32 生成 stdcall stub（callee 清栈），正确对齐
//   - 32 位宿主构建：与 blackbone USE32 特化等效
// ============================================================================

/// 从函数指针类型剥离调用约定，映射到 RemoteFunctionBase<cc_stdcall, R, Args...>。
/// x64 宿主：__stdcall 被归一化，`R(__stdcall*)(Args...)` 即 `R(*)(Args...)`，
/// 该特化直接匹配；32 位宿主：__stdcall 是真实类型区分，也匹配此形态。
template <typename T>
struct HookFnSig;  // 仅支持函数指针类型

template <typename R, typename... Args>
struct HookFnSig<R(__stdcall *)(Args...)> {
    using type = blackbone::RemoteFunctionBase<blackbone::cc_stdcall, R, Args...>;
};

/// 按导出名建立 stdcall 远程函数对象（共通实现）。
/// `name` 必须是**目标模块内的精确导出名**（GetExport 精确匹配）。
template <typename T>
typename HookFnSig<T>::type MakeStdcallRemoteFunction(blackbone::Process &proc,
                                                      const std::wstring &dll_name,
                                                      const char *exact_name) {
    auto ptr = proc.modules().GetExport(dll_name, exact_name);
    return typename HookFnSig<T>::type(proc, ptr ? ptr->procAddress : 0, nullptr);
}

/// 解析导出名（消 32 位 @N 装饰）后建立远程函数对象。
///
/// 为什么需要这层：blackbone 的 `MakeRemoteFunction` → `ProcessModules::GetExport`
/// 只做**精确名**匹配，而 32 位 dll 的导出名带 `__stdcall` 修饰（`_Name@N`）——
/// 直接传未修饰名在 32 位目标上一律解析失败，表现为
/// `remote function 'SetDisplayHook' not found in op_c_api_x86.dll`，
/// 进而导致所有依赖注入的显示模式在 32 位游戏上绑定失败（详见 HookExportName.h）。
///
/// `dll_name` 必须是**注入用的那个模块名**（`op_c_api_x86.dll` / `op_x64.dll` 等）：
/// 本函数按 `RuntimeEnvironment::getBasePath() + "\\" + dll_name` 定位本地文件读导出表。
/// **不要**拿它解析 `kernel32.dll` 之类的系统模块（不在该目录下，也无需解析）——
/// 系统模块用 MakeSystemRemoteFunction。
template <typename T>
typename HookFnSig<T>::type MakeHookRemoteFunction(blackbone::Process &proc,
                                                   const std::wstring &dll_name,
                                                   const char *plain_name) {
    const std::wstring dll_path = RuntimeEnvironment::getBasePath() + L"\\" + dll_name;
    const std::string resolved = ResolveRemoteExportName(dll_path, plain_name);
    // 解析是纯本地查表（立即完成，不保留指针），传局部缓冲区安全。
    return MakeStdcallRemoteFunction<T>(proc, dll_name, resolved.c_str());
}

/// 系统模块（kernel32 等）专用：导出名本就无修饰、也不存在本地对照文件，
/// 直接按精确名解析。调用约定同样按 stdcall 生成 stub（API 即 stdcall）。
template <typename T>
typename HookFnSig<T>::type MakeSystemRemoteFunction(blackbone::Process &proc,
                                                     const std::wstring &dll_name,
                                                     const char *exact_name) {
    return MakeStdcallRemoteFunction<T>(proc, dll_name, exact_name);
}

// ============================================================================
// 【2026-09-29 P0②】跨位数**句柄传参宽度**
//
// 症状：x64 宿主绑 32 位目标走 dx 注入，SetDisplayHook(HWND, int) 远程调用
//   在目标进程内**函数体完整执行完**（注入侧日志实证）后，目标进程 0xC0000005
//   （蜀门 client.exe / 32 位记事本均稳定复现）。对照实验：同一宿主下
//     kernel32 GetCurrentProcessId(0 参)、MulDiv(3 个 int)、
//     注入 DLL 的 SetInputLock(int)（1 参）全部干净返回 —— **只有带 HWND 的调用崩**。
//
// 根因：AsmVariant 依**宿主** sizeof(T) 决定参数宽度。x64 宿主下 sizeof(HWND)=8，
//   PrepareCallAssembly 对「imm 且 size>sizeof(uint32_t)」的 x86 参数做
//   `imm → dataStruct` 转换（8 字节按值压栈）：
//     ① 后续参数整体串位 —— render_type 实际收到 struct 高 4 字节，恒为 0
//        （真机日志实证：修复前 render_type=0 且不匹配任何渲染分支；
//         修复后 render_type=131072，正确进入 locate D3D9）；
//     ② 栈布局与真实 32 位 ABI 不符 → 远程 stub 收尾踩坏栈 → 崩目标进程。
//   同源先例：`SetDllDirectoryW(nullptr)` 跨 Wow64「取结果抛异常」的既有噪音
//   （nullptr_t 在 x64 宿主上同样 size=8 → 误走 dataStruct）。
//
// 修法：32 位 ABI 中 HWND 就是 4 字节，故 32 位目标显式用 uint32_t 传句柄；
//   64 位目标沿用 HWND（AsmHelper64 忽略约定与宽度转换，行为不变）。
// ============================================================================

/// 调用「long(__stdcall)(HWND, int)」形态的注入侧导出，自动按目标位数选句柄宽度。
/// 语义与直接调用远程函数对象等价（返回值含 status / result），便于调用方记录诊断。
inline blackbone::call_result_t<long> CallHookHwndIntFn(blackbone::Process &proc,
                                                        const std::wstring &dll_name,
                                                        const char *plain_name,
                                                        uintptr_t hwnd, int arg2) {
    // STATUS_OBJECT_NAME_NOT_FOUND：解析不到导出时返回，调用方据 result()!=1 走失败分支。
    constexpr NTSTATUS kNotFound = static_cast<NTSTATUS>(0xC0000034u);

    if (proc.core().isWow64()) {
        using f32_t = long(__stdcall *)(uint32_t, int);
        auto p = MakeHookRemoteFunction<f32_t>(proc, dll_name, plain_name);
        if (!p)
            return blackbone::call_result_t<long>(0, kNotFound);
        return p(static_cast<uint32_t>(hwnd), arg2);
    }

    using f64_t = long(__stdcall *)(HWND, int);
    auto p = MakeHookRemoteFunction<f64_t>(proc, dll_name, plain_name);
    if (!p)
        return blackbone::call_result_t<long>(0, kNotFound);
    return p(reinterpret_cast<HWND>(hwnd), arg2);
}

}  // namespace op::hook
