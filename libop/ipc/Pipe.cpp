// #include "stdafx.h"
#include "Pipe.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"
#include "../base/WindowsHandle.h"
#include <chrono>
#include <iostream>
#include <tlhelp32.h>
#include <vector>

namespace op {

namespace {

void terminate_process_tree(DWORD pid) {
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

} // namespace

Pipe::Pipe() {
    _hread = _hwrite = _hread2 = _hwrite2 = nullptr;
    _hprocess = nullptr;
    _reading = false;
    _pthread = nullptr;
    ZeroMemory(&_pi, sizeof(_pi));
    ZeroMemory(&_si, sizeof(_si));
    ZeroMemory(&_ai, sizeof(_ai));
}

Pipe::~Pipe() {
    close();
}

int Pipe::open(const std::wstring &cmd) {
    close();

    ZeroMemory(&_pi, sizeof(_pi));
    ZeroMemory(&_si, sizeof(_si));
    ZeroMemory(&_ai, sizeof(_ai));

    _ai.nLength = sizeof(SECURITY_ATTRIBUTES);
    _ai.bInheritHandle = true;
    _ai.lpSecurityDescriptor = nullptr;
    if (!CreatePipe(&_hread, &_hwrite, &_ai, 0))
        return -1;
    if (!SetHandleInformation(_hread, HANDLE_FLAG_INHERIT, 0)) {
        close(0);
        return -2;
    }
    if (!CreatePipe(&_hread2, &_hwrite2, &_ai, 0)) {
        close(0);
        return -3;
    }
    if (!SetHandleInformation(_hwrite2, HANDLE_FLAG_INHERIT, 0)) {
        close(0);
        return -4;
    }

    _si.cb = sizeof(_si);
    _si.hStdError = _hwrite;
    _si.hStdOutput = _hwrite;
    _si.hStdInput = _hread2;
    _si.wShowWindow = SW_HIDE;
    _si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, true, CREATE_NO_WINDOW, nullptr, nullptr, &_si, &_pi)) {
        close(0);
        return -5;
    }

    SAFE_CLOSE(_hwrite);
    SAFE_CLOSE(_hread2);

    _reading = true;
    _pthread = new std::thread(&Pipe::reader, this);
    return 1;
}

int Pipe::close(DWORD process_wait_ms) {
    SAFE_CLOSE(_hwrite2);
    SAFE_CLOSE(_hread2);

    bool process_exited = true;
    if (_pi.hProcess) {
        const DWORD wait_result = ::WaitForSingleObject(_pi.hProcess, process_wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            process_exited = false;
            terminate_process_tree(_pi.dwProcessId);
        }
    }

    // The reader thread exits on its own once the child closes its stdout
    // (ReadFile returns ERROR_BROKEN_PIPE), which is guaranteed after we close
    // our write end and (on timeout) terminate the process tree. Just stop the
    // loop and join; no need to forcibly cancel I/O.
    _reading = false;

    if (_pthread) {
        if (_pthread->joinable()) {
            // 兜底：子进程退出后若仍有孙进程持有管道写端句柄（CreateProcess
            // bInheritHandles=true 会向子进程派生的后代传播），reader 的阻塞
            // ReadFile 永远收不到 BROKEN_PIPE，join 将无限挂起。显式取消该线程
            // 的同步 I/O 使 ReadFile 以 ERROR_OPERATION_ABORTED 返回并结束循环。
            // 线程未处于阻塞 IO 时本调用返回 FALSE（ERROR_NOT_FOUND），无害。
            ::CancelSynchronousIo(_pthread->native_handle());
            _pthread->join();
        }
        SAFE_DELETE(_pthread);
    }
    SAFE_CLOSE(_hread);
    SAFE_CLOSE(_hwrite);
    SAFE_CLOSE(_hwrite2);
    SAFE_CLOSE(_hread2);
    SAFE_CLOSE(_pi.hProcess);
    SAFE_CLOSE(_pi.hThread);
    return 0;
}

void Pipe::on_read(const string &info) {
    std::cout << info << std::endl;
}

int Pipe::on_write(const string &info) {
    if (_reading.load() && _hwrite2) {
        unsigned long wlen = 0;

        return WriteFile(_hwrite2, info.data(), static_cast<DWORD>(info.length() * sizeof(char)), &wlen, nullptr);
    }
    return 0;
}

void Pipe::reader() {
    // Blocking reads: when the child closes its stdout the next ReadFile returns
    // FALSE with ERROR_BROKEN_PIPE, which ends the loop naturally. This removes
    // the previous PeekNamedPipe + Sleep(1) busy-poll and the need to forcibly
    // cancel the I/O from close().
    const static int buf_size = 1 << 12;
    std::vector<char> buf(buf_size);
    unsigned long read_len = 0;
    while (_reading.load()) {
        if (!::ReadFile(_hread, buf.data(), static_cast<DWORD>(buf.size()), &read_len, nullptr)) {
            _reading = false;
            break;
        }
        if (read_len == 0)
            break;
        on_read(string(buf.data(), read_len));
    }
}

bool Pipe::is_open() {
    if (_pi.hProcess) {
        DWORD code = 0;
        ::GetExitCodeProcess(_pi.hProcess, &code);
        return code == STILL_ACTIVE;
    }
    return false;
}

bool Pipe::wait_for_exit(DWORD timeout_ms) {
    if (!_pi.hProcess)
        return false;
    return ::WaitForSingleObject(_pi.hProcess, timeout_ms) == WAIT_OBJECT_0;
}

} // namespace op
