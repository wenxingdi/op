#include "HookExport.h"
#include "../base/Environment.h"
#include "../base/Utils.h"
#include "DisplayHook.h"
#include "ExportGuard.h"
#include "InputHook.h"
#include <atomic>
#include <mutex>

using op::hook::DisplayHook;
using op::hook::guarded;
using op::hook::InputHook;

namespace {

// 这三组计数会被来自宿主不同线程的导出并发读写。裸 int 时代，"判断计数 + 装/拆 hook +
// 计数增减" 之间没有任何同步：并发绑定/解绑可能让模块在仍被使用时被 FreeLibraryAndExitThread
// 卸载，或反之永不卸载（泄漏）。这里用互斥体把整个临界区串行化，计数用 atomic 保证可见性。
std::mutex g_exportMutex;
std::atomic<int> g_ref_count{0};
std::atomic<int> g_display_ref_count{0};
std::atomic<int> g_input_ref_count{0};

// 注入侧导出的异常护栏在本文件所有导出上统一生效，实现见 ExportGuard.h。
// 这些函数**不在宿主进程里跑**：hook DLL 被 blackbone 注入到目标进程（游戏）后，宿主通过
// blackbone::MakeRemoteFunction 远程调用它们；异常穿出 __stdcall 边界 = 目标进程 std::terminate。

// H16: FreeLibraryAndExitThread 会**终止调用线程**，所以本函数只能运行在被注入进程内、
// 由注入线程经 MakeRemoteFunction 远程调用。一旦被宿主本进程直接调用，卸载的就是宿主自己的
// 模块、宿主线程当场消失 —— 宿主里谁调的谁就没了。
// 除注释外再加一道护栏：绝不卸载进程主模块（正常情况这里拿到的是注入的 hook DLL）。
void release_module_if_idle() {
    if (g_ref_count.load() != 0)
        return;

    HMODULE self = static_cast<HMODULE>(RuntimeEnvironment::getInstance());
    if (!self)
        return;
    if (self == ::GetModuleHandleW(nullptr)) {
        setlog("release_module_if_idle: refusing to unload the main module of the current process");
        return;
    }

    ::FreeLibraryAndExitThread(self, 0);
}

} // namespace

long __stdcall SetDisplayHook(HWND hwnd_, int render_type_) {
    return guarded<long>("SetDisplayHook", 0, [&]() -> long {
        int ret = 0;
        RuntimeEnvironment::m_showErrorMsg = 2;
        std::lock_guard<std::mutex> lock(g_exportMutex);
        if (!DisplayHook::is_hooked) {
            ret = DisplayHook::setup(hwnd_, render_type_);
            DisplayHook::is_hooked = ret == 1;
            if (DisplayHook::is_hooked) {
                g_ref_count++;
                g_display_ref_count = 1;
            }
        } else {
            if (DisplayHook::render_hwnd == hwnd_ && DisplayHook::render_type == render_type_) {
                g_display_ref_count++;
                ret = 1;
            } else {
                if (g_display_ref_count > 0) {
                    setlog("DisplayHook already bound hwnd=%p render_type=%d, reject hwnd=%p render_type=%d",
                           DisplayHook::render_hwnd, DisplayHook::render_type, hwnd_, render_type_);
                    return 0;
                }
                DisplayHook::release();
                if (g_ref_count > 0)
                    g_ref_count--;
                ret = DisplayHook::setup(hwnd_, render_type_);
                DisplayHook::is_hooked = ret == 1;
                if (DisplayHook::is_hooked) {
                    g_ref_count++;
                    g_display_ref_count = 1;
                }
            }
        }
        return ret;
    });
}

long __stdcall ReleaseDisplayHook() {
    return guarded<long>("ReleaseDisplayHook", 0, [&]() -> long {
        int ret = 0;
        {
            std::lock_guard<std::mutex> lock(g_exportMutex);
            if (DisplayHook::is_hooked && g_display_ref_count > 0 && --g_display_ref_count == 0) {
                DisplayHook::is_hooked = false;
                ret = DisplayHook::release();
                if (g_ref_count > 0)
                    g_ref_count--;
            }
        }

        // 必须留在锁外：FreeLibraryAndExitThread 不返回，持锁调用会让锁永不解开。
        release_module_if_idle();

        return ret;
    });
}

