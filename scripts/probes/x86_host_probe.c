// x86_host_probe.c — 32 位宿主探针：真载入 op_c_api_x86.dll 并调核心 API。
//
// 用法：
//   x86_host_probe.exe selftest            前台自检（加载 + OpCreate/Ver/GetID/FindWindow/Destroy）
//   x86_host_probe.exe waitmodule <ms>     等待 hook dll 被注入（供 InjectDll 目标进程用）
//
// 编译（本仓库 x86 工具链，见 build/_wb_build_op_x86.py 的 x86_env）：
//   cl /nologo /W3 /O2 /DWIN32 x86_host_probe.c /Fe:x86_host_probe.exe
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void *(__stdcall *OpCreate_t)(void);
typedef void(__stdcall *OpDestroy_t)(void *);
typedef const wchar_t *(__stdcall *OpVer_t)(void);
typedef int(__stdcall *OpGetID_t)(void *);
typedef int(__stdcall *OpSetPath_t)(void *, const wchar_t *);
typedef const wchar_t *(__stdcall *OpGetPath_t)(void *);
typedef intptr_t(__stdcall *OpFindWindow_t)(void *, const wchar_t *, const wchar_t *);


static void *g_mod;

static int selftest(void) {
    OpCreate_t p_OpCreate;
    OpVer_t p_OpVer;
    OpDestroy_t p_OpDestroy;
    OpGetID_t p_OpGetID;
    OpFindWindow_t p_OpFindWindow;
    OpGetPath_t p_OpGetPath;
    const wchar_t *dll = L"op_c_api_x86.dll";
    const wchar_t *ver;
    const wchar_t *path;
    void *h;
    int id;
    intptr_t hwnd;

    setvbuf(stdout, NULL, _IONBF, 0);
    g_mod = LoadLibraryW(dll);
    if (!g_mod) {
        printf("[FAIL] LoadLibraryW(%ls) err=%lu\n", dll, GetLastError());
        return 1;
    }
    printf("[OK] LoadLibrary op_c_api_x86.dll\n");

    p_OpCreate = (OpCreate_t)GetProcAddress(g_mod, "OpCreate");
    p_OpVer = (OpVer_t)GetProcAddress(g_mod, "OpVer");
    p_OpDestroy = (OpDestroy_t)GetProcAddress(g_mod, "OpDestroy");
    p_OpGetID = (OpGetID_t)GetProcAddress(g_mod, "OpGetID");
    p_OpFindWindow = (OpFindWindow_t)GetProcAddress(g_mod, "OpFindWindow");
    p_OpGetPath = (OpGetPath_t)GetProcAddress(g_mod, "OpGetPath");
    if (!p_OpCreate || !p_OpVer || !p_OpDestroy || !p_OpGetID || !p_OpFindWindow || !p_OpGetPath) {
        printf("[FAIL] GetProcAddress\n");
        return 2;
    }

    ver = p_OpVer();
    printf("[OK] OpVer -> %ls\n", ver ? ver : L"(null)");

    h = p_OpCreate();
    if (!h) {
        printf("[FAIL] OpCreate\n");
        return 3;
    }
    printf("[OK] OpCreate -> handle\n");

    id = p_OpGetID(h);
    printf(id >= 0 ? "[OK] OpGetID -> %d\n" : "[FAIL] OpGetID -> %d\n", id);

    printf("[..] calling OpGetPath\n");
    path = p_OpGetPath(h);
    printf("[OK] OpGetPath -> %ls\n", path ? path : L"(null)");

    // 找自身窗口（探针是控制台，找不到也属正常——只验证不崩、返回 0/-1 合法值）
    hwnd = p_OpFindWindow(h, NULL, L"not_existent_window_xyz");
    printf(hwnd == 0 ? "[OK] OpFindWindow(miss) -> 0\n" : "[FAIL] OpFindWindow(miss) -> %td\n", hwnd);

    p_OpDestroy(h);
    printf("[OK] OpDestroy\n");

    FreeLibrary(g_mod);
    printf("[DONE] x86 selftest all passed\n");
    return 0;
}

static int waitmodule(int timeout_ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)timeout_ms) {
        if (GetModuleHandleW(L"op_c_api_x86.dll")) {
            printf("[HOOK-LOADED] after %lu ms\n", GetTickCount() - t0);
            return 0;
        }
        Sleep(100);
    }
    printf("[TIMEOUT] hook not injected within %d ms\n", timeout_ms);
    return 1;
}

typedef int(__stdcall *OpSleep_t)(void *, int);
typedef int(__stdcall *OpGetScreenWidth_t)(void *); // 返回值即宽度（无出参），见 op_c_api.h
typedef const wchar_t *(__stdcall *OpGetBasePath_t)(void *);

static int stress(void) {
    const wchar_t *dll = L"op_c_api_x86.dll";
    void *h;
    long v = -99;
    const wchar_t *s;

    setvbuf(stdout, NULL, _IONBF, 0);
    g_mod = LoadLibraryW(dll);
    if (!g_mod) {
        printf("[FAIL] LoadLibrary\n");
        return 1;
    }
    {
        OpCreate_t pCreate = (OpCreate_t)GetProcAddress(g_mod, "OpCreate");
        OpGetBasePath_t pBase = (OpGetBasePath_t)GetProcAddress(g_mod, "OpGetBasePath");
        OpGetPath_t pPath = (OpGetPath_t)GetProcAddress(g_mod, "OpGetPath");
        OpGetScreenWidth_t pW = (OpGetScreenWidth_t)GetProcAddress(g_mod, "OpGetScreenWidth");
        OpDestroy_t pDestroy = (OpDestroy_t)GetProcAddress(g_mod, "OpDestroy");

        printf("[stress] create\n");
        h = pCreate();
        printf("[stress] handle=%p\n", h);

        printf("[stress] call OpGetScreenWidth\n");
        v = pW(h);
        printf("[stress] screen w=%ld (raw GetSystemMetrics(SM_CXSCREEN)=%d)\n",
               v, GetSystemMetrics(SM_CXSCREEN));

        printf("[stress] call OpGetBasePath\n");
        s = pBase(h);
        printf("[stress] base=%ls\n", s ? s : L"(null)");

        printf("[stress] call OpGetPath\n");
        s = pPath(h);
        printf("[stress] path=%ls\n", s ? s : L"(null)");

        printf("[stress] destroy\n");
        pDestroy(h);
        printf("[stress] done\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "waitmodule") == 0)
        return waitmodule(argc >= 3 ? atoi(argv[2]) : 10000);
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
        return selftest();
    if (argc >= 2 && strcmp(argv[1], "stress") == 0)
        return stress();
    printf("usage: %s selftest | waitmodule <ms> | stress\n", argv[0]);
    return 9;
}
