// #include "stdafx.h"
#include "DllInjector.h"

#include "base/WindowsHandle.h"

namespace op {

namespace {

class remote_process_memory {
  public:
    remote_process_memory(HANDLE process, void *address, SIZE_T size) noexcept
        : process_(process), address_(address), size_(size) {
    }

    ~remote_process_memory() {
        reset();
    }

    remote_process_memory(const remote_process_memory &) = delete;
    remote_process_memory &operator=(const remote_process_memory &) = delete;

    void *get() const noexcept {
        return address_;
    }

    void reset() noexcept {
        if (process_ && address_) {
            // MEM_RELEASE 才真正归还远程地址空间；MEM_DECOMMIT 只释放物理页，
            // 虚拟地址空间仍被占用，重复注入会累积不可用地址（MEM_RELEASE 时
            // dwSize 必须为 0）。
            ::VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
            address_ = nullptr;
        }
    }

  private:
    HANDLE process_ = nullptr;
    void *address_ = nullptr;
    SIZE_T size_ = 0;
};

} // namespace

DllInjector::DllInjector() {
}

DllInjector::~DllInjector() {
}

BOOL DllInjector::EnablePrivilege(BOOL enable) {
    // 得到令牌句柄
    HANDLE token_handle = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY | TOKEN_READ, &token_handle))
        return FALSE;
    op::win32::unique_handle token(token_handle);

    // 得到特权值
    LUID luid;
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid))
        return FALSE;

    // 提升令牌句柄权限
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(tp), NULL, NULL))
        return FALSE;

    return TRUE;
}

long DllInjector::InjectDll(DWORD pid, LPCTSTR dllPath, long &error_code) {

    op::win32::unique_handle process(::OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
    /**pid = processInfo.dwProcessId;
     *process = processInfo.hProcess;*/
    if (!process) {
        error_code = ::GetLastError();
        return -1;
    }
    DWORD dllPathSize = ((DWORD)wcslen(dllPath) + 1) * sizeof(TCHAR);

    // 申请内存用来存放DLL路径
    remote_process_memory remoteMemory(process.get(),
                                       VirtualAllocEx(process.get(), NULL, dllPathSize, MEM_COMMIT,
                                                      PAGE_EXECUTE_READWRITE),
                                       dllPathSize);
    if (remoteMemory.get() == NULL) {
        // setlog(L"申请内存失败，错误代码：%u\n", GetLastError());
        error_code = ::GetLastError();
        return -2;
    }

    // 写入DLL路径
    if (!WriteProcessMemory(process.get(), remoteMemory.get(), dllPath, dllPathSize, NULL)) {
        // setlog(L"写入内存失败，错误代码：%u\n", GetLastError());
        error_code = ::GetLastError();
        return -3;
    }

    // 创建远线程调用LoadLibrary
    auto lpfn = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!lpfn) {
        error_code = ::GetLastError();
        return -4;
    }
    op::win32::unique_handle remoteThread(
        CreateRemoteThread(process.get(), NULL, 0, (LPTHREAD_START_ROUTINE)lpfn, remoteMemory.get(), 0, NULL));
    if (!remoteThread) {
        // setlog(L"创建远线程失败，错误代码：%u\n", GetLastError());
        error_code = ::GetLastError();
        return -5;
    }
    // 等待远线程结束（带 10s 超时：目标进程挂起时不再无限阻塞调用线程。
    // 超时后远程线程可能仍在目标进程执行，RAII 会释放路径内存——
    // 目标进程若已卡死 10s 本就异常，属可接受取舍）
    DWORD waitResult = ::WaitForSingleObject(remoteThread.get(), 10000);
    if (waitResult != WAIT_OBJECT_0) {
        error_code = (waitResult == WAIT_TIMEOUT) ? ERROR_TIMEOUT : ::GetLastError();
        return -6;
    }
    // 取DLL在目标进程的句柄（LoadLibraryW 失败时远线程返回 0）
    DWORD remoteModule = 0;
    if (!::GetExitCodeThread(remoteThread.get(), &remoteModule) || remoteModule == 0) {
        error_code = ::GetLastError() ? ::GetLastError() : ERROR_PROC_NOT_FOUND;
        return -7;
    }

    // 恢复线程
    // ResumeThread(processInfo.hThread);
    error_code = 0;
    return 1;
}

} // namespace op
