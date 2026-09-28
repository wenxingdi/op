// inject_dll 失败上报测试（本进程自测，不依赖游戏/外部进程）
//
// 背景（2026-09-28 蜀门全量测阶段 3 真机暴露）：`OpInjectDll` 直接把
// `DllInjector::InjectDll` 的返回值透传出去，而后者用 **1=成功、-1~-7=各类失败**；
// 对外（C API / COM / Python / 大漠）的约定却是「0=失败、非 0=成功」，
// `_ok()` 的 `if value:` 于是把 **-1 判成成功** ⇒ inject_dll 永远无法上报失败
// （实测：不存在的进程名 / 不存在的 dll 都返回 True）。
//
// 本文件把这个约定钉住：**返回值必须是 0/1 二值，负值不得外泄**。
#include <gtest/gtest.h>

#include <op_c_api.h>

#include <Windows.h>

#include <string>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// op 的 FindWindowByProcess 按进程名找窗口 —— 测试进程没有顶层窗口时 hwnd=0、pid=0，
// 正向用例会退化成"找不到窗口"而拿不到真实结果。故先建一个可见窗口。
struct VisibleWindow {
    HWND hwnd = nullptr;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool Create() {
        const wchar_t *cls = L"op_test_inject_cls";
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        if (::GetClassInfoW(wc.hInstance, cls, &wc) == 0) {
            wc.lpfnWndProc = WndProc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = cls;
            if (::RegisterClassW(&wc) == 0)
                return false;
        }
        hwnd = ::CreateWindowExW(0, cls, L"op_test_inject", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 240, 160,
                                 nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr)
            return false;
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);
        return true;
    }

    ~VisibleWindow() {
        if (hwnd)
            ::DestroyWindow(hwnd);
    }
};

// 本进程 exe 名（如 op_test.exe）—— 进程名匹配口径由 op 内部决定，别硬编码。
std::wstring SelfProcessName() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"op_test.exe";
    const std::wstring full(path);
    const auto pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? full : full.substr(pos + 1);
}

// 一个确定已加载的模块路径（kernel32 在本进程中必然已加载）——
// 注入它只会让 LoadLibraryW 返回既有句柄（引用计数 +1），不会引入新代码。
std::wstring LoadedModulePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(::GetModuleHandleW(L"kernel32.dll"), path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"";
    return std::wstring(path);
}

} // namespace

// 核心不变式：任何输入下都不得把内部负错误码泄漏给调用方。
// 反向验证：修复前"不存在的进程名"返回 -1 → 本条必然 FAIL（且报出具体值）。
TEST(InjectDllTest, NeverLeaksNegativeErrorCode) {
    CApiHandle op;
    ASSERT_NE(op.handle, nullptr);

    VisibleWindow win;
    ASSERT_TRUE(win.Create()) << "建测试窗口失败，无法定位本进程";

    const std::wstring self = SelfProcessName();
    const std::wstring dll = LoadedModulePath();
    ASSERT_FALSE(dll.empty());

    const int cases[] = {
        OpInjectDll(op.handle, self.c_str(), dll.c_str()),                                  // 正常
        OpInjectDll(op.handle, L"no_such_process_zzz.exe", dll.c_str()),                    // 进程不存在
        OpInjectDll(op.handle, self.c_str(), L"C:\\no_such_dir\\no_such_dll_zzz.dll"),      // dll 不存在
    };
    const char *names[] = {"正常", "进程不存在", "dll 不存在"};

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(cases[i] == 0 || cases[i] == 1)
            << "inject_dll(" << names[i] << ") 返回 " << cases[i]
            << " —— 对外约定是 0/1 二值，负值一律视为泄漏内部错误码";
    }
}

// 反向：进程不存在 → 必须报失败（修复前返回 -1，被判成"成功"）
TEST(InjectDllTest, UnknownProcessReportsFailure) {
    CApiHandle op;
    ASSERT_NE(op.handle, nullptr);

    const std::wstring dll = LoadedModulePath();
    ASSERT_FALSE(dll.empty());

    const int ret = OpInjectDll(op.handle, L"no_such_process_zzz.exe", dll.c_str());
    EXPECT_EQ(ret, 0) << "进程不存在却返回 " << ret << "（期望 0=失败）";
}

// 反向：进程存在但 dll 不存在 → 必须报失败
TEST(InjectDllTest, MissingDllReportsFailure) {
    CApiHandle op;
    ASSERT_NE(op.handle, nullptr);

    VisibleWindow win;
    ASSERT_TRUE(win.Create()) << "建测试窗口失败，无法定位本进程";
    const std::wstring self = SelfProcessName();

    const int ret = OpInjectDll(op.handle, self.c_str(), L"C:\\no_such_dir\\no_such_dll_zzz.dll");
    EXPECT_EQ(ret, 0) << "dll 不存在却返回 " << ret << "（期望 0=失败）";
}

// 正向：往本进程注入一个确定已加载的模块 → 必须成功（端到端成功用例）
TEST(InjectDllTest, InjectLoadedModuleIntoSelfSucceeds) {
    CApiHandle op;
    ASSERT_NE(op.handle, nullptr);

    VisibleWindow win;
    ASSERT_TRUE(win.Create()) << "建测试窗口失败，无法定位本进程";
    const std::wstring self = SelfProcessName();
    const std::wstring dll = LoadedModulePath();
    ASSERT_FALSE(dll.empty());

    // 前置：本进程能否被 OpenProcess(PROCESS_ALL_ACCESS)。非管理员/受限环境会失败，
    // 那是环境限制而非产品缺陷 —— 直接 SKIP，避免污染回归基线。
    HANDLE probe = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, ::GetCurrentProcessId());
    if (probe == nullptr) {
        GTEST_SKIP() << "本进程无法以 PROCESS_ALL_ACCESS 打开（GetLastError=" << ::GetLastError()
                     << "），跳过正向注入用例";
        return;
    }
    ::CloseHandle(probe);

    const int ret = OpInjectDll(op.handle, self.c_str(), dll.c_str());
    EXPECT_EQ(ret, 1) << "往本进程注入已加载模块失败（返回值 " << ret << "）；详见 cwd/__op.log";
}
