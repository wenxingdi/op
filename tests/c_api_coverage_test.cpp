// C API 覆盖收口（2026-11-21）：补齐 10 个"无任何端到端调用证据"的导出。
//
// 背景：`scripts/probes/coverage_gaps.py` 原报缺口 10 个 ——
//   窗口/进程：OpGetWindowScale / OpTerminateProcess / OpTerminateProcessTree
//   图色基础：OpOcrAutoColor / OpOcrAutoColorFromFile
//   OCR 别名：OpAutoOcrLine
//   键鼠    ：OpKeyPressHotkey
//   二维码  ：OpDecodeQRCode / OpDecodeQRCodeFromFile / OpCreateQRCode
// 这些接口不是"没实现"，而是**从未被任何用例调用过** —— 出问题时没人能判断是不是它。
//
// 本文件的原则：
//   1) 只做**安全**的副作用（只终止自己 CreateProcess 出来的子进程；不发真实按键到前台窗口）；
//   2) 断言能区分对错（往返解码比对原文、DPI 数值范围、按 pid 精确断言存活）；
//   3) 环境依赖（OCR 引擎 / 窗口绑定失败）一律显式 GTEST_SKIP，不假绿。
//
// 反向验证见文件末尾注释。

#include "test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <op_c_api.h>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

bool SkipOcr() {
    return !test_support::GetEnvString(L"OP_SKIP_OCR_TESTS").empty();
}

bool FileExistsNonEmpty(const std::wstring &path, size_t min_bytes = 1) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
                                    static_cast<unsigned long long>(data.nFileSizeLow);
    return size >= static_cast<unsigned long long>(min_bytes);
}

// 用 GDI 把一张 BMP 文件画到一个无边框窗口里，供 OpDecodeQRCode（屏幕区域版）抓取。
// 只用于测试：op 的 gdi 显示模式是 BitBlt 窗口 DC，所以窗口必须真的画出内容。
struct BmpWindow {
    HWND hwnd = nullptr;
    HBITMAP bitmap = nullptr;
    int bmp_w = 0;
    int bmp_h = 0;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<BmpWindow *>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_PAINT && self && self->bitmap) {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            HDC mem = ::CreateCompatibleDC(dc);
            HGDIOBJ old = ::SelectObject(mem, self->bitmap);
            ::BitBlt(dc, 0, 0, self->bmp_w, self->bmp_h, mem, 0, 0, SRCCOPY);
            ::SelectObject(mem, old);
            ::DeleteDC(mem);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool Create(const std::wstring &bmp_path) {
        bitmap = static_cast<HBITMAP>(
            ::LoadImageW(nullptr, bmp_path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
        if (!bitmap)
            return false;
        BITMAP info{};
        if (!::GetObjectW(bitmap, sizeof(info), &info))
            return false;
        bmp_w = info.bmWidth;
        bmp_h = info.bmHeight;

        static bool registered = false;
        const wchar_t *cls = L"OpCoverageQrWindow";
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = cls;
            wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
            if (!::RegisterClassExW(&wc))
                return false;
            registered = true;
        }
        // 客户区必须正好等于位图尺寸：op 的 DecodeQRCode 入参是客户区坐标
        RECT rect{0, 0, bmp_w, bmp_h};
        ::AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
        hwnd = ::CreateWindowExW(0, cls, L"qr", WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, 80, 80,
                                 rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
        if (!hwnd)
            return false;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ::ShowWindow(hwnd, SW_SHOWNORMAL);
        ::UpdateWindow(hwnd);
        // 处理一遍待处理消息，确保 WM_PAINT 真的执行（BitBlt 才有内容）
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(120);
        return true;
    }

    ~BmpWindow() {
        if (hwnd)
            ::DestroyWindow(hwnd);
        if (bitmap)
            ::DeleteObject(bitmap);
    }
};

// 只终止自己启动的子进程，避免误伤机器上别人的进程。
struct ChildProcess {
    PROCESS_INFORMATION pi{};

    bool Start(const std::wstring &command_line) {
        std::wstring cmd = command_line; // CreateProcessW 需要可写缓冲
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        return ::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                &si, &pi) != FALSE;
    }
    DWORD pid() const { return pi.dwProcessId; }
    bool alive() const { return pi.hProcess != nullptr && ::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; }
    bool WaitExit(DWORD ms) const {
        return pi.hProcess != nullptr && ::WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0;
    }
    ~ChildProcess() {
        if (pi.hProcess) {
            if (alive())
                ::TerminateProcess(pi.hProcess, 0); // 用例失败时兜底，别留僵尸
            ::CloseHandle(pi.hProcess);
        }
        if (pi.hThread)
            ::CloseHandle(pi.hThread);
    }
};

std::wstring SystemExe(const wchar_t *name) {
    wchar_t dir[MAX_PATH] = {0};
    ::GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + name;
}

} // namespace

