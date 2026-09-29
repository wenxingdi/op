// plain_carrier.cpp —— 最"素"的窗口载体：只依赖 user32 / gdi32 / kernel32。
//
// 为什么还需要一个载体：dx_carrier 更真实（真交换链 + 四象限纯色），但它**静态导入**
// d3d9.dll / d3d11.dll / dinput8.dll —— "目标进程尚未加载这些库"这个前提在它身上永远
// 不成立，于是 op 的**按需模块加载**（ApiResolver 的 on-demand 分支）无从验证：
// 拿它当靶子只会看到用例被 SKIP（前提不成立），或者更糟——看着 PASS 却什么也没证明。
// 本载体启动后模块表里只有系统基础库，是那条路径唯一的干净靶子。
//
// 用法：plain_carrier.exe [--backend <忽略>] [--seconds N] [--report FILE] [--w N] [--h N]
// 回报文件与 dx_carrier 同格式（hwnd= / pid= / size= / READY），测试侧 CarrierProcess
// 的解析逻辑可直接复用（--backend 由它统一拼装，本载体无图形后端，接受但忽略）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND g_hwnd = nullptr;
static int g_w = 400;
static int g_h = 300;
static bool g_running = true;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_running = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // 四象限，与 dx_carrier 同色序（左上红 / 右上绿 / 左下蓝 / 右下白）：
        // 万一有人拿它去测 gdi 抓取，判据可以通用。
        const COLORREF colors[4] = {RGB(0xFF, 0, 0), RGB(0, 0xFF, 0), RGB(0, 0, 0xFF), RGB(0xFF, 0xFF, 0xFF)};
        for (int i = 0; i < 4; ++i) {
            RECT q;
            q.left = rc.left + (i % 2) * (rc.right - rc.left) / 2;
            q.top = rc.top + (i / 2) * (rc.bottom - rc.top) / 2;
            q.right = rc.left + (i % 2 + 1) * (rc.right - rc.left) / 2;
            q.bottom = rc.top + (i / 2 + 1) * (rc.bottom - rc.top) / 2;
            HBRUSH brush = CreateSolidBrush(colors[i]);
            FillRect(dc, &q, brush);
            DeleteObject(brush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char **argv) {
    const char *report = nullptr;
    int seconds = 15;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--report") && i + 1 < argc)
            report = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--w") && i + 1 < argc)
            g_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h") && i + 1 < argc)
            g_h = atoi(argv[++i]);
        // --backend / --msaa / --dinput / --solid 等由测试侧统一拼装，本载体没有图形后端，忽略。
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PlainCarrierWnd";
    if (!RegisterClassExW(&wc)) {
        printf("[plain] register class FAILED err=%lu\n", GetLastError());
        fflush(stdout);
        return 2;
    }

    RECT rc = {0, 0, g_w, g_h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, L"PlainCarrierWnd", L"plain_carrier", WS_OVERLAPPEDWINDOW, 100, 100,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        printf("[plain] create window FAILED err=%lu\n", GetLastError());
        fflush(stdout);
        return 2;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    printf("[plain] hwnd=%p pid=%lu size=%dx%d\n", static_cast<void *>(g_hwnd), GetCurrentProcessId(), g_w, g_h);
    fflush(stdout);

    if (report) {
        FILE *fp = fopen(report, "w");
        if (fp) {
            fprintf(fp, "hwnd=0x%llX\npid=%lu\nsize=%dx%d\nbackend=plain\nmsaa=0\ndinput=0\nREADY\n",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_hwnd)), GetCurrentProcessId(), g_w,
                    g_h);
            fclose(fp);
        }
    }

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg;
    while (g_running && GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(16);
    }
    printf("[plain] exit after %d seconds budget\n", seconds);
    fflush(stdout);
    return 0;
}
