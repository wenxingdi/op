#pragma once
// 进程树终止（跨模块共享）。原实现住在 ipc/Pipe.cpp 的匿名命名空间，
// 2026-10-15 P1 新增 OpTerminateProcessTree 需要复用，提到此处成 inline 共享。
// 语义：先快照子进程列表，终止本进程并等待至多 1 秒，再递归终止各子树
// （先父后子，避免子进程被孤儿化后脱离快照）。
#include "../base/WindowsHandle.h"
#include <vector>
#include <windows.h>
#include <tlhelp32.h>

namespace op {
namespace proctree {

inline void terminate_process_tree(DWORD pid) {
    std::vector<DWORD> children;

    op::win32::unique_handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot) {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot.get(), &entry)) {
            do {
                if (entry.th32ParentProcessID == pid)
                    children.push_back(entry.th32ProcessID);
            } while (::Process32NextW(snapshot.get(), &entry));
        }
    }

    op::win32::unique_handle process(::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
    if (process) {
        ::TerminateProcess(process.get(), 0);
        ::WaitForSingleObject(process.get(), 1000);
    }

    for (DWORD child_pid : children)
        terminate_process_tree(child_pid);
}

} // namespace proctree
} // namespace op