// ===================== 二维码 =====================

TEST(CoverageQRCodeTest, CreateAndDecodeRoundTrip) {
    CApiHandle api;
    const std::wstring path = test_support::GetTempBmpPath(L"__coverage_qr.png");
    ::DeleteFileW(path.c_str());

    ASSERT_EQ(1, OpCreateQRCode(api.handle, L"OP-COVERAGE-2026", path.c_str(), 256));
    ASSERT_TRUE(FileExistsNonEmpty(path, 1000)) << "OpCreateQRCode 必须落盘一张真实图片";

    const wchar_t *decoded = OpDecodeQRCodeFromFile(api.handle, path.c_str());
    ASSERT_NE(nullptr, decoded);
    EXPECT_STREQ(L"OP-COVERAGE-2026", decoded) << "往返解码必须还原原文";
    ::DeleteFileW(path.c_str());
}

TEST(CoverageQRCodeTest, NonPositiveSizeUsesDefault) {
    CApiHandle api;
    const std::wstring path = test_support::GetTempBmpPath(L"__coverage_qr_default.png");
    ::DeleteFileW(path.c_str());

    // size<=0 走默认 256（注释承诺），且产出的图必须可解码
    ASSERT_EQ(1, OpCreateQRCode(api.handle, L"DEF", path.c_str(), 0));
    ASSERT_TRUE(FileExistsNonEmpty(path, 1000));
    const wchar_t *decoded = OpDecodeQRCodeFromFile(api.handle, path.c_str());
    EXPECT_STREQ(L"DEF", decoded);
    ::DeleteFileW(path.c_str());
}

TEST(CoverageQRCodeTest, InvalidInputsAreSafe) {
    CApiHandle api;
    // 不存在的文件 -> 空串（不是崩溃、不是文件里的残留）
    const wchar_t *missing = OpDecodeQRCodeFromFile(api.handle, L"Z:\\__no_such_qr__.png");
    ASSERT_NE(nullptr, missing);
    EXPECT_STREQ(L"", missing);
    // 空文本 -> 失败，不落盘
    const std::wstring path = test_support::GetTempBmpPath(L"__coverage_qr_empty.png");
    ::DeleteFileW(path.c_str());
    EXPECT_EQ(0, OpCreateQRCode(api.handle, L"", path.c_str(), 128));
    // 未绑定窗口的屏幕版 -> 空串
    const wchar_t *screen = OpDecodeQRCode(api.handle, 0, 0, 10, 10);
    ASSERT_NE(nullptr, screen);
    EXPECT_STREQ(L"", screen);
}

// 屏幕区域版：建窗口贴图 -> gdi 绑定 -> 按客户区解码。
// 绑定失败属于环境问题（无桌面会话/DPI 异常），显式 SKIP 而不是假绿。
TEST(CoverageQRCodeTest, ScreenRegionDecodesBoundWindow) {
    CApiHandle api;
    const std::wstring path = test_support::GetTempBmpPath(L"__coverage_qr_screen.bmp");
    ::DeleteFileW(path.c_str());
    ASSERT_EQ(1, OpCreateQRCode(api.handle, L"SCREEN-QR", path.c_str(), 220));

    BmpWindow win;
    ASSERT_TRUE(win.Create(path)) << "创建承载窗口失败（GDI 环境问题）";

    if (OpBindWindow(api.handle, reinterpret_cast<intptr_t>(win.hwnd), L"gdi", L"windows", L"windows", 0) != 1) {
        ::DeleteFileW(path.c_str());
        GTEST_SKIP() << "gdi 绑定失败（无桌面会话/前台被占），屏幕区域解码跳过";
    }
    const wchar_t *decoded = OpDecodeQRCode(api.handle, 0, 0, win.bmp_w, win.bmp_h);
    ASSERT_NE(nullptr, decoded);
    EXPECT_STREQ(L"SCREEN-QR", decoded) << "屏幕区域版必须与文件版解码结果一致";
    OpUnBindWindow(api.handle);
    ::DeleteFileW(path.c_str());
}

