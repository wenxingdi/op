// DPI 上下文一致性测试（批 62）：DPI 不感知的目标窗口上，区域/坐标必须与截帧同口径。
//
// 背景（2026-11-23 实测）：目标进程 DPI 不感知（VNC 查看器 / 老程序 / 部分游戏）而宿主
// DPI aware 时，跨进程 GetClientRect 返回**物理像素**，而窗口 DC 的截帧是**逻辑像素**，
// 150% 缩放下差 1.5 倍。op 的区域/找图/OCR 全部以帧像素为坐标空间，所以窗口度量与
// 屏幕↔客户区换算必须切到目标窗口自己的 DPI 上下文，否则：
//   · 框选回填的坐标整体偏大 1.5 倍（区域截到别处）
//   · GetClientSize 报 606x421 而帧只有 404x281（越界判定失效、越界读垃圾）
//
// 为什么必须跨进程：SetThreadDpiAwarenessContext 只能降级、不能把 unaware 线程提升为
// aware，而「aware 宿主 + unaware 目标」这个组合要求宿主与目标处于不同进程
// （窗口的 awareness 由创建它的线程决定），同进程造不出有判别力的条件。
// 实测（scripts/probes/_probe_dpi_xproc.py，150% 环境）：
//   aware 宿主跨进程 GetClientRect = 300x225（物理）
//   同一线程切 UNAWARE 上下文后    = 200x150（逻辑）
//   宿主/窗口像素比 = 1.5
//
// 基准怎么定（很关键）：Windows 的 ClientToScreen/ScreenToClient 对 DPI 不感知目标
// **只把起点换成物理坐标，增量不缩放**（实测 ClientToScreen(30,30) = 原点+30 而非
// 原点+45）。所以原生 API 的返回值不能当"物理坐标"基准，否则断言会跟着错。
// 本文件改用**物理屏幕像素扫描**建立独立基准：直接读屏幕找红块的实际物理位置，
// 再要求 op 的坐标换算必须命中它。修复前 op 会偏 15px（物理），判别力足够。

#include "op_c_api.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

// 靶子：客户区 200x150，左上角 (10,10)-(50,50) 画纯红块。
constexpr int kClientW = 200;
constexpr int kClientH = 150;
constexpr int kRedLeft = 10;
constexpr int kRedTop = 10;
constexpr int kRedRight = 50;
constexpr int kRedBottom = 50;
constexpr int kRedCenterX = (kRedLeft + kRedRight) / 2;
constexpr int kRedCenterY = (kRedTop + kRedBottom) / 2;
// 取色判据点：红块内部深处（避开边缘插值）。
constexpr int kProbeX = 20;
constexpr int kProbeY = 20;

// RAII：临时把当前线程切到指定 DPI 上下文，模拟 DPI-aware 宿主（OPTool 就是 PerMonitorV2）。
class ThreadDpiScope {
  public:
    explicit ThreadDpiScope(DPI_AWARENESS_CONTEXT context)
        : previous_(::SetThreadDpiAwarenessContext(context)), ok_(previous_ != nullptr) {
    }
    ~ThreadDpiScope() {
        if (ok_)
            ::SetThreadDpiAwarenessContext(previous_);
    }
    ThreadDpiScope(const ThreadDpiScope &) = delete;
    ThreadDpiScope &operator=(const ThreadDpiScope &) = delete;
    bool ok() const {
        return ok_;
    }

  private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
    bool ok_ = false;
};

