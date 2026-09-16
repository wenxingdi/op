// dx_target.cpp —— DX 输入通道验证专用目标窗口
//
// 用途：为 op 的 dx 输入通道提供可复现的验证目标，摆脱"依赖第三方程序窗口"的不确定性。
//
// 设计要点：
//   1. 显式 LoadLibraryW("dinput8.dll")，满足 op 的 DirectInput 通道门槛
//      （libop/hook/InputHook.cpp:883 要求 hook_dinput() 成功，否则整体输入绑定失败）
//   2. 窗口标题实时回报收到的输入计数：move/down/up/key
//      → 验证脚本可读标题自动判定"目标是否响应"，无需人工观察
//   3. 把 exe 放在没有 onnxruntime.dll 的目录里运行，即可验证 DELAYLOAD 修复是否生效
//      （修复前：注入的 op_c_api_x64.dll 找不到 onnxruntime.dll → 0xC0000135）
//
// 构建：python scripts/build_dx_target.py [输出目录]
// 使用：scripts/dx_probe.py --hwnd <窗口句柄> --quick --no-click

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static volatile LONG g_move = 0;
static volatile LONG g_down = 0;
static volatile LONG g_up = 0;
static volatile LONG g_key = 0;
static volatile LONG g_wheel = 0;
static HWND g_hwnd = NULL;
static const wchar_t *kClassName = L"OpDxTargetWnd";

static void refresh_title() {
    wchar_t buf[256];
    _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
                 L"OpDxTarget move=%ld down=%ld up=%ld key=%ld wheel=%ld",
                 g_move, g_down, g_up, g_key, g_wheel);
    if (g_hwnd)
        SetWindowTextW(g_hwnd, buf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        InterlockedIncrement(&g_move);
        refresh_title();
        break;
    case WM_LBUTTONDOWN:
        InterlockedIncrement(&g_down);
        refresh_title();
        break;
    case WM_LBUTTONUP:
        InterlockedIncrement(&g_up);
        refresh_title();
        break;
    case WM_KEYDOWN:
        InterlockedIncrement(&g_key);
        refresh_title();
        break;
    case WM_MOUSEWHEEL:
        InterlockedIncrement(&g_wheel);
        refresh_title();
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(24, 28, 36));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 200, 255));
        wchar_t line[256];
        _snwprintf_s(line, ARRAYSIZE(line), _TRUNCATE,
                     L"OP DX TARGET\n\nmove=%ld\ndown=%ld\nup=%ld\nkey=%ld\nwheel=%ld",
                     g_move, g_down, g_up, g_key, g_wheel);
        DrawTextW(dc, line, -1, &rc, DT_CENTER | DT_VCENTER | DT_NOCLIP);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        refresh_title();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // 关键：显式加载 dinput8.dll，让目标进程具备 DirectInput 设备创建能力
    HMODULE di8 = LoadLibraryW(L"dinput8.dll");

    FILE *fp = NULL;
    if (fopen_s(&fp, "dx_target.log", "w") == 0 && fp) {
        fprintf(fp, "dinput8.dll = %s\n", di8 ? "LOADED" : "MISSING");
        fprintf(fp, "pid = %lu\n", (unsigned long)GetCurrentProcessId());
        fflush(fp);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        if (fp) fprintf(fp, "RegisterClassExW failed err=%lu\n", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, L"OpDxTarget starting",
                                WS_OVERLAPPEDWINDOW, 120, 120, 560, 380,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) {
        if (fp) fprintf(fp, "CreateWindowExW failed err=%lu\n", GetLastError());
        return 2;
    }
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 500, NULL);
    refresh_title();

    if (fp) {
        fprintf(fp, "hwnd = %lld\n", (long long)(INT_PTR)hwnd);
        fflush(fp);
        fclose(fp);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (di8)
        FreeLibrary(di8);
    return 0;
}
