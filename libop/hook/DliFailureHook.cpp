// DliFailureHook.cpp —— 延迟加载失败兜底（2026-09-16，方案 A1）
//
// 【背景】
//   op_x64.dll / op_c_api_x64.dll 会被注入目标进程（DX 输入通道 hook）。
//   二者隐式依赖 onnxruntime.dll（内置 ONNX OCR 引擎）。静态导入时，
//   「目标进程能否解析该依赖」就成了注入成功的前置条件 —— 实测 BlueStacks 5
//   返回 0xC0000135(STATUS_DLL_NOT_FOUND)，DX 三通道 10/10 绑定失败。
//   为此已在 libop/CMakeLists.txt 对其加 /DELAYLOAD:onnxruntime.dll。
//
// 【遗留问题（本文件解决）】
//   /DELAYLOAD 只把「加载期失败」推迟成「运行期失败」：注入本身不再报错，
//   但目标进程执行 SetInputHook → InputHook::setup 时若触碰 ONNX 符号，
//   延迟加载助手会在【目标进程】按标准搜索序(exe 目录→cwd→系统→PATH)找
//   onnxruntime.dll —— 目标进程通常没有该文件（也不该要求它有）→ 加载失败
//   → 目标进程 0xC0000005 崩溃。
//   实测唯一变量对照：目标目录「有」onnxruntime.dll → bind=1 且进程存活；
//   「无」→ bind=0 且进程崩溃。根因闭环。
//
// 【本文件做法】
//   实现 MSVC 延迟加载失败钩子 __pfnDliFailureHook2：当 onnxruntime.dll 解析
//   失败时，改用【本模块所在目录】的绝对路径 + LOAD_WITH_ALTERED_SEARCH_PATH
//   重新加载（op 安装目录下 onnxruntime.dll 与插件 DLL 同目录）。
//   → 目标进程无需自带 onnxruntime.dll，dx 绑定不再导致目标进程崩溃。
//   → 非 onnxruntime 的加载失败一律不干预，保持默认行为。
//
// 【约束/注意】
//   1) __pfnDliFailureHook2 是"每模块一份"的全局：必须在【每个】带 /DELAYLOAD
//      的模块内定义。op_x64 与 op_c_api_x64 各自独立编译本文件
//      （见 CMakeLists 的 OP_COM_SOURCES / OP_C_API_SOURCES）。
//   2) delayimp.h 默认把该符号声明为 const（只读段），用户要覆写必须先定义
//      DELAYIMP_INSECURE_WRITABLE_HOOKS，否则类型修饰不匹配。
//   3) 钩子内不调用 setlog：它是崩溃路径上的最后一道兜底，自身不能再依赖
//      任何可能未就绪的运行时设施；仅用 OutputDebugStringW 留痕。

#define DELAYIMP_INSECURE_WRITABLE_HOOKS

#include <windows.h>
#include <delayimp.h>
#include <string.h>
#include <string>

namespace {

// 延迟加载表里登记的唯一延迟项（与 CMakeLists 的 /DELAYLOAD:onnxruntime.dll 对应）
constexpr const char *kDelayedDll = "onnxruntime.dll";

// 取当前模块（本 DLL）所在目录，结果末尾不带反斜杠
bool self_directory(wchar_t (&dir)[MAX_PATH]) {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&self_directory), &self)) {
        return false;
    }
    const DWORD len = ::GetModuleFileNameW(self, dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    wchar_t *slash = ::wcsrchr(dir, L'\\');
    if (slash == nullptr) {
        return false;
    }
    *slash = L'\0';
    return true;
}

FARPROC WINAPI on_dli_failure(unsigned notify, PDelayLoadInfo info) {
    // 只处理「DLL 加载失败」。取函数地址失败(dliFailGetProc)等不作干预。
    if (notify != dliFailLoadLib || info == nullptr || info->szDll == nullptr) {
        return nullptr;
    }
    if (::_stricmp(info->szDll, kDelayedDll) != 0) {
        return nullptr;
    }

    wchar_t dir[MAX_PATH] = {0};
    if (!self_directory(dir)) {
        return nullptr;
    }

    std::wstring path(dir);
    path += L"\\";
    path += L"onnxruntime.dll";

    // LOAD_WITH_ALTERED_SEARCH_PATH：onnxruntime 自身的依赖
    // （onnxruntime_providers_shared.dll、VC 运行库）也从其所在目录解析。
    HMODULE mod = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (mod == nullptr) {
        wchar_t msg[256] = {0};
        ::wsprintfW(msg, L"[op] delay-load fallback failed: %hs (err=%lu)\n", kDelayedDll,
                    ::GetLastError());
        ::OutputDebugStringW(msg);
    }
    // 返回有效 HMODULE 即视为加载成功，延迟加载助手随后用它解析导入符号。
    return reinterpret_cast<FARPROC>(mod);
}

}  // namespace

// MSVC 延迟加载助手在 onnxruntime.dll 解析失败时回调本钩子。
extern "C" PfnDliHook __pfnDliFailureHook2 = on_dli_failure;
