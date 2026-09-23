#include "OpContext.h"
#include "OpResult.h"

#include "base/Utils.h"
#include "window/WindowLayout.h"

#include <libop.h>

#include <cwchar>
#include <string>
#include <vector>

#include <algorithm>
#include <imm.h>
#include <mutex>
#include <thread>

#undef FindWindow
#undef FindWindowEx
#undef SetWindowText

namespace {

bool parse_layout_type(long value, op::window_layout::Type &type) {
    switch (value) {
    case 0:
        type = op::window_layout::Type::Grid;
        return true;
    case 1:
        type = op::window_layout::Type::Diagonal;
        return true;
    case 2:
        type = op::window_layout::Type::Cascade;
        return true;
    default:
        setlog(L"LayoutWindows: 非法 layout_type=%ld（0=宫格 1=对角线 2=层叠）", value);
        return false;
    }
}

bool parse_size_mode(long value, op::window_layout::SizeMode &mode) {
    switch (value) {
    case 0:
        mode = op::window_layout::SizeMode::Keep;
        return true;
    case 1:
        mode = op::window_layout::SizeMode::Uniform;
        return true;
    default:
        setlog(L"LayoutWindows: 非法 size_mode=%ld（0=保持原大小 1=统一客户区大小）", value);
        return false;
    }
}

bool parse_anchor_mode(long value, op::window_layout::AnchorMode &mode) {
    switch (value) {
    case 0:
        mode = op::window_layout::AnchorMode::Window;
        return true;
    case 1:
        mode = op::window_layout::AnchorMode::Client;
        return true;
    default:
        setlog(L"LayoutWindows: 非法 anchor_mode=%ld（0=窗口外框 1=客户区）", value);
        return false;
    }
}

bool parse_window_list(const wchar_t *hwnds, std::vector<HWND> &windows) {
    if (hwnds == nullptr || hwnds[0] == L'\0') {
        setlog(L"LayoutWindows: 窗口句柄串为空");
        return false;
    }

    std::vector<std::wstring> items;
    split(hwnds, items, L"|");
    if (items.empty())
        return false;

    windows.clear();
    windows.reserve(items.size());
    for (const auto &item : items) {
        wchar_t *end = nullptr;
        const auto value = _wcstoi64(item.c_str(), &end, 0);
        if (end == item.c_str() || (end && *end != L'\0')) {
            setlog(L"LayoutWindows: 句柄串含非法项 \"%s\"（应为十进制 hwnd 竖线分隔）", item.c_str());
            return false;
        }
        if (value <= 0) {
            // HWND 0/负值必然过不了 IsWindow，拦在这里避免"前面窗口已摆好才失败"的半应用状态
            setlog(L"LayoutWindows: 句柄串含非法 hwnd 值 %lld（必须为正整数）", value);
            return false;
        }
        windows.push_back(reinterpret_cast<HWND>(static_cast<LONG_PTR>(value)));
    }

    return !windows.empty();
}

std::wstring get_window_class_name(HWND hwnd) {
    std::vector<wchar_t> buffer(256, L'\0');
    for (;;) {
        const int copied = ::GetClassNameW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
        if (copied <= 0)
            return L"";
        if (static_cast<size_t>(copied) < buffer.size() - 1)
            return std::wstring(buffer.data(), static_cast<size_t>(copied));
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

std::wstring get_window_title(HWND hwnd) {
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0)
        return L"";

    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0)
        return L"";
    return std::wstring(buffer.data(), static_cast<size_t>(copied));
}

LONG_PTR hwnd_result(HWND hwnd) {
    return reinterpret_cast<LONG_PTR>(hwnd);
}

} // namespace

void op::Op::EnumWindow(LONG_PTR parent, const wchar_t *title, const wchar_t *class_name, long filter,
                            std::wstring &retstr) {
    m_context->window_service.EnumWindow(reinterpret_cast<HWND>(parent), title, class_name, filter, retstr);
}

void op::Op::EnumWindowByProcess(const wchar_t *process_name, const wchar_t *title, const wchar_t *class_name,
                                     long filter, std::wstring &retstring) {
    m_context->window_service.EnumWindow(nullptr, title, class_name, filter, retstring, process_name);
}

