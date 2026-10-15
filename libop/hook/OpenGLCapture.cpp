#include "OpenGLCapture.h"

#include "DisplayHook.h"
#include "DetourGuard.h"
#include "SharedFrame.h"
#include "../capture/FrameInfo.h"
#include "../hook/ApiResolver.h"
#include "../ipc/ProcessMutex.h"
#include "../ipc/SharedMemory.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"
#include <atomic>
#include <gl\glu.h>

#define DEBUG_HOOK 0

namespace op::hook {

using op::capture::FrameInfo;

// N2 说明见 ApiResolver.h 的 CachedResolveApi 注释（"失败不得当终值缓存"语义）：
// gl 与 egl 必须各一套槽 —— 它们查的是不同的 dll（opengl32.dll / libglesv2.dll），
// 槽由各 capture 函数以数组形式传给 gl_capture_common。

// HK3: gl 与 egl 两路捕获逐行级重复（仅缓存槽 / dll 名 / 日志标签不同），收为一份。
// slots 三个原子槽分别对应 glPixelStorei / glReadBuffer / glReadPixels。
long gl_capture_common(std::atomic<void *> *slots, const char *dll, const char *tag) {
    using glPixelStorei_t = decltype(glPixelStorei) *;
    using glReadBuffer_t = decltype(glReadBuffer) *;
    using glReadPixels_t = decltype(glReadPixels) *;

    // H21: 这里原来还解析了一个 pglGetIntegerv，但全程零调用 —— 只参与"解析失败就停捕获"
    // 的判断，等于凭白多一个可能让捕获静默停掉的失败点。删掉。
    // N2: 三个指针改走缓存 —— glBegin detour 一帧可命中数千次，原先每帧 3 次
    // GetModuleHandleA+GetProcAddress 是纯白付。失败语义与原先一致（下帧重试）。
    auto pglPixelStorei = reinterpret_cast<glPixelStorei_t>(CachedResolveApi(slots[0], dll, "glPixelStorei"));
    auto pglReadBuffer = reinterpret_cast<glReadBuffer_t>(CachedResolveApi(slots[1], dll, "glReadBuffer"));
    auto pglReadPixels = reinterpret_cast<glReadPixels_t>(CachedResolveApi(slots[2], dll, "glReadPixels"));
    if (!pglPixelStorei || !pglReadBuffer || !pglReadPixels) {
        setlog("%s resolve %s APIs failed, disable capture", tag, dll);
        DisplayHook::set_capture_enabled(false);
        return 0;
    }
    RECT rc;
    ::GetClientRect(DisplayHook::render_hwnd, &rc);
    int width = rc.right - rc.left, height = rc.bottom - rc.top;

    pglPixelStorei(GL_PACK_ALIGNMENT, 1);
    pglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    pglReadBuffer(GL_FRONT);

    // HK4: 共享帧写入收拢进 SharedFrameWriter（GL 直接 ReadPixels 进像素区）。
    static_assert(sizeof(FrameInfo) == 28);
    SharedFrameWriter frame(DisplayHook::shared_res_name, DisplayHook::mutex_name);
    bool open_failed = false;
    const auto pixels = frame.prepare(DisplayHook::render_hwnd, width, height, &open_failed);
    if (open_failed) {
        setlog("%s shared frame open failed, disable capture", tag);
        DisplayHook::set_capture_enabled(false);
        return 0;
    }
    if (!pixels.empty()) {
        pglReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());
    }
    return 0;
}

long gl_capture() {
    static std::atomic<void *> slots[3] = {};
    return gl_capture_common(slots, "opengl32.dll", "gl");
}

long egl_capture() {
    static std::atomic<void *> slots[3] = {};
    // N2: 与 gl 侧同语义（egl 路径走 eglSwapBuffers，帧率不高，但缓存策略保持一致）。
    return gl_capture_common(slots, "libglesv2.dll", "egl");
}

void __stdcall gl_hkglBegin(GLenum mode) {
    // H2: 覆盖下方跳板调用，release 靠该计数确认跳板可安全释放。
    DetourScope guard;
    using glBegin_t = decltype(glBegin) *;

    if (DisplayHook::capture_enabled())
        gl_capture();
    ((glBegin_t)DisplayHook::old_address.load(std::memory_order_acquire))(mode);
}

void __stdcall gl_hkwglSwapBuffers(HDC hdc) {
    DetourScope guard;
    using wglSwapBuffers_t = void(__stdcall *)(HDC hdc);
    if (DisplayHook::capture_enabled())
        gl_capture();
    ((wglSwapBuffers_t)DisplayHook::old_address.load(std::memory_order_acquire))(hdc);
}

unsigned int __stdcall gl_hkeglSwapBuffers(void *dpy, void *surface) {
    DetourScope guard;
    using eglSwapBuffers_t = decltype(gl_hkeglSwapBuffers) *;
    if (DisplayHook::capture_enabled())
        egl_capture();
    return ((eglSwapBuffers_t)DisplayHook::old_address.load(std::memory_order_acquire))(dpy, surface);
}

void __stdcall gl_hkglFinish(void) {
    DetourScope guard;
    using glFinish_t = decltype(glFinish) *;
    if (DisplayHook::capture_enabled())
        gl_capture();
    ((glFinish_t)DisplayHook::old_address.load(std::memory_order_acquire))();
}

} // namespace op::hook
