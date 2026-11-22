// OpDraw.cpp: 屏幕透明叠加层绘制（P2 新增 2026-10-15，参考 OLA DrawGui 最小组）。
// 进程级单例：专用线程 + 全屏分层窗口（WS_EX_LAYERED|TRANSPARENT|TOPMOST，穿透点击），
// GDI+ 渲到 32bpp ARGB 位图后经 UpdateLayeredWindow 上屏。
// 定位：调试可视化（找图结果标注），不追求高帧率——图元变更才重绘。
#include "OpContext.h"
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

enum class DrawCmdType { Rect, Line, Text, Circle, Image };

struct DrawCmd {
    DrawCmdType type;
    long x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    long width = 1;
    long font_size = 14;
    DWORD color = 0xFFFFFFFF; // 0xAABBGGRR（GDI+ ARGB）
    std::wstring text;
    // Circle：半径放在 radius；Image：文件路径放在 text，alpha(0~255) 放在 width。
    long radius = 0;
    BYTE alpha = 255;
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
        } else if (cmd.type == DrawCmdType::Circle) {
            const Gdiplus::REAL r = static_cast<Gdiplus::REAL>(cmd.radius < 1 ? 1 : cmd.radius);
            const Gdiplus::REAL cx = static_cast<Gdiplus::REAL>(cmd.x1 - vx);
            const Gdiplus::REAL cy = static_cast<Gdiplus::REAL>(cmd.y1 - vy);
            Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(cmd.width < 1 ? 1 : cmd.width));
            g.DrawEllipse(&pen, cx - r, cy - r, r * 2.0f, r * 2.0f);
        } else if (cmd.type == DrawCmdType::Image) {
            // 路径加载失败时静默跳过（叠加层不该因为一张图不存在就整体不画）。
            // GDI+ 的 Image 会锁文件；这里的用法是"画完即弃"，不需要长期持有。
            Gdiplus::Image img(cmd.text.c_str());
            if (img.GetLastStatus() == Gdiplus::Ok) {
                // GDI+ 的 ColorMatrix 是 REAL m[5][5]，没有 m00/m11 成员
                Gdiplus::ColorMatrix cm = {};
                for (int i = 0; i < 5; ++i)
                    cm.m[i][i] = 1.0f;
                // alpha 走矩阵第 5 行第 5 列：0 = 全透明，1 = 原样
                cm.m[3][3] = static_cast<Gdiplus::REAL>(cmd.alpha) / 255.0f;
                Gdiplus::ImageAttributes attrs;
                attrs.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                const Gdiplus::Rect dst(static_cast<INT>(cmd.x1 - vx), static_cast<INT>(cmd.y1 - vy),
                                        static_cast<INT>(img.GetWidth()), static_cast<INT>(img.GetHeight()));
                g.DrawImage(&img, dst, 0, 0, static_cast<INT>(img.GetWidth()), static_cast<INT>(img.GetHeight()),
                            Gdiplus::UnitPixel, &attrs);
            }
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


// ---- 绑定窗口内绘制（批 B 2026-11-22）----
// 与屏幕叠加层的区别（照 OLA DrawGui*Ex 的语义）：
//   * 坐标是**绑定窗口客户区**（与找色/鼠标同一口径），不是屏幕坐标；
//   * 画在一块独立透明窗口上，**不写目标 DX 后台缓冲** -> 不进 DX 截图；
//   * 窗口移动/缩放时跟着走（定时跟随 + 每次绘制时校正）。
// 实现选择：不做 DWM Thumbnail / 不注入，只用一个 WS_EX_LAYERED|TRANSPARENT 的
// 子覆盖窗口 + 轮询跟随。理由是它不依赖目标进程、不改变截图语义，失败模式温和。
class BindOverlay {
  public:
    bool ensure_started() {
        std::call_once(start_flag_, [this] { start_thread(); });
        if (ready_.load(std::memory_order_acquire))
            return true;
        if (ready_event_) {
            ::WaitForSingleObject(ready_event_, 5000);
            ::CloseHandle(ready_event_);
            ready_event_ = nullptr;
        }
        return ready_.load(std::memory_order_acquire);
    }

    // hwnd = 绑定窗口。追加图元并触发重绘。
    bool add(HWND target, DrawCmd cmd) {
        if (!ensure_started() || !target)
            return false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            target_ = target;
            cmds_.push_back(std::move(cmd));
        }
        ::PostMessageW(hwnd_, kRedrawMsg, 0, 0);
        return true;
    }

    bool clear() {
        if (!ensure_started())
            return false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cmds_.clear();
        }
        ::PostMessageW(hwnd_, kRedrawMsg, 0, 0);
        return true;
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mutex_);
        return cmds_.size();
    }

    void render();
    HWND hwnd() const {
        return hwnd_;
    }
    void set_hwnd(HWND h) {
        hwnd_ = h;
    }
    void mark_ready(ULONG_PTR token) {
        gdi_token_ = token;
        ready_.store(true, std::memory_order_release);
    }
    HANDLE ready_event() {
        return ready_event_;
    }
    void set_ready_event(HANDLE h) {
        ready_event_ = h;
    }

    static BindOverlay &instance() {
        static BindOverlay inst;
        return inst;
    }

  private:
    void start_thread();
    std::once_flag start_flag_;
    std::atomic<bool> ready_{false};
    HANDLE ready_event_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND target_ = nullptr;
    std::mutex mutex_;
    std::vector<DrawCmd> cmds_;
    ULONG_PTR gdi_token_ = 0;
};

