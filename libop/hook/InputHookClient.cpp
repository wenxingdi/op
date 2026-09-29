#include "InputHookClient.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"
#include "../base/Environment.h"
#include "HookModule.h"
#include "HookRemoteCall.h"
#include "BlackBone/Process/Process.h"
#include "BlackBone/Process/RPC/RemoteFunction.hpp"
#include <mutex>
#include <unordered_map>

namespace {

std::mutex g_mutex;

// 引用计数 + 绑定时的目标进程 id。
// 必须缓存 pid：宿主常见「目标窗口先销毁、随后才解绑」的收尾顺序（对象析构、脚本退出），
// 此时 HWND 已失效、GetWindowThreadProcessId 取不到 pid，导致远端 Hook 永远留在目标进程里，
// 而目标进程仍存活时后续重新绑定会被 HookExport 的 "is_hooked && input_hwnd != 新hwnd" 挡掉。
struct HookBindRef {
    long refs = 0;
    DWORD pid = 0;
};
std::unordered_map<HWND, HookBindRef> g_bind_refs;

std::wstring resolve_hook_dll(blackbone::Process &proc) {
    // GetMainModule() 在目标 PEB 读取失败（受保护进程 / 模块枚举受限）时返回空指针。
    // 原先直接解引用 -> 宿主访问违例崩溃。回退按宿主自身位数猜（同位数目标概率最高）。
    // 注意返回类型是 BlackBone 的 ModuleDataPtr（智能指针），判空用 !main_module。
    auto main_module = proc.modules().GetMainModule();
    if (!main_module) {
        setlog(L"resolve_hook_dll: GetMainModule null, fallback to host bitness");
        return op::hook::ResolveHookModuleName(sizeof(void *) == 8);
    }
    return op::hook::ResolveHookModuleName(main_module->type == blackbone::eModType::mt_mod64);
}

// out_pid：把内部已取到的目标 pid 透出给调用方缓存。宿主常见「窗口先销毁、随后才解绑」的
// 收尾顺序，若解绑时再取一次 pid 会得到 0，导致远端 Hook 永久残留。
// 返回 1 表示远端已装好；0 表示失败。
long call_set_input_hook(HWND hwnd, int mode, DWORD *out_pid) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return 0;

    long ret = 0;
    // BlackBone RPC 可能抛异常（目标进程状态异常时），与 call_ping_hook 对齐全部兜住。
    try {
        blackbone::Process proc;
        const NTSTATUS status = proc.Attach(pid);
        if (!NT_SUCCESS(status)) {
            setlog(L"input hook attach failed. pid=%d hwnd=%p status=0x%X", pid, hwnd, status);
            return 0;
        }

        const std::wstring dll_name = resolve_hook_dll(proc);
        bool injected = proc.modules().GetModule(dll_name) != nullptr;
        if (!injected) {
            const std::wstring dll_path = RuntimeEnvironment::getBasePath() + L"\\" + dll_name;
            if (::PathFileExistsW(dll_path.c_str())) {
                auto inject_ret = proc.modules().Inject(dll_path);
                injected = inject_ret ? true : false;
                if (!injected) {
                    setlog(L"input hook inject failed. pid=%d hwnd=%p status=0x%X dll=%s", pid, hwnd,
                           inject_ret.status, dll_path.c_str());
                }
            } else {
                setlog(L"input hook dll not exists: %s", dll_path.c_str());
            }
        }

        if (injected) {
            // 句柄宽度按目标位数自动选择（P0②：见 HookRemoteCall.h 的 CallHookHwndIntFn 头注）。
            auto call_ret = op::hook::CallHookHwndIntFn(proc, dll_name, "SetInputHook",
                                                        reinterpret_cast<uintptr_t>(hwnd), mode);
            ret = call_ret.result();
            if (ret != 1) {
                setlog(L"SetInputHook remote call failed. hwnd=%p mode=%d status=0x%X ret=%d", hwnd, mode,
                       call_ret.status, (int)ret);
            }
        }

        proc.Detach();
    } catch (...) {
        setlog(L"input hook set RPC exception. pid=%d hwnd=%p", pid, hwnd);
        ret = 0;
    }

    if (ret == 1 && out_pid) {
        *out_pid = pid;
    }
    return ret;
}