void op::Op::ClientToScreen(LONG_PTR hwnd, long *x, long *y, long *bret) {
    internal::set_result(bret, m_context->window_service.ClientToScreen(reinterpret_cast<HWND>(hwnd), *x, *y));
}

void op::Op::FindWindow(const wchar_t *class_name, const wchar_t *title, LONG_PTR *rethwnd) {
    internal::set_result(rethwnd, hwnd_result(m_context->window_service.FindWindow(class_name, title)));
}

void op::Op::FindWindowByProcess(const wchar_t *process_name, const wchar_t *class_name, const wchar_t *title,
                                     LONG_PTR *rethwnd) {

    HWND hwnd = nullptr;
    m_context->window_service.FindWindowByProcess(class_name, title, hwnd, process_name);
    internal::set_result(rethwnd, hwnd_result(hwnd));
}

void op::Op::FindWindowByProcessId(long process_id, const wchar_t *class_name, const wchar_t *title,
                                       LONG_PTR *rethwnd) {
    HWND hwnd = nullptr;
    m_context->window_service.FindWindowByProcess(class_name, title, hwnd, NULL, process_id);
    internal::set_result(rethwnd, hwnd_result(hwnd));
}

void op::Op::FindWindowEx(LONG_PTR parent, const wchar_t *class_name, const wchar_t *title, LONG_PTR *rethwnd) {
    internal::set_result(
        rethwnd, hwnd_result(m_context->window_service.FindWindowEx(reinterpret_cast<HWND>(parent), class_name, title)));
}

void op::Op::GetClientRect(LONG_PTR hwnd, long *x1, long *y1, long *x2, long *y2, long *nret) {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
    auto target = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
    internal::set_result(nret, m_context->window_service.GetClientRect(target, left, top, right, bottom));
    // 最小化窗口的矩形是 -32000 哨兵值。它会被调用方当作正常尺寸继续参与
    // 坐标换算与裁剪，结果是一个巨大的负数（表现为"坐标全乱"），而这里
    // 是唯一能提前说明原因的地方。
    if (target && ::IsIconic(target)) {
        setlog(L"get_client_rect: hwnd=%p is minimized, the rect is a -32000 sentinel; restore it first",
               static_cast<void *>(target));
    }
    internal::set_result(x1, left);
    internal::set_result(y1, top);
    internal::set_result(x2, right);
    internal::set_result(y2, bottom);
}

void op::Op::GetClientSize(LONG_PTR hwnd, long *width, long *height, long *nret) {
    long client_width = 0;
    long client_height = 0;
    internal::set_result(
        nret, m_context->window_service.GetClientSize(reinterpret_cast<HWND>(hwnd), client_width, client_height));
    internal::set_result(width, client_width);
    internal::set_result(height, client_height);
}

void op::Op::GetForegroundFocus(LONG_PTR *rethwnd) {
    internal::set_result(rethwnd, hwnd_result(::GetFocus()));
}

void op::Op::GetForegroundWindow(LONG_PTR *rethwnd) {
    internal::set_result(rethwnd, hwnd_result(::GetForegroundWindow()));
}

void op::Op::GetMousePointWindow(LONG_PTR *rethwnd) {
    //::Sleep(2000);
    HWND hwnd = nullptr;
    m_context->window_service.GetMousePointWindow(hwnd);
    internal::set_result(rethwnd, hwnd_result(hwnd));
}

void op::Op::GetPointWindow(long x, long y, LONG_PTR *rethwnd) {
    HWND hwnd = nullptr;
    m_context->window_service.GetMousePointWindow(hwnd, x, y);
    internal::set_result(rethwnd, hwnd_result(hwnd));
}

void op::Op::GetSpecialWindow(long flag, LONG_PTR *rethwnd) {
    internal::set_result(rethwnd, 0);
    if (flag == 0)
        internal::set_result(rethwnd, hwnd_result(GetDesktopWindow()));
    else if (flag == 1) {
        internal::set_result(rethwnd, hwnd_result(::FindWindowW(L"Shell_TrayWnd", NULL)));
    }
}

void op::Op::GetWindow(LONG_PTR hwnd, long flag, LONG_PTR *nret) {
    HWND target = nullptr;
    m_context->window_service.GetWindow(reinterpret_cast<HWND>(hwnd), flag, target);
    internal::set_result(nret, hwnd_result(target));
}

