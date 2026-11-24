#include "DpiContext.h"

#include <cmath>

namespace op::win32 {

namespace {

// DPI_AWARENESS_CONTEXT 与 HANDLE 同型（都是 void*），这里显式起别名避免误用。
using dpi_awareness_context = HANDLE;

struct dpi_api_table {
    BOOL(WINAPI *are_equal)(dpi_awareness_context, dpi_awareness_context) = nullptr;
    dpi_awareness_context(WINAPI *get_thread)(void) = nullptr;
    dpi_awareness_context(WINAPI *get_window)(HWND) = nullptr;
    dpi_awareness_context(WINAPI *set_thread)(dpi_awareness_context) = nullptr;
    bool available = false;
};

// SetThreadDpiAwarenessContext / GetWindowDpiAwarenessContext 是 Win10 1607 起才有的
// 导出，静态链接会在更老的系统上加载失败。全部动态解析，缺任一项即整体退化为恒等。
const dpi_api_table &dpi_api() noexcept {
    static const dpi_api_table table = [] {
        dpi_api_table t;
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (!user32)
            return t;
        t.are_equal = reinterpret_cast<decltype(t.are_equal)>(
            ::GetProcAddress(user32, "AreDpiAwarenessContextsEqual"));
        t.get_thread =
            reinterpret_cast<decltype(t.get_thread)>(::GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
        t.get_window =
            reinterpret_cast<decltype(t.get_window)>(::GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
        t.set_thread =
            reinterpret_cast<decltype(t.set_thread)>(::GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        t.available = t.are_equal && t.get_thread && t.get_window && t.set_thread;
        return t;
    }();
    return table;
}

// 同一上下文下的客户区尺寸：取够大的一边（极窄窗口用宽度算比例会放大取整误差）。
int client_extent_now(HWND hwnd) noexcept {
    RECT rc{};
    if (!::GetClientRect(hwnd, &rc))
        return 0;
    const int width = static_cast<int>(rc.right - rc.left);
    if (width >= 8)
        return width;
    const int height = static_cast<int>(rc.bottom - rc.top);
    return height >= 8 ? height : width;
}

} // namespace

bool dpi_context_api_available() noexcept {
    return dpi_api().available;
}

dpi_context_scope::dpi_context_scope(HWND hwnd) noexcept {
    const dpi_api_table &api = dpi_api();
    if (!api.available || !hwnd || !::IsWindow(hwnd))
        return;

    const dpi_awareness_context target = api.get_window(hwnd);
    const dpi_awareness_context current = api.get_thread();
    // 已经是同一上下文时不做任何事：普通 DPI 感知窗口走这条，行为与旧版完全一致。
    if (!target || api.are_equal(target, current))
        return;

    dpi_awareness_context previous = api.set_thread(target);
    if (!previous)
        return;
    previous_ = previous;
    changed_ = true;
}

dpi_context_scope::~dpi_context_scope() {
    if (!changed_)
        return;
    const dpi_api_table &api = dpi_api();
    if (api.available && api.set_thread) {
        api.set_thread(static_cast<dpi_awareness_context>(previous_));
    }
}

double window_pixel_scale(HWND hwnd) noexcept {
    if (!hwnd || !::IsWindow(hwnd) || !dpi_api().available)
        return 1.0;

    // 同进程窗口不换算：Windows 的跨 DPI 像素虚拟化只发生在跨进程读取时，同进程
    // ClientToScreen 的增量恒为逻辑像素（实测 (0,0)→(30,30) 增量就是 30，而非 45），
    // 此时再除比例会把坐标缩小 1.5 倍。测试自建窗口全部走这条。
    DWORD owner_pid = 0;
    ::GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid == ::GetCurrentProcessId())
        return 1.0;

    // 宿主上下文口径（切换前）
    const int host_extent = client_extent_now(hwnd);
    if (host_extent <= 0)
        return 1.0;

    const dpi_context_scope scope(hwnd);
    if (!scope.changed())
        return 1.0;

    const int own_extent = client_extent_now(hwnd);
    if (own_extent <= 0)
        return 1.0;

    const double scale = static_cast<double>(host_extent) / static_cast<double>(own_extent);
    // 只认真实的整数倍缩放（100/125/150/175/200%…）。取整后偏差过大说明量到了
    // 过渡态（窗口正在 resize），按 1.0 处理更安全：宁可不缩放，也不要乱缩放。
    const double rounded = std::round(scale * 4.0) / 4.0; // 0.25 粒度覆盖 125%/175%
    if (rounded <= 0.0 || std::fabs(scale - rounded) > 0.05)
        return 1.0;
    return scale;
}

POINT screen_to_client_frame(HWND hwnd, POINT host_screen) noexcept {
    if (!hwnd || !::IsWindow(hwnd))
        return host_screen;

    // 用 ClientToScreen(0,0) 取「物理客户区原点」，再做差 —— 不能改用 ScreenToClient：
    // 它对**同进程**窗口返回逻辑客户区偏移、对**跨进程**窗口返回物理偏移（实测差 1.5 倍），
    // 口径随场景变化，无法判别。ClientToScreen 则恒返回物理屏幕坐标（宿主 aware 时）。
    POINT origin{0, 0};
    if (!::ClientToScreen(hwnd, &origin))
        return host_screen;

    POINT offset{host_screen.x - origin.x, host_screen.y - origin.y};
    const double scale = window_pixel_scale(hwnd);
    if (scale == 1.0)
        return offset;

    // 客户区内部是纯缩放关系（非客户区不缩放，已由「减原点」消掉），除比例不引入边框偏差
    offset.x = static_cast<LONG>(std::lround(static_cast<double>(offset.x) / scale));
    offset.y = static_cast<LONG>(std::lround(static_cast<double>(offset.y) / scale));
    return offset;
}

POINT client_frame_to_screen(HWND hwnd, POINT frame_client) noexcept {
    POINT origin{0, 0};
    if (!hwnd || !::IsWindow(hwnd))
        return frame_client;

    // 宿主上下文里的物理客户区原点
    if (!::ClientToScreen(hwnd, &origin))
        return frame_client;

    const double scale = window_pixel_scale(hwnd);
    if (scale == 1.0) {
        origin.x += frame_client.x;
        origin.y += frame_client.y;
        return origin;
    }
    origin.x += static_cast<LONG>(std::lround(static_cast<double>(frame_client.x) * scale));
    origin.y += static_cast<LONG>(std::lround(static_cast<double>(frame_client.y) * scale));
    return origin;
}

} // namespace op::win32
