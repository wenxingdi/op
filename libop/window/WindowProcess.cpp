#include "WindowService.h"

#include "../base/Utils.h"
#include "RunAppPath.h"
#include "base/WindowsHandle.h"

#include <Tlhelp32.h>
#include <cwchar>
#include <shellapi.h>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <psapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace op {

namespace {

template <typename Target, typename Value> void set_out(Target *target, Value value) {
    if (target)
        *target = static_cast<Target>(value);
}

void append_process_id(std::wstring &result, DWORD pid) {
    if (!result.empty())
        result.push_back(L',');
    result += std::to_wstring(pid);
}

std::wstring get_process_module_path(HANDLE process) {
    HMODULE module = nullptr;
    DWORD bytes_needed = 0;
    if (!::EnumProcessModules(process, &module, sizeof(module), &bytes_needed))
        return L"";

    std::vector<wchar_t> buffer(1024, L'\0');
    for (;;) {
        const DWORD copied = ::GetModuleFileNameExW(process, module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0)
            return L"";
        if (copied < buffer.size() - 1)
            return std::wstring(buffer.data(), copied);
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

std::wstring find_process_name(DWORD pid) {
    PROCESSENTRY32 pe32 = {sizeof(PROCESSENTRY32)};
    op::win32::unique_handle process_snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!process_snapshot)
        return L"";

    if (::Process32First(process_snapshot.get(), &pe32)) {
        do {
            if (pe32.th32ProcessID == pid)
                return pe32.szExeFile;
        } while (::Process32Next(process_snapshot.get(), &pe32));
    }

    return L"";
}

} // namespace

BOOL WindowService::EnumProcessbyName(DWORD dwPID, LPCWSTR ExeName, LONG type) {
    if (enum_process_success_count == 0) {
        npid.clear();
        PROCESSENTRY32 pe32 = {sizeof(PROCESSENTRY32)};
        op::win32::unique_handle process_snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!process_snapshot)
            return FALSE;
        if (::Process32First(process_snapshot.get(), &pe32)) {
            do {
                if (type == 1) {
                    // 模糊匹配子串。当前全库调用方均用默认 type=0（精确匹配），
                    // 此分支暂无内部调用方，保留以兼容潜在的外部/历史用法。
                    if (wcsstr(pe32.szExeFile, ExeName) != NULL) // 模糊匹配
                    {
                        npid.push_back(pe32.th32ProcessID);
                    }
                } else {
                    if (!_wcsicmp(pe32.szExeFile, ExeName)) {
                        npid.push_back(pe32.th32ProcessID);
                    }
                }

            } while (::Process32Next(process_snapshot.get(), &pe32));
        }
        enum_process_success_count = static_cast<int>(npid.size());
        if (enum_process_success_count > 0)
            return TRUE;
    } else {
        for (const DWORD pid : npid) {
            if (dwPID == pid)
                return TRUE;
        }
    }

    return FALSE;
}

bool WindowService::EnumProcess(const wchar_t *name, std::wstring &retstring) {
    retstring.clear();
    retstringlen = 0;
    if (!name || wcslen(name) < 1)
        return false;

    enum_process_success_count = 0;
    npid.clear();
    if (!EnumProcessbyName(0, name)) {
        setlog(L"EnumProcess: no process matched name=%s", name);
        return false;
    }

    for (const DWORD pid : npid)
        append_process_id(retstring, pid);

    return true;
}

int WindowService::GetProcessNumber() // 获取CPU个数
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
}

// 时间格式转换
__int64 WindowService::FileTimeToInt64(const FILETIME &time) {
    ULARGE_INTEGER tt;
    tt.LowPart = time.dwLowDateTime;
    tt.HighPart = time.dwHighDateTime;
    return (tt.QuadPart);
}