void op::Op::GetWindowClass(LONG_PTR hwnd, std::wstring &retstring) {
    retstring = get_window_class_name(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)));
}

void op::Op::GetWindowRect(LONG_PTR hwnd, long *x1, long *y1, long *x2, long *y2, long *nret) {
    RECT winrect = {};
    internal::set_result(nret, ::GetWindowRect(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), &winrect));
    internal::set_result(x1, winrect.left);
    internal::set_result(y1, winrect.top);
    internal::set_result(x2, winrect.right);
    internal::set_result(y2, winrect.bottom);
}

void op::Op::GetWindowState(LONG_PTR hwnd, long flag, long *rethwnd) {
    internal::set_result(rethwnd,
                         m_context->window_service.GetWindowState(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), flag));
}

void op::Op::GetWindowTitle(LONG_PTR hwnd, std::wstring &rettitle) {
    rettitle = get_window_title(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)));
}

void op::Op::MoveWindow(LONG_PTR hwnd, long x, long y, long *nret) {
    RECT winrect;
    HWND target = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
    ::GetWindowRect(target, &winrect);
    int width = winrect.right - winrect.left;
    int height = winrect.bottom - winrect.top;
    internal::set_result(nret, ::MoveWindow(target, x, y, width, height, false));
}

void op::Op::ScreenToClient(LONG_PTR hwnd, long *x, long *y, long *nret) {
    POINT point;
    point.x = x ? *x : 0;
    point.y = y ? *y : 0;
    internal::set_result(nret, ::ScreenToClient(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), &point));
    internal::set_result(x, point.x);
    internal::set_result(y, point.y);
}

void op::Op::SetClientSize(LONG_PTR hwnd, long width, long height, long *nret) {
    internal::set_result(nret, m_context->window_service.SetWindowSize(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)),
                                                                       width, height));
}

void op::Op::SetWindowState(LONG_PTR hwnd, long flag, long *nret) {
    internal::set_result(nret,
                         m_context->window_service.SetWindowState(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), flag));
}

void op::Op::SetWindowSize(LONG_PTR hwnd, long width, long height, long *nret) {
    internal::set_result(nret, m_context->window_service.SetWindowSize(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)),
                                                                       width, height, 1));
}

void op::Op::LayoutWindows(const wchar_t *hwnds, long layout_type, long columns, long start_x, long start_y,
                                long gap_x, long gap_y, long size_mode, long window_width, long window_height,
                                long anchor_mode, long *ret) {
    internal::set_result(ret, 0L);

    std::vector<HWND> windows;
    if (!parse_window_list(hwnds, windows))
        return;

    op::window_layout::Options options;
    if (!parse_layout_type(layout_type, options.type))
        return;
    if (!parse_size_mode(size_mode, options.size_mode))
        return;
    if (!parse_anchor_mode(anchor_mode, options.anchor_mode))
        return;

    options.columns = columns;
    options.start_x = start_x;
    options.start_y = start_y;
    options.gap_x = gap_x;
    options.gap_y = gap_y;
    options.window_width = window_width;
    options.window_height = window_height;

    internal::set_result(ret, op::window_layout::Layout(windows, options));
}

void op::Op::SetWindowText(LONG_PTR hwnd, const wchar_t *title, long *nret) {
    internal::set_result(nret, ::SetWindowTextW(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), title));
}

void op::Op::SetWindowTransparent(LONG_PTR hwnd, long trans, long *nret) {
    internal::set_result(nret,
                         m_context->window_service.SetWindowTransparent(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)),
                                                                        trans));
}

void op::Op::SendString(LONG_PTR hwnd, const wchar_t *str, long *ret) {
    internal::set_result(ret, m_context->window_service.SendString(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), str));
}

void op::Op::SendStringIme(LONG_PTR hwnd, const wchar_t *str, long *ret) {
    internal::set_result(ret,
                         m_context->window_service.SendStringIme(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), str));
}