// 返回值：1 = 远端已释放；0 = attach 失败或未装（进程多半已退出，可安全清理本地引用）；
// -1 = RPC 抛异常（进程可能存活但远端调用失败、注入 DLL 可能残留）-> 调用方应保留条目重试。
long call_release_input_hook(DWORD pid) {
    if (pid == 0)
        return 0;

    long ret = 0;
    try {
        blackbone::Process proc;
        const NTSTATUS status = proc.Attach(pid);
        if (!NT_SUCCESS(status)) {
            setlog(L"input hook release attach failed. pid=%d status=0x%X", pid, status);
            return 0;
        }

        const std::wstring dll_name = resolve_hook_dll(proc);
        using release_input_hook_t = long(__stdcall *)();
        auto remote = op::hook::MakeHookRemoteFunction<release_input_hook_t>(proc, dll_name, "ReleaseInputHook");
        if (remote) {
            auto call_ret = remote();
            ret = call_ret.result();
        } else {
            setlog(L"remote function 'ReleaseInputHook' not found in %s.", dll_name.c_str());
        }

        proc.Detach();
    } catch (...) {
        setlog(L"input hook release RPC exception. pid=%d", pid);
        return -1;
    }
    return ret;
}

long call_set_input_lock(HWND hwnd, int lock) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return 0;

    long ret = 0;
    try {
        blackbone::Process proc;
        const NTSTATUS status = proc.Attach(pid);
        if (!NT_SUCCESS(status)) {
            setlog(L"input hook lock attach failed. pid=%d hwnd=%p status=0x%X", pid, hwnd, status);
            return 0;
        }

        const std::wstring dll_name = resolve_hook_dll(proc);
        using set_input_lock_t = long(__stdcall *)(int);
        auto remote = op::hook::MakeHookRemoteFunction<set_input_lock_t>(proc, dll_name, "SetInputLock");
        if (remote) {
            auto call_ret = remote(lock);
            ret = call_ret.result();
        } else {
            setlog(L"remote function 'SetInputLock' not found in %s.", dll_name.c_str());
        }

        proc.Detach();
    } catch (...) {
        setlog(L"input hook lock RPC exception. pid=%d hwnd=%p", pid, hwnd);
        ret = 0;
    }
    return ret;
}

long call_set_input_attr(HWND hwnd, int attrs) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return 0;

    long ret = 0;
    try {
        blackbone::Process proc;
        const NTSTATUS status = proc.Attach(pid);
        if (!NT_SUCCESS(status)) {
            setlog(L"input hook attr attach failed. pid=%d hwnd=%p status=0x%X", pid, hwnd, status);
            return 0;
        }

        const std::wstring dll_name = resolve_hook_dll(proc);
        using set_input_attr_t = long(__stdcall *)(int);
        auto remote = op::hook::MakeHookRemoteFunction<set_input_attr_t>(proc, dll_name, "SetInputAttr");
        if (remote) {
            auto call_ret = remote(attrs);
            ret = call_ret.result();
        } else {
            setlog(L"remote function 'SetInputAttr' not found in %s.", dll_name.c_str());
        }

        proc.Detach();
    } catch (...) {
        setlog(L"input hook attr RPC exception. pid=%d hwnd=%p", pid, hwnd);
        ret = 0;
    }
    return ret;
}

// 轻量回环：只对已注入 Hook 的窗口调一次远端导出。BlackBone RPC 可能抛异常（目标进程
// 状态异常时），这里全部兜住并按"不应答"返回。
long call_ping_hook(HWND hwnd) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return 0;

    long ret = 0;
    try {
        blackbone::Process proc;
        const NTSTATUS status = proc.Attach(pid);
        if (!NT_SUCCESS(status))
            return 0;

        const std::wstring dll_name = resolve_hook_dll(proc);
        using ping_t = unsigned long(__stdcall *)();
        auto remote = op::hook::MakeHookRemoteFunction<ping_t>(proc, dll_name, "GetInputCursorShapeHashLow");
        if (remote) {
            remote(); // 只验证可调用性，返回值无意义
            ret = 1;
        }
        proc.Detach();
    } catch (...) {
        ret = 0;
    }
    return ret;
}

bool call_cursor_shape(HWND hwnd, unsigned long long &hash, unsigned long long &meta) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return false;

    try {
        blackbone::Process proc;
        const NTSTATUS status = proc.Attach(pid);
        if (!NT_SUCCESS(status)) {
            setlog(L"input hook cursor attach failed. pid=%d hwnd=%p status=0x%X", pid, hwnd, status);
            return false;
        }

        const std::wstring dll_name = resolve_hook_dll(proc);
        using cursor_part_t = unsigned long(__stdcall *)();
        auto hash_low = op::hook::MakeHookRemoteFunction<cursor_part_t>(proc, dll_name, "GetInputCursorShapeHashLow");
        auto hash_high = op::hook::MakeHookRemoteFunction<cursor_part_t>(proc, dll_name, "GetInputCursorShapeHashHigh");
        auto meta_low = op::hook::MakeHookRemoteFunction<cursor_part_t>(proc, dll_name, "GetInputCursorShapeMetaLow");
        auto meta_high = op::hook::MakeHookRemoteFunction<cursor_part_t>(proc, dll_name, "GetInputCursorShapeMetaHigh");
        if (!hash_low || !hash_high || !meta_low || !meta_high) {
            proc.Detach();
            return false;
        }

        hash = static_cast<unsigned long long>(hash_low().result()) |
               (static_cast<unsigned long long>(hash_high().result()) << 32);
        meta = static_cast<unsigned long long>(meta_low().result()) |
               (static_cast<unsigned long long>(meta_high().result()) << 32);
        proc.Detach();
    } catch (...) {
        setlog(L"input hook cursor RPC exception. pid=%d hwnd=%p", pid, hwnd);
        return false;
    }
    return true;
}

} // namespace

