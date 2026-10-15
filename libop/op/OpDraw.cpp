// OpDraw.cpp: 屏幕透明叠加层绘制（P2 新增 2026-10-15，参考 OLA DrawGui 最小组）。
// 进程级单例：专用线程 + 全屏分层窗口（WS_EX_LAYERED|TRANSPARENT|TOPMOST，穿透点击），
// GDI+ 渲到 32bpp ARGB 位图后经 UpdateLayeredWindow 上屏。
// 定位：调试可视化（找图结果标注），不追求高帧率——图元变更才重绘。
#include "OpResult.h"

#include "base/Utils.h"

#include <libop.h>

#include <Windows.h>
#include <gdiplus.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr UINT kRedrawMsg = WM_APP + 71;

enum class DrawCmdType { Rect, Line, Text };

struct DrawCmd {
    DrawCmdType type;
    long x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    long width = 1;
    long font_size = 14;
    DWORD color = 0xFFFFFFFF; // 0xAABBGGRR（GDI+ ARGB）
    std::wstring text;
};

// 解析 "RRGGBB"（与找色同格式）为 GDI+ ARGB。失败返回 false。
bool parse_draw_color(const wchar_t *color, DWORD &out) {
    if (!color || !color[0])
        return false;
    unsigned v = 0;
    if (swscanf(color, L"%x", &v) != 1 || v > 0xFFFFFF)
        return false;
    const BYTE r = static_cast<BYTE>((v >> 16) & 0xFF);
    const BYTE g = static_cast<BYTE>((v >> 8) & 0xFF);
    const BYTE b = static_cast<BYTE>(v & 0xFF);
    out = 0xFF000000u | (static_cast<DWORD>(b) << 16) | (static_cast<DWORD>(g) << 8) | r;
    return true;
}

class DrawOverlay {
  public:
    // 首次调用创建线程与窗口（惰性）。失败返回 false。
    bool ensure_started() {
        std::call_once(start_flag_, [this] { start_thread(); });
        if (ready_.load(std::memory_order_acquire))
            return true;
        // call_once 只保证 start_thread 返回，工作线程可能尚未完成建窗：等就绪事件（含失败路径也会置位）
        if (ready_event_) {
            ::WaitForSingleObject(ready_event_, 5000);
            ::CloseHandle(ready_event_);
            ready_event_ = nullptr;
        }
        return ready_.load(std::memory_order_acquire);
    }

    bool add(DrawCmd cmd) {
        if (!ensure_started())
            return false;
        {
            std::lock_guard<std::mutex> lk(cmds_mutex_);
            cmds_.push_back(std::move(cmd));
        }
        ::PostMessageW(hwnd_, kRedrawMsg, 0, 0);
        return true;
    }

    bool clear() {
        if (!ensure_started())
            return false;
        {
            std::lock_guard<std::mutex> lk(cmds_mutex_);
            cmds_.clear();
        }
        ::PostMessageW(hwnd_, kRedrawMsg, 0, 0);
        return true;
    }

    bool set_visible(bool show) {
        if (!ensure_started())
            return false;
        ::PostMessageW(hwnd_, show ? kShowMsg_() : kHideMsg_(), 0, 0);
        return true;
    }

    // 渲染入口：仅 overlay 线程调用。
    void render();

    HWND hwnd() const {
        return hwnd_;
    }

    std::vector<DrawCmd> snapshot() {
        std::lock_guard<std::mutex> lk(cmds_mutex_);
        return cmds_;
    }

    void set_hwnd(HWND hwnd) {
        hwnd_ = hwnd;
    }

    void mark_ready() {
        ready_.store(true, std::memory_order_release);
    }

    ULONG_PTR &gdi_token() {
        return gdi_token_;
    }

    static DrawOverlay &instance() {
        static DrawOverlay inst;
        return inst;
    }

  private:
    static UINT kShowMsg_() {
        return kRedrawMsg + 1;
    }
    static UINT kHideMsg_() {
        return kRedrawMsg + 2;
    }

    void start_thread();

    std::once_flag start_flag_;
    std::atomic<bool> ready_{false};
    HANDLE ready_event_ = nullptr;
    HWND hwnd_ = nullptr;
    std::mutex cmds_mutex_;
    std::vector<DrawCmd> cmds_;
    ULONG_PTR gdi_token_ = 0;
};

// 线程消息循环收到的重绘/显隐在窗口过程里处理。
LRESULT CALLBACK DrawOverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kRedrawMsg) {
        DrawOverlay::instance().render();
        return 0;
    }
    if (msg == kRedrawMsg + 1) {
        ::ShowWindow(hwnd, SW_SHOW);
        return 0;
    }
    if (msg == kRedrawMsg + 2) {
        ::ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void DrawOverlay::start_thread() {
    ready_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread([] {
        auto &self = DrawOverlay::instance();
        auto signal_ready = [&self] {
            if (self.ready_event_)
                ::SetEvent(self.ready_event_);
        };
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&self.gdi_token(), &input, nullptr) != Gdiplus::Ok) {
            signal_ready();
            return;
        }

        const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HINSTANCE inst = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DrawOverlayWndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"OpDrawOverlayW";
        ::RegisterClassExW(&wc);

        // 分层 + 穿透点击 + 置顶 + 不抢焦点
        const DWORD ex_style = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        HWND hwnd = ::CreateWindowExW(ex_style, wc.lpszClassName, L"OpDrawOverlay", WS_POPUP, vx, vy, vw, vh, nullptr,
                                      nullptr, inst, nullptr);
        if (!hwnd) {
            signal_ready();
            Gdiplus::GdiplusShutdown(self.gdi_token());
            return;
        }
        self.set_hwnd(hwnd);
        self.mark_ready();
        signal_ready();
        ::ShowWindow(hwnd, SW_SHOW);

        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        Gdiplus::GdiplusShutdown(self.gdi_token());
    }).detach();
}

