#include "OpContext.h"
#include "OpResult.h"

#include "ipc/CommandRunner.h"
#include "ipc/ProcessTree.h"
#include "../base/WindowsHandle.h"
#include "base/Utils.h"
#include "window/DllInjector.h"

#include <libop.h>

#include <string>
#include <vector>

namespace {

std::wstring decode_command_output(const std::string &text) {
    if (text.empty())
        return L"";

    const int utf8_len =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (utf8_len > 0) {
        std::wstring out(utf8_len, L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(),
                              utf8_len);
        return out;
    }

    return _s2wstring(text);
}

} // namespace

void op::Op::InjectDll(const wchar_t *process_name, const wchar_t *dll_name, long *ret) {
    LONG_PTR hwnd = 0;
    FindWindowByProcess(process_name, L"", L"", &hwnd);
    long pid = 0;
    GetWindowProcessId(hwnd, &pid);
    internal::set_result(ret, 0L);
    if (!DllInjector::EnablePrivilege(TRUE)) {
        setlog("InjectDll false: EnablePrivilege false erro_code=%08X", ::GetLastError());
        return;
    }
    long error_code = 0;
    const long injected = DllInjector::InjectDll(pid, dll_name, error_code);
    // DllInjector 用 1 表示成功、**-1~-7 表示各类失败**，而对外（C API / COM / Python / 大漠）的
    // 约定是「0=失败、非 0=成功」。直接透传会把 -1 判成成功 ⇒ inject_dll 永远无法上报失败，
    // 因此必须在这里归一化为 0/1；失败原因落 __op.log（set_show_error_msg(2) 可见）。
    if (injected <= 0) {
        setlog("InjectDll false: pid=%d dll=%s injector_ret=%d error_code=%08X", pid,
               _ws2string(dll_name ? dll_name : L"").c_str(), injected, static_cast<unsigned>(error_code));
    }
    internal::set_result(ret, injected > 0 ? 1L : 0L);
}

void op::Op::EnumProcess(const wchar_t *name, std::wstring &retstring) {
    m_context->window_service.EnumProcess(name, retstring);
}

void op::Op::GetProcessInfo(long pid, std::wstring &retstring) {
    m_context->window_service.GetProcessInfo(pid, retstring);
}

void op::Op::GetWindowProcessId(LONG_PTR hwnd, long *nretpid) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), &pid);
    internal::set_result(nretpid, pid);
}

void op::Op::GetWindowProcessPath(LONG_PTR hwnd, std::wstring &retstring) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), &pid);
    m_context->window_service.GetProcesspath(pid, retstring);
}

void op::Op::RunApp(const wchar_t *cmdline, long mode, unsigned long *pid, long *ret) {
    // 成功时返回新进程 pid，失败时返回 0。
    internal::set_result(ret, m_context->window_service.RunApp(cmdline, mode, pid));
}

void op::Op::WinExec(const wchar_t *cmdline, long cmdshow, long *ret) {
    auto str = _ws2string(cmdline);
    internal::set_result(ret, ::WinExec(str.c_str(), cmdshow) > 31 ? 1 : 0);
}

void op::Op::GetCmdStr(const wchar_t *cmd, long millseconds, std::wstring &retstr) {
    CommandRunner command_runner;
    auto str = command_runner.GetCmdStr(cmd ? std::wstring(cmd) : std::wstring(),
                                        millseconds <= 0 ? 5 : static_cast<DWORD>(millseconds));
    retstr = decode_command_output(str);
}

void op::Op::TerminateProcess(long pid, long *ret) {
    internal::set_result(ret, 0L);
    if (pid <= 0)
        return;
    const op::win32::unique_handle process(::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                                         static_cast<DWORD>(pid)));
    if (!process)
        return;
    if (::TerminateProcess(process.get(), 0) == FALSE)
        return;
    ::WaitForSingleObject(process.get(), 1000);
    internal::set_result(ret, 1L);
}

void op::Op::TerminateProcessTree(long pid, long *ret) {
    internal::set_result(ret, 0L);
    if (pid <= 0)
        return;
    // 复用 ipc/ProcessTree.h（原 Pipe.cpp 匿名命名空间实现，P1 提升为共享）。
    // 语义：先快照子进程列表，终止本进程并等待，再递归各子树。
    proctree::terminate_process_tree(static_cast<DWORD>(pid));
    // terminate_process_tree 无返回值：以进程已不可打开为准判定成功
    const op::win32::unique_handle check(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                                       static_cast<DWORD>(pid)));
    internal::set_result(ret, check ? 0L : 1L);
}
