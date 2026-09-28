#pragma once

// blackbone 依赖层：按导出名解析后建立远程函数对象。
// 纯解析逻辑在 HookExportName.h（零 blackbone，可单测），这里只做接线。

#include "HookExportName.h"
#include "../base/Environment.h"

#include "BlackBone/Process/Process.h"
#include "BlackBone/Process/RPC/RemoteFunction.hpp"

#include <string>

namespace op::hook {

/// 解析导出名后建立远程函数对象。
///
/// 为什么需要这层：blackbone 的 `MakeRemoteFunction` → `ProcessModules::GetExport`
/// 只做**精确名**匹配，而 32 位 dll 的导出名带 `__stdcall` 修饰（`_Name@N`）——
/// 直接传未修饰名在 32 位目标上一律解析失败，表现为
/// `remote function 'SetDisplayHook' not found in op_c_api_x86.dll`，
/// 进而导致所有依赖注入的显示模式在 32 位游戏上绑定失败（详见 HookExportName.h）。
///
/// `dll_name` 必须是**注入用的那个模块名**（`op_c_api_x86.dll` / `op_x64.dll` 等）：
/// 本函数按 `RuntimeEnvironment::getBasePath() + "\\" + dll_name` 定位本地文件读导出表。
/// **不要**拿它解析 `kernel32.dll` 之类的系统模块（不在该目录下，也无需解析）。
template <typename T>
blackbone::RemoteFunction<T> MakeHookRemoteFunction(blackbone::Process &proc,
                                                   const std::wstring &dll_name,
                                                   const char *plain_name) {
    const std::wstring dll_path = RuntimeEnvironment::getBasePath() + L"\\" + dll_name;
    const std::string resolved = ResolveRemoteExportName(dll_path, plain_name);
    // MakeRemoteFunction 内部立即完成名字解析并返回结果对象（不保留该指针），
    // 所以传 resolved.c_str() 这个局部缓冲区是安全的。
    return blackbone::MakeRemoteFunction<T>(proc, dll_name, resolved.c_str());
}

}  // namespace op::hook