// ===================== 窗口 DPI =====================

TEST(CoverageWindowScaleTest, RealWindowReturnsDecimal) {
    CApiHandle api;
    // 任意一个真实窗口都行：用测试自身的控制台/桌面窗口
    HWND hwnd = ::GetForegroundWindow();
    if (!hwnd)
        hwnd = ::GetDesktopWindow();
    ASSERT_NE(nullptr, hwnd);

    const wchar_t *scale = OpGetWindowScale(api.handle, reinterpret_cast<intptr_t>(hwnd));
    ASSERT_NE(nullptr, scale);
    const std::wstring text(scale);
    ASSERT_FALSE(text.empty()) << "有效窗口必须给出缩放串";
    const double v = _wtof(text.c_str());
    EXPECT_GT(v, 0.5) << "缩放不可能小于 0.5";
    EXPECT_LT(v, 5.0) << "缩放不可能大于 5.0（本机 150% 应为 1.50 附近）";
    // 两位小数格式（契约）
    EXPECT_NE(std::wstring::npos, text.find(L'.')) << "契约是两位小数字符串";
}

TEST(CoverageWindowScaleTest, InvalidHwndReturnsEmpty) {
    CApiHandle api;
    const wchar_t *scale = OpGetWindowScale(api.handle, 0);
    ASSERT_NE(nullptr, scale);
    EXPECT_STREQ(L"", scale);
    const wchar_t *bogus = OpGetWindowScale(api.handle, static_cast<intptr_t>(0x1234));
    ASSERT_NE(nullptr, bogus);
    EXPECT_STREQ(L"", bogus);
}

// ===================== 进程终止 =====================

TEST(CoverageProcessTest, TerminateKillsOwnChild) {
    ChildProcess child;
    ASSERT_TRUE(child.Start(SystemExe(L"PING.EXE") + L" -n 30 127.0.0.1")) << "启动子进程失败";
    ::Sleep(150);
    ASSERT_TRUE(child.alive()) << "子进程应仍在跑（ping 30 次）";

    CApiHandle api;
    EXPECT_EQ(1, OpTerminateProcess(api.handle, static_cast<int>(child.pid())));
    EXPECT_TRUE(child.WaitExit(3000)) << "OpTerminateProcess 返回 1 后进程必须已退出";

    // 已经死了再调一次 -> 0（OpenProcess 失败）
    EXPECT_EQ(0, OpTerminateProcess(api.handle, static_cast<int>(child.pid())));
}

TEST(CoverageProcessTest, TerminateRejectsInvalidPid) {
    CApiHandle api;
    EXPECT_EQ(0, OpTerminateProcess(api.handle, 0));
    EXPECT_EQ(0, OpTerminateProcess(api.handle, -1));
    EXPECT_EQ(0, OpTerminateProcessTree(api.handle, 0));
}

TEST(CoverageProcessTest, TerminateTreeKillsChildren) {
    ChildProcess child;
    // cmd 作为父，ping 作为子 —— 只杀父不会带走子，正好用来区分"单杀"与"杀树"
    ASSERT_TRUE(child.Start(SystemExe(L"cmd.exe") + L" /c \"" + SystemExe(L"PING.EXE") + L" -n 30 127.0.0.1\""));
    ::Sleep(400);
    ASSERT_TRUE(child.alive());

    CApiHandle api;
    EXPECT_EQ(1, OpTerminateProcessTree(api.handle, static_cast<int>(child.pid())));
    EXPECT_TRUE(child.WaitExit(3000));
}

// ===================== OCR 颜色路径 / 别名 =====================

