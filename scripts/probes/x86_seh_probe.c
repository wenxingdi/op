// x86_seh_probe.c — 抓 OpGetPath 的异常码/地址
#include <windows.h>
#include <stdio.h>

typedef void *(__stdcall *OpCreate_t)(void);
typedef const wchar_t *(__stdcall *OpGetPath_t)(void *);
typedef void(__stdcall *OpDestroy_t)(void *);

static void *g_mod;
static void *g_h;
static OpGetPath_t g_pGetPath;

static int Filter(EXCEPTION_POINTERS *ep, DWORD *code, DWORD *addr) {
    *code = ep->ExceptionRecord->ExceptionCode;
    if (ep->ExceptionRecord->ExceptionInformation[0] == 1 || 0) {
        *addr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
    } else {
        *addr = (DWORD)ep->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(void) {
    DWORD code = 0, addr = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    g_mod = LoadLibraryW(L"op_c_api_x86.dll");
    if (!g_mod) { printf("LoadLibrary fail\n"); return 1; }
    {
        OpCreate_t pCreate = (OpCreate_t)GetProcAddress(g_mod, "OpCreate");
        OpDestroy_t pDestroy = (OpDestroy_t)GetProcAddress(g_mod, "OpDestroy");
        g_pGetPath = (OpGetPath_t)GetProcAddress(g_mod, "OpGetPath");
        g_h = pCreate();
        printf("handle=%p\n", g_h);

        __try {
            const wchar_t *s = g_pGetPath(g_h);
            wprintf(L"path=%s\n", s);
        } __except (Filter(GetExceptionInformation(), &code, &addr)) {
            printf("EXC code=0x%08X addr=0x%08X\n", code, addr);
            printf("m_context-ish? getpath fn=%p\n", g_pGetPath);
        }
        pDestroy(g_h);
        printf("destroy ok\n");
    }
    return 0;
}