// 宿主与目标窗口像素比 = 1 时（100% 缩放）没有可判别差异，跳过而不是给假绿。
bool system_has_dpi_scaling() {
    const ThreadDpiScope scope(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return ::GetDpiForSystem() != 96;
}

// 物理屏幕像素扫描：在绑定窗口客户区原点附近找红块，返回它的物理中心。
// 这是**独立于 op 换算逻辑**的物理基准 —— 修复前 op 的坐标会偏 15px 以上。
bool find_red_block_center(HWND hwnd, POINT &center, RECT *found = nullptr) {
    POINT origin{0, 0};
    if (!::ClientToScreen(hwnd, &origin))
        return false;

    // 一次性把客户区原点附近抓成位图再在内存里扫描：
    // GetPixel 逐点读屏幕 DC 在某些显示驱动上极慢（12321 次曾把用例拖到分钟级）。
    const int scan_w = 111;
    const int scan_h = 111;
    HDC screen = ::GetDC(nullptr);
    if (!screen)
        return false;
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = mem ? ::CreateCompatibleBitmap(screen, scan_w, scan_h) : nullptr;
    if (!mem || !bmp) {
        if (mem)
            ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
        return false;
    }
    const HGDIOBJ old = ::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, scan_w, scan_h, screen, origin.x, origin.y, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = scan_w;
    bi.bmiHeader.biHeight = -scan_h; // 负高 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(scan_w) * scan_h * 4);
    const int copied = ::GetDIBits(mem, bmp, 0, scan_h, pixels.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    if (copied == 0)
        return false;

    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    for (int y = 0; y < scan_h; ++y) {
        for (int x = 0; x < scan_w; ++x) {
            const unsigned char *px = &pixels[(static_cast<size_t>(y) * scan_w + x) * 4];
            // 32bpp BI_RGB 的字节序是 BGRA
            const int b = px[0];
            const int g = px[1];
            const int r = px[2];
            if (r < 200 || g > 80 || b > 80)
                continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x)
        return false;
    center.x = origin.x + (min_x + max_x) / 2;
    center.y = origin.y + (min_y + max_y) / 2;
    if (found) {
        found->left = origin.x + min_x;
        found->top = origin.y + min_y;
        found->right = origin.x + max_x;
        found->bottom = origin.y + max_y;
    }
    printf("[diag scan] 原点(%d,%d) 红块 x[%d..%d] y[%d..%d] 中心(%d,%d)\n", origin.x, origin.y, min_x, max_x,
           min_y, max_y, center.x, center.y);
    fflush(stdout);
    return true;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// 子进程端：造 DPI 不感知 / DPI 感知的目标窗口
// ════════════════════════════════════════════════════════════════════════════
namespace op_dpi_e2e {

namespace {

LRESULT CALLBACK TargetWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT client{};
        ::GetClientRect(hwnd, &client);
        HBRUSH white = ::CreateSolidBrush(RGB(255, 255, 255));
        ::FillRect(dc, &client, white);
        ::DeleteObject(white);
        HBRUSH red = ::CreateSolidBrush(RGB(255, 0, 0));
        RECT red_rect{kRedLeft, kRedTop, kRedRight, kRedBottom};
        ::FillRect(dc, &red_rect, red);
        ::DeleteObject(red);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

// 把 awareness 上下文翻译成可读标签（伪句柄不能直接比较，必须用
// AreDpiAwarenessContextsEqual 判定）。
std::string awareness_label(const void *ctx) {
    using are_equal_t = BOOL(WINAPI *)(void *, void *);
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const auto are_equal = reinterpret_cast<are_equal_t>(::GetProcAddress(user32, "AreDpiAwarenessContextsEqual"));
    if (!are_equal || !ctx)
        return "unknown";
    void *handle = const_cast<void *>(ctx);
    if (are_equal(handle, reinterpret_cast<void *>(static_cast<intptr_t>(-1))))
        return "UNAWARE";
    if (are_equal(handle, reinterpret_cast<void *>(static_cast<intptr_t>(-2))))
        return "SYSTEM_AWARE";
    if (are_equal(handle, reinterpret_cast<void *>(static_cast<intptr_t>(-3))))
        return "PER_MONITOR_AWARE";
    if (are_equal(handle, reinterpret_cast<void *>(static_cast<intptr_t>(-4))))
        return "PER_MONITOR_AWARE_V2";
    return "other";
}

bool write_info(const char *path, HWND hwnd) {
    std::FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    // 附带 awareness 诊断：确认子进程确实处于预期的 DPI 上下文。
    using get_ctx_t = void *(WINAPI *)(void);
    using get_win_ctx_t = void *(WINAPI *)(HWND);
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const auto get_thread =
        reinterpret_cast<get_ctx_t>(::GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
    const auto get_window =
        reinterpret_cast<get_win_ctx_t>(::GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
    const void *thread_ctx = get_thread ? get_thread() : nullptr;
    const void *window_ctx = get_window ? get_window(hwnd) : nullptr;
    std::fprintf(f, "hwnd=%llu\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
    std::fprintf(f, "pid=%lu\n", static_cast<unsigned long>(::GetCurrentProcessId()));
    std::fprintf(f, "thread_ctx=%s\n", awareness_label(thread_ctx).c_str());
    std::fprintf(f, "window_ctx=%s\n", awareness_label(window_ctx).c_str());
    std::fprintf(f, "dpi=%d\n", static_cast<int>(::GetDpiForWindow(hwnd)));
    std::fprintf(f, "process_dpi_aware=%d\n", static_cast<int>(::IsProcessDPIAware()));
    std::fclose(f);
    return true;
}

// 共同实现：建窗 → 写 info 文件 → 消息循环（等父进程 WM_CLOSE）。
// 本进程**完全不碰任何 DPI API** —— 保持系统默认的 DPI 不感知，
// 这正是要复现的目标形态（VNC 查看器 / 老程序 / 部分游戏）。
//
// ⚠ 这里原本还有一个「DPI 感知对照组」子进程（进程级 PMv2），现已删除：
//   本机 Win11 24H2 上 user32.SetProcessDpiAwarenessContext 对 PER_MONITOR_AWARE /
//   PER_MONITOR_AWARE_V2 直接返回 E_INVALIDARG(87)，shcore.dll 又不再导出该函数，
//   进程级 PMv2 只能靠 manifest 声明 —— 造不出真正的 aware 目标窗口。
//   零回归改由同进程 aware 窗口用例覆盖（等价代码路径）。
int run_child(const char *info_path) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TargetWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"op_dpi_e2e_unaware_target";
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    ::RegisterClassExW(&wc);

    RECT rc{0, 0, kClientW, kClientH};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME; // 无厚边框，客户区换算稳定
    ::AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"op_dpi_e2e_target", style, 260, 200,
                                   rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        write_info(info_path, nullptr);
        return 2;
    }
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(hwnd);
    ::SetForegroundWindow(hwnd);

    if (!write_info(info_path, hwnd))
        return 3;

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}

} // namespace

// DPI 不感知目标（模拟 VNC 查看器 / 老程序 / 部分游戏）。
int run_target(const char *info_path) {
    return run_child(info_path);
}

} // namespace op_dpi_e2e

// ════════════════════════════════════════════════════════════════════════════
// 父进程端：拉起子进程并拿到目标窗口
// ════════════════════════════════════════════════════════════════════════════
namespace {

class ChildTargetWindow {
  public:
    ~ChildTargetWindow() {
        stop();
    }
    ChildTargetWindow() = default;
    ChildTargetWindow(const ChildTargetWindow &) = delete;
    ChildTargetWindow &operator=(const ChildTargetWindow &) = delete;

    // 拉起 DPI 不感知目标子进程。
    bool start() {
        wchar_t exe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);

        // info 文件放 workbench/tmp（cwd=仓库根），避免测试中断时在仓库根留垃圾
        info_path_ = std::wstring(L"workbench/tmp/op_dpi_e2e_") + std::to_wstring(::GetCurrentProcessId()) +
                     L"_unaware.txt";
        ::DeleteFileW(info_path_.c_str());

        std::wstring cmd = L"\"";
        cmd += exe;
        cmd += L"\" --dpi-target=";
        cmd += info_path_;
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(exe, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            return false;
        process_ = pi;

        // 子进程建窗 + 写文件需要一点时间，轮询等待（最长 10 秒）。
        for (int i = 0; i < 200; ++i) {
            if (read_info())
                return true;
            ::Sleep(50);
        }
        stop();
        return false;
    }

    HWND hwnd() const {
        return hwnd_;
    }

    // 子进程上报的诊断字段（awareness / dpi 等）
    const std::string &info(const std::string &key) const {
        static const std::string empty;
        auto it = info_.find(key);
        return it == info_.end() ? empty : it->second;
    }

    void stop() {
        if (hwnd_) {
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            hwnd_ = nullptr;
        }
        if (process_.hProcess) {
            ::WaitForSingleObject(process_.hProcess, 5000);
            if (::WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT)
                ::TerminateProcess(process_.hProcess, 0);
            ::CloseHandle(process_.hThread);
            ::CloseHandle(process_.hProcess);
            process_ = PROCESS_INFORMATION{};
        }
        if (!info_path_.empty()) {
            ::DeleteFileW(info_path_.c_str());
            info_path_.clear();
        }
    }

  private:
    bool read_info() {
        std::ifstream in(info_path_, std::ios::binary);
        if (!in)
            return false;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        while (pos < content.size()) {
            const size_t eol = content.find('\n', pos);
            const std::string line =
                content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                info_[line.substr(0, eq)] = line.substr(eq + 1);
            if (eol == std::string::npos)
                break;
            pos = eol + 1;
        }
        const auto it = info_.find("hwnd");
        if (it == info_.end())
            return false;
        const uintptr_t value = std::strtoull(it->second.c_str(), nullptr, 10);
        if (value == 0)
            return false; // 子进程建窗失败
        hwnd_ = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
        return true;
    }

    std::map<std::string, std::string> info_;
    std::wstring info_path_;
    PROCESS_INFORMATION process_{};
    HWND hwnd_ = nullptr;
};

// 坐标换算的公共断言：帧坐标 ↔ 物理屏幕坐标必须双向自洽。
// red_center 是**扫描得到的**红块物理中心（独立基准）。
void expect_frame_coord_matches_physical(op_handle handle, HWND hwnd, POINT red_center, const char *tag) {
    int fx = kRedCenterX;
    int fy = kRedCenterY;
    ASSERT_EQ(1, OpClientToScreen(handle, reinterpret_cast<intptr_t>(hwnd), &fx, &fy));
    printf("[diag %s] 帧(%d,%d) → 物理(%d,%d)；扫描到的红块物理中心(%d,%d)\n", tag, kRedCenterX, kRedCenterY, fx,
           fy, red_center.x, red_center.y);
    fflush(stdout);
    EXPECT_NEAR(red_center.x, fx, 3) << "ClientToScreen 必须给出红块的物理屏幕位置";
    EXPECT_NEAR(red_center.y, fy, 3);

    int bx = red_center.x;
    int by = red_center.y;
    ASSERT_EQ(1, OpScreenToClient(handle, reinterpret_cast<intptr_t>(hwnd), &bx, &by));
    // ±1 容差：物理像素 → 帧像素的除法有舍入，且扫描出来的红块边界本身有 1px 插值误差。
    EXPECT_NEAR(kRedCenterX, bx, 1) << "ScreenToClient 必须输出帧口径客户区坐标（修复前会偏 1.5 倍）";
    EXPECT_NEAR(kRedCenterY, by, 1);
}

} // namespace

// ── 用例 1：跨进程 DPI 不感知目标 —— 坐标与帧必须同口径 ────────────────────
TEST(DpiContextTest, UnawareCrossProcessTargetFrameCoordMatchesScreenPick) {
    if (!system_has_dpi_scaling())
        GTEST_SKIP() << "系统 DPI=96（100% 缩放），不存在物理/逻辑差异，用例无判别力";

    // 宿主线程切到 PerMonitorV2（与 OPTool 一致）；进程级设置不动，避免影响其他用例。
    const ThreadDpiScope host(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ASSERT_TRUE(host.ok()) << "线程切 PMv2 失败";

    ChildTargetWindow target;
    ASSERT_TRUE(target.start()) << "拉起 DPI 不感知子进程失败";
    HWND hwnd = target.hwnd();
    ASSERT_NE(hwnd, nullptr);
    printf("[diag unaware] thread_ctx=%s window_ctx=%s dpi=%s\n", target.info("thread_ctx").c_str(),
           target.info("window_ctx").c_str(), target.info("dpi").c_str());
    fflush(stdout);

    op_handle handle = OpCreate();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(1, OpBindWindow(handle, reinterpret_cast<intptr_t>(hwnd), L"gdi", L"normal", L"normal", 0));
    int cw = 0, ch = 0;
    ASSERT_EQ(1, OpGetClientSize(handle, reinterpret_cast<intptr_t>(hwnd), &cw, &ch));
    EXPECT_EQ(kClientW, cw) << "GetClientSize 必须是帧口径（窗口自身上下文），修复前会报物理尺寸";
    EXPECT_EQ(kClientH, ch);

    // ② 建立独立的物理基准：扫描屏幕找红块的实际物理位置
    POINT red_center{};
    RECT red_rect{};
    ASSERT_TRUE(find_red_block_center(hwnd, red_center, &red_rect))
        << "屏幕上没扫到红块（子进程窗口可能被遮挡）";

    // ③ 坐标换算必须命中该物理位置
    expect_frame_coord_matches_physical(handle, hwnd, red_center, "unaware");

    // ④ 帧坐标取色必须命中红块（证明帧坐标与截帧像素同空间）
    const wchar_t *color = OpGetColor(handle, kProbeX, kProbeY);
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(0, _wcsicmp(color, L"FF0000")) << "帧坐标取色 " << kProbeX << "," << kProbeY << " 得到 " << color
                                             << "，不是红块 FF0000 → 区域整体错位";

    // ⑤ 窗口缩放比必须报实测比例 1.50（GetDpiForWindow 对 unaware 窗口恒返 96，
    //    只按 DPI 推算会误报 1.00）
    const wchar_t *scale = OpGetWindowScale(handle, reinterpret_cast<intptr_t>(hwnd));
    ASSERT_NE(scale, nullptr);
    EXPECT_EQ(0, wcscmp(L"1.50", scale)) << "窗口缩放比应是实测比例 1.50，实际 " << scale;

    OpUnBindWindow(handle);
    OpDestroy(handle);
}

// ── 用例 2：反向一致性 —— ClientToScreen 必须回到宿主视角的物理屏幕坐标 ──────
TEST(DpiContextTest, UnawareCrossProcessClientToScreenRoundTrip) {
    if (!system_has_dpi_scaling())
        GTEST_SKIP() << "系统 DPI=96（100% 缩放），不存在物理/逻辑差异，用例无判别力";

    const ThreadDpiScope host(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ASSERT_TRUE(host.ok()) << "线程切 PMv2 失败";

    ChildTargetWindow target;
    ASSERT_TRUE(target.start()) << "拉起 DPI 不感知子进程失败";
    HWND hwnd = target.hwnd();
    ASSERT_NE(hwnd, nullptr);

    op_handle handle = OpCreate();
    ASSERT_NE(handle, nullptr);

    POINT red_center{};
    ASSERT_TRUE(find_red_block_center(hwnd, red_center)) << "屏幕上没扫到红块（子进程窗口可能被遮挡）";

    // 帧坐标 → 物理 → 帧坐标，必须回到原点
    int x = kRedCenterX;
    int y = kRedCenterY;
    ASSERT_EQ(1, OpClientToScreen(handle, reinterpret_cast<intptr_t>(hwnd), &x, &y));
    EXPECT_NEAR(red_center.x, x, 3) << "ClientToScreen 输出必须是红块的物理屏幕坐标";
    EXPECT_NEAR(red_center.y, y, 3);

    ASSERT_EQ(1, OpScreenToClient(handle, reinterpret_cast<intptr_t>(hwnd), &x, &y));
    EXPECT_NEAR(kRedCenterX, x, 1);
    EXPECT_NEAR(kRedCenterY, y, 1);

    OpDestroy(handle);
}

// ── 用例 3：零回归 —— 同进程 DPI 感知窗口，坐标换算与原生 API 完全一致 ────────
// 为什么用同进程而不是跨进程 aware 目标：本机 Win11 24H2 上进程级 PMv2 已无法通过
// API 设置（E_INVALIDARG），只能靠 manifest，无法在测试里现造一个 aware 的跨进程目标。
// 同进程 aware 窗口走的是「宿主与目标同 DPI 上下文」这条路径 —— 修复前后 op 的
// ScreenToClient/ClientToScreen 都必须与原生 API 逐像素一致，这就是零回归判据。
TEST(DpiContextTest, AwareSameProcessCoordMatchesNativeApi) {
    const ThreadDpiScope host(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ASSERT_TRUE(host.ok()) << "线程切 PMv2 失败";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM w, LPARAM l) -> LRESULT {
        return ::DefWindowProcW(hwnd, msg, w, l);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"op_dpi_aware_coord_target";
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    ASSERT_TRUE(::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS);

    RECT rc{0, 0, kClientW, kClientH};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME;
    ::AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"op_dpi_aware_coord", style, 260, 200,
                                  rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    op_handle handle = OpCreate();
    ASSERT_NE(handle, nullptr);

    // 原生基准：同进程 aware 窗口无 DPI 虚拟化，原生 ClientToScreen 就是物理坐标
    POINT native{kRedCenterX, kRedCenterY};
    ASSERT_TRUE(::ClientToScreen(hwnd, &native) != FALSE);

    int x = kRedCenterX;
    int y = kRedCenterY;
    ASSERT_EQ(1, OpClientToScreen(handle, reinterpret_cast<intptr_t>(hwnd), &x, &y));
    EXPECT_EQ(native.x, x) << "aware 目标上 ClientToScreen 必须与原生一致";
    EXPECT_EQ(native.y, y);

    ASSERT_EQ(1, OpScreenToClient(handle, reinterpret_cast<intptr_t>(hwnd), &x, &y));
    EXPECT_EQ(kRedCenterX, x);
    EXPECT_EQ(kRedCenterY, y);

    // 客户区尺寸必须与原生 GetClientRect 一致
    RECT measured{};
    ::GetClientRect(hwnd, &measured);
    int cw = 0, ch = 0;
    ASSERT_EQ(1, OpGetClientSize(handle, reinterpret_cast<intptr_t>(hwnd), &cw, &ch));
    EXPECT_EQ(measured.right - measured.left, cw);
    EXPECT_EQ(measured.bottom - measured.top, ch);

    OpDestroy(handle);
    ::DestroyWindow(hwnd);
}

// ── 用例 4：同进程 DPI 感知窗口 —— 缩放比走 DPI 推算分支，数值不变 ──────────
TEST(DpiContextTest, AwareSameProcessScaleMatchesDpiRatio) {
    const ThreadDpiScope host(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ASSERT_TRUE(host.ok());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM w, LPARAM l) -> LRESULT {
        return ::DefWindowProcW(hwnd, msg, w, l);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"op_dpi_aware_same_process";
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    ASSERT_TRUE(::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS);

    RECT rc{0, 0, kClientW, kClientH};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME;
    ::AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"op_dpi_aware", style, 260, 200, rc.right - rc.left,
                                  rc.bottom - rc.top, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    op_handle handle = OpCreate();
    ASSERT_NE(handle, nullptr);

    // 独立期望值：DPI 感知窗口的缩放就是所在显示器 DPI / 96
    wchar_t expected[16] = {};
    swprintf_s(expected, _countof(expected), L"%.2f", ::GetDpiForWindow(hwnd) / 96.0);
    const wchar_t *scale = OpGetWindowScale(handle, reinterpret_cast<intptr_t>(hwnd));
    ASSERT_NE(scale, nullptr);
    EXPECT_EQ(0, wcscmp(expected, scale)) << "期望 " << expected << "，实际 " << scale;

    // 同上下文的窗口，客户区尺寸用哪种方式量都一样
    RECT measured{};
    ::GetClientRect(hwnd, &measured);
    int width = 0, height = 0;
    ASSERT_EQ(1, OpGetClientSize(handle, reinterpret_cast<intptr_t>(hwnd), &width, &height));
    EXPECT_EQ(measured.right - measured.left, width);
    EXPECT_EQ(measured.bottom - measured.top, height);

    OpDestroy(handle);
    ::DestroyWindow(hwnd);
}
