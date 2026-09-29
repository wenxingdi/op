// #include "stdafx.h"
#include "ApiResolver.h"
#include "../base/Types.h"
#include "../base/Utils.h" // setlog

void *ResolveApi(const char *mod_name, const char *func_name) {
    auto hdll = ::GetModuleHandleA(mod_name);
    if (!hdll) {
        //_error_code = -1;
        return NULL;
    }
    void *paddress = (void *)::GetProcAddress(hdll, func_name);
    if (!paddress) {
        //_error_code = -2;
        return NULL;
    }
    //_error_code = 0;
    return paddress;
}

namespace {

// LOAD_LIBRARY_SEARCH_SYSTEM32：只从 System32 找（32 位进程下由 WOW64 重定向到 SysWOW64），
// 不查 cwd / PATH / 目标进程目录。显式写数值而非引 winbase.h 宏，避免 SDK 版本差异
// （Win7 无此宏定义，但系统在装了 KB2533623 后支持该值）。
constexpr DWORD kLoadLibrarySearchSystem32 = 0x00000800;

// 环境变量只读一次：注入侧的绑定调用是低频操作，但会反复经过这里（display + input 两侧，
// 每个 render_type 都要查模块），没必要每次都进内核。三态用 atomic 表达
// "未初始化 / 关闭 / 开启"，避免多线程同时初始化时的重复读取。
std::atomic<int> g_on_demand{-1};

bool ReadOnDemandEnv() {
    wchar_t buf[8] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"OP_NO_ONDEMAND_LOAD", buf, 8);
    if (n == 0 || n >= 8)
        return true; // 未设置（或异常长）都按默认"开启"
    // 只认明确的真值，避免 "=0" 被当成关闭后又有人写 "=false" 产生歧义。
    const std::wstring v(buf);
    return !(v == L"1" || v == L"true" || v == L"TRUE" || v == L"yes" || v == L"on");
}

// 模块名恒为 ASCII（d3d9.dll / dinput8.dll …），手工扩宽即可，不必引 MultiByteToWideChar。
std::wstring WidenAscii(const char *s) {
    std::wstring w;
    if (!s)
        return w;
    for (; *s; ++s)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

bool FileExistsIn(const std::wstring &dir_with_slash, const std::wstring &name) {
    return ::GetFileAttributesW((dir_with_slash + name).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 系统目录（带尾反斜杠）。32 位进程里被 WOW64 重定向到 SysWOW64 —— 那正是对应该位数的
// dinput8/d3d9，也正是 LoadLibraryEx(LOAD_LIBRARY_SEARCH_SYSTEM32) 会取的那一份。
std::wstring SystemDirectoryWithSlash() {
    wchar_t buf[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
        dir.push_back(L'\\');
    return dir;
}

// 目标进程 exe 目录 / 当前工作目录下若有同名 DLL，说明它可能用的是**私有**图形/输入库
// （d3d9 wrapper、ReShade/ENB、模拟器自带 opengl32 等）。此时绝不能抢先加载 System32 版本：
// 模块名一旦被我们占住，目标随后的 LoadLibrary("d3d9.dll") 会直接复用我们这份，
// 等于静默替换掉它的图形栈 —— 后果比"绑不上"严重得多。
//
// 例外（**踩过的坑**）：目标 exe 本身落在系统目录里时（notepad.exe 在 SysWOW64、
// 各种 System32 自带程序），"exe 目录"与"系统目录"是同一个 —— 那里出现 dinput8.dll
// 完全正常，不是私有库。若不排除，所有系统目录里的目标都会被误判为"被遮蔽"而拒绝加载，
// 按需加载等于对它们永久失效（notepad 实测：bind=0，且日志无任何加载记录）。
bool ModuleShadowedByTarget(const std::wstring &mod_name_w) {
    const std::wstring sys_dir = SystemDirectoryWithSlash();

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
        std::wstring dir(exe_path);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash + 1);
            if (_wcsicmp(dir.c_str(), sys_dir.c_str()) != 0 && FileExistsIn(dir, mod_name_w))
                return true;
        }
    }
    wchar_t cwd[MAX_PATH] = {};
    if (::GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
        std::wstring dir(cwd);
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
            dir.push_back(L'\\');
        if (_wcsicmp(dir.c_str(), sys_dir.c_str()) != 0 && FileExistsIn(dir, mod_name_w))
            return true;
    }
    return false;
}

} // namespace

bool OnDemandModuleLoadEnabled() {
    int cached = g_on_demand.load(std::memory_order_relaxed);
    if (cached < 0) {
        cached = ReadOnDemandEnv() ? 1 : 0;
        g_on_demand.store(cached, std::memory_order_relaxed);
    }
    return cached == 1;
}

void *LoadApiModule(const char *mod_name) {
    if (!mod_name || !*mod_name)
        return nullptr;

    // 已加载就直接用（绝大多数情况）。注意：**不要** FreeLibrary 我们加载进来的模块——
    // 目标进程随后自己 LoadLibrary 时会复用同一模块并自增引用计数，vtable/函数地址完全一致，
    // hook 依然有效；而我们若提前释放，可能把它的引用计数打穿导致模块被卸载。
    if (HMODULE loaded = ::GetModuleHandleA(mod_name))
        return loaded;

    if (!OnDemandModuleLoadEnabled())
        return nullptr;

    const std::wstring mod_w = WidenAscii(mod_name);
    if (ModuleShadowedByTarget(mod_w)) {
        setlog("api resolver: skip on-demand load %s (target ships a same-named DLL)", mod_name);
        return nullptr;
    }

    HMODULE h = ::LoadLibraryExA(mod_name, nullptr, kLoadLibrarySearchSystem32);
    if (h) {
        setlog("api resolver: loaded %s on demand (system32)", mod_name);
        return h;
    }

    // 老系统（无 KB2533623）不支持 SEARCH_SYSTEM32，退回标准搜索顺序。此路径存在同名
    // DLL 劫持的理论风险，但仅在"系统目录里确实没有该模块"时才会走到，记日志以便追溯。
    const DWORD first_error = ::GetLastError();
    h = ::LoadLibraryA(mod_name);
    if (h) {
        setlog("api resolver: loaded %s on demand (fallback search, err=%lu)", mod_name, first_error);
        return h;
    }
    setlog("api resolver: on-demand load %s failed err=%lu", mod_name, ::GetLastError());
    return nullptr;
}

void *ResolveApiLazy(const char *mod_name, const char *func_name) {
    if (void *p = ResolveApi(mod_name, func_name))
        return p;
    if (!LoadApiModule(mod_name))
        return nullptr;
    return ResolveApi(mod_name, func_name);
}