double WindowService::get_cpu_usage(DWORD ProcessID) // 获取指定进程CPU使用率
{
    // cpu数量
    static int processor_count_ = -1;
    // 每个进程独立保存上一次采样，避免全局 static 在并发/多进程调用时互相覆盖算错
    static std::map<DWORD, std::pair<__int64, __int64>> g_last; // pid -> (system_time, time)
    static std::mutex g_mutex;

    FILETIME now;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    __int64 system_time;
    __int64 time;
    // 	__int64 system_time_delta;
    // 	__int64 time_delta;

    double cpu = -1;

    if (processor_count_ == -1) {
        processor_count_ = GetProcessNumber();
    }

    GetSystemTimeAsFileTime(&now);

    // HANDLE hProcess =
    // OpenProcess(PROCESS_QUERY_INFORMATION/*PROCESS_ALL_ACCESS*/, false,
    // ProcessID);
    op::win32::unique_handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ProcessID));

    if (!process) {
        return -1;
    }
    if (!GetProcessTimes(process.get(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        return -1;
    }
    system_time = (FileTimeToInt64(kernel_time) + FileTimeToInt64(user_time)) / processor_count_; // CPU使用时间
    time = FileTimeToInt64(now);                                                                  // 现在的时间

    // 读取该进程上一次采样，并写入本次，供下次差算
    std::pair<__int64, __int64> prev{0, 0};
    std::pair<__int64, __int64> first{system_time, time}; // 本次首采
    bool has_prev = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_last.find(ProcessID);
        if (it != g_last.end()) {
            prev = it->second;
            has_prev = true;
        }
    }

    Sleep(1000); // 保留原采样间隔

    // hProcess = OpenProcess(PROCESS_QUERY_INFORMATION/*PROCESS_ALL_ACCESS*/,
    // false, ProcessID);

    process.reset(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ProcessID));

    if (!process) {
        return -1;
    }
    if (!GetProcessTimes(process.get(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        return -1;
    }
    GetSystemTimeAsFileTime(&now);
    system_time = (FileTimeToInt64(kernel_time) + FileTimeToInt64(user_time)) / processor_count_; // CPU使用时间
    time = FileTimeToInt64(now);                                                                  // 现在的时间

    // 用第二次采样作为下次基准（差分窗口回到 ~1s；原实现存第一次采样导致连续调用窗口 ~2s）
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_last[ProcessID] = {system_time, time};
    }

    if (has_prev) {
        // 用本进程上一次采样做差（滑动窗口，更准确）
        cpu = ((double)(system_time - prev.first) / (double)(time - prev.second)) * 100;
    } else {
        // 首次：使用本次调用内的两次采样做差（等价于原实现）
        cpu = ((double)(system_time - first.first) / (double)(time - first.second)) * 100;
    }
    return cpu;
}

// 或者指定进程内存使用率
DWORD WindowService::GetMemoryInfo(DWORD ProcessID) {
    PROCESS_MEMORY_COUNTERS pmc;
    DWORD memoryInK = 0;
    op::win32::unique_handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ProcessID));

    if (process && GetProcessMemoryInfo(process.get(), &pmc, sizeof(pmc))) {
        // memoryInK = pmc.WorkingSetSize/1024;		//单位为k
        memoryInK = static_cast<DWORD>(pmc.WorkingSetSize);
    }

    return memoryInK;
}

bool WindowService::GetProcessInfo(LONG pid, std::wstring &retstring) {
    retstring.clear();

    const std::wstring process_name = find_process_name(static_cast<DWORD>(pid));
    if (process_name.empty())
        return false;

    std::wstring process_path;
    GetProcesspath(static_cast<DWORD>(pid), process_path);
    // get_cpu_usage 失败返回 -1，原实现强转 DWORD 会得到 4294967295 出现在结果串里。
    const double cpu_value = get_cpu_usage(static_cast<DWORD>(pid));
    const auto cpu = cpu_value >= 0.0 ? static_cast<DWORD>(cpu_value) : 0;
    const auto meminfo = GetMemoryInfo(static_cast<DWORD>(pid));

    retstring = process_name + L"|" + process_path + L"|" + std::to_wstring(cpu) + L"|" + std::to_wstring(meminfo);
    return true;
}

bool WindowService::GetProcesspath(DWORD ProcessID, std::wstring &process_path) {
    process_path.clear();

    op::win32::unique_handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ProcessID));
    if (!process)
        return false;

    process_path = get_process_module_path(process.get());
    return !process_path.empty();
}