LRESULT CALLBACK BindOverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kRedrawMsg) {
        BindOverlay::instance().render();
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void BindOverlay::start_thread() {
    set_ready_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::thread([] {
        auto &self = BindOverlay::instance();
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            ::SetEvent(self.ready_event());
            return;
        }
        HINSTANCE inst = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BindOverlayWndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"OpDrawBindOverlayW";
        ::RegisterClassExW(&wc);
        const DWORD ex_style = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        HWND hwnd = ::CreateWindowExW(ex_style, wc.lpszClassName, L"OpDrawBind", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      inst, nullptr);
        if (!hwnd) {
            ::SetEvent(self.ready_event());
            Gdiplus::GdiplusShutdown(token);
            return;
        }
        self.set_hwnd(hwnd);
        self.mark_ready(token);
        ::SetEvent(self.ready_event());
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        // 跟随定时器：绑定窗口移动/缩放时把覆盖层挪过去（250ms 足够，开销可忽略）
        ::SetTimer(hwnd, 1, 250, nullptr);
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_TIMER && msg.hwnd == hwnd && msg.wParam == 1) {
                self.render(); // render 内部会先校正位置
                continue;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        Gdiplus::GdiplusShutdown(token);
    }).detach();
}

void BindOverlay::render() {
    std::vector<DrawCmd> cmds;
    HWND target = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cmds = cmds_;
        target = target_;
    }
    if (!hwnd_ || !target || !::IsWindow(target))
        return;

    RECT rc = {};
    ::GetClientRect(target, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0)
        return;
    // 客户区左上角在屏幕上的位置（含边框偏移）；窗口最小化时不可见
    POINT origin = {0, 0};
    ::ClientToScreen(target, &origin);
    if (::IsIconic(target)) {
        ::ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    Gdiplus::Bitmap bmp(cw, ch, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    for (const auto &cmd : cmds) {
        const Gdiplus::Color color(cmd.color);
        const Gdiplus::REAL lw = static_cast<Gdiplus::REAL>(cmd.width < 1 ? 1 : cmd.width);
        switch (cmd.type) {
        case DrawCmdType::Rect: {
            Gdiplus::Pen pen(color, lw);
            g.DrawRectangle(&pen, static_cast<INT>(cmd.x1), static_cast<INT>(cmd.y1), static_cast<INT>(cmd.x2 - cmd.x1),
                            static_cast<INT>(cmd.y2 - cmd.y1));
            break;
        }
        case DrawCmdType::Line: {
            Gdiplus::Pen pen(color, lw);
            g.DrawLine(&pen, static_cast<INT>(cmd.x1), static_cast<INT>(cmd.y1), static_cast<INT>(cmd.x2),
                       static_cast<INT>(cmd.y2));
            break;
        }
        case DrawCmdType::Circle: {
            const Gdiplus::REAL r = static_cast<Gdiplus::REAL>(cmd.radius < 1 ? 1 : cmd.radius);
            Gdiplus::Pen pen(color, lw);
            g.DrawEllipse(&pen, static_cast<Gdiplus::REAL>(cmd.x1) - r, static_cast<Gdiplus::REAL>(cmd.y1) - r, r * 2.0f,
                          r * 2.0f);
            break;
        }
        case DrawCmdType::Text: {
            Gdiplus::Font font(L"Microsoft YaHei",
                               static_cast<Gdiplus::REAL>(cmd.font_size > 0 ? cmd.font_size : 14),
                               Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush brush(color);
            g.DrawString(cmd.text.c_str(), static_cast<INT>(cmd.text.size()), &font,
                         Gdiplus::PointF(static_cast<Gdiplus::REAL>(cmd.x1), static_cast<Gdiplus::REAL>(cmd.y1)), &brush);
            break;
        }
        case DrawCmdType::Image: {
            Gdiplus::Image img(cmd.text.c_str());
            if (img.GetLastStatus() == Gdiplus::Ok) {
                Gdiplus::ColorMatrix cm = {};
                for (int i = 0; i < 5; ++i)
                    cm.m[i][i] = 1.0f;
                cm.m[3][3] = static_cast<Gdiplus::REAL>(cmd.alpha) / 255.0f;
                Gdiplus::ImageAttributes attrs;
                attrs.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                const Gdiplus::Rect dst(static_cast<INT>(cmd.x1), static_cast<INT>(cmd.y1),
                                        static_cast<INT>(img.GetWidth()), static_cast<INT>(img.GetHeight()));
                g.DrawImage(&img, dst, 0, 0, static_cast<INT>(img.GetWidth()), static_cast<INT>(img.GetHeight()),
                            Gdiplus::UnitPixel, &attrs);
            }
            break;
        }
        }
    }

    // 与屏幕层同一套上屏方式
    Gdiplus::Rect rect(0, 0, cw, ch);
    Gdiplus::BitmapData bd = {};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC screen_dc = ::GetDC(nullptr);
    HDC mem_dc = ::CreateCompatibleDC(screen_dc);
    HBITMAP dib = ::CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        const size_t row_bytes = static_cast<size_t>(cw) * 4;
        for (int y = 0; y < ch; ++y)
            memcpy(static_cast<char *>(bits) + row_bytes * y, static_cast<const char *>(bd.Scan0) + bd.Stride * y,
                   row_bytes);
        HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem_dc, dib));
        POINT dst = {origin.x, origin.y};
        POINT src = {0, 0};
        SIZE size = {cw, ch};
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

// ---- 批 B 新增图元与绑定窗口绘制（2026-11-22）----

void op::Op::DrawCircle(long x, long y, long radius, const wchar_t *color, long width, long *ret) {
    internal::set_result(ret, 0L);
    DrawCmd cmd;
    cmd.type = DrawCmdType::Circle;
    cmd.x1 = x;
    cmd.y1 = y;
    cmd.radius = radius;
    cmd.width = width;
    if (!parse_draw_color(color, cmd.color)) {
        setlog(L"DrawCircle: 颜色非法 \"%s\"（应为 RRGGBB）", color ? color : L"");
        return;
    }
    if (DrawOverlay::instance().add(std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawImage(long x, long y, const wchar_t *file_name, long alpha, long *ret) {
    internal::set_result(ret, 0L);
    if (!file_name || !file_name[0])
        return;
    DrawCmd cmd;
    cmd.type = DrawCmdType::Image;
    cmd.x1 = x;
    cmd.y1 = y;
    cmd.text = file_name;
    // alpha 语义：0~255；传 0 视为 255（"没填"比"全透明"更符合直觉，
    // 全透明在叠加层上没有可见效果，误用概率更高）。
    cmd.alpha = (alpha <= 0 || alpha > 255) ? 255 : static_cast<BYTE>(alpha);
    if (DrawOverlay::instance().add(std::move(cmd)))
        internal::set_result(ret, 1L);
}

// 绑定窗口内绘制：坐标为**客户区**，与找色/鼠标同口径。
// 未绑定窗口时显式失败（返回 0）——静默画到屏幕上会让人以为是坐标系问题。
bool op_bind_overlay_hwnd(op::internal::OpContext *ctx, HWND &hwnd) {
    if (!ctx)
        return false;
    // BindingSession 持有绑定窗口句柄；未绑定时返回 0
    HWND target = reinterpret_cast<HWND>(ctx->bkproc.GetBindWindow());
    if (!target || !::IsWindow(target))
        return false;
    hwnd = target;
    return true;
}

void op::Op::DrawRectEx(long x, long y, long width, long height, long mode, long line_width, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = nullptr;
    if (!op_bind_overlay_hwnd(m_context.get(), target)) {
        setlog(L"DrawRectEx: 未绑定窗口（请先 BindWindow）");
        return;
    }
    if (width <= 0 || height <= 0)
        return;
    DrawCmd cmd;
    // mode：0 填充 1 描边 2 填充+描边。当前图元只画描边，填充用 line_width 大值近似会失真，
    // 故 mode 0 也按描边处理，并在日志里说明（不假装支持填充）。
    if (mode == 0)
        setlog(L"DrawRectEx: mode=0(填充) 暂按描边绘制，填充待后续实现");
    cmd.type = DrawCmdType::Rect;
    cmd.x1 = x;
    cmd.y1 = y;
    cmd.x2 = x + width;
    cmd.y2 = y + height;
    cmd.width = line_width > 0 ? line_width : 1;
    cmd.color = 0xFFFFFFFF; // 默认白色，如需其它颜色用后续 DrawSetColor 接口（本批未加）
    if (BindOverlay::instance().add(target, std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawCircleEx(long x, long y, long radius, long line_width, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = nullptr;
    if (!op_bind_overlay_hwnd(m_context.get(), target)) {
        setlog(L"DrawCircleEx: 未绑定窗口");
        return;
    }
    DrawCmd cmd;
    cmd.type = DrawCmdType::Circle;
    cmd.x1 = x;
    cmd.y1 = y;
    cmd.radius = radius;
    cmd.width = line_width > 0 ? line_width : 1;
    if (BindOverlay::instance().add(target, std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawTextEx(long x, long y, const wchar_t *text, long font_size, long *ret) {
    internal::set_result(ret, 0L);
    HWND target = nullptr;
    if (!op_bind_overlay_hwnd(m_context.get(), target)) {
        setlog(L"DrawTextEx: 未绑定窗口");
        return;
    }
    if (!text || !text[0])
        return;
    DrawCmd cmd;
    cmd.type = DrawCmdType::Text;
    cmd.x1 = x;
    cmd.y1 = y;
    cmd.font_size = font_size > 0 ? font_size : 14;
    cmd.text = text;
    if (BindOverlay::instance().add(target, std::move(cmd)))
        internal::set_result(ret, 1L);
}

void op::Op::DrawClearBind(long *ret) {
    internal::set_result(ret, BindOverlay::instance().clear() ? 1L : 0L);
}

void op::Op::DrawGetCount(long *screen_count, long *bind_count) {
    internal::set_result(screen_count, static_cast<long>(DrawOverlay::instance().snapshot().size()));
    internal::set_result(bind_count, static_cast<long>(BindOverlay::instance().count()));
}