namespace {

// ---------------- 窗口几何锁 ----------------
// 守护线程 50ms 轮询:锁定项的窗口被外部移动/缩放时自动拉回锚点矩形。
// 设计取舍:跨进程子类化 WM_WINDOWPOSCHANGED 需要注入 DLL,轮询 + SetWindowPos 足够且零注入。
struct GeoLockEntry {
    HWND hwnd = nullptr;
    bool lock_pos = false;
    bool lock_size = false;
    bool orig_style_saved = false;
    LONG_PTR orig_style = 0;
    RECT anchor = {};
};

std::mutex g_geo_lock_mtx;
std::vector<GeoLockEntry> g_geo_locks;
std::thread g_geo_lock_thread;
std::once_flag g_geo_lock_thread_once;

void geo_lock_worker() {
    for (;;) {
        Sleep(50);
        std::vector<GeoLockEntry> snapshot;
        {
            std::lock_guard<std::mutex> lk(g_geo_lock_mtx);
            snapshot = g_geo_locks;
        }
        for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
            if (!IsWindow(it->hwnd))
                continue; // 窗口已销毁:条目由下次解锁/开关调用清理
            RECT now = {};
            if (!::GetWindowRect(it->hwnd, &now))
                continue;
            const LONG want_w = it->anchor.right - it->anchor.left;
            const LONG want_h = it->anchor.bottom - it->anchor.top;
            const LONG want_x = it->lock_pos ? it->anchor.left : now.left;
            const LONG want_y = it->lock_pos ? it->anchor.top : now.top;
            const LONG want_cx = it->lock_size ? want_w : now.right - now.left;
            const LONG want_cy = it->lock_size ? want_h : now.bottom - now.top;
            if (want_x != now.left || want_y != now.top || want_cx != now.right - now.left ||
                want_cy != now.bottom - now.top) {
                // 注意:SetWindowPos 会同步向目标窗口发送 WM_WINDOWPOSCHANGED,
                // 目标线程消息循环不活动(如测试中纯 Sleep)时回弹会延迟到其恢复泵消息后——与 DM/AJ 行为一致。
                SetWindowPos(it->hwnd, nullptr, want_x, want_y, want_cx, want_cy,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            }
        }
    }
}

GeoLockEntry *geo_lock_find_locked(HWND hwnd) {
    for (auto &e : g_geo_locks) {
        if (e.hwnd == hwnd)
            return &e;
    }
    return nullptr;
}

void geo_lock_ensure_thread() {
    std::call_once(g_geo_lock_thread_once, []() { g_geo_lock_thread = std::thread(geo_lock_worker); });
}

// 返回 false 表示该条目已无任何锁标志,调用方应删除
bool geo_lock_prune(GeoLockEntry &e) { return !e.lock_pos && !e.lock_size && !e.orig_style_saved; }

// ---------------- 输入法(imm32 动态加载,免改链接库) ----------------
struct ImmApi {
    HMODULE mod = nullptr;
    HIMC(WINAPI *get_context)(HWND) = nullptr;
    BOOL(WINAPI *set_open_status)(HIMC, BOOL) = nullptr;
    BOOL(WINAPI *release_context)(HWND, HIMC) = nullptr;
    BOOL(WINAPI *associate_context_ex)(HWND, HIMC, DWORD) = nullptr;
    bool ready = false;
};

ImmApi &imm_api() {
    static ImmApi api = [] {
        ImmApi a;
        a.mod = LoadLibraryW(L"imm32.dll");
        if (a.mod) {
            a.get_context = reinterpret_cast<HIMC(WINAPI *)(HWND)>(GetProcAddress(a.mod, "ImmGetContext"));
            a.set_open_status =
                reinterpret_cast<BOOL(WINAPI *)(HIMC, BOOL)>(GetProcAddress(a.mod, "ImmSetOpenStatus"));
            a.release_context =
                reinterpret_cast<BOOL(WINAPI *)(HWND, HIMC)>(GetProcAddress(a.mod, "ImmReleaseContext"));
            a.associate_context_ex = reinterpret_cast<BOOL(WINAPI *)(HWND, HIMC, DWORD)>(
                GetProcAddress(a.mod, "ImmAssociateContextEx"));
            a.ready = a.get_context && a.set_open_status && a.release_context;
        }
        return a;
    }();
    return api;
}

} // namespace

