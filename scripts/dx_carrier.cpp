// dx_carrier.cpp —— 图形捕获真机载体（独立进程，多渲染后端）
//
// 为什么需要它：
//   tests/ 里所有窗口载体都是纯 GDI（FillRect），没有真实 D3D/OpenGL 交换链，
//   于是 op 的 DisplayHook（hook Present / EndScene / wglSwapBuffers）在回归里从未被触发过。
//   本程序是**独立进程**的渲染目标，供 op 走真实注入路径绑定后验证「帧内容是否被正确取回」。
//
// 关键设计：
//   1. 画面是**四象限纯色**（左上红/右上绿/左下蓝/右下白），不是单色。
//      单色测不出红蓝互换与错位；四象限同时覆盖：全黑、红蓝互换、原点偏移、尺寸错。
//   2. D3D10/11 走「CPU 生成像素 -> UpdateSubresource -> CopyResource 到后缓冲」，
//      D3D9/GL 走「scissor + Clear 分象限」，都零 shader，像素完全可控。
//   3. 启动后把 hwnd/pid/size 打到 stdout 并 flush，测试侧读走即可绑定。
//   4. --msaa N 可开多重采样：D3D9 无 resolve 分支（已知缺口），D3D10/11 有。
//
// 构建：python scripts/build_dx_carrier.py
// 用法：dx_carrier.exe --backend d3d11 --seconds 20
//       dx_carrier.exe --backend d3d9  --msaa 4
//       dx_carrier.exe --backend gl
//       dx_carrier.exe --backend d3d11 --solid FF0000   # 单色（反向验证用）

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static const wchar_t *kClassName = L"OpDxCarrierWnd";
static HWND g_hwnd = NULL;
static volatile LONG g_frames = 0;
static int g_width = 400;
static int g_height = 300;

// 四象限 RGB（TL, TR, BL, BR），与 hook_carrier_probe.py 的 QUADRANTS 严格对应
static unsigned int g_quad[4] = {0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static bool CreateCarrierWindow() {
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // 无边框 + 可见：窗口尺寸即客户区尺寸，op 的 GetColor 坐标系（客户区原点）直接对应画面
    g_hwnd = CreateWindowExW(0, kClassName, L"OpDxCarrier", WS_POPUP | WS_VISIBLE,
                             40, 40, g_width, g_height, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return false;
    UpdateWindow(g_hwnd);
    return true;
}

// BGRA 顶行优先像素（D3D10/11 的 UpdateSubresource 约定）
static void BuildBGRA(unsigned char *pixels, int w, int h) {
    for (int y = 0; y < h; ++y) {
        const int row = (y < h / 2) ? 0 : 2;
        for (int x = 0; x < w; ++x) {
            const unsigned int c = g_quad[row + (x < w / 2 ? 0 : 1)];
            unsigned char *p = pixels + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<unsigned char>(c & 0xFF);          // B
            p[1] = static_cast<unsigned char>((c >> 8) & 0xFF);   // G
            p[2] = static_cast<unsigned char>((c >> 16) & 0xFF);  // R
            p[3] = 0xFF;
        }
    }
}

static void FillQuadrantRects(RECT rects[4], int w, int h) {
    rects[0] = {0, 0, w / 2, h / 2};
    rects[1] = {w / 2, 0, w, h / 2};
    rects[2] = {0, h / 2, w / 2, h};
    rects[3] = {w / 2, h / 2, w, h};
}

// ----------------------------------------------------------------- D3D9

struct CarrierD3D9 {
    IDirect3D9 *d3d = nullptr;
    IDirect3DDevice9 *device = nullptr;
    IDirect3DTexture9 *texSys = nullptr;
    IDirect3DTexture9 *texDef = nullptr;
    int w = 0, h = 0;
    int msaa = 0;
    unsigned char *scratch = nullptr;

    bool Init(HWND hwnd, int width, int height, int samples) {
        w = width;
        h = height;
        msaa = samples;
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d)
            return false;

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.BackBufferWidth = static_cast<UINT>(w);
        pp.BackBufferHeight = static_cast<UINT>(h);
        pp.hDeviceWindow = hwnd;
        pp.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(D3DMULTISAMPLE_NONE);
        pp.MultiSampleQuality = 0;
        if (msaa > 1) {
            if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                          D3DFMT_A8R8G8B8, TRUE,
                                                          static_cast<D3DMULTISAMPLE_TYPE>(msaa), NULL))) {
                pp.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(msaa);
            } else {
                printf("[carrier] MSAA %dx unsupported, fallback to none\n", msaa);
                msaa = 0;
            }
        }

        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                       &pp, &device);
        if (FAILED(hr)) {
            printf("[carrier] D3D9 CreateDevice FAILED hr=0x%08X\n", static_cast<unsigned>(hr));
            return false;
        }

        scratch = static_cast<unsigned char *>(malloc(static_cast<size_t>(w) * h * 4));
        if (!scratch)
            return false;
        BuildBGRA(scratch, w, h);

        hr = device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texSys, NULL);
        if (FAILED(hr))
            return false;
        D3DLOCKED_RECT lr = {};
        if (FAILED(texSys->LockRect(0, &lr, NULL, 0)))
            return false;
        for (int y = 0; y < h; ++y) {
            memcpy(static_cast<unsigned char *>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                   scratch + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
        }
        texSys->UnlockRect(0);

        hr = device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texDef, NULL);
        if (FAILED(hr))
            return false;
        if (FAILED(device->UpdateTexture(texSys, texDef)))
            return false;
        return true;
    }

    void Frame() {
        IDirect3DSurface9 *backbuffer = nullptr;
        IDirect3DSurface9 *src = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
            SUCCEEDED(texDef->GetSurfaceLevel(0, &src))) {
            device->BeginScene();
            device->StretchRect(src, NULL, backbuffer, NULL, D3DTEXF_NONE);
            device->EndScene();
            device->Present(NULL, NULL, NULL, NULL);
            InterlockedIncrement(&g_frames);
            src->Release();
            backbuffer->Release();
        }
    }

    void Shutdown() {
        if (texSys) texSys->Release();
        if (texDef) texDef->Release();
        if (device) device->Release();
        if (d3d) d3d->Release();
        free(scratch);
    }
};