long __stdcall SetInputHook(HWND hwnd_, int) {
    return guarded<long>("SetInputHook", 0, [&]() -> long {
        int ret = 0;
        std::lock_guard<std::mutex> lock(g_exportMutex);
        if (!InputHook::is_hooked) {
            ret = InputHook::setup(hwnd_);
            InputHook::is_hooked = ret == 1;
            if (InputHook::is_hooked) {
                g_ref_count++;
                g_input_ref_count = 1;
            }
        } else if (InputHook::input_hwnd == hwnd_) {
            // 鼠标和键盘 dx 共用一个远端 Hook，等双方都释放后再卸载。
            g_input_ref_count++;
            ret = 1;
        } else {
            // 自愈重绑：is_hooked 为真但 hwnd 不匹配——旧 Hook 是残留状态（宿主曾异常
            // 退出、未经 UnBind；或目标窗口已重建而旧窗口句柄仍被记着）。直接拒绝会让
            // 该进程内 dx 永久不可用，先释放旧 Hook 再按新窗口重装。
            InputHook::release();
            InputHook::is_hooked = false;
            if (g_ref_count > 0)
                g_ref_count--;
            ret = InputHook::setup(hwnd_);
            InputHook::is_hooked = ret == 1;
            if (InputHook::is_hooked) {
                g_ref_count++;
                g_input_ref_count = 1;
            }
        }
        return ret;
    });
}

long __stdcall ReleaseInputHook() {
    return guarded<long>("ReleaseInputHook", 0, [&]() -> long {
        bool became_idle = false;
        {
            std::lock_guard<std::mutex> lock(g_exportMutex);
            if (InputHook::is_hooked && g_input_ref_count > 0 && --g_input_ref_count == 0) {
                InputHook::release();
                InputHook::is_hooked = false;
                if (g_ref_count > 0)
                    g_ref_count--;
                became_idle = true;
            }
        }

        // 同 ReleaseDisplayHook：卸载调用必须留在临界区之外。
        if (became_idle)
            release_module_if_idle();

        return 1;
    });
}

long __stdcall SetInputLock(int lock) {
    return guarded<long>("SetInputLock", lock == 0 ? 1 : 0, [&]() -> long {
        if (!InputHook::is_hooked)
            return lock == 0 ? 1 : 0;
        return InputHook::lockInput(lock);
    });
}

long __stdcall SetInputAttr(int attrs) {
    return guarded<long>("SetInputAttr", 0, [&]() -> long {
        // 未 Hook 时设置没有意义：setup 会把通道重置成默认全开。
        if (!InputHook::is_hooked)
            return 0;
        return InputHook::setInputAttr(attrs);
    });
}

unsigned long long __stdcall GetInputCursorShapeHash() {
    return guarded<unsigned long long>("GetInputCursorShapeHash", 0ull,
                                       [&]() -> unsigned long long { return InputHook::cursorShapeHash(); });
}

unsigned long long __stdcall GetInputCursorShapeMeta() {
    return guarded<unsigned long long>("GetInputCursorShapeMeta", 0ull,
                                       [&]() -> unsigned long long { return InputHook::cursorShapeMeta(); });
}

unsigned long __stdcall GetInputCursorShapeHashLow() {
    return guarded<unsigned long>("GetInputCursorShapeHashLow", 0ul, [&]() -> unsigned long {
        return static_cast<unsigned long>(InputHook::cursorShapeHash() & 0xFFFFFFFFull);
    });
}

unsigned long __stdcall GetInputCursorShapeHashHigh() {
    return guarded<unsigned long>("GetInputCursorShapeHashHigh", 0ul, [&]() -> unsigned long {
        return static_cast<unsigned long>((InputHook::cursorShapeHash() >> 32) & 0xFFFFFFFFull);
    });
}

unsigned long __stdcall GetInputCursorShapeMetaLow() {
    return guarded<unsigned long>("GetInputCursorShapeMetaLow", 0ul, [&]() -> unsigned long {
        return static_cast<unsigned long>(InputHook::cursorShapeMeta() & 0xFFFFFFFFull);
    });
}

unsigned long __stdcall GetInputCursorShapeMetaHigh() {
    return guarded<unsigned long>("GetInputCursorShapeMetaHigh", 0ul, [&]() -> unsigned long {
        return static_cast<unsigned long>((InputHook::cursorShapeMeta() >> 32) & 0xFFFFFFFFull);
    });
}