long WindowService::RunApp(const std::wstring &cmd, long mode, DWORD *pid) {
    set_out(pid, 0);

    // 2026-09-29 方案A+：两种启动方式并存，mode 兼顾「启动链」与「工作目录」：
    //   mode & 1 == 0 → 工作目录 = 继承调用方 cwd
    //   mode & 1 == 1 → 工作目录 = 可执行文件所在目录（ExtractAppDirectory 推导）
    //   mode & 2 == 0 → 启动链 = ShellExecuteExW（默认；对齐大漠 RunApp 行为，
    //                    exe/.lnk/文档/网址通吃）
    //   mode & 2 == 2 → 启动链 = CreateProcessW（保留原路径；对引导器有自校验的
    //                    程序可能被拒（如蜀门 game.exe 报 "ucfile/render.lua 没找到"，
    //                    2026-09-29 真机实测），但对控制台/裸进程场景更直接）
    // 背景：蜀门 game.exe 经 Shell 启动链完全正常（lnk 与 ShellExecuteW 直启 exe 均验证），
    // 大漠 RunApp 内部走 Shell 故 exe 路径可用；.lnk 非 PE 文件只有 Shell 能解析。
    const bool use_app_dir = (mode & 1) != 0;
    const bool use_create_process = (mode & 2) != 0;

    std::wstring curr_dir;
    if (use_app_dir) {
        curr_dir = op::runapp::ExtractAppDirectory(cmd);
        if (curr_dir.empty())
            setlog(L"RunApp: mode=%d but no directory in cmd=%s, inherit current directory",
                   static_cast<int>(mode), cmd.c_str());
    }

    // ---------------- CreateProcessW 路径（mode=2/3）：整串命令行，带空格无引号路径也能解析
    if (use_create_process) {
        auto cmdptr = std::make_unique<wchar_t[]>(cmd.length() + 1);
        memcpy(cmdptr.get(), cmd.data(), cmd.length() * sizeof(wchar_t));
        cmdptr.get()[cmd.length()] = 0; // C字符串需要末尾有0
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        ZeroMemory(&pi, sizeof(pi));
        const BOOL bret = ::CreateProcessW(nullptr, cmdptr.get(), NULL, NULL, FALSE, 0,
                                           nullptr,
                                           curr_dir.empty() ? nullptr : curr_dir.c_str(),
                                           &si, &pi);
        if (bret) {
            set_out(pid, pi.dwProcessId);
            op::win32::unique_handle process(pi.hProcess);
            op::win32::unique_handle thread(pi.hThread);
            return 1;
        }
        setlog(L"RunApp: CreateProcessW failed, cmd=%s, err=%lu", cmd.c_str(), ::GetLastError());
        return 0;
    }

    // ---------------- ShellExecuteExW 路径（mode=0/1，默认）：对齐大漠
    // 拆出「程序路径」与「参数」（ShellExecuteExW 要求分字段）：
    // 带引号 → 引号内是程序路径、其后是参数（Windows 惯例：含空格的路径必须加引号）；
    // 无引号 → **整串当 lpFile、不拆参数**。不能按第一个空白拆——会把
    // "D:\Program Files (x86)\game.exe" 这类无引号含空格路径拆成 "D:\Program"（2026-09-29
    // 蜀门真机复现：ShellExecuteExW 找不到 "D:\Program" → 整体失败）。Shell 对带空格的
    // lpFile 自带容错解析。
    std::wstring file;
    std::wstring params;
    {
        const size_t n = cmd.size();
        size_t start = 0;
        while (start < n && (cmd[start] == L' ' || cmd[start] == L'\t'))
            ++start;
        if (start < n && cmd[start] == L'"') {
            const size_t close = cmd.find(L'"', start + 1);
            if (close != std::wstring::npos) {
                file = cmd.substr(start + 1, close - start - 1);
                params = cmd.substr(close + 1);
                size_t p = 0;
                while (p < params.size() && (params[p] == L' ' || params[p] == L'\t'))
                    ++p;
                params.erase(0, p);
            }
        }
        if (file.empty())
            file = cmd.substr(start);
    }

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = curr_dir.empty() ? nullptr : curr_dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (::ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            set_out(pid, ::GetProcessId(sei.hProcess));
            ::CloseHandle(sei.hProcess);
        }
        // Shell 对某些目标（如 .bat/协议/已运行的单实例程序）不提供进程句柄：pid=0 但视为成功
        return 1;
    }
    setlog(L"RunApp: ShellExecuteExW failed, cmd=%s, err=%lu", cmd.c_str(), ::GetLastError());
    return 0;
}

} // namespace op
