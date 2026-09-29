#include "DisplayHook.h"

#include "D3D10Capture.h"
#include "D3D11Capture.h"
#include "D3D12Capture.h"
#include "D3D9Capture.h"
#include "DetourGuard.h"
#include "MinHook.h"
#include "MinHookRuntime.h"
#include "OpenGLCapture.h"
#include "kiero.hpp"
#include "kiero_d3d9.hpp"
#include "kiero_d3d10.hpp"
#include "kiero_d3d11.hpp"
#include "kiero_d3d12.hpp"
#include "kiero_opengl.hpp"
#include "../hook/ApiResolver.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"
#include <atomic>
#include <string>
#include <vector>

namespace op::hook {

HWND DisplayHook::render_hwnd = NULL;
int DisplayHook::render_type = 0;
std::wstring DisplayHook::shared_res_name;
std::wstring DisplayHook::mutex_name;
std::atomic<void *> DisplayHook::old_address{nullptr};
void *DisplayHook::hook_target;
bool DisplayHook::is_hooked = false;

// is_capture 的**内存序是有正确性意义的，不可降级为 relaxed**。
//
// setup() 的发布顺序是"先写绑定态（render_hwnd / shared_res_name / mutex_name，都是普通
// 变量与 std::wstring），最后 set_capture_enabled(true)"；而 detour 侧是"先读
// capture_enabled()，再读这些绑定态"。默认 seq_cst 在这个组合里恰好构成 release/acquire
// 配对：任何看到 true 的渲染线程都一定能看到先前写入的名字与窗口句柄。
// 若谁把它"优化"成 relaxed，detour 就可能读到半写的 std::wstring（并发读写 = UB，
// 典型表现是无规律的崩溃或花屏），且这种问题极难复现。
static std::atomic<int> is_capture{0};

namespace {

constexpr int kPresentIndex = 8;
constexpr int kD3D9EndSceneIndex = 42;

// H2: MH_RemoveHook 会释放 trampoline，拆钩前等在途 detour 退出的上限。
// 正常一帧的 capture+Present 在毫秒级，500ms 已是极宽裕；超时只留证不阻塞拆钩。
constexpr int kDetourDrainTimeoutMs = 500;

template <typename T> void *method_at(const std::vector<T> &methods, size_t index) {
    return index < methods.size() ? reinterpret_cast<void *>(methods[index]) : nullptr;
}

// 按需加载：kiero 的 locate<Implementation_*> 全程只用 GetModuleHandleA，目标进程此刻
// 尚未加载该图形库时会直接返回 Error_ModuleNotFound —— 与"目标根本不用这条 API"混为一谈。
// 这里按 render_type 先补齐模块（只从 System32 找，见 ApiResolver 注释），把时机问题消掉。
// 加载失败不致命：kiero 随后仍会如实报 Error_ModuleNotFound 并让 setup 返回 0。
void ensure_render_modules(int render_type) {
    switch (render_type) {
    case RDT_DX_DEFAULT:
    case RDT_DX_D3D9:
        ::LoadApiModule("d3d9.dll");
        break;
    case RDT_DX_D3D10:
        ::LoadApiModule("dxgi.dll");
        ::LoadApiModule("d3d10.dll");
        break;
    case RDT_DX_D3D11:
        ::LoadApiModule("dxgi.dll");
        ::LoadApiModule("d3d11.dll");
        break;
    case RDT_DX_D3D12:
        ::LoadApiModule("dxgi.dll");
        ::LoadApiModule("d3d12.dll");
        break;
    case RDT_GL_DEFAULT:
    case RDT_GL_STD:
    case RDT_GL_NOX:
    case RDT_GL_FI:
        ::LoadApiModule("opengl32.dll");
        break;
    // RDT_GL_ES（libEGL.dll）**故意不按需加载**：libEGL 是模拟器/运行时自带的私有库，
    // 不是系统组件，System32 里没有正品可加载；一旦走 fallback 搜索路径，可能把我们自己
    // 找来的 libEGL 抢先塞进目标进程，反而顶替掉它本该用的那份。保持"只用已加载的"。
    default:
        break; // normal / gdi 系列走 GDI 抓取，不依赖被 hook 的图形模块
    }
}

int locate_render_method(int render_type, void **target, void **detour) {
    if (!target || !detour)
        return 0;

    *target = nullptr;
    *detour = nullptr;

    ensure_render_modules(render_type);

    if (render_type == RDT_DX_DEFAULT || render_type == RDT_DX_D3D9) {
        kiero::D3D9Output output;
        const kiero::Error error = kiero::locate<kiero::Implementation_D3D9>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate D3D9 failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = method_at(output.device_methods, kD3D9EndSceneIndex);
        *detour = reinterpret_cast<void *>(dx9_hkEndScene);
    } else if (render_type == RDT_DX_D3D10) {
        kiero::D3D10Output output;
        const kiero::Error error = kiero::locate<kiero::Implementation_D3D10>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate D3D10 failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = method_at(output.swapchain_methods, kPresentIndex);
        *detour = reinterpret_cast<void *>(dx10_hkPresent);
    } else if (render_type == RDT_DX_D3D11) {
        kiero::D3D11Output output;
        const kiero::Error error = kiero::locate<kiero::Implementation_D3D11>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate D3D11 failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = method_at(output.swapchain_methods, kPresentIndex);
        *detour = reinterpret_cast<void *>(dx11_hkPresent);
    } else if (render_type == RDT_DX_D3D12) {
        kiero::D3D12Output output;
        const kiero::Error error = kiero::locate<kiero::Implementation_D3D12>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate D3D12 failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = method_at(output.swapchain_methods, kPresentIndex);
        *detour = reinterpret_cast<void *>(dx12_hkPresent);
    } else if (render_type == RDT_GL_DEFAULT || render_type == RDT_GL_NOX) {
        kiero::OpenGLOutput output;
        const kiero::Error error = kiero::locate<kiero::Implementation_OpenGL>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate OpenGL failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = output.methods["wglSwapBuffers"];
        *detour = reinterpret_cast<void *>(gl_hkwglSwapBuffers);
    } else if (render_type == RDT_GL_STD) {
        kiero::OpenGLOutput output;
        const kiero::Error error = kiero::locate<kiero::Implementation_OpenGL>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate OpenGL failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = output.methods["glBegin"];
        *detour = reinterpret_cast<void *>(gl_hkglBegin);
    } else if (render_type == RDT_GL_ES) {
        *target = ResolveApi("libEGL.dll", "eglSwapBuffers");
        *detour = reinterpret_cast<void *>(gl_hkeglSwapBuffers);
    } else if (render_type == RDT_GL_FI) {
        kiero::OpenGLOutput output;
        const kiero::Error error = kiero::locate<kiero::Implementation_OpenGL>(nullptr, &output);
        if (error != kiero::Error_Nil) {
            setlog("DisplayHook locate OpenGL failed render_type=%d error=%d", render_type, error);
            return 0;
        }
        *target = output.methods["glFinish"];
        *detour = reinterpret_cast<void *>(gl_hkglFinish);
    }

    return *target && *detour ? 1 : 0;
}

} // namespace

int DisplayHook::setup(HWND hwnd_, int render_type_) {
    // H23: 全部"绑定态"字段（目标窗口 / 共享资源名 / render_type）推迟到钩子**真正装上之后**
    // 才发布。原实现一进函数就改写 render_hwnd/shared_res_name/mutex_name，locate 失败返回 0 时
    // 这些字段已指向新窗口 —— 调用方虽拿到失败，进程里却留着半套脏状态，之后所有日志与诊断
    // 都会指着一个从未被 Hook 的窗口。
    old_address.store(nullptr);
    hook_target = nullptr;

    void *address = nullptr;
    if (!locate_render_method(render_type_, &hook_target, &address)) {
        setlog("DisplayHook setup locate failed hwnd=%p render_type=%d target=%p detour=%p", hwnd_, render_type_,
               hook_target, address);
        hook_target = nullptr;
        return 0;
    }

    if (!AcquireMinHook()) {
        setlog("DisplayHook setup AcquireMinHook failed hwnd=%p render_type=%d target=%p detour=%p", hwnd_, render_type_,
               hook_target, address);
        hook_target = nullptr;
        return 0;
    }

    // MinHook 把 trampoline 写进出参；先收进局部量再发布到 atomic，避免其它线程读到半值。
    void *trampoline = nullptr;
    const MH_STATUS create_status = MH_CreateHook(hook_target, address, &trampoline);
    if (create_status != MH_OK) {
        setlog("DisplayHook setup MH_CreateHook failed hwnd=%p render_type=%d target=%p detour=%p create=%d", hwnd_,
               render_type_, hook_target, address, create_status);
        ReleaseMinHook();
        hook_target = nullptr;
        return 0;
    }

    // H2b（setup 侧的同类竞态）: 必须**先发布 trampoline 再启用**。
    // 原实现是 MH_EnableHook 成功之后才 store —— 而目标进程的渲染线程随时可能命中新装的
    // detour；在那几纳秒里 detour 读到的 old_address 还是 nullptr，于是跳板调用变成
    // "调空指针"。顺序反过来就没有这个窗口：没启用前没人会进 detour。
    old_address.store(trampoline);
    const MH_STATUS enable_status = MH_EnableHook(hook_target);
    if (enable_status != MH_OK) {
        setlog("DisplayHook setup MH_EnableHook failed hwnd=%p render_type=%d target=%p detour=%p enable=%d", hwnd_,
               render_type_, hook_target, address, enable_status);
        MH_DisableHook(hook_target);
        MH_RemoveHook(hook_target);
        ReleaseMinHook();
        old_address.store(nullptr);
        hook_target = nullptr;
        return 0;
    }

    // 钩子已生效，此刻才发布绑定态。
    DisplayHook::render_hwnd = hwnd_;
    DisplayHook::shared_res_name = MakeOpSharedResourceName(hwnd_);
    DisplayHook::mutex_name = MakeOpMutexName(hwnd_);
    render_type = render_type_;

    set_capture_enabled(true);
    return is_capture.load();
}

int DisplayHook::release() {
    // H2: 拆除顺序是本函数唯一的正确性要点，不可调整。
    // 1) 先停捕获 —— 此后新进入的 detour 立即空转，不再碰共享内存。
    set_capture_enabled(false);
    if (hook_target) {
        // 2) 恢复目标函数原入口（新调用不再进 detour）；MinHook 内部会冻结/解冻其它线程。
        MH_DisableHook(hook_target);
        // 3) 等在途 detour 退出。第 2 步只保证"新调用不再进入"，已经进到 detour 里、
        //    还没执行跳板调用的线程要等它走完；否则第 4 步释放的 trampoline 正在被它使用。
        if (!DetourGuard::wait_idle(kDetourDrainTimeoutMs)) {
            setlog("DisplayHook release: %d detour(s) still in flight after %dms, remove hook anyway",
                   DetourGuard::inflight(), kDetourDrainTimeoutMs);
        }
        MH_RemoveHook(hook_target);
        ReleaseMinHook();
    }
    old_address.store(nullptr);
    hook_target = nullptr;
    // H24: 原实现只清 render_hwnd/render_type，把两个资源名留着。下一次 setup 固然会覆盖，
    // 但两次绑定之间任何读它们的地方（日志、诊断、占位释放）都会拿到上一个窗口的名字。
    shared_res_name.clear();
    mutex_name.clear();
    render_hwnd = NULL;
    render_type = 0;
    // H20: 让下一次绑定重新打印一次首帧格式诊断，而不是永远沉默。
    dx11_reset_diagnostics();
    dx12_reset_diagnostics();
    return 1;
}

bool DisplayHook::capture_enabled() {
    return is_capture.load() != 0;
}

void DisplayHook::set_capture_enabled(bool enabled) {
    is_capture.store(enabled ? 1 : 0);
}

} // namespace op::hook