// ----------------------------------------------------------------- D3D11

struct CarrierD3D11 {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGISwapChain *swapchain = nullptr;
    ID3D11Texture2D *source = nullptr;

    bool Init(HWND hwnd, int w, int h, char *driver, int samples) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = static_cast<UINT>(w);
        desc.BufferDesc.Height = static_cast<UINT>(h);
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = hwnd;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (samples > 1) {
            desc.SampleDesc.Count = static_cast<UINT>(samples);
            printf("[carrier] D3D11 MSAA requested count=%d\n", samples);
        }

        D3D_DRIVER_TYPE types[2] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
        HRESULT hr = E_FAIL;
        for (int i = 0; i < 2; ++i) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, types[i], nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &desc, &swapchain, &device,
                                               nullptr, &context);
            if (SUCCEEDED(hr)) {
                strncpy_s(driver, 16, i == 0 ? "HARDWARE" : "WARP", _TRUNCATE);
                break;
            }
        }
        if (FAILED(hr)) {
            printf("[carrier] D3D11CreateDeviceAndSwapChain FAILED hr=0x%08X\n", static_cast<unsigned>(hr));
            return false;
        }

        ID3D11Texture2D *backbuffer = nullptr;
        if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backbuffer))))
            return false;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = 0;
        hr = device->CreateTexture2D(&td, nullptr, &source);
        backbuffer->Release();
        if (FAILED(hr))
            return false;
        return true;
    }

    bool Upload(const unsigned char *pixels, int w, int h) {
        context->UpdateSubresource(source, 0, nullptr, pixels, static_cast<UINT>(w) * 4, 0);
        return true;
    }

    void Frame() {
        ID3D11Texture2D *backbuffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backbuffer)))) {
            context->CopyResource(backbuffer, source);
            backbuffer->Release();
            swapchain->Present(0, 0);
            InterlockedIncrement(&g_frames);
        }
    }

    void Shutdown() {
        if (source) source->Release();
        if (context) context->Release();
        if (device) device->Release();
        if (swapchain) swapchain->Release();
    }
};

// ----------------------------------------------------------------- OpenGL

struct CarrierGL {
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    int w = 0, h = 0;

    bool Init(HWND hwnd, int width, int height) {
        w = width;
        h = height;
        dc = GetDC(hwnd);
        if (!dc)
            return false;

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.iLayerType = PFD_MAIN_PLANE;

        const int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
            printf("[carrier] SetPixelFormat FAILED err=%lu\n", GetLastError());
            return false;
        }
        rc = wglCreateContext(dc);
        if (!rc || !wglMakeCurrent(dc, rc)) {
            printf("[carrier] wglCreateContext/MakeCurrent FAILED\n");
            return false;
        }
        printf("[carrier] GL_VENDOR=%s GL_RENDERER=%s\n",
               reinterpret_cast<const char *>(glGetString(GL_VENDOR)),
               reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        return true;
    }

    void Frame() {
        RECT rects[4];
        FillQuadrantRects(rects, w, h);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < 4; ++i) {
            const unsigned int c = g_quad[i];
            // GL 的 scissor 原点在左下角
            const int y0 = h - rects[i].bottom;
            const int y1 = h - rects[i].top;
            glScissor(rects[i].left, y0, rects[i].right - rects[i].left, y1 - y0);
            glClearColor(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);

        // opengl.std 通道 hook 的是 glBegin、opengl.fi 通道 hook 的是 glFinish ——
        // 若应用两者都不调用，这两条通道永远拿不到帧（实测全黑）。
        // 这里补一对空的 glBegin/glEnd 与一次 glFinish：不改变任何像素，只让 hook 点可触发。
        glBegin(GL_QUADS);
        glEnd();
        glFinish();

        SwapBuffers(dc);
        InterlockedIncrement(&g_frames);
    }

    void Shutdown() {
        if (rc) {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(rc);
        }
        if (dc && g_hwnd)
            ReleaseDC(g_hwnd, dc);
    }
};