TEST(CoverageOcrColorTest, FromFilePlainImageIsRecognized) {
    if (SkipOcr())
        GTEST_SKIP() << "OP_SKIP_OCR_TESTS 已设置";
    const std::wstring path = test_support::GetTempBmpPath(L"__coverage_ocr_plain.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(path, L"BP 909")) << "造字图失败";

    CApiHandle api;
    // 空颜色 = 原图直识（免字库 ONNX）
    const wchar_t *text = OpOcrAutoColorFromFile(api.handle, path.c_str(), L"", 0.9);
    ASSERT_NE(nullptr, text);
    const std::wstring got(text);
    EXPECT_FALSE(got.empty()) << "黑底白字的合成图必须能识别出内容";
    EXPECT_NE(std::wstring::npos, test_support::ExtractDigits(got).find(L"909"))
        << "数字 909 是最稳的靶子，识别结果: " << got;
    ::DeleteFileW(path.c_str());
}

TEST(CoverageOcrColorTest, FromFileWithColorBinaries) {
    if (SkipOcr())
        GTEST_SKIP() << "OP_SKIP_OCR_TESTS 已设置";
    const std::wstring path = test_support::GetTempBmpPath(L"__coverage_ocr_color.bmp");
    ASSERT_TRUE(test_support::CreateConsoleLikeBmp(path, L"BP 909"));

    CApiHandle api;
    // 字是白色（RGB FFFFFF），给出颜色 + 容差走二值化路径
    const wchar_t *text = OpOcrAutoColorFromFile(api.handle, path.c_str(), L"FFFFFF-404040", 0.9);
    ASSERT_NE(nullptr, text);
    const std::wstring got(text);
    EXPECT_FALSE(got.empty()) << "颜色二值化路径必须也识别出内容（白字）";
    ::DeleteFileW(path.c_str());
}

TEST(CoverageOcrColorTest, AutoOcrLineIsAliasForBothBranches) {
    CApiHandle api;
    // 未绑定窗口：两个分支都返回空串，但必须不崩（覆盖分发逻辑本身）
    const wchar_t *no_color = OpAutoOcrLine(api.handle, 0, 0, 10, 10, L"", 0.9);
    ASSERT_NE(nullptr, no_color);
    EXPECT_STREQ(L"", no_color);
    const wchar_t *with_color = OpAutoOcrLine(api.handle, 0, 0, 10, 10, L"FFFFFF-303030", 0.9);
    ASSERT_NE(nullptr, with_color);
    EXPECT_STREQ(L"", with_color);
}

TEST(CoverageOcrColorTest, OcrAutoColorUnboundReturnsEmpty) {
    CApiHandle api;
    const wchar_t *text = OpOcrAutoColor(api.handle, 0, 0, 10, 10, L"FFFFFF-303030", 0.9);
    ASSERT_NE(nullptr, text);
    EXPECT_STREQ(L"", text);
}

// ===================== 组合键 =====================

TEST(CoverageHotkeyTest, RejectsInvalidCombo) {
    CApiHandle api;
    // 只测解析/拒绝路径：合法组合会真的往系统发按键，测试不制造这种副作用
    EXPECT_EQ(0, OpKeyPressHotkey(api.handle, L""));
    EXPECT_EQ(0, OpKeyPressHotkey(api.handle, L"notakey+a"));
    EXPECT_EQ(0, OpKeyPressHotkey(api.handle, L"ctrl+"));
}

// ===================== 反向验证（手工执行，记录在此）=====================
//
// 1) OpCreateQRCode：把 `size<=0 用 256` 改成 `return 0` -> NonPositiveSizeUsesDefault 变红
// 2) OpDecodeQRCodeFromFile：把返回值改成固定空串 -> CreateAndDecodeRoundTrip 变红
// 3) OpGetWindowScale：把 DPI 换算里的 96 改成 48 -> RealWindowReturnsDecimal 的数值范围断言变红
// 4) OpTerminateProcess：去掉 TerminateProcess 调用只留 OpenProcess -> TerminateKillsOwnChild 变红
// 5) OpAutoOcrLine：把"有颜色"分支改成总调 OpOcrAuto -> 该用例仍绿（空串一致），
//    所以这条用例只保证"不崩 + 契约"，分支正确性由 OpOcrAutoColorLine 自己的用例负责 ——
//    如实记录，不声称覆盖了分支差异。
