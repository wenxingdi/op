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
#include <gl\glu.h>

#define DEBUG_HOOK 0

namespace op::hook {

using op::capture::FrameInfo;

long gl_capture() {
    using glPixelStorei_t = decltype(glPixelStorei) *;
    using glReadBuffer_t = decltype(glReadBuffer) *;
    using glReadPixels_t = decltype(glReadPixels) *;

    // H21: 这里原来还解析了一个 pglGetIntegerv，但全程零调用 —— 只参与"解析失败就停捕获"
    // 的判断，等于凭白多一个可能让捕获静默停掉的失败点。删掉。
    auto pglPixelStorei = (glPixelStorei_t)ResolveApi("opengl32.dll", "glPixelStorei");
    auto pglReadBuffer = (glReadBuffer_t)ResolveApi("opengl32.dll", "glReadBuffer");
    auto pglReadPixels = (glReadPixels_t)ResolveApi("opengl32.dll", "glReadPixels");
    if (!pglPixelStorei || !pglReadBuffer || !pglReadPixels) {
        setlog("gl resolve opengl32 APIs failed, disable capture");
        DisplayHook::set_capture_enabled(false);
        return 0;
    }
    RECT rc;
    ::GetClientRect(DisplayHook::render_hwnd, &rc);
    int width = rc.right - rc.left, height = rc.bottom - rc.top;

    pglPixelStorei(GL_PACK_ALIGNMENT, 1);
    pglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    pglReadBuffer(GL_FRONT);

    SharedMemory mem;
    ProcessMutex mutex;
    if (mem.open(DisplayHook::shared_res_name) && mutex.open(DisplayHook::mutex_name)) {
        mutex.lock();
        uchar *pshare = mem.data<byte>();
        if (SharedFrameHasCapacity(mem, width, height)) {
            reinterpret_cast<FrameInfo *>(pshare)->format(DisplayHook::render_hwnd, width, height);
            pglReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pshare + sizeof(FrameInfo));
        } else {
            WriteSharedFrameHeader(mem, DisplayHook::render_hwnd, width, height);
        }
        mutex.unlock();
    } else {
        DisplayHook::set_capture_enabled(false);
#if DEBUG_HOOK
        setlog(L"egl !mem.open(DisplayHook::%s)&&mutex.open(DisplayHook::%s)",
               DisplayHook::shared_res_name.c_str(), DisplayHook::mutex_name.c_str());
#endif // DEBUG_HOOK
    }
    return 0;
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

long egl_capture() {
    using glPixelStorei_t = decltype(glPixelStorei) *;
    using glReadBuffer_t = decltype(glReadBuffer) *;
    using glReadPixels_t = decltype(glReadPixels) *;

    // H21: 同 gl_capture —— pglGetIntegerv 解析了但零调用，已删。
    auto pglPixelStorei = (glPixelStorei_t)ResolveApi("libglesv2.dll", "glPixelStorei");
    auto pglReadBuffer = (glReadBuffer_t)ResolveApi("libglesv2.dll", "glReadBuffer");
    auto pglReadPixels = (glReadPixels_t)ResolveApi("libglesv2.dll", "glReadPixels");
    if (!pglPixelStorei || !pglReadBuffer || !pglReadPixels) {
        // 与 gl_capture 对齐：解析失败即停捕获，否则每帧重复解析这 4 个 API。
        setlog("egl resolve libglesv2 APIs failed, disable capture");
        DisplayHook::set_capture_enabled(false);
#if DEBUG_HOOK
        setlog(L"egl !mem.open(DisplayHook::%s)&&mutex.open(DisplayHook::%s)",
               DisplayHook::shared_res_name.c_str(), DisplayHook::mutex_name.c_str());
#endif // DEBUG_HOOK

        return 0;
    }
    RECT rc;
    ::GetClientRect(DisplayHook::render_hwnd, &rc);
    int width = rc.right - rc.left, height = rc.bottom - rc.top;

    pglPixelStorei(GL_PACK_ALIGNMENT, 1);
    pglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    pglReadBuffer(GL_FRONT);

    SharedMemory mem;
    ProcessMutex mutex;
    if (mem.open(DisplayHook::shared_res_name) && mutex.open(DisplayHook::mutex_name)) {
        mutex.lock();
        uchar *pshare = mem.data<byte>();
        if (SharedFrameHasCapacity(mem, width, height)) {
            reinterpret_cast<FrameInfo *>(pshare)->format(DisplayHook::render_hwnd, width, height);
            pglReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pshare + sizeof(FrameInfo));
        } else {
            WriteSharedFrameHeader(mem, DisplayHook::render_hwnd, width, height);
        }
        mutex.unlock();
    } else {
        DisplayHook::set_capture_enabled(false);
#if DEBUG_HOOK
        setlog(L"egl !mem.open(DisplayHook::%s)&&mutex.open(DisplayHook::%s)",
               DisplayHook::shared_res_name.c_str(), DisplayHook::mutex_name.c_str());
#endif // DEBUG_HOOK
    }
    return 0;
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