namespace op::hook::input_hook_client {

long Bind(HWND hwnd, int mode) {
    if (!::IsWindow(hwnd))
        return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto &entry = g_bind_refs[hwnd];
    // 鼠标和键盘 dx 会共用同一个目标进程 Hook，宿主侧只做一次注入。
    if (entry.refs > 0) {
        ++entry.refs;
        return 1;
    }

    // pid 由 call_set_input_hook 内部透出复用。原实现在绑定成功后**再取一次** pid：
    // 若窗口恰好在这中间销毁，取到的是 0，解绑时 call_release_input_hook(0) 直接返回，
    // 远端 Hook 就永久留在目标进程里（bb98e92 修的正是这类残留）。
    DWORD bound_pid = 0;
    const long ret = call_set_input_hook(hwnd, mode, &bound_pid);
    if (ret == 1) {
        entry.refs = 1;
        entry.pid = bound_pid;
    } else {
        g_bind_refs.erase(hwnd);
    }
    return ret;
}

long UnBind(HWND hwnd) {
    if (!hwnd)
        return 1;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_bind_refs.find(hwnd);
    if (it == g_bind_refs.end())
        return 1;

    if (--it->second.refs > 0)
        return 1;

    // 先请求远端释放再清本地引用：ret==1（远端已释放）或 ret==0（attach 失败，进程多半
    // 已退出、注入物随进程消亡）都可安全清理；ret==-1 表示 RPC 抛异常（进程可能存活但远端
    // 调用失败、注入 DLL 可能残留），保留条目供宿主重试 UnBind，避免状态不可恢复。
    // 一律用绑定阶段缓存的 pid 定位目标进程，不依赖 hwnd 是否仍然有效。
    const DWORD pid = it->second.pid;
    const long ret = call_release_input_hook(pid);
    if (ret == -1) {
        // H25: 上面已经把 refs 减到 0 才发的 RPC，这里必须**回滚**，
        // 否则条目留在 refs<=0（继续重试 UnBind 会一路减到负数，计数语义脏），
        // 更实际的危害是：期间若有 Bind，`entry.refs > 0` 判为假 -> 再注入一次，
        // 而远端旧 Hook 还挂在目标进程里。回滚后重试 UnBind 仍能正常走到释放分支。
        ++it->second.refs;
        setlog(L"input hook release RPC exception, rollback bind ref for retry. hwnd=%p pid=%d", hwnd, pid);
        return 0;
    }
    g_bind_refs.erase(it);
    return ret == 1 ? 1 : ret;
}

long LockInput(HWND hwnd, int lock) {
    if (lock < 0 || lock > 3)
        return 0;
    if (!hwnd)
        return lock == 0 ? 1 : 0;

    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_bind_refs.find(hwnd) == g_bind_refs.end())
        return lock == 0 ? 1 : 0;

    return call_set_input_lock(hwnd, lock);
}

long SetInputAttr(HWND hwnd, int attrs) {
    if (!hwnd)
        return 0;

    std::lock_guard<std::mutex> guard(g_mutex);
    // 只有已经注入过 Hook 的窗口才有远端通道开关可设。
    if (g_bind_refs.find(hwnd) == g_bind_refs.end())
        return 0;

    return call_set_input_attr(hwnd, attrs);
}

long PingHook(HWND hwnd) {
    if (!::IsWindow(hwnd))
        return 0;

    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_bind_refs.find(hwnd) == g_bind_refs.end())
        return 0;

    return call_ping_hook(hwnd);
}

bool GetCursorShape(HWND hwnd, unsigned long long &hash, unsigned long long &meta) {
    if (!hwnd)
        return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_bind_refs.find(hwnd) == g_bind_refs.end())
        return false;

    return call_cursor_shape(hwnd, hash, meta);
}

} // namespace op::hook::input_hook_client