// ----------------------------------------------------------------- main

static void PrintUsage() {
    printf("usage: dx_carrier.exe --backend d3d9|d3d11|opengl|d3d12 [--solid RRGGBB] "
           "[--msaa N] [--seconds N] [--w N] [--h N] [--report FILE]\n");
}

int main(int argc, char **argv) {
    // DPI 感知必须开：不感知时 Windows 会虚拟化放大本进程窗口（实测系统 150% 缩放下
    // 400x300 会变成 600x450 物理像素），而 op 全程按物理像素工作 —— 两边尺寸语义不一致，
    // 取帧链路会按错误尺寸读缓冲。
    if (HMODULE user32_module = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI * SetDpiCtxFn)(DPI_AWARENESS_CONTEXT);
        auto set_ctx = reinterpret_cast<SetDpiCtxFn>(GetProcAddress(user32_module, "SetProcessDpiAwarenessContext"));
        if (set_ctx)
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else
            SetProcessDPIAware();
    }

    const char *backend = "d3d11";
    const char *solid = nullptr;
    const char *report_path = nullptr;
    int seconds = 15;
    int msaa = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            backend = argv[++i];
        else if (!strcmp(argv[i], "--solid") && i + 1 < argc)
            solid = argv[++i];
        else if (!strcmp(argv[i], "--report") && i + 1 < argc)
            report_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--msaa") && i + 1 < argc)
            msaa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--w") && i + 1 < argc)
            g_width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h") && i + 1 < argc)
            g_height = atoi(argv[++i]);
        else {
            PrintUsage();
            return 1;
        }
    }

    if (solid) {
        const unsigned int c = static_cast<unsigned int>(strtoul(solid, nullptr, 16));
        g_quad[0] = g_quad[1] = g_quad[2] = g_quad[3] = c;
        printf("[carrier] mode=solid #%06X\n", c & 0xFFFFFF);
    } else {
        printf("[carrier] mode=quadrants TL=red TR=green BL=blue BR=white\n");
    }

    if (strcmp(backend, "d3d12") == 0) {
        printf("[carrier] d3d12 not implemented yet\n");
        return 3;
    }

    if (!CreateCarrierWindow()) {
        printf("[carrier] create window FAILED err=%lu\n", GetLastError());
        return 2;
    }

    CarrierD3D9 c9;
    CarrierD3D11 c11;
    CarrierGL cgl;
    char driver[16] = "?";
    unsigned char *pixels = nullptr;
    bool ok = false;

    if (strcmp(backend, "d3d9") == 0) {
        ok = c9.Init(g_hwnd, g_width, g_height, msaa);
    } else if (strcmp(backend, "d3d11") == 0 || strcmp(backend, "d3d10") == 0) {
        ok = c11.Init(g_hwnd, g_width, g_height, driver, msaa);
        if (ok) {
            pixels = static_cast<unsigned char *>(malloc(static_cast<size_t>(g_width) * g_height * 4));
            if (pixels) {
                BuildBGRA(pixels, g_width, g_height);
                ok = c11.Upload(pixels, g_width, g_height);
            } else {
                ok = false;
            }
        }
    } else if (strcmp(backend, "opengl") == 0) {
        ok = cgl.Init(g_hwnd, g_width, g_height);
    } else {
        printf("[carrier] unknown backend=%s\n", backend);
        PrintUsage();
        return 1;
    }

    if (!ok) {
        printf("[carrier] backend=%s init FAILED\n", backend);
        return 4;
    }

    printf("[carrier] backend=%s driver=%s msaa=%d\n", backend, driver, msaa);
    printf("[carrier] hwnd=0x%llX pid=%lu size=%dx%d\n",
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_hwnd)),
           GetCurrentProcessId(), g_width, g_height);
    printf("[carrier] READY\n");
    fflush(stdout);

    // --report <file>：把就绪信息写文件，供测试侧轮询。
    // 用文件而非管道，避免测试端在 ReadFile 上阻塞（进程未就绪时读端会一直等）。
    if (report_path) {
        FILE *fp = fopen(report_path, "wb");
        if (fp) {
            fprintf(fp, "hwnd=0x%llX\npid=%lu\nsize=%dx%d\nREADY\n",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_hwnd)),
                    GetCurrentProcessId(), g_width, g_height);
            fclose(fp);
        }
    }

    const DWORD deadline = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    MSG msg = {};
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (strcmp(backend, "d3d9") == 0)
            c9.Frame();
        else if (strcmp(backend, "d3d11") == 0 || strcmp(backend, "d3d10") == 0)
            c11.Frame();
        else
            cgl.Frame();
        Sleep(16);
    }

    printf("[carrier] exit after presenting %ld frames\n", static_cast<long>(g_frames));
    fflush(stdout);

    if (strcmp(backend, "d3d9") == 0)
        c9.Shutdown();
    else if (strcmp(backend, "d3d11") == 0 || strcmp(backend, "d3d10") == 0)
        c11.Shutdown();
    else
        cgl.Shutdown();
    free(pixels);
    DestroyWindow(g_hwnd);
    return 0;
}