void op::Op::LockWindowPosition(LONG_PTR hwnd, long enable, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
    if (!target || !IsWindow(target))
        return;

    geo_lock_ensure_thread();
    std::lock_guard<std::mutex> lk(g_geo_lock_mtx);
    GeoLockEntry *e = geo_lock_find_locked(target);
    if (enable) {
        if (!e) {
            g_geo_locks.push_back(GeoLockEntry{});
            e = &g_geo_locks.back();
            e->hwnd = target;
            ::GetWindowRect(target, &e->anchor);
        } else if (!e->lock_pos) {
            // 重新锚定当前位置(避免旧锚点残留)
            ::GetWindowRect(target, &e->anchor);
        }
        e->lock_pos = true;
    } else if (e) {
        e->lock_pos = false;
        if (geo_lock_prune(*e)) {
            auto it = std::find_if(g_geo_locks.begin(), g_geo_locks.end(),
                                   [&](const GeoLockEntry &x) { return x.hwnd == target; });
            if (it != g_geo_locks.end())
                g_geo_locks.erase(it);
        }
    }
    internal::set_result(ret, 1L);
}

void op::Op::LockWindowSize(LONG_PTR hwnd, long enable, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
    if (!target || !IsWindow(target))
        return;

    geo_lock_ensure_thread();
    std::lock_guard<std::mutex> lk(g_geo_lock_mtx);
    GeoLockEntry *e = geo_lock_find_locked(target);
    if (enable) {
        if (!e) {
            g_geo_locks.push_back(GeoLockEntry{});
            e = &g_geo_locks.back();
            e->hwnd = target;
            ::GetWindowRect(target, &e->anchor);
        } else if (!e->lock_size) {
            ::GetWindowRect(target, &e->anchor);
        }
        e->lock_size = true;
    } else if (e) {
        e->lock_size = false;
        if (geo_lock_prune(*e)) {
            auto it = std::find_if(g_geo_locks.begin(), g_geo_locks.end(),
                                   [&](const GeoLockEntry &x) { return x.hwnd == target; });
            if (it != g_geo_locks.end())
                g_geo_locks.erase(it);
        }
    }
    internal::set_result(ret, 1L);
}

void op::Op::DisableMinMax(LONG_PTR hwnd, long enable, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
    if (!target || !IsWindow(target))
        return;

    constexpr LONG_PTR kMask = WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
    std::lock_guard<std::mutex> lk(g_geo_lock_mtx);
    GeoLockEntry *e = geo_lock_find_locked(target);
    if (enable) {
        if (!e) {
            g_geo_locks.push_back(GeoLockEntry{});
            e = &g_geo_locks.back();
            e->hwnd = target;
        }
        if (!e->orig_style_saved) {
            e->orig_style = GetWindowLongPtrW(target, GWL_STYLE);
            e->orig_style_saved = true;
        }
        SetWindowLongPtrW(target, GWL_STYLE, e->orig_style & ~kMask);
    } else if (e && e->orig_style_saved) {
        // 恢复原始样式;若位置/尺寸锁已不存在则删除条目
        SetWindowLongPtrW(target, GWL_STYLE, e->orig_style);
        e->orig_style = 0;
        e->orig_style_saved = false;
        if (geo_lock_prune(*e)) {
            auto it = std::find_if(g_geo_locks.begin(), g_geo_locks.end(),
                                   [&](const GeoLockEntry &x) { return x.hwnd == target; });
            if (it != g_geo_locks.end())
                g_geo_locks.erase(it);
        }
    }
    SetWindowPos(target, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    internal::set_result(ret, 1L);
}

void op::Op::SetIme(LONG_PTR hwnd, long enable, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
    if (!target || !IsWindow(target))
        return;

    ImmApi &api = imm_api();
    if (!api.ready)
        return;

    HIMC himc = api.get_context(target);
    if (himc) {
        api.set_open_status(himc, enable ? TRUE : FALSE);
        api.release_context(target, himc);
        internal::set_result(ret, 1L);
        return;
    }
    // 窗口尚无输入上下文:关闭时直接取消其默认 IME 关联
    if (!enable && api.associate_context_ex) {
        if (api.associate_context_ex(target, nullptr, IACE_DEFAULT))
            internal::set_result(ret, 1L);
    }
}