void DrawOverlay::render() {
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0 || !hwnd_)
        return;

    // GDI+ 渲到 ARGB 位图
    Gdiplus::Bitmap bmp(vw, vh, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const auto cmds = snapshot();
    for (const auto &cmd : cmds) {
        const Gdiplus::Color color(cmd.color);
        if (cmd.type == DrawCmdType::Rect) {
            Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(cmd.width < 1 ? 1 : cmd.width));
            const int x = static_cast<int>(cmd.x1 - vx);
            const int y = static_cast<int>(cmd.y1 - vy);
            const int w = static_cast<int>(cmd.x2 - cmd.x1);
            const int h = static_cast<int>(cmd.y2 - cmd.y1);
            g.DrawRectangle(&pen, x, y, w, h);
        } else if (cmd.type == DrawCmdType::Line) {
            Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(cmd.width < 1 ? 1 : cmd.width));
            g.DrawLine(&pen, static_cast<INT>(cmd.x1 - vx), static_cast<INT>(cmd.y1 - vy),
                       static_cast<INT>(cmd.x2 - vx), static_cast<INT>(cmd.y2 - vy));
        } else {
            Gdiplus::Font font(L"Microsoft YaHei", static_cast<Gdiplus::REAL>(cmd.font_size > 0 ? cmd.font_size : 14),
                               Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush brush(color);
            g.DrawString(cmd.text.c_str(), static_cast<INT>(cmd.text.size()), &font,
                         Gdiplus::PointF(static_cast<Gdiplus::REAL>(cmd.x1 - vx), static_cast<Gdiplus::REAL>(cmd.y1 - vy)),
                         &brush);
        }
    }

    // ARGB 位图拷到 DIB 供 UpdateLayeredWindow
    Gdiplus::Rect rect(0, 0, vw, vh);
    Gdiplus::BitmapData bd = {};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC screen_dc = ::GetDC(nullptr);
    HDC mem_dc = ::CreateCompatibleDC(screen_dc);
    HBITMAP dib = ::CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        // ARGB 32bpp 与 DIB BGRA 32bpp 内存布局一致（小端），直接整块拷贝
        const size_t row_bytes = static_cast<size_t>(vw) * 4;
        if (bd.Stride == static_cast<INT>(row_bytes)) {
            memcpy(bits, bd.Scan0, row_bytes * static_cast<size_t>(vh));
        } else {
            for (int y = 0; y < vh; ++y)
                memcpy(static_cast<char *>(bits) + row_bytes * y,
                       static_cast<const char *>(bd.Scan0) + bd.Stride * y, row_bytes);
        }
        HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem_dc, dib));
        POINT dst = {vx, vy};
        POINT src = {0, 0};
        SIZE size = {vw, vh};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(hwnd_, screen_dc, &dst, &size, mem_dc, &src, 0, &blend, ULW_ALPHA);
        ::SelectObject(mem_dc, old);
    }
    if (dib)
        ::DeleteObject(dib);
    ::DeleteDC(mem_dc);
    ::ReleaseDC(nullptr, screen_dc);
    bmp.UnlockBits(&bd);
}

} // namespace

void op::Op::DrawRect(long x1, long y1, long x2, long y2, const wchar_t *color, long width, long *ret) {
    internal::set_result(ret, 0L);
    DrawCmd cmd;
    cmd.type = DrawCmdType::Rect;
    cmd.x1 = x1;
    cmd.y1 = y1;
    cmd.x2 = x2;
    cmd.y2 = y2;
    cmd.width = width;
    if (!parse_draw_color(color, cmd.color)) {
        setlog(L"DrawRect: 颜色非法 \"%s\"（应为 RRGGBB）", color ? color : L"");
        return;
    }
    if (DrawOverlay::instance().add(std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawLine(long x1, long y1, long x2, long y2, const wchar_t *color, long width, long *ret) {
    internal::set_result(ret, 0L);
    DrawCmd cmd;
    cmd.type = DrawCmdType::Line;
    cmd.x1 = x1;
    cmd.y1 = y1;
    cmd.x2 = x2;
    cmd.y2 = y2;
    cmd.width = width;
    if (!parse_draw_color(color, cmd.color)) {
        setlog(L"DrawLine: 颜色非法 \"%s\"（应为 RRGGBB）", color ? color : L"");
        return;
    }
    if (DrawOverlay::instance().add(std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawText(long x, long y, const wchar_t *text, const wchar_t *color, long font_size, long *ret) {
    internal::set_result(ret, 0L);
    if (!text || !text[0])
        return;
    DrawCmd cmd;
    cmd.type = DrawCmdType::Text;
    cmd.x1 = x;
    cmd.y1 = y;
    cmd.font_size = font_size;
    cmd.text = text;
    if (!parse_draw_color(color, cmd.color)) {
        setlog(L"DrawText: 颜色非法 \"%s\"（应为 RRGGBB）", color ? color : L"");
        return;
    }
    if (DrawOverlay::instance().add(std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawClear(long *ret) {
    internal::set_result(ret, DrawOverlay::instance().clear() ? 1L : 0L);
}

void op::Op::DrawSetVisible(long show, long *ret) {
    internal::set_result(ret, DrawOverlay::instance().set_visible(show != 0) ? 1L : 0L);
}
